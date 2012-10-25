/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=80:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implement global service to track stack of JSContext. */

#include "xpcprivate.h"
#include "XPCWrapper.h"
#include "mozilla/Mutex.h"
#include "nsDOMJSUtils.h"
#include "nsIScriptGlobalObject.h"
#include "nsNullPrincipal.h"
#include "mozilla/dom/BindingUtils.h"

using namespace mozilla;
using mozilla::dom::DestroyProtoOrIfaceCache;

/***************************************************************************/

XPCJSContextStack::~XPCJSContextStack()
{
    if (mOwnSafeJSContext) {
        JS_DestroyContext(mOwnSafeJSContext);
        mOwnSafeJSContext = nullptr;
    }
}

JSContext*
XPCJSContextStack::Pop()
{
    MOZ_ASSERT(!mStack.IsEmpty());

    uint32_t idx = mStack.Length() - 1; // The thing we're popping

    JSContext *cx = mStack[idx].cx;

    mStack.RemoveElementAt(idx);
    if (idx == 0)
        return cx;

    --idx; // Advance to new top of the stack

    XPCJSContextInfo &e = mStack[idx];
    NS_ASSERTION(!e.suspendDepth || e.cx, "Shouldn't have suspendDepth without a cx!");
    if (e.cx) {
        if (e.suspendDepth) {
            JS_ResumeRequest(e.cx, e.suspendDepth);
            e.suspendDepth = 0;
        }

        if (e.savedFrameChain) {
            // Pop() can be called outside any request for e.cx.
            JSAutoRequest ar(e.cx);
            JS_RestoreFrameChain(e.cx);
            e.savedFrameChain = false;
        }
    }
    return cx;
}

static nsIPrincipal*
GetPrincipalFromCx(JSContext *cx)
{
    nsIScriptContextPrincipal* scp = GetScriptContextPrincipalFromJSContext(cx);
    if (scp) {
        nsIScriptObjectPrincipal* globalData = scp->GetObjectPrincipal();
        if (globalData)
            return globalData->GetPrincipal();
    }
    return nullptr;
}

bool
XPCJSContextStack::Push(JSContext *cx)
{
    if (mStack.Length() == 0) {
        mStack.AppendElement(cx);
        return true;
    }

    XPCJSContextInfo &e = mStack[mStack.Length() - 1];
    if (e.cx) {
        if (e.cx == cx) {
            nsIScriptSecurityManager* ssm = XPCWrapper::GetSecurityManager();
            if (ssm) {
                if (nsIPrincipal* globalObjectPrincipal = GetPrincipalFromCx(cx)) {
                    nsIPrincipal* subjectPrincipal = ssm->GetCxSubjectPrincipal(cx);
                    bool equals = false;
                    globalObjectPrincipal->Equals(subjectPrincipal, &equals);
                    if (equals) {
                        mStack.AppendElement(cx);
                        return true;
                    }
                }
            }
        }

        {
            // Push() can be called outside any request for e.cx.
            JSAutoRequest ar(e.cx);
            if (!JS_SaveFrameChain(e.cx))
                return false;
            e.savedFrameChain = true;
        }

        if (!cx)
            e.suspendDepth = JS_SuspendRequest(e.cx);
    }

    mStack.AppendElement(cx);
    return true;
}

#ifdef DEBUG
bool
XPCJSContextStack::DEBUG_StackHasJSContext(JSContext *cx)
{
    for (uint32_t i = 0; i < mStack.Length(); i++)
        if (cx == mStack[i].cx)
            return true;
    return false;
}
#endif

static JSBool
SafeGlobalResolve(JSContext *cx, JSHandleObject obj, JSHandleId id)
{
    JSBool resolved;
    return JS_ResolveStandardClass(cx, obj, id, &resolved);
}

static void
SafeFinalize(JSFreeOp *fop, JSObject* obj)
{
    nsIScriptObjectPrincipal* sop =
        static_cast<nsIScriptObjectPrincipal*>(xpc_GetJSPrivate(obj));
    NS_IF_RELEASE(sop);
    DestroyProtoOrIfaceCache(obj);
}

static JSClass global_class = {
    "global_for_XPCJSContextStack_SafeJSContext",
    XPCONNECT_GLOBAL_FLAGS,
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, SafeGlobalResolve, JS_ConvertStub, SafeFinalize,
    NULL, NULL, NULL, NULL, TraceXPCGlobal
};

// We just use the same reporter as the component loader
// XXX #include angels cry.
extern void
mozJSLoaderErrorReporter(JSContext *cx, const char *message, JSErrorReport *rep);

JSContext*
XPCJSContextStack::GetSafeJSContext()
{
    if (mSafeJSContext)
        return mSafeJSContext;

    // Start by getting the principal holder and principal for this
    // context.  If we can't manage that, don't bother with the rest.
    nsRefPtr<nsNullPrincipal> principal = new nsNullPrincipal();
    nsresult rv = principal->Init();
    if (NS_FAILED(rv))
        return NULL;

    nsCOMPtr<nsIScriptObjectPrincipal> sop = new PrincipalHolder(principal);

    nsRefPtr<nsXPConnect> xpc = nsXPConnect::GetXPConnect();
    if (!xpc)
        return NULL;

    XPCJSRuntime* xpcrt = xpc->GetRuntime();
    if (!xpcrt)
        return NULL;

    JSRuntime *rt = xpcrt->GetJSRuntime();
    if (!rt)
        return NULL;

    mSafeJSContext = JS_NewContext(rt, 8192);
    if (!mSafeJSContext)
        return NULL;

    JSObject *glob;
    {
        // scoped JS Request
        JSAutoRequest req(mSafeJSContext);

        JS_SetErrorReporter(mSafeJSContext, mozJSLoaderErrorReporter);

        JSCompartment *compartment;
        nsresult rv = xpc_CreateGlobalObject(mSafeJSContext, &global_class,
                                             principal, principal, false,
                                             &glob, &compartment);
        if (NS_FAILED(rv))
            glob = nullptr;

        if (glob) {
            // Make sure the context is associated with a proper compartment
            // and not the default compartment.
            JS_SetGlobalObject(mSafeJSContext, glob);

            // Note: make sure to set the private before calling
            // InitClasses
            nsIScriptObjectPrincipal* priv = nullptr;
            sop.swap(priv);
            JS_SetPrivate(glob, priv);
        }

        // After this point either glob is null and the
        // nsIScriptObjectPrincipal ownership is either handled by the
        // nsCOMPtr or dealt with, or we'll release in the finalize
        // hook.
        if (glob && NS_FAILED(xpc->InitClasses(mSafeJSContext, glob))) {
            glob = nullptr;
        }
    }
    if (mSafeJSContext && !glob) {
        // Destroy the context outside the scope of JSAutoRequest that
        // uses the context in its destructor.
        JS_DestroyContext(mSafeJSContext);
        mSafeJSContext = nullptr;
    }

    // Save it off so we can destroy it later.
    mOwnSafeJSContext = mSafeJSContext;

    return mSafeJSContext;
}

/***************************************************************************/

NS_IMPL_ISUPPORTS1(nsXPCJSContextStackIterator, nsIJSContextStackIterator)

NS_IMETHODIMP
nsXPCJSContextStackIterator::Reset(nsIJSContextStack *aStack)
{
    NS_ASSERTION(aStack == nsXPConnect::GetXPConnect(),
                 "aStack must be implemented by XPConnect singleton");
    mStack = XPCJSRuntime::Get()->GetJSContextStack()->GetStack();
    if (mStack->IsEmpty())
        mStack = nullptr;
    else
        mPosition = mStack->Length() - 1;

    return NS_OK;
}

NS_IMETHODIMP
nsXPCJSContextStackIterator::Done(bool *aDone)
{
    *aDone = !mStack;
    return NS_OK;
}

NS_IMETHODIMP
nsXPCJSContextStackIterator::Prev(JSContext **aContext)
{
    if (!mStack)
        return NS_ERROR_NOT_INITIALIZED;

    *aContext = mStack->ElementAt(mPosition).cx;

    if (mPosition == 0)
        mStack = nullptr;
    else
        --mPosition;

    return NS_OK;
}

