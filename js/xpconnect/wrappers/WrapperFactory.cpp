/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WaiveXrayWrapper.h"
#include "FilteringWrapper.h"
#include "XrayWrapper.h"
#include "AccessCheck.h"
#include "XPCWrapper.h"
#include "ChromeObjectWrapper.h"

#include "xpcprivate.h"
#include "dombindings.h"
#include "XPCMaps.h"
#include "mozilla/dom/BindingUtils.h"
#include "jsfriendapi.h"
#include "mozilla/Likely.h"

using namespace js;

namespace xpc {

// When chrome pulls a naked property across the membrane using
// .wrappedJSObject, we want it to cross the membrane into the
// chrome compartment without automatically being wrapped into an
// X-ray wrapper. We achieve this by wrapping it into a special
// transparent wrapper in the origin (non-chrome) compartment. When
// an object with that special wrapper applied crosses into chrome,
// we know to not apply an X-ray wrapper.
DirectWrapper XrayWaiver(WrapperFactory::WAIVE_XRAY_WRAPPER_FLAG);

// When objects for which we waived the X-ray wrapper cross into
// chrome, we wrap them into a special cross-compartment wrapper
// that transitively extends the waiver to all properties we get
// off it.
WaiveXrayWrapper WaiveXrayWrapper::singleton(0);

static JSObject *
GetCurrentOuter(JSContext *cx, JSObject *obj)
{
    obj = JS_ObjectToOuterObject(cx, obj);
    if (!obj)
        return nullptr;

    if (IsWrapper(obj) && !js::GetObjectClass(obj)->ext.innerObject) {
        obj = UnwrapObject(obj);
        NS_ASSERTION(js::GetObjectClass(obj)->ext.innerObject,
                     "weird object, expecting an outer window proxy");
    }

    return obj;
}

JSObject *
WrapperFactory::GetXrayWaiver(JSObject *obj)
{
    // Object should come fully unwrapped but outerized.
    MOZ_ASSERT(obj == UnwrapObject(obj));
    MOZ_ASSERT(!js::GetObjectClass(obj)->ext.outerObject);
    CompartmentPrivate *priv = GetCompartmentPrivate(obj);
    MOZ_ASSERT(priv);

    if (!priv->waiverWrapperMap)
        return NULL;
    return xpc_UnmarkGrayObject(priv->waiverWrapperMap->Find(obj));
}

JSObject *
WrapperFactory::CreateXrayWaiver(JSContext *cx, JSObject *obj)
{
    // The caller is required to have already done a lookup.
    // NB: This implictly performs the assertions of GetXrayWaiver.
    MOZ_ASSERT(!GetXrayWaiver(obj));
    CompartmentPrivate *priv = GetCompartmentPrivate(obj);

    // Get a waiver for the proto.
    JSObject *proto = js::GetObjectProto(obj);
    if (proto && !(proto = WaiveXray(cx, proto)))
        return nullptr;

    // Create the waiver.
    JSAutoCompartment ac(cx, obj);
    if (!JS_WrapObject(cx, &proto))
        return nullptr;
    JSObject *waiver = Wrapper::New(cx, obj, proto,
                                    JS_GetGlobalForObject(cx, obj),
                                    &XrayWaiver);
    if (!waiver)
        return nullptr;

    // Add the new waiver to the map. It's important that we only ever have
    // one waiver for the lifetime of the target object.
    if (!priv->waiverWrapperMap) {
        priv->waiverWrapperMap = JSObject2JSObjectMap::
                                   newMap(XPC_WRAPPER_MAP_SIZE);
        MOZ_ASSERT(priv->waiverWrapperMap);
    }
    if (!priv->waiverWrapperMap->Add(obj, waiver))
        return nullptr;
    return waiver;
}

JSObject *
WrapperFactory::WaiveXray(JSContext *cx, JSObject *obj)
{
    obj = UnwrapObject(obj);

    // We have to make sure that if we're wrapping an outer window, that
    // the .wrappedJSObject also wraps the outer window.
    obj = GetCurrentOuter(cx, obj);

    JSObject *waiver = GetXrayWaiver(obj);
    if (waiver)
        return waiver;
    return CreateXrayWaiver(cx, obj);
}

// DoubleWrap is called from PrepareForWrapping to maintain the state that
// we're supposed to waive Xray wrappers for the given on. On entrance, it
// expects |cx->compartment != obj->compartment()|. The returned object will
// be in the same compartment as |obj|.
JSObject *
WrapperFactory::DoubleWrap(JSContext *cx, JSObject *obj, unsigned flags)
{
    if (flags & WrapperFactory::WAIVE_XRAY_WRAPPER_FLAG) {
        JSAutoCompartment ac(cx, obj);
        return WaiveXray(cx, obj);
    }
    return obj;
}

JSObject *
WrapperFactory::PrepareForWrapping(JSContext *cx, JSObject *scope, JSObject *obj, unsigned flags)
{
    // Don't unwrap an outer window, just double wrap it if needed.
    if (js::GetObjectClass(obj)->ext.innerObject)
        return DoubleWrap(cx, obj, flags);

    // Here are the rules for wrapping:
    // We should never get a proxy here (the JS engine unwraps those for us).
    JS_ASSERT(!IsWrapper(obj));

    // As soon as an object is wrapped in a security wrapper, it morphs to be
    // a fat wrapper. (see also: bug XXX).
    if (IS_SLIM_WRAPPER(obj) && !MorphSlimWrapper(cx, obj))
        return nullptr;

    // We only hand out outer objects to script.
    obj = GetCurrentOuter(cx, obj);
    if (!obj)
        return nullptr;

    if (js::GetObjectClass(obj)->ext.innerObject)
        return DoubleWrap(cx, obj, flags);

    // Now, our object is ready to be wrapped, but several objects (notably
    // nsJSIIDs) have a wrapper per scope. If we are about to wrap one of
    // those objects in a security wrapper, then we need to hand back the
    // wrapper for the new scope instead. Also, global objects don't move
    // between scopes so for those we also want to return the wrapper. So...
    if (!IS_WN_WRAPPER(obj) || !js::GetObjectParent(obj))
        return DoubleWrap(cx, obj, flags);

    XPCWrappedNative *wn = static_cast<XPCWrappedNative *>(xpc_GetJSPrivate(obj));

    JSAutoCompartment ac(cx, obj);
    XPCCallContext ccx(JS_CALLER, cx, obj);

    {
        if (NATIVE_HAS_FLAG(&ccx, WantPreCreate)) {
            // We have a precreate hook. This object might enforce that we only
            // ever create JS object for it.

            // Note: this penalizes objects that only have one wrapper, but are
            // being accessed across compartments. We would really prefer to
            // replace the above code with a test that says "do you only have one
            // wrapper?"
            JSObject *originalScope = scope;
            nsresult rv = wn->GetScriptableInfo()->GetCallback()->
                PreCreate(wn->Native(), cx, scope, &scope);
            NS_ENSURE_SUCCESS(rv, DoubleWrap(cx, obj, flags));

            // If the handed back scope differs from the passed-in scope and is in
            // a separate compartment, then this object is explicitly requesting
            // that we don't create a second JS object for it: create a security
            // wrapper.
            if (js::GetObjectCompartment(originalScope) != js::GetObjectCompartment(scope))
                return DoubleWrap(cx, obj, flags);

            JSObject *currentScope = JS_GetGlobalForObject(cx, obj);
            if (MOZ_UNLIKELY(scope != currentScope)) {
                // The wrapper claims it wants to be in the new scope, but
                // currently has a reflection that lives in the old scope. This
                // can mean one of two things, both of which are rare:
                //
                // 1 - The object has a PreCreate hook (we checked for it above),
                // but is deciding to request one-wrapper-per-scope (rather than
                // one-wrapper-per-native) for some reason. Usually, a PreCreate
                // hook indicates one-wrapper-per-native. In this case we want to
                // make a new wrapper in the new scope.
                //
                // 2 - We're midway through wrapper reparenting. The document has
                // moved to a new scope, but |wn| hasn't been moved yet, and
                // we ended up calling JS_WrapObject() on its JS object. In this
                // case, we want to return the existing wrapper.
                //
                // So we do a trick: call PreCreate _again_, but say that we're
                // wrapping for the old scope, rather than the new one. If (1) is
                // the case, then PreCreate will return the scope we pass to it
                // (the old scope). If (2) is the case, PreCreate will return the
                // scope of the document (the new scope).
                JSObject *probe;
                rv = wn->GetScriptableInfo()->GetCallback()->
                    PreCreate(wn->Native(), cx, currentScope, &probe);

                // Check for case (2).
                if (probe != currentScope) {
                    MOZ_ASSERT(probe == scope);
                    return DoubleWrap(cx, obj, flags);
                }

                // Ok, must be case (1). Fall through and create a new wrapper.
            }

            // Nasty hack for late-breaking bug 781476. This will confuse identity checks,
            // but it's probably better than any of our alternatives.
            //
            // Note: We have to ignore domain here. The JS engine assumes that, given a
            // compartment c, if c->wrap(x) returns a cross-compartment wrapper at time t0,
            // it will also return a cross-compartment wrapper for any time t1 > t0 unless
            // an explicit transplant is performed. In particular, wrapper recomputation
            // assumes that recomputing a wrapper will always result in a wrapper.
            //
            // This doesn't actually pose a security issue, because we'll still compute
            // the correct (opaque) wrapper for the object below given the security
            // characteristics of the two compartments.
            if (!AccessCheck::isChrome(js::GetObjectCompartment(scope)) &&
                 AccessCheck::subsumesIgnoringDomain(js::GetObjectCompartment(scope),
                                                     js::GetObjectCompartment(obj)))
            {
                return DoubleWrap(cx, obj, flags);
            }
        }
    }

    // NB: Passing a holder here inhibits slim wrappers under
    // WrapNativeToJSVal.
    nsCOMPtr<nsIXPConnectJSObjectHolder> holder;

    // This public WrapNativeToJSVal API enters the compartment of 'scope'
    // so we don't have to.
    jsval v;
    nsresult rv =
        nsXPConnect::FastGetXPConnect()->WrapNativeToJSVal(cx, scope, wn->Native(), nullptr,
                                                           &NS_GET_IID(nsISupports), false,
                                                           &v, getter_AddRefs(holder));
    if (NS_SUCCEEDED(rv)) {
        obj = JSVAL_TO_OBJECT(v);
        NS_ASSERTION(IS_WN_WRAPPER(obj), "bad object");

        // Because the underlying native didn't have a PreCreate hook, we had
        // to a new (or possibly pre-existing) XPCWN in our compartment.
        // This could be a problem for chrome code that passes XPCOM objects
        // across compartments, because the effects of QI would disappear across
        // compartments.
        //
        // So whenever we pull an XPCWN across compartments in this manner, we
        // give the destination object the union of the two native sets. We try
        // to do this cleverly in the common case to avoid too much overhead.
        XPCWrappedNative *newwn = static_cast<XPCWrappedNative *>(xpc_GetJSPrivate(obj));
        XPCNativeSet *unionSet = XPCNativeSet::GetNewOrUsed(ccx, newwn->GetSet(),
                                                            wn->GetSet(), false);
        if (!unionSet)
            return nullptr;
        newwn->SetSet(unionSet);
    }

    return DoubleWrap(cx, obj, flags);
}

static XPCWrappedNative *
GetWrappedNative(JSContext *cx, JSObject *obj)
{
    obj = JS_ObjectToInnerObject(cx, obj);
    return IS_WN_WRAPPER(obj)
           ? static_cast<XPCWrappedNative *>(js::GetObjectPrivate(obj))
           : nullptr;
}

enum XrayType {
    XrayForDOMObject,
    XrayForDOMProxyObject,
    XrayForWrappedNative,
    NotXray
};

static XrayType
GetXrayType(JSObject *obj)
{
    if (mozilla::dom::IsDOMObject(obj))
        return XrayForDOMObject;

    if (mozilla::dom::oldproxybindings::instanceIsProxy(obj))
        return XrayForDOMProxyObject;

    js::Class* clasp = js::GetObjectClass(obj);
    if (IS_WRAPPER_CLASS(clasp) || clasp->ext.innerObject) {
        NS_ASSERTION(clasp->ext.innerObject || IS_WN_WRAPPER_OBJECT(obj),
                     "We forgot to Morph a slim wrapper!");
        return XrayForWrappedNative;
    }
    return NotXray;
}

JSObject *
WrapperFactory::Rewrap(JSContext *cx, JSObject *obj, JSObject *wrappedProto, JSObject *parent,
                       unsigned flags)
{
    NS_ASSERTION(!IsWrapper(obj) ||
                 GetProxyHandler(obj) == &XrayWaiver ||
                 js::GetObjectClass(obj)->ext.innerObject,
                 "wrapped object passed to rewrap");
    NS_ASSERTION(JS_GetClass(obj) != &XrayUtils::HolderClass, "trying to wrap a holder");

    JSCompartment *origin = js::GetObjectCompartment(obj);
    JSCompartment *target = js::GetContextCompartment(cx);
    bool usingXray = false;

    // By default we use the wrapped proto of the underlying object as the
    // prototype for our wrapper, but we may select something different below.
    JSObject *proxyProto = wrappedProto;

    Wrapper *wrapper;
    CompartmentPrivate *targetdata = GetCompartmentPrivate(target);
    if (AccessCheck::isChrome(target)) {
        if (AccessCheck::isChrome(origin)) {
            wrapper = &CrossCompartmentWrapper::singleton;
        } else {
            if (flags & WAIVE_XRAY_WRAPPER_FLAG) {
                // If we waived the X-ray wrapper for this object, wrap it into a
                // special wrapper to transitively maintain the X-ray waiver.
                wrapper = &WaiveXrayWrapper::singleton;
            } else {
                // Native objects must be wrapped into an X-ray wrapper.
                XrayType type = GetXrayType(obj);
                if (type == XrayForDOMObject) {
                    wrapper = &XrayDOM::singleton;
                } else if (type == XrayForDOMProxyObject) {
                    wrapper = &XrayProxy::singleton;
                } else if (type == XrayForWrappedNative) {
                    typedef XrayWrapper<CrossCompartmentWrapper> Xray;
                    usingXray = true;
                    wrapper = &Xray::singleton;
                } else {
                    wrapper = &CrossCompartmentWrapper::singleton;
                }
            }
        }
    } else if (AccessCheck::isChrome(origin)) {
        JSFunction *fun = JS_GetObjectFunction(obj);
        if (fun) {
            if (JS_IsBuiltinEvalFunction(fun) || JS_IsBuiltinFunctionConstructor(fun)) {
                JS_ReportError(cx, "Not allowed to access chrome eval or Function from content");
                return nullptr;
            }
        }

        XPCWrappedNative *wn;
        if (targetdata &&
            (wn = GetWrappedNative(cx, obj)) &&
            wn->HasProto() && wn->GetProto()->ClassIsDOMObject()) {
            typedef XrayWrapper<CrossCompartmentSecurityWrapper> Xray;
            usingXray = true;
            if (IsLocationObject(obj))
                wrapper = &FilteringWrapper<Xray, LocationPolicy>::singleton;
            else
                wrapper = &FilteringWrapper<Xray, CrossOriginAccessiblePropertiesOnly>::singleton;
        } else if (mozilla::dom::IsDOMObject(obj)) {
            wrapper = &FilteringWrapper<XrayDOM, CrossOriginAccessiblePropertiesOnly>::singleton;
        } else if (mozilla::dom::oldproxybindings::instanceIsProxy(obj)) {
            wrapper = &FilteringWrapper<XrayProxy, CrossOriginAccessiblePropertiesOnly>::singleton;
        } else if (IsComponentsObject(obj)) {
            wrapper = &FilteringWrapper<CrossCompartmentSecurityWrapper,
                                        ComponentsObjectPolicy>::singleton;
        } else {
            wrapper = &ChromeObjectWrapper::singleton;

            // If the prototype of the chrome object being wrapped is a prototype
            // for a standard class, use the one from the content compartment so
            // that we can safely take advantage of things like .forEach().
            //
            // If the prototype chain of chrome object |obj| looks like this:
            //
            // obj => foo => bar => chromeWin.StandardClass.prototype
            //
            // The prototype chain of COW(obj) looks lke this:
            //
            // COW(obj) => COW(foo) => COW(bar) => contentWin.StandardClass.prototype
            JSProtoKey key = JSProto_Null;
            JSObject *unwrappedProto = NULL;
            if (wrappedProto && IsCrossCompartmentWrapper(wrappedProto) &&
                (unwrappedProto = Wrapper::wrappedObject(wrappedProto))) {
                JSAutoCompartment ac(cx, unwrappedProto);
                key = JS_IdentifyClassPrototype(cx, unwrappedProto);
            }
            if (key != JSProto_Null) {
                JSObject *homeProto;
                if (!JS_GetClassPrototype(cx, key, &homeProto))
                    return NULL;
                MOZ_ASSERT(homeProto);
                proxyProto = homeProto;
            }
        }
    } else if (AccessCheck::subsumes(target, origin)) {
        // For the same-origin case we use a transparent wrapper, unless one
        // of the following is true:
        // * The object is flagged as needing a SOW.
        // * The object is a Location object.
        // * The object is a Components object.
        // * The context compartment specifically requested Xray vision into
        //   same-origin compartments.
        //
        // The first two cases always require a security wrapper for non-chrome
        // access, regardless of the origin of the object.
        XrayType type;
        if (AccessCheck::needsSystemOnlyWrapper(obj)) {
            wrapper = &FilteringWrapper<CrossCompartmentSecurityWrapper,
                                        OnlyIfSubjectIsSystem>::singleton;
        } else if (IsLocationObject(obj)) {
            typedef XrayWrapper<CrossCompartmentSecurityWrapper> Xray;
            usingXray = true;
            wrapper = &FilteringWrapper<Xray, LocationPolicy>::singleton;
        } else if (IsComponentsObject(obj)) {
            wrapper = &FilteringWrapper<CrossCompartmentSecurityWrapper,
                                        ComponentsObjectPolicy>::singleton;
        } else if (!targetdata || !targetdata->wantXrays ||
                   (type = GetXrayType(obj)) == NotXray) {
            wrapper = &CrossCompartmentWrapper::singleton;
        } else if (type == XrayForDOMObject) {
            wrapper = &XrayDOM::singleton;
        } else if (type == XrayForDOMProxyObject) {
            wrapper = &XrayProxy::singleton;
        } else {
            typedef XrayWrapper<CrossCompartmentWrapper> Xray;
            usingXray = true;
            wrapper = &Xray::singleton;
        }
    } else {
        NS_ASSERTION(!AccessCheck::needsSystemOnlyWrapper(obj),
                     "bad object exposed across origins");

        // Cross origin we want to disallow scripting and limit access to
        // a predefined set of properties. XrayWrapper adds a property
        // (.wrappedJSObject) which allows bypassing the XrayWrapper, but
        // we filter out access to that property.
        XrayType type = GetXrayType(obj);
        if (type == NotXray) {
            wrapper = &FilteringWrapper<CrossCompartmentSecurityWrapper,
                                        CrossOriginAccessiblePropertiesOnly>::singleton;
        } else if (type == XrayForDOMObject) {
            wrapper = &FilteringWrapper<XrayDOM,
                                        CrossOriginAccessiblePropertiesOnly>::singleton;
        } else if (type == XrayForDOMProxyObject) {
            wrapper = &FilteringWrapper<XrayProxy,
                                        CrossOriginAccessiblePropertiesOnly>::singleton;
        } else {
            typedef XrayWrapper<CrossCompartmentSecurityWrapper> Xray;
            usingXray = true;

            // Location objects can become same origin after navigation, so we might
            // have to grant transparent access later on.
            if (IsLocationObject(obj)) {
                wrapper = &FilteringWrapper<Xray, LocationPolicy>::singleton;
            } else {
                wrapper = &FilteringWrapper<Xray,
                    CrossOriginAccessiblePropertiesOnly>::singleton;
            }
        }
    }

    JSObject *wrapperObj = Wrapper::New(cx, obj, proxyProto, parent, wrapper);
    if (!wrapperObj || !usingXray)
        return wrapperObj;

    JSObject *xrayHolder = XrayUtils::createHolder(cx, obj, parent);
    if (!xrayHolder)
        return nullptr;
    js::SetProxyExtra(wrapperObj, 0, js::ObjectValue(*xrayHolder));
    return wrapperObj;
}

JSObject *
WrapperFactory::WrapForSameCompartment(JSContext *cx, JSObject *obj)
{
    // Only WNs have same-compartment wrappers.
    //
    // NB: The contract of WrapForSameCompartment says that |obj| may or may not
    // be a security wrapper. This check implicitly handles the security wrapper
    // case.
    if (!IS_WN_WRAPPER(obj))
        return obj;

    // Extract the WN. It should exist.
    XPCWrappedNative *wn = static_cast<XPCWrappedNative *>(xpc_GetJSPrivate(obj));
    MOZ_ASSERT(wn, "Trying to wrap a dead WN!");

    // The WN knows what to do.
    return wn->GetSameCompartmentSecurityWrapper(cx);
}

typedef FilteringWrapper<XrayWrapper<SameCompartmentSecurityWrapper>, LocationPolicy> LW;

bool
WrapperFactory::IsLocationObject(JSObject *obj)
{
    const char *name = js::GetObjectClass(obj)->name;
    return name[0] == 'L' && !strcmp(name, "Location");
}

JSObject *
WrapperFactory::WrapLocationObject(JSContext *cx, JSObject *obj)
{
    JSObject *xrayHolder = XrayUtils::createHolder(cx, obj, js::GetObjectParent(obj));
    if (!xrayHolder)
        return nullptr;
    JSObject *wrapperObj = Wrapper::New(cx, obj, js::GetObjectProto(obj), js::GetObjectParent(obj),
                                        &LW::singleton);
    if (!wrapperObj)
        return nullptr;
    js::SetProxyExtra(wrapperObj, 0, js::ObjectValue(*xrayHolder));
    return wrapperObj;
}

// Call WaiveXrayAndWrap when you have a JS object that you don't want to be
// wrapped in an Xray wrapper. cx->compartment is the compartment that will be
// using the returned object. If the object to be wrapped is already in the
// correct compartment, then this returns the unwrapped object.
bool
WrapperFactory::WaiveXrayAndWrap(JSContext *cx, jsval *vp)
{
    if (JSVAL_IS_PRIMITIVE(*vp))
        return JS_WrapValue(cx, vp);

    JSObject *obj = js::UnwrapObject(JSVAL_TO_OBJECT(*vp));
    obj = GetCurrentOuter(cx, obj);
    if (js::IsObjectInContextCompartment(obj, cx)) {
        *vp = OBJECT_TO_JSVAL(obj);
        return true;
    }

    obj = WaiveXray(cx, obj);
    if (!obj)
        return false;

    *vp = OBJECT_TO_JSVAL(obj);
    return JS_WrapValue(cx, vp);
}

JSObject *
WrapperFactory::WrapSOWObject(JSContext *cx, JSObject *obj)
{
    JSObject *wrapperObj =
        Wrapper::New(cx, obj, JS_GetPrototype(obj), JS_GetGlobalForObject(cx, obj),
                     &FilteringWrapper<SameCompartmentSecurityWrapper,
                     OnlyIfSubjectIsSystem>::singleton);
    return wrapperObj;
}

bool
WrapperFactory::IsComponentsObject(JSObject *obj)
{
    const char *name = js::GetObjectClass(obj)->name;
    return name[0] == 'n' && !strcmp(name, "nsXPCComponents");
}

JSObject *
WrapperFactory::WrapComponentsObject(JSContext *cx, JSObject *obj)
{
    JSObject *wrapperObj =
        Wrapper::New(cx, obj, JS_GetPrototype(obj), JS_GetGlobalForObject(cx, obj),
                     &FilteringWrapper<SameCompartmentSecurityWrapper, ComponentsObjectPolicy>::singleton);

    return wrapperObj;
}

JSObject *
WrapperFactory::WrapForSameCompartmentXray(JSContext *cx, JSObject *obj)
{
    // We should be same-compartment here.
    MOZ_ASSERT(js::IsObjectInContextCompartment(obj, cx));

    // Sort out what kind of Xray we can do. If we can't Xray, bail.
    XrayType type = GetXrayType(obj);
    if (type == NotXray)
        return NULL;

    // Select the appropriate proxy handler.
    Wrapper *wrapper = NULL;
    if (type == XrayForWrappedNative)
        wrapper = &XrayWrapper<DirectWrapper>::singleton;
    else if (type == XrayForDOMProxyObject)
        wrapper = &XrayWrapper<DirectWrapper, ProxyXrayTraits>::singleton;
    else if (type == XrayForDOMObject)
        wrapper = &XrayWrapper<DirectWrapper, DOMXrayTraits>::singleton;
    else
        MOZ_NOT_REACHED("Bad Xray type");

    // Make the Xray.
    JSObject *parent = JS_GetGlobalForObject(cx, obj);
    JSObject *wrapperObj = Wrapper::New(cx, obj, NULL, parent, wrapper);
    if (!wrapperObj)
        return NULL;

    // Make the holder. Note that this is currently for WNs only until we fix
    // bug 761704.
    if (type == XrayForWrappedNative) {
        JSObject *xrayHolder = XrayUtils::createHolder(cx, obj, parent);
        if (!xrayHolder)
            return nullptr;
        js::SetProxyExtra(wrapperObj, 0, js::ObjectValue(*xrayHolder));
    }
    return wrapperObj;
}


bool
WrapperFactory::XrayWrapperNotShadowing(JSObject *wrapper, jsid id)
{
    ResolvingId *rid = ResolvingId::getResolvingIdFromWrapper(wrapper);
    return rid->isXrayShadowing(id);
}

/*
 * Calls to JS_TransplantObject* should go through these helpers here so that
 * waivers get fixed up properly.
 */

static bool
FixWaiverAfterTransplant(JSContext *cx, JSObject *oldWaiver, JSObject *newobj)
{
    MOZ_ASSERT(Wrapper::wrapperHandler(oldWaiver) == &XrayWaiver);
    MOZ_ASSERT(!js::IsCrossCompartmentWrapper(newobj));

    // Create a waiver in the new compartment. We know there's not one already
    // because we _just_ transplanted, which means that |newobj| was either
    // created from scratch, or was previously cross-compartment wrapper (which
    // should have no waiver). CreateXrayWaiver asserts this.
    JSObject *newWaiver = WrapperFactory::CreateXrayWaiver(cx, newobj);
    if (!newWaiver)
        return false;

    // Update all the cross-compartment references to oldWaiver to point to
    // newWaiver.
    if (!js::RemapAllWrappersForObject(cx, oldWaiver, newWaiver))
        return false;

    // There should be no same-compartment references to oldWaiver, and we
    // just remapped all cross-compartment references. It's dead, so we can
    // remove it from the map.
    CompartmentPrivate *priv = GetCompartmentPrivate(oldWaiver);
    JSObject *key = Wrapper::wrappedObject(oldWaiver);
    MOZ_ASSERT(priv->waiverWrapperMap->Find(key));
    priv->waiverWrapperMap->Remove(key);
    return true;
}

JSObject *
TransplantObject(JSContext *cx, JSObject *origobj, JSObject *target)
{
    JSObject *oldWaiver = WrapperFactory::GetXrayWaiver(origobj);
    JSObject *newIdentity = JS_TransplantObject(cx, origobj, target);
    if (!newIdentity || !oldWaiver)
       return newIdentity;

    if (!FixWaiverAfterTransplant(cx, oldWaiver, newIdentity))
        return NULL;
    return newIdentity;
}

JSObject *
TransplantObjectWithWrapper(JSContext *cx,
                            JSObject *origobj, JSObject *origwrapper,
                            JSObject *targetobj, JSObject *targetwrapper)
{
    JSObject *oldWaiver = WrapperFactory::GetXrayWaiver(origobj);
    JSObject *newSameCompartmentWrapper =
      js_TransplantObjectWithWrapper(cx, origobj, origwrapper, targetobj,
                                     targetwrapper);
    if (!newSameCompartmentWrapper || !oldWaiver)
        return newSameCompartmentWrapper;

    JSObject *newIdentity = Wrapper::wrappedObject(newSameCompartmentWrapper);
    JS_ASSERT(js::IsWrapper(newIdentity));
    if (!FixWaiverAfterTransplant(cx, oldWaiver, newIdentity))
        return NULL;
    return newSameCompartmentWrapper;
}

}
