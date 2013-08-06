/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 sw=2 et tw=78: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "XPCWrapper.h"
#include "AccessCheck.h"
#include "WrapperFactory.h"
#include "AccessCheck.h"

using namespace xpc;
namespace XPCNativeWrapper {

static inline
JSBool
ThrowException(nsresult ex, JSContext *cx)
{
  XPCThrower::Throw(ex, cx);

  return false;
}

static JSBool
UnwrapNW(JSContext *cx, unsigned argc, jsval *vp)
{
  if (argc != 1) {
    return ThrowException(NS_ERROR_XPC_NOT_ENOUGH_ARGS, cx);
  }

  JS::RootedValue v(cx, JS_ARGV(cx, vp)[0]);
  if (!v.isObject() || !js::IsWrapper(&v.toObject())) {
    JS_SET_RVAL(cx, vp, v);
    return true;
  }

  if (AccessCheck::wrapperSubsumes(&v.toObject())) {
    bool ok = xpc::WrapperFactory::WaiveXrayAndWrap(cx, v.address());
    NS_ENSURE_TRUE(ok, false);
  }

  JS_SET_RVAL(cx, vp, v);
  return true;
}

static JSBool
XrayWrapperConstructor(JSContext *cx, unsigned argc, jsval *vp)
{
  if (argc == 0) {
    return ThrowException(NS_ERROR_XPC_NOT_ENOUGH_ARGS, cx);
  }

  JS::RootedValue v(cx, JS_ARGV(cx, vp)[0]);
  if (!v.isObject()) {
    JS_SET_RVAL(cx, vp, v);
    return true;
  }

  *vp = JS::ObjectValue(*js::UncheckedUnwrap(&v.toObject()));
  return JS_WrapValue(cx, vp);
}
// static
bool
AttachNewConstructorObject(XPCCallContext &ccx, JSObject *aGlobalObject)
{
  JSFunction *xpcnativewrapper =
    JS_DefineFunction(ccx, aGlobalObject, "XPCNativeWrapper",
                      XrayWrapperConstructor, 1,
                      JSPROP_READONLY | JSPROP_PERMANENT | JSFUN_STUB_GSOPS | JSFUN_CONSTRUCTOR);
  if (!xpcnativewrapper) {
    return false;
  }
  return JS_DefineFunction(ccx, JS_GetFunctionObject(xpcnativewrapper), "unwrap", UnwrapNW, 1,
                           JSPROP_READONLY | JSPROP_PERMANENT) != nullptr;
}

} // namespace XPCNativeWrapper

namespace XPCWrapper {

JSObject *
UnsafeUnwrapSecurityWrapper(JSObject *obj)
{
  if (js::IsProxy(obj)) {
    return js::UncheckedUnwrap(obj);
  }

  return obj;
}

} // namespace XPCWrapper
