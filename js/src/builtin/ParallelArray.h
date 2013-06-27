/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ParallelArray_h__
#define ParallelArray_h__

#include "jsapi.h"
#include "jscntxt.h"
#include "jsobj.h"

#include "ion/Ion.h"
#include "vm/ForkJoin.h"
#include "vm/ThreadPool.h"

namespace js {

class ParallelArrayObject : public JSObject
{
    static Class protoClass;
    static JSFunctionSpec methods[];
    static const uint32_t NumFixedSlots = 4;
    static const uint32_t NumCtors = 4;
    static FixedHeapPtr<PropertyName> ctorNames[NumCtors];

    static bool initProps(JSContext *cx, HandleObject obj);

  public:
    static Class class_;

    static JSBool construct(JSContext *cx, unsigned argc, Value *vp);
    static JSBool constructHelper(JSContext *cx, MutableHandleFunction ctor, CallArgs &args);

    // Creates a new ParallelArray instance with the correct number of slots
    // and so forth.
    //
    // NOTE: This object will NOT have the correct type object! It is
    // up to the caller to adjust the type object appropriately
    // before releasing the object into the wild.  You probably want
    // to be calling construct() above, which will adjust the type
    // object for you, since ParallelArray type objects must be setup
    // in a rather particular way to interact well with the
    // self-hosted code.  See constructHelper() for details.
    static JSObject *newInstance(JSContext *cx);

    // Get the constructor function for argc number of arguments.
    static JSFunction *getConstructor(JSContext *cx, unsigned argc);

    static JSObject *initClass(JSContext *cx, HandleObject obj);
    static bool is(const Value &v);
};

} // namespace js

extern JSObject *
js_InitParallelArrayClass(JSContext *cx, js::HandleObject obj);

#endif // ParallelArray_h__
