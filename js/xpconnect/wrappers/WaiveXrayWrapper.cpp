/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsJSPrincipals.h"

#include "XPCWrapper.h"

#include "WaiveXrayWrapper.h"
#include "AccessCheck.h"
#include "WrapperFactory.h"

namespace xpc {

WaiveXrayWrapper::WaiveXrayWrapper(unsigned flags) : js::CrossCompartmentWrapper(flags)
{
}

WaiveXrayWrapper::~WaiveXrayWrapper()
{
}

bool
WaiveXrayWrapper::getPropertyDescriptor(JSContext *cx, JSObject *wrapper, jsid id,
                                          bool set, js::PropertyDescriptor *desc)
{
    return CrossCompartmentWrapper::getPropertyDescriptor(cx, wrapper, id, set, desc) &&
           WrapperFactory::WaiveXrayAndWrap(cx, &desc->value);
}

bool
WaiveXrayWrapper::getOwnPropertyDescriptor(JSContext *cx, JSObject *wrapper, jsid id,
                                             bool set, js::PropertyDescriptor *desc)
{
    return CrossCompartmentWrapper::getOwnPropertyDescriptor(cx, wrapper, id, set, desc) &&
           WrapperFactory::WaiveXrayAndWrap(cx, &desc->value);
}

bool
WaiveXrayWrapper::get(JSContext *cx, JSObject *wrapper, JSObject *receiver, jsid id,
                        js::Value *vp)
{
    return CrossCompartmentWrapper::get(cx, wrapper, receiver, id, vp) &&
           WrapperFactory::WaiveXrayAndWrap(cx, vp);
}

bool
WaiveXrayWrapper::call(JSContext *cx, JSObject *wrapper, unsigned argc, js::Value *vp)
{
    return CrossCompartmentWrapper::call(cx, wrapper, argc, vp) &&
           WrapperFactory::WaiveXrayAndWrap(cx, vp);
}

bool
WaiveXrayWrapper::construct(JSContext *cx, JSObject *wrapper,
                              unsigned argc, js::Value *argv, js::Value *rval)
{
    return CrossCompartmentWrapper::construct(cx, wrapper, argc, argv, rval) &&
           WrapperFactory::WaiveXrayAndWrap(cx, rval);
}

}
