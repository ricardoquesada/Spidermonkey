/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Sharable code and data for wrapper around JSObjects. */

#include "xpcprivate.h"
#include "nsArrayEnumerator.h"
#include "nsWrapperCache.h"
#include "XPCWrapper.h"
#include "AccessCheck.h"
#include "nsJSUtils.h"
#include "mozilla/Attributes.h"

#include "jsapi.h"

NS_IMPL_THREADSAFE_ISUPPORTS1(nsXPCWrappedJSClass, nsIXPCWrappedJSClass)

// the value of this variable is never used - we use its address as a sentinel
static uint32_t zero_methods_descriptor;

bool AutoScriptEvaluate::StartEvaluating(JSObject *scope, JSErrorReporter errorReporter)
{
    NS_PRECONDITION(!mEvaluated, "AutoScriptEvaluate::Evaluate should only be called once");

    if (!mJSContext)
        return true;

    mEvaluated = true;
    if (!JS_GetErrorReporter(mJSContext)) {
        JS_SetErrorReporter(mJSContext, errorReporter);
        mErrorReporterSet = true;
    }

    JS_BeginRequest(mJSContext);
    mAutoCompartment.construct(mJSContext, scope);

    // Saving the exception state keeps us from interfering with another script
    // that may also be running on this context.  This occurred first with the
    // js debugger, as described in
    // http://bugzilla.mozilla.org/show_bug.cgi?id=88130 but presumably could
    // show up in any situation where a script calls into a wrapped js component
    // on the same context, while the context has a nonzero exception state.
    // Because JS_SaveExceptionState/JS_RestoreExceptionState use malloc
    // and addroot, we avoid them if possible by returning null (as opposed to
    // a JSExceptionState with no information) when there is no pending
    // exception.
    if (JS_IsExceptionPending(mJSContext)) {
        mState = JS_SaveExceptionState(mJSContext);
        JS_ClearPendingException(mJSContext);
    }

    return true;
}

AutoScriptEvaluate::~AutoScriptEvaluate()
{
    if (!mJSContext || !mEvaluated)
        return;
    if (mState)
        JS_RestoreExceptionState(mJSContext, mState);
    else
        JS_ClearPendingException(mJSContext);

    JS_EndRequest(mJSContext);

    // If this is a JSContext that has a private context that provides a
    // nsIXPCScriptNotify interface, then notify the object the script has
    // been executed.
    //
    // Note: We rely on the rule that if any JSContext in our JSRuntime has
    // private data that points to an nsISupports subclass, it has also set
    // the JSOPTION_PRIVATE_IS_NSISUPPORTS option.

    if (JS_GetOptions(mJSContext) & JSOPTION_PRIVATE_IS_NSISUPPORTS) {
        nsCOMPtr<nsIXPCScriptNotify> scriptNotify =
            do_QueryInterface(static_cast<nsISupports*>
                                         (JS_GetContextPrivate(mJSContext)));
        if (scriptNotify)
            scriptNotify->ScriptExecuted();
    }

    if (mErrorReporterSet)
        JS_SetErrorReporter(mJSContext, NULL);
}

// It turns out that some errors may be not worth reporting. So, this
// function is factored out to manage that.
JSBool xpc_IsReportableErrorCode(nsresult code)
{
    if (NS_SUCCEEDED(code))
        return false;

    switch (code) {
        // Error codes that we don't want to report as errors...
        // These generally indicate bad interface design AFAIC.
        case NS_ERROR_FACTORY_REGISTER_AGAIN:
        case NS_BASE_STREAM_WOULD_BLOCK:
            return false;
        default:
            return true;
    }
}

// static
nsresult
nsXPCWrappedJSClass::GetNewOrUsed(XPCCallContext& ccx, REFNSIID aIID,
                                  nsXPCWrappedJSClass** resultClazz)
{
    nsXPCWrappedJSClass* clazz = nullptr;
    XPCJSRuntime* rt = ccx.GetRuntime();

    {   // scoped lock
        XPCAutoLock lock(rt->GetMapLock());
        IID2WrappedJSClassMap* map = rt->GetWrappedJSClassMap();
        clazz = map->Find(aIID);
        NS_IF_ADDREF(clazz);
    }

    if (!clazz) {
        nsCOMPtr<nsIInterfaceInfo> info;
        ccx.GetXPConnect()->GetInfoForIID(&aIID, getter_AddRefs(info));
        if (info) {
            bool canScript, isBuiltin;
            if (NS_SUCCEEDED(info->IsScriptable(&canScript)) && canScript &&
                NS_SUCCEEDED(info->IsBuiltinClass(&isBuiltin)) && !isBuiltin &&
                nsXPConnect::IsISupportsDescendant(info)) {
                clazz = new nsXPCWrappedJSClass(ccx, aIID, info);
                if (clazz && !clazz->mDescriptors)
                    NS_RELEASE(clazz);  // sets clazz to nullptr
            }
        }
    }
    *resultClazz = clazz;
    return NS_OK;
}

nsXPCWrappedJSClass::nsXPCWrappedJSClass(XPCCallContext& ccx, REFNSIID aIID,
                                         nsIInterfaceInfo* aInfo)
    : mRuntime(ccx.GetRuntime()),
      mInfo(aInfo),
      mName(nullptr),
      mIID(aIID),
      mDescriptors(nullptr)
{
    NS_ADDREF(mInfo);
    NS_ADDREF_THIS();

    {   // scoped lock
        XPCAutoLock lock(mRuntime->GetMapLock());
        mRuntime->GetWrappedJSClassMap()->Add(this);
    }

    uint16_t methodCount;
    if (NS_SUCCEEDED(mInfo->GetMethodCount(&methodCount))) {
        if (methodCount) {
            int wordCount = (methodCount/32)+1;
            if (nullptr != (mDescriptors = new uint32_t[wordCount])) {
                int i;
                // init flags to 0;
                for (i = wordCount-1; i >= 0; i--)
                    mDescriptors[i] = 0;

                for (i = 0; i < methodCount; i++) {
                    const nsXPTMethodInfo* info;
                    if (NS_SUCCEEDED(mInfo->GetMethodInfo(i, &info)))
                        SetReflectable(i, XPCConvert::IsMethodReflectable(*info));
                    else {
                        delete [] mDescriptors;
                        mDescriptors = nullptr;
                        break;
                    }
                }
            }
        } else {
            mDescriptors = &zero_methods_descriptor;
        }
    }
}

nsXPCWrappedJSClass::~nsXPCWrappedJSClass()
{
    if (mDescriptors && mDescriptors != &zero_methods_descriptor)
        delete [] mDescriptors;
    if (mRuntime)
    {   // scoped lock
        XPCAutoLock lock(mRuntime->GetMapLock());
        mRuntime->GetWrappedJSClassMap()->Remove(this);
    }
    if (mName)
        nsMemory::Free(mName);
    NS_IF_RELEASE(mInfo);
}

JSObject*
nsXPCWrappedJSClass::CallQueryInterfaceOnJSObject(XPCCallContext& ccx,
                                                  JSObject* jsobj,
                                                  REFNSIID aIID)
{
    JSContext* cx = ccx.GetJSContext();
    JSObject* id;
    jsval retval;
    JSObject* retObj;
    JSBool success = false;
    jsid funid;
    jsval fun;

    // Don't call the actual function on a content object. We'll determine
    // whether or not a content object is capable of implementing the
    // interface (i.e. whether the interface is scriptable) and most content
    // objects don't have QI implementations anyway. Also see bug 503926.
    if (!xpc::AccessCheck::isChrome(js::GetObjectCompartment(jsobj))) {
        return nullptr;
    }

    // OK, it looks like we'll be calling into JS code.
    AutoScriptEvaluate scriptEval(cx);

    // XXX we should install an error reporter that will send reports to
    // the JS error console service.
    if (!scriptEval.StartEvaluating(jsobj))
        return nullptr;

    // check upfront for the existence of the function property
    funid = mRuntime->GetStringID(XPCJSRuntime::IDX_QUERY_INTERFACE);
    if (!JS_GetPropertyById(cx, jsobj, funid, &fun) || JSVAL_IS_PRIMITIVE(fun))
        return nullptr;

    // protect fun so that we're sure it's alive when we call it
    AUTO_MARK_JSVAL(ccx, fun);

    // Ensure that we are asking for a scriptable interface.
    // NB:  It's important for security that this check is here rather
    // than later, since it prevents untrusted objects from implementing
    // some interfaces in JS and aggregating a trusted object to
    // implement intentionally (for security) unscriptable interfaces.
    // We so often ask for nsISupports that we can short-circuit the test...
    if (!aIID.Equals(NS_GET_IID(nsISupports))) {
        nsCOMPtr<nsIInterfaceInfo> info;
        ccx.GetXPConnect()->GetInfoForIID(&aIID, getter_AddRefs(info));
        if (!info)
            return nullptr;
        bool canScript, isBuiltin;
        if (NS_FAILED(info->IsScriptable(&canScript)) || !canScript ||
            NS_FAILED(info->IsBuiltinClass(&isBuiltin)) || isBuiltin)
            return nullptr;
    }

    id = xpc_NewIDObject(cx, jsobj, aIID);
    if (id) {
        // Throwing NS_NOINTERFACE is the prescribed way to fail QI from JS. It
        // is not an exception that is ever worth reporting, but we don't want
        // to eat all exceptions either.

        uint32_t oldOpts =
          JS_SetOptions(cx, JS_GetOptions(cx) | JSOPTION_DONT_REPORT_UNCAUGHT);

        jsval args[1] = {OBJECT_TO_JSVAL(id)};
        success = JS_CallFunctionValue(cx, jsobj, fun, 1, args, &retval);

        JS_SetOptions(cx, oldOpts);

        if (!success) {
            NS_ASSERTION(JS_IsExceptionPending(cx),
                         "JS failed without setting an exception!");

            jsval jsexception = JSVAL_NULL;
            AUTO_MARK_JSVAL(ccx, &jsexception);

            if (JS_GetPendingException(cx, &jsexception)) {
                nsresult rv;
                if (jsexception.isObject()) {
                    // XPConnect may have constructed an object to represent a
                    // C++ QI failure. See if that is the case.
                    nsCOMPtr<nsIXPConnectWrappedNative> wrapper;

                    nsXPConnect::GetXPConnect()->
                        GetWrappedNativeOfJSObject(ccx,
                                                   &jsexception.toObject(),
                                                   getter_AddRefs(wrapper));

                    if (wrapper) {
                        nsCOMPtr<nsIException> exception =
                            do_QueryWrappedNative(wrapper);
                        if (exception &&
                            NS_SUCCEEDED(exception->GetResult(&rv)) &&
                            rv == NS_NOINTERFACE) {
                            JS_ClearPendingException(cx);
                        }
                    }
                } else if (JSVAL_IS_NUMBER(jsexception)) {
                    // JS often throws an nsresult.
                    if (JSVAL_IS_DOUBLE(jsexception))
                        rv = (nsresult)(JSVAL_TO_DOUBLE(jsexception));
                    else
                        rv = (nsresult)(JSVAL_TO_INT(jsexception));

                    if (rv == NS_NOINTERFACE)
                        JS_ClearPendingException(cx);
                }
            }

            // Don't report if reporting was disabled by someone else.
            if (!(oldOpts & JSOPTION_DONT_REPORT_UNCAUGHT))
                JS_ReportPendingException(cx);
        }
    }

    if (success)
        success = JS_ValueToObject(cx, retval, &retObj);

    return success ? retObj : nullptr;
}

/***************************************************************************/

static JSBool
GetNamedPropertyAsVariantRaw(XPCCallContext& ccx,
                             JSObject* aJSObj,
                             jsid aName,
                             nsIVariant** aResult,
                             nsresult* pErr)
{
    nsXPTType type = nsXPTType((uint8_t)TD_INTERFACE_TYPE);
    jsval val;

    return JS_GetPropertyById(ccx, aJSObj, aName, &val) &&
           // Note that this always takes the T_INTERFACE path through
           // JSData2Native, so the value passed for useAllocator
           // doesn't really matter. We pass true for consistency.
           XPCConvert::JSData2Native(ccx, aResult, val, type, true,
                                     &NS_GET_IID(nsIVariant), pErr);
}

// static
nsresult
nsXPCWrappedJSClass::GetNamedPropertyAsVariant(XPCCallContext& ccx,
                                               JSObject* aJSObj,
                                               const nsAString& aName,
                                               nsIVariant** aResult)
{
    JSContext* cx = ccx.GetJSContext();
    JSBool ok;
    jsid id;
    nsresult rv = NS_ERROR_FAILURE;

    AutoScriptEvaluate scriptEval(cx);
    if (!scriptEval.StartEvaluating(aJSObj))
        return NS_ERROR_FAILURE;

    // Wrap the string in a jsval after the AutoScriptEvaluate, so that the
    // resulting value ends up in the correct compartment.
    nsStringBuffer* buf;
    jsval jsstr = XPCStringConvert::ReadableToJSVal(ccx, aName, &buf);
    if (JSVAL_IS_NULL(jsstr))
        return NS_ERROR_OUT_OF_MEMORY;
    if (buf)
        buf->AddRef();

    ok = JS_ValueToId(cx, jsstr, &id) &&
         GetNamedPropertyAsVariantRaw(ccx, aJSObj, id, aResult, &rv);

    return ok ? NS_OK : NS_FAILED(rv) ? rv : NS_ERROR_FAILURE;
}

/***************************************************************************/

// static
nsresult
nsXPCWrappedJSClass::BuildPropertyEnumerator(XPCCallContext& ccx,
                                             JSObject* aJSObj,
                                             nsISimpleEnumerator** aEnumerate)
{
    JSContext* cx = ccx.GetJSContext();

    AutoScriptEvaluate scriptEval(cx);
    if (!scriptEval.StartEvaluating(aJSObj))
        return NS_ERROR_FAILURE;

    JS::AutoIdArray idArray(cx, JS_Enumerate(cx, aJSObj));
    if (!idArray)
        return NS_ERROR_FAILURE;

    nsCOMArray<nsIProperty> propertyArray(idArray.length());
    for (size_t i = 0; i < idArray.length(); i++) {
        jsid idName = idArray[i];

        nsCOMPtr<nsIVariant> value;
        nsresult rv;
        if (!GetNamedPropertyAsVariantRaw(ccx, aJSObj, idName,
                                          getter_AddRefs(value), &rv)) {
            if (NS_FAILED(rv))
                return rv;
            return NS_ERROR_FAILURE;
        }

        jsval jsvalName;
        if (!JS_IdToValue(cx, idName, &jsvalName))
            return NS_ERROR_FAILURE;

        JSString* name = JS_ValueToString(cx, jsvalName);
        if (!name)
            return NS_ERROR_FAILURE;

        size_t length;
        const jschar *chars = JS_GetStringCharsAndLength(cx, name, &length);
        if (!chars)
            return NS_ERROR_FAILURE;

        nsCOMPtr<nsIProperty> property =
            new xpcProperty(chars, (uint32_t) length, value);

        if (!propertyArray.AppendObject(property))
            return NS_ERROR_FAILURE;
    }

    return NS_NewArrayEnumerator(aEnumerate, propertyArray);
}

/***************************************************************************/

NS_IMPL_ISUPPORTS1(xpcProperty, nsIProperty)

xpcProperty::xpcProperty(const PRUnichar* aName, uint32_t aNameLen,
                         nsIVariant* aValue)
    : mName(aName, aNameLen), mValue(aValue)
{
}

/* readonly attribute AString name; */
NS_IMETHODIMP xpcProperty::GetName(nsAString & aName)
{
    aName.Assign(mName);
    return NS_OK;
}

/* readonly attribute nsIVariant value; */
NS_IMETHODIMP xpcProperty::GetValue(nsIVariant * *aValue)
{
    NS_ADDREF(*aValue = mValue);
    return NS_OK;
}

/***************************************************************************/
// This 'WrappedJSIdentity' class and singleton allow us to figure out if
// any given nsISupports* is implemented by a WrappedJS object. This is done
// using a QueryInterface call on the interface pointer with our ID. If
// that call returns NS_OK and the pointer is to our singleton, then the
// interface must be implemented by a WrappedJS object. NOTE: the
// 'WrappedJSIdentity' object is not a real XPCOM object and should not be
// used for anything else (hence it is declared in this implementation file).

// {5C5C3BB0-A9BA-11d2-BA64-00805F8A5DD7}
#define NS_IXPCONNECT_WRAPPED_JS_IDENTITY_CLASS_IID                           \
{ 0x5c5c3bb0, 0xa9ba, 0x11d2,                                                 \
  { 0xba, 0x64, 0x0, 0x80, 0x5f, 0x8a, 0x5d, 0xd7 } }

class WrappedJSIdentity
{
    // no instance methods...
public:
    NS_DECLARE_STATIC_IID_ACCESSOR(NS_IXPCONNECT_WRAPPED_JS_IDENTITY_CLASS_IID)

    static void* GetSingleton()
    {
        static WrappedJSIdentity* singleton = nullptr;
        if (!singleton)
            singleton = new WrappedJSIdentity();
        return (void*) singleton;
    }
};

NS_DEFINE_STATIC_IID_ACCESSOR(WrappedJSIdentity,
                              NS_IXPCONNECT_WRAPPED_JS_IDENTITY_CLASS_IID)

/***************************************************************************/

// static
JSBool
nsXPCWrappedJSClass::IsWrappedJS(nsISupports* aPtr)
{
    void* result;
    NS_PRECONDITION(aPtr, "null pointer");
    return aPtr &&
           NS_OK == aPtr->QueryInterface(NS_GET_IID(WrappedJSIdentity), &result) &&
           result == WrappedJSIdentity::GetSingleton();
}

static JSContext *
GetContextFromObject(JSObject *obj)
{
    // Don't stomp over a running context.
    XPCJSContextStack* stack = XPCJSRuntime::Get()->GetJSContextStack();

    if (stack && stack->Peek())
        return nullptr;

    // In order to get a context, we need a context.
    XPCCallContext ccx(NATIVE_CALLER);
    if (!ccx.IsValid())
        return nullptr;

    JSAutoCompartment ac(ccx, obj);
    XPCWrappedNativeScope* scope =
        XPCWrappedNativeScope::FindInJSObjectScope(ccx, obj);
    XPCContext *xpcc = scope->GetContext();

    if (xpcc) {
        JSContext *cx = xpcc->GetJSContext();
        JS_AbortIfWrongThread(JS_GetRuntime(cx));
        return cx;
    }

    return nullptr;
}

class SameOriginCheckedComponent MOZ_FINAL : public nsISecurityCheckedComponent
{
public:
    SameOriginCheckedComponent(nsXPCWrappedJS* delegate)
        : mDelegate(delegate)
    {}

    NS_DECL_ISUPPORTS
    NS_DECL_NSISECURITYCHECKEDCOMPONENT

private:
    nsRefPtr<nsXPCWrappedJS> mDelegate;
};

NS_IMPL_ADDREF(SameOriginCheckedComponent)
NS_IMPL_RELEASE(SameOriginCheckedComponent)

NS_INTERFACE_MAP_BEGIN(SameOriginCheckedComponent)
    NS_INTERFACE_MAP_ENTRY(nsISecurityCheckedComponent)
NS_INTERFACE_MAP_END_AGGREGATED(mDelegate)

NS_IMETHODIMP
SameOriginCheckedComponent::CanCreateWrapper(const nsIID * iid,
                                             char **_retval)
{
    // XXX This doesn't actually work because nsScriptSecurityManager doesn't
    // know what to do with "sameOrigin" for canCreateWrapper.
    *_retval = NS_strdup("sameOrigin");
    return *_retval ? NS_OK : NS_ERROR_OUT_OF_MEMORY;
}

NS_IMETHODIMP
SameOriginCheckedComponent::CanCallMethod(const nsIID * iid,
                                          const PRUnichar *methodName,
                                          char **_retval)
{
    *_retval = NS_strdup("sameOrigin");
    return *_retval ? NS_OK : NS_ERROR_OUT_OF_MEMORY;
}

NS_IMETHODIMP
SameOriginCheckedComponent::CanGetProperty(const nsIID * iid,
                                           const PRUnichar *propertyName,
                                           char **_retval)
{
    *_retval = NS_strdup("sameOrigin");
    return *_retval ? NS_OK : NS_ERROR_OUT_OF_MEMORY;
}

NS_IMETHODIMP
SameOriginCheckedComponent::CanSetProperty(const nsIID * iid,
                                           const PRUnichar *propertyName,
                                           char **_retval)
{
    *_retval = NS_strdup("sameOrigin");
    return *_retval ? NS_OK : NS_ERROR_OUT_OF_MEMORY;
}

NS_IMETHODIMP
nsXPCWrappedJSClass::DelegatedQueryInterface(nsXPCWrappedJS* self,
                                             REFNSIID aIID,
                                             void** aInstancePtr)
{
    if (aIID.Equals(NS_GET_IID(nsIXPConnectJSObjectHolder))) {
        NS_ADDREF(self);
        *aInstancePtr = (void*) static_cast<nsIXPConnectJSObjectHolder*>(self);
        return NS_OK;
    }

    // Objects internal to xpconnect are the only objects that even know *how*
    // to ask for this iid. And none of them bother refcounting the thing.
    if (aIID.Equals(NS_GET_IID(WrappedJSIdentity))) {
        // asking to find out if this is a wrapper object
        *aInstancePtr = WrappedJSIdentity::GetSingleton();
        return NS_OK;
    }

    if (aIID.Equals(NS_GET_IID(nsIPropertyBag))) {
        // We only want to expose one implementation from our aggregate.
        nsXPCWrappedJS* root = self->GetRootWrapper();

        if (!root->IsValid()) {
            *aInstancePtr = nullptr;
            return NS_NOINTERFACE;
        }

        NS_ADDREF(root);
        *aInstancePtr = (void*) static_cast<nsIPropertyBag*>(root);
        return NS_OK;
    }

    // We can't have a cached wrapper.
    if (aIID.Equals(NS_GET_IID(nsWrapperCache))) {
        *aInstancePtr = nullptr;
        return NS_NOINTERFACE;
    }

    JSContext *context = GetContextFromObject(self->GetJSObject());
    XPCCallContext ccx(NATIVE_CALLER, context);
    if (!ccx.IsValid()) {
        *aInstancePtr = nullptr;
        return NS_NOINTERFACE;
    }

    // We support nsISupportsWeakReference iff the root wrapped JSObject
    // claims to support it in its QueryInterface implementation.
    if (aIID.Equals(NS_GET_IID(nsISupportsWeakReference))) {
        // We only want to expose one implementation from our aggregate.
        nsXPCWrappedJS* root = self->GetRootWrapper();

        // Fail if JSObject doesn't claim support for nsISupportsWeakReference
        if (!root->IsValid() ||
            !CallQueryInterfaceOnJSObject(ccx, root->GetJSObject(), aIID)) {
            *aInstancePtr = nullptr;
            return NS_NOINTERFACE;
        }

        NS_ADDREF(root);
        *aInstancePtr = (void*) static_cast<nsISupportsWeakReference*>(root);
        return NS_OK;
    }

    nsXPCWrappedJS* sibling;

    // Checks for any existing wrapper explicitly constructed for this iid.
    // This includes the current 'self' wrapper. This also deals with the
    // nsISupports case (for which it returns mRoot).
    if (nullptr != (sibling = self->Find(aIID))) {
        NS_ADDREF(sibling);
        *aInstancePtr = sibling->GetXPTCStub();
        return NS_OK;
    }

    // Check if asking for an interface from which one of our wrappers inherits.
    if (nullptr != (sibling = self->FindInherited(aIID))) {
        NS_ADDREF(sibling);
        *aInstancePtr = sibling->GetXPTCStub();
        return NS_OK;
    }

    // else we do the more expensive stuff...

    // Before calling out, ensure that we're not about to claim to implement
    // nsISecurityCheckedComponent for an untrusted object. Doing so causes
    // problems. See bug 352882.
    // But if this is a content object, then we might be wrapping it for
    // content. If our JS object isn't a double-wrapped object (that is, we
    // don't have XPCWrappedJS(XPCWrappedNative(some C++ object))), then it
    // definitely will not have classinfo (and therefore won't be a DOM
    // object). Since content wants to be able to use these objects (directly
    // or indirectly, see bug 483672), we implement nsISecurityCheckedComponent
    // for them and tell caps that they are also bound by the same origin
    // model.

    if (aIID.Equals(NS_GET_IID(nsISecurityCheckedComponent))) {
        // XXX This code checks to see if the given object has chrome (also
        // known as system) principals. It really wants to do a
        // UniversalXPConnect type check.

        *aInstancePtr = nullptr;

        nsXPConnect *xpc = nsXPConnect::GetXPConnect();
        nsCOMPtr<nsIScriptSecurityManager> secMan =
            do_QueryInterface(xpc->GetDefaultSecurityManager());
        if (!secMan)
            return NS_NOINTERFACE;

        JSObject *selfObj = self->GetJSObject();
        nsCOMPtr<nsIPrincipal> objPrin;
        nsresult rv = secMan->GetObjectPrincipal(ccx, selfObj,
                                                 getter_AddRefs(objPrin));
        if (NS_FAILED(rv))
            return rv;

        bool isSystem;
        rv = secMan->IsSystemPrincipal(objPrin, &isSystem);
        if ((NS_FAILED(rv) || !isSystem) &&
            !IS_WRAPPER_CLASS(js::GetObjectClass(selfObj))) {
            // A content object.
            nsRefPtr<SameOriginCheckedComponent> checked =
                new SameOriginCheckedComponent(self);
            if (!checked)
                return NS_ERROR_OUT_OF_MEMORY;
            *aInstancePtr = checked.forget().get();
            return NS_OK;
        }
    }

    // check if the JSObject claims to implement this interface
    JSObject* jsobj = CallQueryInterfaceOnJSObject(ccx, self->GetJSObject(),
                                                   aIID);
    if (jsobj) {
        // protect jsobj until it is actually attached
        AUTO_MARK_JSVAL(ccx, OBJECT_TO_JSVAL(jsobj));

        // We can't use XPConvert::JSObject2NativeInterface() here
        // since that can find a XPCWrappedNative directly on the
        // proto chain, and we don't want that here. We need to find
        // the actual JS object that claimed it supports the interface
        // we're looking for or we'll potentially bypass security
        // checks etc by calling directly through to a native found on
        // the prototype chain.
        //
        // Instead, simply do the nsXPCWrappedJS part of
        // XPConvert::JSObject2NativeInterface() here to make sure we
        // get a new (or used) nsXPCWrappedJS.
        nsXPCWrappedJS* wrapper;
        nsresult rv = nsXPCWrappedJS::GetNewOrUsed(ccx, jsobj, aIID, nullptr,
                                                   &wrapper);
        if (NS_SUCCEEDED(rv) && wrapper) {
            // We need to go through the QueryInterface logic to make
            // this return the right thing for the various 'special'
            // interfaces; e.g.  nsIPropertyBag.
            rv = wrapper->QueryInterface(aIID, aInstancePtr);
            NS_RELEASE(wrapper);
            return rv;
        }
    }

    // else...
    // no can do
    *aInstancePtr = nullptr;
    return NS_NOINTERFACE;
}

JSObject*
nsXPCWrappedJSClass::GetRootJSObject(XPCCallContext& ccx, JSObject* aJSObj)
{
    JSObject* result = CallQueryInterfaceOnJSObject(ccx, aJSObj,
                                                    NS_GET_IID(nsISupports));
    if (!result)
        return aJSObj;
    JSObject* inner = XPCWrapper::Unwrap(ccx, result);
    if (inner)
        return inner;
    return result;
}

void
xpcWrappedJSErrorReporter(JSContext *cx, const char *message,
                          JSErrorReport *report)
{
    if (report) {
        // If it is an exception report, then we can just deal with the
        // exception later (if not caught in the JS code).
        if (JSREPORT_IS_EXCEPTION(report->flags)) {
            // XXX We have a problem with error reports from uncaught exceptions.
            //
            // http://bugzilla.mozilla.org/show_bug.cgi?id=66453
            //
            // The issue is...
            //
            // We can't assume that the exception will *stay* uncaught. So, if
            // we build an nsIXPCException here and the underlying exception
            // really is caught before our script is done running then we blow
            // it by returning failure to our caller when the script didn't
            // really fail. However, This report contains error location info
            // that is no longer available after the script is done. So, if the
            // exception really is not caught (and is a non-engine exception)
            // then we've lost the oportunity to capture the script location
            // info that we *could* have captured here.
            //
            // This is expecially an issue with nested evaluations.
            //
            // Perhaps we could capture an expception here and store it as
            // 'provisional' and then later if there is a pending exception
            // when the script is done then we could maybe compare that in some
            // way with the 'provisional' one in which we captured location info.
            // We would not want to assume that the one discovered here is the
            // same one that is later detected. This could cause us to lie.
            //
            // The thing is. we do not currently store the right stuff to compare
            // these two nsIXPCExceptions (triggered by the same exception jsval
            // in the engine). Maybe we should store the jsval and compare that?
            // Maybe without even rooting it since we will not dereference it.
            // This is inexact, but maybe the right thing to do?
            //
            // if (report->errorNumber == JSMSG_UNCAUGHT_EXCEPTION)) ...
            //

            return;
        }

        if (JSREPORT_IS_WARNING(report->flags)) {
            // XXX printf the warning (#ifdef DEBUG only!).
            // XXX send the warning to the console service.
            return;
        }
    }

    XPCCallContext ccx(NATIVE_CALLER, cx);
    if (!ccx.IsValid())
        return;

    nsCOMPtr<nsIException> e;
    XPCConvert::JSErrorToXPCException(ccx, message, nullptr, nullptr, report,
                                      getter_AddRefs(e));
    if (e)
        ccx.GetXPCContext()->SetException(e);
}

JSBool
nsXPCWrappedJSClass::GetArraySizeFromParam(JSContext* cx,
                                           const XPTMethodDescriptor* method,
                                           const nsXPTParamInfo& param,
                                           uint16_t methodIndex,
                                           uint8_t paramIndex,
                                           nsXPTCMiniVariant* nativeParams,
                                           uint32_t* result)
{
    uint8_t argnum;
    nsresult rv;

    rv = mInfo->GetSizeIsArgNumberForParam(methodIndex, &param, 0, &argnum);
    if (NS_FAILED(rv))
        return false;

    const nsXPTParamInfo& arg_param = method->params[argnum];
    const nsXPTType& arg_type = arg_param.GetType();

    // This should be enforced by the xpidl compiler, but it's not.
    // See bug 695235.
    NS_ABORT_IF_FALSE(arg_type.TagPart() == nsXPTType::T_U32,
                      "size_is references parameter of invalid type.");

    if (arg_param.IsIndirect())
        *result = *(uint32_t*)nativeParams[argnum].val.p;
    else
        *result = nativeParams[argnum].val.u32;

    return true;
}

JSBool
nsXPCWrappedJSClass::GetInterfaceTypeFromParam(JSContext* cx,
                                               const XPTMethodDescriptor* method,
                                               const nsXPTParamInfo& param,
                                               uint16_t methodIndex,
                                               const nsXPTType& type,
                                               nsXPTCMiniVariant* nativeParams,
                                               nsID* result)
{
    uint8_t type_tag = type.TagPart();

    if (type_tag == nsXPTType::T_INTERFACE) {
        if (NS_SUCCEEDED(GetInterfaceInfo()->
                         GetIIDForParamNoAlloc(methodIndex, &param, result))) {
            return true;
        }
    } else if (type_tag == nsXPTType::T_INTERFACE_IS) {
        uint8_t argnum;
        nsresult rv;
        rv = mInfo->GetInterfaceIsArgNumberForParam(methodIndex,
                                                    &param, &argnum);
        if (NS_FAILED(rv))
            return false;

        const nsXPTParamInfo& arg_param = method->params[argnum];
        const nsXPTType& arg_type = arg_param.GetType();

        if (arg_type.TagPart() == nsXPTType::T_IID) {
            if (arg_param.IsIndirect()) {
                nsID** p = (nsID**) nativeParams[argnum].val.p;
                if (!p || !*p)
                    return false;
                *result = **p;
            } else {
                nsID* p = (nsID*) nativeParams[argnum].val.p;
                if (!p)
                    return false;
                *result = *p;
            }
            return true;
        }
    }
    return false;
}

void
nsXPCWrappedJSClass::CleanupPointerArray(const nsXPTType& datum_type,
                                         uint32_t array_count,
                                         void** arrayp)
{
    if (datum_type.IsInterfacePointer()) {
        nsISupports** pp = (nsISupports**) arrayp;
        for (uint32_t k = 0; k < array_count; k++) {
            nsISupports* p = pp[k];
            NS_IF_RELEASE(p);
        }
    } else {
        void** pp = (void**) arrayp;
        for (uint32_t k = 0; k < array_count; k++) {
            void* p = pp[k];
            if (p) nsMemory::Free(p);
        }
    }
}

void
nsXPCWrappedJSClass::CleanupPointerTypeObject(const nsXPTType& type,
                                              void** pp)
{
    NS_ASSERTION(pp,"null pointer");
    if (type.IsInterfacePointer()) {
        nsISupports* p = *((nsISupports**)pp);
        if (p) p->Release();
    } else {
        void* p = *((void**)pp);
        if (p) nsMemory::Free(p);
    }
}

class AutoClearPendingException
{
public:
  AutoClearPendingException(JSContext *cx) : mCx(cx) { }
  ~AutoClearPendingException() { JS_ClearPendingException(mCx); }
private:
  JSContext* mCx;
};

nsresult
nsXPCWrappedJSClass::CheckForException(XPCCallContext & ccx,
                                       const char * aPropertyName,
                                       const char * anInterfaceName,
                                       bool aForceReport)
{
    XPCContext * xpcc = ccx.GetXPCContext();
    JSContext * cx = ccx.GetJSContext();
    nsCOMPtr<nsIException> xpc_exception;
    /* this one would be set by our error reporter */

    xpcc->GetException(getter_AddRefs(xpc_exception));
    if (xpc_exception)
        xpcc->SetException(nullptr);

    // get this right away in case we do something below to cause JS code
    // to run on this JSContext
    nsresult pending_result = xpcc->GetPendingResult();

    jsval js_exception;
    JSBool is_js_exception = JS_GetPendingException(cx, &js_exception);

    /* JS might throw an expection whether the reporter was called or not */
    if (is_js_exception) {
        if (!xpc_exception)
            XPCConvert::JSValToXPCException(ccx, js_exception, anInterfaceName,
                                            aPropertyName,
                                            getter_AddRefs(xpc_exception));

        /* cleanup and set failed even if we can't build an exception */
        if (!xpc_exception) {
            XPCJSRuntime::Get()->SetPendingException(nullptr); // XXX necessary?
        }
    }

    AutoClearPendingException acpe(cx);

    if (xpc_exception) {
        nsresult e_result;
        if (NS_SUCCEEDED(xpc_exception->GetResult(&e_result))) {
            // Figure out whether or not we should report this exception.
            bool reportable = xpc_IsReportableErrorCode(e_result);
            if (reportable) {
                // Always want to report forced exceptions and XPConnect's own
                // errors.
                reportable = aForceReport ||
                    NS_ERROR_GET_MODULE(e_result) == NS_ERROR_MODULE_XPCONNECT;

                // See if an environment variable was set or someone has told us
                // that a user pref was set indicating that we should report all
                // exceptions.
                if (!reportable)
                    reportable = nsXPConnect::ReportAllJSExceptions();

                // Finally, check to see if this is the last JS frame on the
                // stack. If so then we always want to report it.
                if (!reportable) {
                    reportable = !JS_DescribeScriptedCaller(cx, nullptr, nullptr);
                }

                // Ugly special case for GetInterface. It's "special" in the
                // same way as QueryInterface in that a failure is not
                // exceptional and shouldn't be reported. We have to do this
                // check here instead of in xpcwrappedjs (like we do for QI) to
                // avoid adding extra code to all xpcwrappedjs objects.
                if (reportable && e_result == NS_ERROR_NO_INTERFACE &&
                    !strcmp(anInterfaceName, "nsIInterfaceRequestor") &&
                    !strcmp(aPropertyName, "getInterface")) {
                    reportable = false;
                }
            }

            // Try to use the error reporter set on the context to handle this
            // error if it came from a JS exception.
            if (reportable && is_js_exception &&
                JS_GetErrorReporter(cx) != xpcWrappedJSErrorReporter) {
                reportable = !JS_ReportPendingException(cx);
            }

            if (reportable) {
#ifdef DEBUG
                static const char line[] =
                    "************************************************************\n";
                static const char preamble[] =
                    "* Call to xpconnect wrapped JSObject produced this error:  *\n";
                static const char cant_get_text[] =
                    "FAILED TO GET TEXT FROM EXCEPTION\n";

                fputs(line, stdout);
                fputs(preamble, stdout);
                char* text;
                if (NS_SUCCEEDED(xpc_exception->ToString(&text)) && text) {
                    fputs(text, stdout);
                    fputs("\n", stdout);
                    nsMemory::Free(text);
                } else
                    fputs(cant_get_text, stdout);
                fputs(line, stdout);
#endif

                // Log the exception to the JS Console, so that users can do
                // something with it.
                nsCOMPtr<nsIConsoleService> consoleService
                    (do_GetService(XPC_CONSOLE_CONTRACTID));
                if (nullptr != consoleService) {
                    nsresult rv;
                    nsCOMPtr<nsIScriptError> scriptError;
                    nsCOMPtr<nsISupports> errorData;
                    rv = xpc_exception->GetData(getter_AddRefs(errorData));
                    if (NS_SUCCEEDED(rv))
                        scriptError = do_QueryInterface(errorData);

                    if (nullptr == scriptError) {
                        // No luck getting one from the exception, so
                        // try to cook one up.
                        scriptError = do_CreateInstance(XPC_SCRIPT_ERROR_CONTRACTID);
                        if (nullptr != scriptError) {
                            char* exn_string;
                            rv = xpc_exception->ToString(&exn_string);
                            if (NS_SUCCEEDED(rv)) {
                                // use toString on the exception as the message
                                NS_ConvertASCIItoUTF16 newMessage(exn_string);
                                nsMemory::Free((void *) exn_string);

                                // try to get filename, lineno from the first
                                // stack frame location.
                                int32_t lineNumber = 0;
                                nsXPIDLCString sourceName;

                                nsCOMPtr<nsIStackFrame> location;
                                xpc_exception->
                                    GetLocation(getter_AddRefs(location));
                                if (location) {
                                    // Get line number w/o checking; 0 is ok.
                                    location->GetLineNumber(&lineNumber);

                                    // get a filename.
                                    rv = location->GetFilename(getter_Copies(sourceName));
                                }

                                rv = scriptError->InitWithWindowID(newMessage.get(),
                                                                   NS_ConvertASCIItoUTF16(sourceName).get(),
                                                                   nullptr,
                                                                   lineNumber, 0, 0,
                                                                   "XPConnect JavaScript",
                                                                   nsJSUtils::GetCurrentlyRunningCodeInnerWindowID(cx));
                                if (NS_FAILED(rv))
                                    scriptError = nullptr;
                            }
                        }
                    }
                    if (nullptr != scriptError)
                        consoleService->LogMessage(scriptError);
                }
            }
            // Whether or not it passes the 'reportable' test, it might
            // still be an error and we have to do the right thing here...
            if (NS_FAILED(e_result)) {
                XPCJSRuntime::Get()->SetPendingException(xpc_exception);
                return e_result;
            }
        }
    } else {
        // see if JS code signaled failure result without throwing exception
        if (NS_FAILED(pending_result)) {
            return pending_result;
        }
    }
    return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsXPCWrappedJSClass::CallMethod(nsXPCWrappedJS* wrapper, uint16_t methodIndex,
                                const XPTMethodDescriptor* info,
                                nsXPTCMiniVariant* nativeParams)
{
    jsval* sp = nullptr;
    jsval* argv = nullptr;
    uint8_t i;
    nsresult retval = NS_ERROR_FAILURE;
    nsresult pending_result = NS_OK;
    JSBool success;
    JSBool readyToDoTheCall = false;
    nsID  param_iid;
    const char* name = info->name;
    jsval fval;
    JSBool foundDependentParam;

    // Make sure not to set the callee on ccx until after we've gone through
    // the whole nsIXPCFunctionThisTranslator bit.  That code uses ccx to
    // convert natives to JSObjects, but we do NOT plan to pass those JSObjects
    // to our real callee.
    JSContext *context = GetContextFromObject(wrapper->GetJSObject());
    XPCCallContext ccx(NATIVE_CALLER, context);
    if (!ccx.IsValid())
        return retval;

    XPCContext *xpcc = ccx.GetXPCContext();
    JSContext *cx = xpc_UnmarkGrayContext(ccx.GetJSContext());

    if (!cx || !xpcc || !IsReflectable(methodIndex))
        return NS_ERROR_FAILURE;

    JSObject *obj = wrapper->GetJSObject();
    JSObject *thisObj = obj;

    JSAutoCompartment ac(cx, obj);
    ccx.SetScopeForNewJSObjects(obj);

    JS::AutoValueVector args(cx);
    AutoScriptEvaluate scriptEval(cx);

    // XXX ASSUMES that retval is last arg. The xpidl compiler ensures this.
    uint8_t paramCount = info->num_args;
    uint8_t argc = paramCount -
        (paramCount && XPT_PD_IS_RETVAL(info->params[paramCount-1].flags) ? 1 : 0);

    if (!scriptEval.StartEvaluating(obj, xpcWrappedJSErrorReporter))
        goto pre_call_clean_up;

    xpcc->SetPendingResult(pending_result);
    xpcc->SetException(nullptr);
    XPCJSRuntime::Get()->SetPendingException(nullptr);

    // We use js_Invoke so that the gcthings we use as args will be rooted by
    // the engine as we do conversions and prepare to do the function call.

    // setup stack

    // if this isn't a function call then we don't need to push extra stuff
    if (!(XPT_MD_IS_SETTER(info->flags) || XPT_MD_IS_GETTER(info->flags))) {
        // We get fval before allocating the stack to avoid gc badness that can
        // happen if the GetProperty call leaves our request and the gc runs
        // while the stack we allocate contains garbage.

        // If the interface is marked as a [function] then we will assume that
        // our JSObject is a function and not an object with a named method.

        bool isFunction;
        if (NS_FAILED(mInfo->IsFunction(&isFunction)))
            goto pre_call_clean_up;

        // In the xpidl [function] case we are making sure now that the
        // JSObject is callable. If it is *not* callable then we silently
        // fallback to looking up the named property...
        // (because jst says he thinks this fallback is 'The Right Thing'.)
        //
        // In the normal (non-function) case we just lookup the property by
        // name and as long as the object has such a named property we go ahead
        // and try to make the call. If it turns out the named property is not
        // a callable object then the JS engine will throw an error and we'll
        // pass this along to the caller as an exception/result code.

        if (isFunction &&
            JS_TypeOfValue(ccx, OBJECT_TO_JSVAL(obj)) == JSTYPE_FUNCTION) {
            fval = OBJECT_TO_JSVAL(obj);

            // We may need to translate the 'this' for the function object.

            if (paramCount) {
                const nsXPTParamInfo& firstParam = info->params[0];
                if (firstParam.IsIn()) {
                    const nsXPTType& firstType = firstParam.GetType();

                    if (firstType.IsInterfacePointer()) {
                        nsIXPCFunctionThisTranslator* translator;

                        IID2ThisTranslatorMap* map =
                            mRuntime->GetThisTranslatorMap();

                        {
                            XPCAutoLock lock(mRuntime->GetMapLock()); // scoped lock
                            translator = map->Find(mIID);
                        }

                        if (translator) {
                            bool hideFirstParamFromJS = false;
                            nsIID* newWrapperIID = nullptr;
                            nsCOMPtr<nsISupports> newThis;

                            if (NS_FAILED(translator->
                                          TranslateThis((nsISupports*)nativeParams[0].val.p,
                                                        mInfo, methodIndex,
                                                        &hideFirstParamFromJS,
                                                        &newWrapperIID,
                                                        getter_AddRefs(newThis)))) {
                                goto pre_call_clean_up;
                            }
                            if (hideFirstParamFromJS) {
                                NS_ERROR("HideFirstParamFromJS not supported");
                                goto pre_call_clean_up;
                            }
                            if (newThis) {
                                jsval v;
                                xpcObjectHelper helper(newThis);
                                JSBool ok =
                                  XPCConvert::NativeInterface2JSObject(ccx,
                                                                       &v, nullptr, helper, newWrapperIID,
                                                                       nullptr, false, nullptr);
                                if (newWrapperIID)
                                    nsMemory::Free(newWrapperIID);
                                if (!ok) {
                                    goto pre_call_clean_up;
                                }
                                thisObj = JSVAL_TO_OBJECT(v);
                                if (!JS_WrapObject(cx, &thisObj))
                                    goto pre_call_clean_up;
                            }
                        }
                    }
                }
            }
        } else if (!JS_GetMethod(cx, obj, name, &thisObj, &fval)) {
            // XXX We really want to factor out the error reporting better and
            // specifically report the failure to find a function with this name.
            // This is what we do below if the property is found but is not a
            // function. We just need to factor better so we can get to that
            // reporting path from here.
            goto pre_call_clean_up;
        }
    }

    if (!args.resize(argc)) {
        retval = NS_ERROR_OUT_OF_MEMORY;
        goto pre_call_clean_up;
    }

    argv = args.begin();
    sp = argv;

    // build the args
    // NB: This assignment *looks* wrong because we haven't yet called our
    // function. However, we *have* already entered the compartmen that we're
    // about to call, and that's the global that we want here. In other words:
    // we're trusting the JS engine to come up with a good global to use for
    // our object (whatever it was).
    for (i = 0; i < argc; i++) {
        const nsXPTParamInfo& param = info->params[i];
        const nsXPTType& type = param.GetType();
        nsXPTType datum_type;
        uint32_t array_count;
        bool isArray = type.IsArray();
        jsval val = JSVAL_NULL;
        AUTO_MARK_JSVAL(ccx, &val);
        bool isSizedString = isArray ?
                false :
                type.TagPart() == nsXPTType::T_PSTRING_SIZE_IS ||
                type.TagPart() == nsXPTType::T_PWSTRING_SIZE_IS;


        // verify that null was not passed for 'out' param
        if (param.IsOut() && !nativeParams[i].val.p) {
            retval = NS_ERROR_INVALID_ARG;
            goto pre_call_clean_up;
        }

        if (isArray) {
            if (NS_FAILED(mInfo->GetTypeForParam(methodIndex, &param, 1,
                                                 &datum_type)))
                goto pre_call_clean_up;
        } else
            datum_type = type;

        if (param.IsIn()) {
            nsXPTCMiniVariant* pv;

            if (param.IsIndirect())
                pv = (nsXPTCMiniVariant*) nativeParams[i].val.p;
            else
                pv = &nativeParams[i];

            if (datum_type.IsInterfacePointer() &&
                !GetInterfaceTypeFromParam(cx, info, param, methodIndex,
                                           datum_type, nativeParams,
                                           &param_iid))
                goto pre_call_clean_up;

            if (isArray || isSizedString) {
                if (!GetArraySizeFromParam(cx, info, param, methodIndex,
                                           i, nativeParams, &array_count))
                    goto pre_call_clean_up;
            }

            if (isArray) {
                XPCLazyCallContext lccx(ccx);
                if (!XPCConvert::NativeArray2JS(lccx, &val,
                                                (const void**)&pv->val,
                                                datum_type, &param_iid,
                                                array_count, nullptr))
                    goto pre_call_clean_up;
            } else if (isSizedString) {
                if (!XPCConvert::NativeStringWithSize2JS(ccx, &val,
                                                         (const void*)&pv->val,
                                                         datum_type,
                                                         array_count, nullptr))
                    goto pre_call_clean_up;
            } else {
                if (!XPCConvert::NativeData2JS(ccx, &val, &pv->val, type,
                                               &param_iid, nullptr))
                    goto pre_call_clean_up;
            }
        }

        if (param.IsOut() || param.IsDipper()) {
            // create an 'out' object
            JSObject* out_obj = NewOutObject(cx, obj);
            if (!out_obj) {
                retval = NS_ERROR_OUT_OF_MEMORY;
                goto pre_call_clean_up;
            }

            if (param.IsIn()) {
                if (!JS_SetPropertyById(cx, out_obj,
                                        mRuntime->GetStringID(XPCJSRuntime::IDX_VALUE),
                                        &val)) {
                    goto pre_call_clean_up;
                }
            }
            *sp++ = OBJECT_TO_JSVAL(out_obj);
        } else
            *sp++ = val;
    }

    readyToDoTheCall = true;

pre_call_clean_up:
    // clean up any 'out' params handed in
    for (i = 0; i < paramCount; i++) {
        const nsXPTParamInfo& param = info->params[i];
        if (!param.IsOut())
            continue;

        const nsXPTType& type = param.GetType();
        if (!type.deprecated_IsPointer())
            continue;
        void* p;
        if (!(p = nativeParams[i].val.p))
            continue;

        if (param.IsIn()) {
            if (type.IsArray()) {
                void** pp;
                if (nullptr != (pp = *((void***)p))) {

                    // we need to get the array length and iterate the items
                    uint32_t array_count;
                    nsXPTType datum_type;

                    if (NS_SUCCEEDED(mInfo->GetTypeForParam(methodIndex, &param,
                                                            1, &datum_type)) &&
                        datum_type.deprecated_IsPointer() &&
                        GetArraySizeFromParam(cx, info, param, methodIndex,
                                              i, nativeParams, &array_count) &&
                        array_count) {

                        CleanupPointerArray(datum_type, array_count, pp);
                    }

                    // always release the array if it is inout
                    nsMemory::Free(pp);
                }
            } else
                CleanupPointerTypeObject(type, (void**)p);
        }
        *((void**)p) = nullptr;
    }

    // Make sure "this" doesn't get deleted during this call.
    nsCOMPtr<nsIXPCWrappedJSClass> kungFuDeathGrip(this);

    if (!readyToDoTheCall)
        return retval;

    // do the deed - note exceptions

    JS_ClearPendingException(cx);

    jsval rval;
    if (XPT_MD_IS_GETTER(info->flags)) {
        success = JS_GetProperty(cx, obj, name, argv);
        rval = *argv;
    } else if (XPT_MD_IS_SETTER(info->flags)) {
        success = JS_SetProperty(cx, obj, name, argv);
        rval = *argv;
    } else {
        if (!JSVAL_IS_PRIMITIVE(fval)) {
            uint32_t oldOpts = JS_GetOptions(cx);
            JS_SetOptions(cx, oldOpts | JSOPTION_DONT_REPORT_UNCAUGHT);

            success = JS_CallFunctionValue(cx, thisObj, fval, argc, argv, &rval);

            JS_SetOptions(cx, oldOpts);
        } else {
            // The property was not an object so can't be a function.
            // Let's build and 'throw' an exception.

            static const nsresult code =
                    NS_ERROR_XPC_JSOBJECT_HAS_NO_FUNCTION_NAMED;
            static const char format[] = "%s \"%s\"";
            const char * msg;
            char* sz = nullptr;

            if (nsXPCException::NameAndFormatForNSResult(code, nullptr, &msg) && msg)
                sz = JS_smprintf(format, msg, name);

            nsCOMPtr<nsIException> e;

            XPCConvert::ConstructException(code, sz, GetInterfaceName(), name,
                                           nullptr, getter_AddRefs(e), nullptr, nullptr);
            xpcc->SetException(e);
            if (sz)
                JS_smprintf_free(sz);
            success = false;
        }
    }

    if (!success) {
        bool forceReport;
        if (NS_FAILED(mInfo->IsFunction(&forceReport)))
            forceReport = false;

        // May also want to check if we're moving from content->chrome and force
        // a report in that case.

        return CheckForException(ccx, name, GetInterfaceName(), forceReport);
    }

    XPCJSRuntime::Get()->SetPendingException(nullptr); // XXX necessary?

    // convert out args and result
    // NOTE: this is the total number of native params, not just the args
    // Convert independent params only.
    // When we later convert the dependent params (if any) we will know that
    // the params upon which they depend will have already been converted -
    // regardless of ordering.

    foundDependentParam = false;
    for (i = 0; i < paramCount; i++) {
        const nsXPTParamInfo& param = info->params[i];
        NS_ABORT_IF_FALSE(!param.IsShared(), "[shared] implies [noscript]!");
        if (!param.IsOut() && !param.IsDipper())
            continue;

        const nsXPTType& type = param.GetType();
        if (type.IsDependent()) {
            foundDependentParam = true;
            continue;
        }

        jsval val;
        uint8_t type_tag = type.TagPart();
        nsXPTCMiniVariant* pv;

        if (param.IsDipper())
            pv = (nsXPTCMiniVariant*) &nativeParams[i].val.p;
        else
            pv = (nsXPTCMiniVariant*) nativeParams[i].val.p;

        if (param.IsRetval())
            val = rval;
        else if (JSVAL_IS_PRIMITIVE(argv[i]) ||
                 !JS_GetPropertyById(cx, JSVAL_TO_OBJECT(argv[i]),
                                     mRuntime->GetStringID(XPCJSRuntime::IDX_VALUE),
                                     &val))
            break;

        // setup allocator and/or iid

        if (type_tag == nsXPTType::T_INTERFACE) {
            if (NS_FAILED(GetInterfaceInfo()->
                          GetIIDForParamNoAlloc(methodIndex, &param,
                                                &param_iid)))
                break;
        }

        if (!XPCConvert::JSData2Native(ccx, &pv->val, val, type,
                                       !param.IsDipper(), &param_iid, nullptr))
            break;
    }

    // if any params were dependent, then we must iterate again to convert them.
    if (foundDependentParam && i == paramCount) {
        for (i = 0; i < paramCount; i++) {
            const nsXPTParamInfo& param = info->params[i];
            if (!param.IsOut())
                continue;

            const nsXPTType& type = param.GetType();
            if (!type.IsDependent())
                continue;

            jsval val;
            nsXPTCMiniVariant* pv;
            nsXPTType datum_type;
            uint32_t array_count;
            bool isArray = type.IsArray();
            bool isSizedString = isArray ?
                    false :
                    type.TagPart() == nsXPTType::T_PSTRING_SIZE_IS ||
                    type.TagPart() == nsXPTType::T_PWSTRING_SIZE_IS;

            pv = (nsXPTCMiniVariant*) nativeParams[i].val.p;

            if (param.IsRetval())
                val = rval;
            else if (!JS_GetPropertyById(cx, JSVAL_TO_OBJECT(argv[i]),
                                         mRuntime->GetStringID(XPCJSRuntime::IDX_VALUE),
                                         &val))
                break;

            // setup allocator and/or iid

            if (isArray) {
                if (NS_FAILED(mInfo->GetTypeForParam(methodIndex, &param, 1,
                                                     &datum_type)))
                    break;
            } else
                datum_type = type;

            if (datum_type.IsInterfacePointer()) {
               if (!GetInterfaceTypeFromParam(cx, info, param, methodIndex,
                                              datum_type, nativeParams,
                                              &param_iid))
                   break;
            }

            if (isArray || isSizedString) {
                if (!GetArraySizeFromParam(cx, info, param, methodIndex,
                                           i, nativeParams, &array_count))
                    break;
            }

            if (isArray) {
                if (array_count &&
                    !XPCConvert::JSArray2Native(ccx, (void**)&pv->val, val,
                                                array_count, datum_type,
                                                &param_iid, nullptr))
                    break;
            } else if (isSizedString) {
                if (!XPCConvert::JSStringWithSize2Native(ccx,
                                                         (void*)&pv->val, val,
                                                         array_count, datum_type,
                                                         nullptr))
                    break;
            } else {
                if (!XPCConvert::JSData2Native(ccx, &pv->val, val, type,
                                               true, &param_iid,
                                               nullptr))
                    break;
            }
        }
    }

    if (i != paramCount) {
        // We didn't manage all the result conversions!
        // We have to cleanup any junk that *did* get converted.

        for (uint8_t k = 0; k < i; k++) {
            const nsXPTParamInfo& param = info->params[k];
            if (!param.IsOut())
                continue;
            const nsXPTType& type = param.GetType();
            if (!type.deprecated_IsPointer())
                continue;
            void* p;
            if (!(p = nativeParams[k].val.p))
                continue;

            if (type.IsArray()) {
                void** pp;
                if (nullptr != (pp = *((void***)p))) {
                    // we need to get the array length and iterate the items
                    uint32_t array_count;
                    nsXPTType datum_type;

                    if (NS_SUCCEEDED(mInfo->GetTypeForParam(methodIndex, &param,
                                                            1, &datum_type)) &&
                        datum_type.deprecated_IsPointer() &&
                        GetArraySizeFromParam(cx, info, param, methodIndex,
                                              k, nativeParams, &array_count) &&
                        array_count) {

                        CleanupPointerArray(datum_type, array_count, pp);
                    }
                    nsMemory::Free(pp);
                }
            } else
                CleanupPointerTypeObject(type, (void**)p);
            *((void**)p) = nullptr;
        }
    } else {
        // set to whatever the JS code might have set as the result
        retval = pending_result;
    }

    return retval;
}

const char*
nsXPCWrappedJSClass::GetInterfaceName()
{
    if (!mName)
        mInfo->GetName(&mName);
    return mName;
}

JSObject*
nsXPCWrappedJSClass::NewOutObject(JSContext* cx, JSObject* scope)
{
    return JS_NewObject(cx, nullptr, nullptr, JS_GetGlobalForObject(cx, scope));
}


NS_IMETHODIMP
nsXPCWrappedJSClass::DebugDump(int16_t depth)
{
#ifdef DEBUG
    depth-- ;
    XPC_LOG_ALWAYS(("nsXPCWrappedJSClass @ %x with mRefCnt = %d", this, mRefCnt.get()));
    XPC_LOG_INDENT();
        char* name;
        mInfo->GetName(&name);
        XPC_LOG_ALWAYS(("interface name is %s", name));
        if (name)
            nsMemory::Free(name);
        char * iid = mIID.ToString();
        XPC_LOG_ALWAYS(("IID number is %s", iid ? iid : "invalid"));
        if (iid)
            NS_Free(iid);
        XPC_LOG_ALWAYS(("InterfaceInfo @ %x", mInfo));
        uint16_t methodCount = 0;
        if (depth) {
            uint16_t i;
            nsCOMPtr<nsIInterfaceInfo> parent;
            XPC_LOG_INDENT();
            mInfo->GetParent(getter_AddRefs(parent));
            XPC_LOG_ALWAYS(("parent @ %x", parent.get()));
            mInfo->GetMethodCount(&methodCount);
            XPC_LOG_ALWAYS(("MethodCount = %d", methodCount));
            mInfo->GetConstantCount(&i);
            XPC_LOG_ALWAYS(("ConstantCount = %d", i));
            XPC_LOG_OUTDENT();
        }
        XPC_LOG_ALWAYS(("mRuntime @ %x", mRuntime));
        XPC_LOG_ALWAYS(("mDescriptors @ %x count = %d", mDescriptors, methodCount));
        if (depth && mDescriptors && methodCount) {
            depth--;
            XPC_LOG_INDENT();
            for (uint16_t i = 0; i < methodCount; i++) {
                XPC_LOG_ALWAYS(("Method %d is %s%s", \
                                i, IsReflectable(i) ? "":" NOT ","reflectable"));
            }
            XPC_LOG_OUTDENT();
            depth++;
        }
    XPC_LOG_OUTDENT();
#endif
    return NS_OK;
}
