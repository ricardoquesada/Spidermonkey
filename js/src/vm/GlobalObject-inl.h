/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GlobalObject_inl_h___
#define GlobalObject_inl_h___

namespace js {

inline void
GlobalObject::setFlags(int32_t flags)
{
    setSlot(FLAGS, Int32Value(flags));
}

inline void
GlobalObject::initFlags(int32_t flags)
{
    initSlot(FLAGS, Int32Value(flags));
}

inline void
GlobalObject::setDetailsForKey(JSProtoKey key, JSObject *ctor, JSObject *proto)
{
    JS_ASSERT(getSlotRef(key).isUndefined());
    JS_ASSERT(getSlotRef(JSProto_LIMIT + key).isUndefined());
    JS_ASSERT(getSlotRef(2 * JSProto_LIMIT + key).isUndefined());
    setSlot(key, ObjectValue(*ctor));
    setSlot(JSProto_LIMIT + key, ObjectValue(*proto));
    setSlot(2 * JSProto_LIMIT + key, ObjectValue(*ctor));
}

inline void
GlobalObject::setObjectClassDetails(JSFunction *ctor, JSObject *proto)
{
    setDetailsForKey(JSProto_Object, ctor, proto);
}

inline void
GlobalObject::setFunctionClassDetails(JSFunction *ctor, JSObject *proto)
{
    setDetailsForKey(JSProto_Function, ctor, proto);
}

void
GlobalObject::setThrowTypeError(JSFunction *fun)
{
    JS_ASSERT(getSlotRef(THROWTYPEERROR).isUndefined());
    setSlot(THROWTYPEERROR, ObjectValue(*fun));
}

void
GlobalObject::setOriginalEval(JSObject *evalobj)
{
    JS_ASSERT(getSlotRef(EVAL).isUndefined());
    setSlot(EVAL, ObjectValue(*evalobj));
}

void
GlobalObject::setCreateArrayFromBufferHelper(uint32_t slot, Handle<JSFunction*> fun)
{
    JS_ASSERT(getSlotRef(slot).isUndefined());
    setSlot(slot, ObjectValue(*fun));
}

void
GlobalObject::setBooleanValueOf(Handle<JSFunction*> valueOfFun)
{
    JS_ASSERT(getSlotRef(BOOLEAN_VALUEOF).isUndefined());
    setSlot(BOOLEAN_VALUEOF, ObjectValue(*valueOfFun));
}

void
GlobalObject::setCreateDataViewForThis(Handle<JSFunction*> fun)
{
    JS_ASSERT(getSlotRef(CREATE_DATAVIEW_FOR_THIS).isUndefined());
    setSlot(CREATE_DATAVIEW_FOR_THIS, ObjectValue(*fun));
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<uint8_t>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_UINT8, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<int8_t>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_INT8, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<uint16_t>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_UINT16, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<int16_t>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_INT16, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<uint32_t>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_UINT32, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<int32_t>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_INT32, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<float>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_FLOAT32, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<double>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_FLOAT64, fun);
}

template<>
inline void
GlobalObject::setCreateArrayFromBuffer<uint8_clamped>(Handle<JSFunction*> fun)
{
    setCreateArrayFromBufferHelper(FROM_BUFFER_UINT8CLAMPED, fun);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<uint8_t>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_UINT8);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<int8_t>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_INT8);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<uint16_t>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_UINT16);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<int16_t>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_INT16);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<uint32_t>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_UINT32);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<int32_t>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_INT32);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<float>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_FLOAT32);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<double>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_FLOAT64);
}

template<>
inline Value
GlobalObject::createArrayFromBuffer<uint8_clamped>() const
{
    return createArrayFromBufferHelper(FROM_BUFFER_UINT8CLAMPED);
}

void
GlobalObject::setProtoGetter(JSFunction *protoGetter)
{
    JS_ASSERT(getSlotRef(PROTO_GETTER).isUndefined());
    setSlot(PROTO_GETTER, ObjectValue(*protoGetter));
}

void
GlobalObject::setIntrinsicsHolder(JSObject *obj)
{
    JS_ASSERT(getSlotRef(INTRINSICS).isUndefined());
    setSlot(INTRINSICS, ObjectValue(*obj));
}

} // namespace js

#endif
