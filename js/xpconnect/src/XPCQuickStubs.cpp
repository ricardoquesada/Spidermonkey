/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Util.h"

#include "jsapi.h"
#include "jsfriendapi.h"
#include "nsCOMPtr.h"
#include "xpcprivate.h"
#include "XPCInlines.h"
#include "XPCQuickStubs.h"
#include "mozilla/dom/BindingUtils.h"

using namespace mozilla;
using namespace JS;

extern const char* xpc_qsStringTable;

static inline QITableEntry *
GetOffsets(nsISupports *identity, XPCWrappedNativeProto* proto)
{
    QITableEntry* offsets = proto ? proto->GetOffsets() : nullptr;
    if (!offsets) {
        static NS_DEFINE_IID(kThisPtrOffsetsSID, NS_THISPTROFFSETS_SID);
        identity->QueryInterface(kThisPtrOffsetsSID, (void**)&offsets);
    }
    return offsets;
}

static inline QITableEntry *
GetOffsetsFromSlimWrapper(JSObject *obj)
{
    NS_ASSERTION(IS_SLIM_WRAPPER(obj), "What kind of object is this?");
    return GetOffsets(static_cast<nsISupports*>(xpc_GetJSPrivate(obj)),
                      GetSlimWrapperProto(obj));
}

static const xpc_qsHashEntry *
LookupEntry(uint32_t tableSize, const xpc_qsHashEntry *table, const nsID &iid)
{
    size_t i;
    const xpc_qsHashEntry *p;

    i = iid.m0 % tableSize;
    do
    {
        p = table + i;
        if (p->iid.Equals(iid))
            return p;
        i = p->chain;
    } while (i != XPC_QS_NULL_INDEX);
    return nullptr;
}

static const xpc_qsHashEntry *
LookupInterfaceOrAncestor(uint32_t tableSize, const xpc_qsHashEntry *table,
                          const nsID &iid)
{
    const xpc_qsHashEntry *entry = LookupEntry(tableSize, table, iid);
    if (!entry) {
        /*
         * On a miss, we have to search for every interface the object
         * supports, including ancestors.
         */
        nsCOMPtr<nsIInterfaceInfo> info;
        if (NS_FAILED(nsXPConnect::GetXPConnect()->GetInfoForIID(&iid, getter_AddRefs(info))))
            return nullptr;

        const nsIID *piid;
        for (;;) {
            nsCOMPtr<nsIInterfaceInfo> parent;
            if (NS_FAILED(info->GetParent(getter_AddRefs(parent))) ||
                !parent ||
                NS_FAILED(parent->GetIIDShared(&piid))) {
                break;
            }
            entry = LookupEntry(tableSize, table, *piid);
            if (entry)
                break;
            info.swap(parent);
        }
    }
    return entry;
}

static void
PointerFinalize(JSFreeOp *fop, JSObject *obj)
{
    JSPropertyOp *popp = static_cast<JSPropertyOp *>(JS_GetPrivate(obj));
    delete popp;
}

JSClass
PointerHolderClass = {
    "Pointer", JSCLASS_HAS_PRIVATE,
    JS_PropertyStub, JS_DeletePropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, PointerFinalize
};

JSBool
xpc_qsDefineQuickStubs(JSContext *cx, JSObject *protoArg, unsigned flags,
                       uint32_t ifacec, const nsIID **interfaces,
                       uint32_t tableSize, const xpc_qsHashEntry *table,
                       const xpc_qsPropertySpec *propspecs,
                       const xpc_qsFunctionSpec *funcspecs,
                       const char *stringTable)
{
    /*
     * Walk interfaces in reverse order to behave like XPConnect when a
     * feature is defined in more than one of the interfaces.
     *
     * XPCNativeSet::FindMethod returns the first matching feature it finds,
     * searching the interfaces forward.  Here, definitions toward the
     * front of 'interfaces' overwrite those toward the back.
     */
    RootedObject proto(cx, protoArg);
    for (uint32_t i = ifacec; i-- != 0;) {
        const nsID &iid = *interfaces[i];
        const xpc_qsHashEntry *entry =
            LookupInterfaceOrAncestor(tableSize, table, iid);

        if (entry) {
            for (;;) {
                // Define quick stubs for attributes.
                const xpc_qsPropertySpec *ps = propspecs + entry->prop_index;
                const xpc_qsPropertySpec *ps_end = ps + entry->n_props;
                for ( ; ps < ps_end; ++ps) {
                    if (!JS_DefineProperty(cx, proto,
                                           stringTable + ps->name_index,
                                           JSVAL_VOID,
                                           (JSPropertyOp)ps->getter,
                                           (JSStrictPropertyOp)ps->setter,
                                           flags | JSPROP_SHARED | JSPROP_NATIVE_ACCESSORS))
                        return false;
                }

                // Define quick stubs for methods.
                const xpc_qsFunctionSpec *fs = funcspecs + entry->func_index;
                const xpc_qsFunctionSpec *fs_end = fs + entry->n_funcs;
                for ( ; fs < fs_end; ++fs) {
                    if (!JS_DefineFunction(cx, proto,
                                           stringTable + fs->name_index,
                                           reinterpret_cast<JSNative>(fs->native),
                                           fs->arity, flags))
                        return false;
                }

                if (entry->newBindingProperties) {
                    mozilla::dom::DefineWebIDLBindingPropertiesOnXPCProto(cx, proto, entry->newBindingProperties);
                }
                // Next.
                size_t j = entry->parentInterface;
                if (j == XPC_QS_NULL_INDEX)
                    break;
                entry = table + j;
            }
        }
    }

    return true;
}

JSBool
xpc_qsThrow(JSContext *cx, nsresult rv)
{
    XPCThrower::Throw(rv, cx);
    return false;
}

/**
 * Get the interface name and member name (for error messages).
 *
 * We could instead have each quick stub pass its name to the error-handling
 * functions, as that name is statically known.  But that would be redundant;
 * the information is handy at runtime anyway.  Also, this code often produces
 * a more specific error message, e.g. "[nsIDOMHTMLDocument.appendChild]"
 * rather than "[nsIDOMNode.appendChild]".
 */
static void
GetMemberInfo(JSObject *obj, jsid memberId, const char **ifaceName)
{
    *ifaceName = "Unknown";

    // Don't try to generate a useful name if there are security wrappers,
    // because it isn't worth the risk of something going wrong just to generate
    // an error message. Instead, only handle the simple case where we have the
    // reflector in hand.
    if (IS_WRAPPER_CLASS(js::GetObjectClass(obj))) {
        XPCWrappedNativeProto *proto;
        if (IS_SLIM_WRAPPER_OBJECT(obj)) {
            proto = GetSlimWrapperProto(obj);
        } else {
            MOZ_ASSERT(IS_WN_WRAPPER_OBJECT(obj));
            XPCWrappedNative *wrapper =
                static_cast<XPCWrappedNative *>(js::GetObjectPrivate(obj));
            proto = wrapper->GetProto();
        }
        if (proto) {
            XPCNativeSet *set = proto->GetSet();
            if (set) {
                XPCNativeMember *member;
                XPCNativeInterface *iface;

                if (set->FindMember(memberId, &member, &iface))
                    *ifaceName = iface->GetNameString();
            }
        }
    }
}

static void
GetMethodInfo(JSContext *cx, jsval *vp, const char **ifaceNamep, jsid *memberIdp)
{
    RootedObject funobj(cx, JSVAL_TO_OBJECT(JS_CALLEE(cx, vp)));
    NS_ASSERTION(JS_ObjectIsFunction(cx, funobj),
                 "JSNative callee should be Function object");
    RootedString str(cx, JS_GetFunctionId(JS_GetObjectFunction(funobj)));
    RootedId methodId(cx, str ? INTERNED_STRING_TO_JSID(cx, str) : JSID_VOID);
    GetMemberInfo(JSVAL_TO_OBJECT(vp[1]), methodId, ifaceNamep);
    *memberIdp = methodId;
}

static bool
ThrowCallFailed(JSContext *cx, nsresult rv,
                const char *ifaceName, HandleId memberId, const char *memberName)
{
    /* Only one of memberId or memberName should be given. */
    MOZ_ASSERT(JSID_IS_VOID(memberId) != !memberName);

    // From XPCThrower::ThrowBadResult.
    char* sz;
    const char* format;
    const char* name;

    /*
     *  If there is a pending exception when the native call returns and
     *  it has the same error result as returned by the native call, then
     *  the native call may be passing through an error from a previous JS
     *  call. So we'll just throw that exception into our JS.
     */
    if (XPCThrower::CheckForPendingException(rv, cx))
        return false;

    // else...

    if (!nsXPCException::NameAndFormatForNSResult(NS_ERROR_XPC_NATIVE_RETURNED_FAILURE, nullptr, &format) ||
        !format) {
        format = "";
    }

    JSAutoByteString memberNameBytes;
    if (!memberName) {
        memberName = JSID_IS_STRING(memberId)
                     ? memberNameBytes.encodeLatin1(cx, JSID_TO_STRING(memberId))
                     : "unknown";
    }
    if (nsXPCException::NameAndFormatForNSResult(rv, &name, nullptr)
        && name) {
        sz = JS_smprintf("%s 0x%x (%s) [%s.%s]",
                         format, rv, name, ifaceName, memberName);
    } else {
        sz = JS_smprintf("%s 0x%x [%s.%s]",
                         format, rv, ifaceName, memberName);
    }

    XPCThrower::BuildAndThrowException(cx, rv, sz);

    if (sz)
        JS_smprintf_free(sz);

    return false;
}

JSBool
xpc_qsThrowGetterSetterFailed(JSContext *cx, nsresult rv, JSObject *obj,
                              jsid memberIdArg)
{
    RootedId memberId(cx, memberIdArg);
    const char *ifaceName;
    GetMemberInfo(obj, memberId, &ifaceName);
    return ThrowCallFailed(cx, rv, ifaceName, memberId, NULL);
}

JSBool
xpc_qsThrowGetterSetterFailed(JSContext *cx, nsresult rv, JSObject *objArg,
                              const char* memberName)
{
    RootedObject obj(cx, objArg);
    JSString *str = JS_InternString(cx, memberName);
    if (!str) {
        return false;
    }
    return xpc_qsThrowGetterSetterFailed(cx, rv, obj,
                                         INTERNED_STRING_TO_JSID(cx, str));
}

JSBool
xpc_qsThrowGetterSetterFailed(JSContext *cx, nsresult rv, JSObject *obj,
                              uint16_t memberIndex)
{
    return xpc_qsThrowGetterSetterFailed(cx, rv, obj,
                                         xpc_qsStringTable + memberIndex);
}

JSBool
xpc_qsThrowMethodFailed(JSContext *cx, nsresult rv, jsval *vp)
{
    const char *ifaceName;
    RootedId memberId(cx);
    GetMethodInfo(cx, vp, &ifaceName, memberId.address());
    return ThrowCallFailed(cx, rv, ifaceName, memberId, NULL);
}

JSBool
xpc_qsThrowMethodFailedWithCcx(XPCCallContext &ccx, nsresult rv)
{
    ThrowBadResult(rv, ccx);
    return false;
}

bool
xpc_qsThrowMethodFailedWithDetails(JSContext *cx, nsresult rv,
                                   const char *ifaceName,
                                   const char *memberName)
{
    return ThrowCallFailed(cx, rv, ifaceName, JSID_VOIDHANDLE, memberName);
}

static void
ThrowBadArg(JSContext *cx, nsresult rv, const char *ifaceName,
            jsid memberId, const char *memberName, unsigned paramnum)
{
    /* Only one memberId or memberName should be given. */
    MOZ_ASSERT(JSID_IS_VOID(memberId) != !memberName);

    // From XPCThrower::ThrowBadParam.
    char* sz;
    const char* format;

    if (!nsXPCException::NameAndFormatForNSResult(rv, nullptr, &format))
        format = "";

    JSAutoByteString memberNameBytes;
    if (!memberName) {
        memberName = JSID_IS_STRING(memberId)
                     ? memberNameBytes.encodeLatin1(cx, JSID_TO_STRING(memberId))
                     : "unknown";
    }
    sz = JS_smprintf("%s arg %u [%s.%s]",
                     format, (unsigned int) paramnum, ifaceName, memberName);

    XPCThrower::BuildAndThrowException(cx, rv, sz);

    if (sz)
        JS_smprintf_free(sz);
}

void
xpc_qsThrowBadArg(JSContext *cx, nsresult rv, jsval *vp, unsigned paramnum)
{
    const char *ifaceName;
    RootedId memberId(cx);
    GetMethodInfo(cx, vp, &ifaceName, memberId.address());
    ThrowBadArg(cx, rv, ifaceName, memberId, NULL, paramnum);
}

void
xpc_qsThrowBadArgWithCcx(XPCCallContext &ccx, nsresult rv, unsigned paramnum)
{
    XPCThrower::ThrowBadParam(rv, paramnum, ccx);
}

void
xpc_qsThrowBadArgWithDetails(JSContext *cx, nsresult rv, unsigned paramnum,
                             const char *ifaceName, const char *memberName)
{
    ThrowBadArg(cx, rv, ifaceName, JSID_VOID, memberName, paramnum);
}

void
xpc_qsThrowBadSetterValue(JSContext *cx, nsresult rv,
                          JSObject *obj, jsid propIdArg)
{
    RootedId propId(cx, propIdArg);
    const char *ifaceName;
    GetMemberInfo(obj, propId, &ifaceName);
    ThrowBadArg(cx, rv, ifaceName, propId, NULL, 0);
}

void
xpc_qsThrowBadSetterValue(JSContext *cx, nsresult rv,
                          JSObject *objArg, const char* propName)
{
    RootedObject obj(cx, objArg);
    JSString *str = JS_InternString(cx, propName);
    if (!str) {
        return;
    }
    xpc_qsThrowBadSetterValue(cx, rv, obj, INTERNED_STRING_TO_JSID(cx, str));
}

void
xpc_qsThrowBadSetterValue(JSContext *cx, nsresult rv, JSObject *obj,
                          uint16_t name_index)
{
    xpc_qsThrowBadSetterValue(cx, rv, obj, xpc_qsStringTable + name_index);
}

JSBool
xpc_qsGetterOnlyPropertyStub(JSContext *cx, JSHandleObject obj, JSHandleId id, JSBool strict,
                             JSMutableHandleValue vp)
{
    return JS_ReportErrorFlagsAndNumber(cx,
                                        JSREPORT_WARNING | JSREPORT_STRICT |
                                        JSREPORT_STRICT_MODE_ERROR,
                                        js_GetErrorMessage, NULL,
                                        JSMSG_GETTER_ONLY);
}

JSBool
xpc_qsGetterOnlyNativeStub(JSContext *cx, unsigned argc, jsval *vp)
{
    return JS_ReportErrorFlagsAndNumber(cx,
                                        JSREPORT_WARNING | JSREPORT_STRICT |
                                        JSREPORT_STRICT_MODE_ERROR,
                                        js_GetErrorMessage, NULL,
                                        JSMSG_GETTER_ONLY);
}

xpc_qsDOMString::xpc_qsDOMString(JSContext *cx, jsval v, jsval *pval,
                                 StringificationBehavior nullBehavior,
                                 StringificationBehavior undefinedBehavior)
{
    typedef implementation_type::char_traits traits;
    // From the T_DOMSTRING case in XPCConvert::JSData2Native.
    JSString *s = InitOrStringify<traits>(cx, v, pval, nullBehavior,
                                          undefinedBehavior);
    if (!s)
        return;

    size_t len;
    const jschar *chars = JS_GetStringCharsZAndLength(cx, s, &len);
    if (!chars) {
        mValid = false;
        return;
    }

    new(mBuf) implementation_type(chars, len);
    mValid = true;
}

xpc_qsACString::xpc_qsACString(JSContext *cx, jsval v, jsval *pval,
                               StringificationBehavior nullBehavior,
                               StringificationBehavior undefinedBehavior)
{
    typedef implementation_type::char_traits traits;
    // From the T_CSTRING case in XPCConvert::JSData2Native.
    JSString *s = InitOrStringify<traits>(cx, v, pval, nullBehavior,
                                          undefinedBehavior);
    if (!s)
        return;

    size_t len = JS_GetStringEncodingLength(cx, s);
    if (len == size_t(-1)) {
        mValid = false;
        return;
    }

    JSAutoByteString bytes(cx, s);
    if (!bytes) {
        mValid = false;
        return;
    }

    new(mBuf) implementation_type(bytes.ptr(), len);
    mValid = true;
}

xpc_qsAUTF8String::xpc_qsAUTF8String(JSContext *cx, jsval v, jsval *pval)
{
    typedef nsCharTraits<PRUnichar> traits;
    // From the T_UTF8STRING  case in XPCConvert::JSData2Native.
    JSString *s = InitOrStringify<traits>(cx, v, pval, eNull, eNull);
    if (!s)
        return;

    size_t len;
    const PRUnichar *chars = JS_GetStringCharsZAndLength(cx, s, &len);
    if (!chars) {
        mValid = false;
        return;
    }

    new(mBuf) implementation_type(chars, len);
    mValid = true;
}

static nsresult
getNative(nsISupports *idobj,
          QITableEntry* entries,
          HandleObject obj,
          const nsIID &iid,
          void **ppThis,
          nsISupports **pThisRef,
          jsval *vp)
{
    // Try using the QITableEntry to avoid the extra AddRef and Release.
    if (entries) {
        for (QITableEntry* e = entries; e->iid; e++) {
            if (e->iid->Equals(iid)) {
                *ppThis = (char*) idobj + e->offset - entries[0].offset;
                *vp = OBJECT_TO_JSVAL(obj);
                *pThisRef = nullptr;
                return NS_OK;
            }
        }
    }

    nsresult rv = idobj->QueryInterface(iid, ppThis);
    *pThisRef = static_cast<nsISupports*>(*ppThis);
    if (NS_SUCCEEDED(rv))
        *vp = OBJECT_TO_JSVAL(obj);
    return rv;
}

inline nsresult
getNativeFromWrapper(JSContext *cx,
                     XPCWrappedNative *wrapper,
                     const nsIID &iid,
                     void **ppThis,
                     nsISupports **pThisRef,
                     jsval *vp)
{
    RootedObject obj(cx, wrapper->GetFlatJSObject());
    return getNative(wrapper->GetIdentityObject(), wrapper->GetOffsets(),
                     obj, iid, ppThis, pThisRef, vp);
}


nsresult
getWrapper(JSContext *cx,
           JSObject *obj,
           XPCWrappedNative **wrapper,
           JSObject **cur,
           XPCWrappedNativeTearOff **tearoff)
{
    // We can have at most three layers in need of unwrapping here:
    // * A (possible) security wrapper
    // * A (possible) Xray waiver
    // * A (possible) outer window
    //
    // If we pass stopAtOuter == false, we can handle all three with one call
    // to js::CheckedUnwrap.
    if (js::IsWrapper(obj)) {
        obj = js::CheckedUnwrap(obj, /* stopAtOuter = */ false);

        // The safe unwrap might have failed if we encountered an object that
        // we're not allowed to unwrap. If it didn't fail though, we should be
        // done with wrappers.
        if (!obj)
            return NS_ERROR_XPC_SECURITY_MANAGER_VETO;
        MOZ_ASSERT(!js::IsWrapper(obj));
    }

    // Start with sane values.
    *wrapper = nullptr;
    *cur = nullptr;
    *tearoff = nullptr;

    if (dom::IsDOMObject(obj)) {
        *cur = obj;

        return NS_OK;
    }

    // Handle tearoffs.
    //
    // If |obj| is of the tearoff class, that means we're dealing with a JS
    // object reflection of a particular interface (ie, |foo.nsIBar|). These
    // JS objects are parented to their wrapper, so we snag the tearoff object
    // along the way (if desired), and then set |obj| to its parent.
    js::Class* clasp = js::GetObjectClass(obj);
    if (clasp == &XPC_WN_Tearoff_JSClass) {
        *tearoff = (XPCWrappedNativeTearOff*) js::GetObjectPrivate(obj);
        obj = js::GetObjectParent(obj);
    }

    // If we've got a WN or slim wrapper, store things the way callers expect.
    // Otherwise, leave things null and return.
    if (IS_WRAPPER_CLASS(clasp)) {
        if (IS_WN_WRAPPER_OBJECT(obj))
            *wrapper = (XPCWrappedNative*) js::GetObjectPrivate(obj);
        else
            *cur = obj;
    }

    return NS_OK;
}

nsresult
castNative(JSContext *cx,
           XPCWrappedNative *wrapper,
           JSObject *curArg,
           XPCWrappedNativeTearOff *tearoff,
           const nsIID &iid,
           void **ppThis,
           nsISupports **pThisRef,
           jsval *vp,
           XPCLazyCallContext *lccx)
{
    RootedObject cur(cx, curArg);
    if (wrapper) {
        nsresult rv = getNativeFromWrapper(cx,wrapper, iid, ppThis, pThisRef,
                                           vp);

        if (lccx && NS_SUCCEEDED(rv))
            lccx->SetWrapper(wrapper, tearoff);

        if (rv != NS_ERROR_NO_INTERFACE)
            return rv;
    } else if (cur) {
        nsISupports *native;
        QITableEntry *entries;
        if (mozilla::dom::UnwrapDOMObjectToISupports(cur, native)) {
            entries = nullptr;
        } else if (IS_SLIM_WRAPPER(cur)) {
            native = static_cast<nsISupports*>(xpc_GetJSPrivate(cur));
            entries = GetOffsetsFromSlimWrapper(cur);
        } else {
            *pThisRef = nullptr;
            return NS_ERROR_ILLEGAL_VALUE;
        }

        if (NS_SUCCEEDED(getNative(native, entries, cur, iid, ppThis, pThisRef, vp))) {
            if (lccx) {
                // This only matters for unwrapping of this objects, so we
                // shouldn't end up here for the new DOM bindings.
                NS_ABORT_IF_FALSE(IS_SLIM_WRAPPER(cur),
                                  "what kind of wrapper is this?");
                lccx->SetWrapper(cur);
            }

            return NS_OK;
        }
    }

    *pThisRef = nullptr;
    return NS_ERROR_XPC_BAD_OP_ON_WN_PROTO;
}

JSBool
xpc_qsUnwrapThisFromCcxImpl(XPCCallContext &ccx,
                            const nsIID &iid,
                            void **ppThis,
                            nsISupports **pThisRef,
                            jsval *vp)
{
    nsISupports *native = ccx.GetIdentityObject();
    if (!native)
        return xpc_qsThrow(ccx.GetJSContext(), NS_ERROR_XPC_HAS_BEEN_SHUTDOWN);

    RootedObject obj(ccx, ccx.GetFlattenedJSObject());
    nsresult rv = getNative(native, GetOffsets(native, ccx.GetProto()),
                            obj, iid, ppThis, pThisRef, vp);
    if (NS_FAILED(rv))
        return xpc_qsThrow(ccx.GetJSContext(), rv);
    return true;
}

nsresult
xpc_qsUnwrapArgImpl(JSContext *cx,
                    jsval v,
                    const nsIID &iid,
                    void **ppArg,
                    nsISupports **ppArgRef,
                    jsval *vp)
{
    nsresult rv;
    RootedObject src(cx, xpc_qsUnwrapObj(v, ppArgRef, &rv));
    if (!src) {
        *ppArg = nullptr;

        return rv;
    }

    XPCWrappedNative *wrapper;
    XPCWrappedNativeTearOff *tearoff;
    JSObject *obj2;
    rv = getWrapper(cx, src, &wrapper, &obj2, &tearoff);
    NS_ENSURE_SUCCESS(rv, rv);

    if (wrapper || obj2) {
        if (NS_FAILED(castNative(cx, wrapper, obj2, tearoff, iid, ppArg,
                                 ppArgRef, vp, nullptr)))
            return NS_ERROR_XPC_BAD_CONVERT_JS;
        return NS_OK;
    }
    // else...
    // Slow path.

    // Try to unwrap a slim wrapper.
    nsISupports *iface;
    if (XPCConvert::GetISupportsFromJSObject(src, &iface)) {
        if (!iface || NS_FAILED(iface->QueryInterface(iid, ppArg))) {
            *ppArgRef = nullptr;
            return NS_ERROR_XPC_BAD_CONVERT_JS;
        }

        *ppArgRef = static_cast<nsISupports*>(*ppArg);
        return NS_OK;
    }

    // Create the ccx needed for quick stubs.
    XPCCallContext ccx(JS_CALLER, cx);
    if (!ccx.IsValid()) {
        *ppArgRef = nullptr;
        return NS_ERROR_XPC_BAD_CONVERT_JS;
    }

    nsRefPtr<nsXPCWrappedJS> wrappedJS;
    rv = nsXPCWrappedJS::GetNewOrUsed(ccx, src, iid, nullptr,
                                      getter_AddRefs(wrappedJS));
    if (NS_FAILED(rv) || !wrappedJS) {
        *ppArgRef = nullptr;
        return rv;
    }

    // We need to go through the QueryInterface logic to make this return
    // the right thing for the various 'special' interfaces; e.g.
    // nsIPropertyBag. We must use AggregatedQueryInterface in cases where
    // there is an outer to avoid nasty recursion.
    rv = wrappedJS->QueryInterface(iid, ppArg);
    if (NS_SUCCEEDED(rv)) {
        *ppArgRef = static_cast<nsISupports*>(*ppArg);
        *vp = OBJECT_TO_JSVAL(wrappedJS->GetJSObject());
    }
    return rv;
}

JSBool
xpc_qsJsvalToCharStr(JSContext *cx, jsval v, JSAutoByteString *bytes)
{
    JSString *str;

    MOZ_ASSERT(!bytes->ptr());
    if (JSVAL_IS_STRING(v)) {
        str = JSVAL_TO_STRING(v);
    } else if (JSVAL_IS_VOID(v) || JSVAL_IS_NULL(v)) {
        return true;
    } else {
        if (!(str = JS_ValueToString(cx, v)))
            return false;
    }
    return !!bytes->encodeLatin1(cx, str);
}

JSBool
xpc_qsJsvalToWcharStr(JSContext *cx, jsval v, jsval *pval, const PRUnichar **pstr)
{
    JSString *str;

    if (JSVAL_IS_STRING(v)) {
        str = JSVAL_TO_STRING(v);
    } else if (JSVAL_IS_VOID(v) || JSVAL_IS_NULL(v)) {
        *pstr = NULL;
        return true;
    } else {
        if (!(str = JS_ValueToString(cx, v)))
            return false;
        *pval = STRING_TO_JSVAL(str);  // Root the new string.
    }

    const jschar *chars = JS_GetStringCharsZ(cx, str);
    if (!chars)
        return false;

    *pstr = static_cast<const PRUnichar *>(chars);
    return true;
}

namespace xpc {

bool
NonVoidStringToJsval(JSContext *cx, nsAString &str, JS::Value *rval)
{
    nsStringBuffer* sharedBuffer;
    jsval jsstr = XPCStringConvert::ReadableToJSVal(cx, str, &sharedBuffer);
    if (JSVAL_IS_NULL(jsstr))
        return false;
    *rval = jsstr;
    if (sharedBuffer) {
        // The string was shared but ReadableToJSVal didn't addref it.
        // Move the ownership from str to jsstr.
        str.ForgetSharedBuffer();
    }
    return true;
}

} // namespace xpc

JSBool
xpc_qsStringToJsstring(JSContext *cx, nsString &str, JSString **rval)
{
    // From the T_DOMSTRING case in XPCConvert::NativeData2JS.
    if (str.IsVoid()) {
        *rval = nullptr;
        return true;
    }

    nsStringBuffer* sharedBuffer;
    jsval jsstr = XPCStringConvert::ReadableToJSVal(cx, str, &sharedBuffer);
    if (JSVAL_IS_NULL(jsstr))
        return false;
    *rval = JSVAL_TO_STRING(jsstr);
    if (sharedBuffer) {
        // The string was shared but ReadableToJSVal didn't addref it.
        // Move the ownership from str to jsstr.
        str.ForgetSharedBuffer();
    }
    return true;
}

JSBool
xpc_qsXPCOMObjectToJsval(XPCLazyCallContext &lccx, qsObjectHelper &aHelper,
                         const nsIID *iid, XPCNativeInterface **iface,
                         jsval *rval)
{
    NS_PRECONDITION(iface, "Who did that and why?");

    // From the T_INTERFACE case in XPCConvert::NativeData2JS.
    // This is one of the slowest things quick stubs do.

    JSContext *cx = lccx.GetJSContext();

    nsresult rv;
    if (!XPCConvert::NativeInterface2JSObject(lccx, rval, nullptr,
                                              aHelper, iid, iface,
                                              true, &rv)) {
        // I can't tell if NativeInterface2JSObject throws JS exceptions
        // or not.  This is a sloppy stab at the right semantics; the
        // method really ought to be fixed to behave consistently.
        if (!JS_IsExceptionPending(cx))
            xpc_qsThrow(cx, NS_FAILED(rv) ? rv : NS_ERROR_UNEXPECTED);
        return false;
    }

#ifdef DEBUG
    JSObject* jsobj = JSVAL_TO_OBJECT(*rval);
    if (jsobj && !js::GetObjectParent(jsobj))
        NS_ASSERTION(js::GetObjectClass(jsobj)->flags & JSCLASS_IS_GLOBAL,
                     "Why did we recreate this wrapper?");
#endif

    return true;
}

JSBool
xpc_qsVariantToJsval(XPCLazyCallContext &lccx,
                     nsIVariant *p,
                     jsval *rval)
{
    // From the T_INTERFACE case in XPCConvert::NativeData2JS.
    // Error handling is in XPCWrappedNative::CallMethod.
    if (p) {
        nsresult rv;
        JSBool ok = XPCVariant::VariantDataToJS(lccx, p, &rv, rval);
        if (!ok)
            xpc_qsThrow(lccx.GetJSContext(), rv);
        return ok;
    }
    *rval = JSVAL_NULL;
    return true;
}

#ifdef DEBUG
void
xpc_qsAssertContextOK(JSContext *cx)
{
    XPCJSContextStack* stack = XPCJSRuntime::Get()->GetJSContextStack();

    JSContext *topJSContext = stack->Peek();

    // This is what we're actually trying to assert here.
    NS_ASSERTION(cx == topJSContext, "wrong context on XPCJSContextStack!");
}

void
xpcObjectHelper::AssertGetClassInfoResult()
{
    MOZ_ASSERT(mXPCClassInfo ||
               static_cast<nsINode*>(GetCanonical())->IsDOMBinding(),
               "GetClassInfo() should only return null for new DOM bindings!");
}
#endif
