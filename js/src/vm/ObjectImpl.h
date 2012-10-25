/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ObjectImpl_h___
#define ObjectImpl_h___

#include "mozilla/Assertions.h"
#include "mozilla/StandardInteger.h"

#include "jsfriendapi.h"
#include "jsinfer.h"
#include "jsval.h"

#include "gc/Barrier.h"
#include "vm/NumericConversions.h"
#include "vm/String.h"

namespace js {

class Debugger;
class ObjectImpl;

class AutoPropDescArrayRooter;

static inline PropertyOp
CastAsPropertyOp(JSObject *object)
{
    return JS_DATA_TO_FUNC_PTR(PropertyOp, object);
}

static inline StrictPropertyOp
CastAsStrictPropertyOp(JSObject *object)
{
    return JS_DATA_TO_FUNC_PTR(StrictPropertyOp, object);
}

/*
 * Properties are stored differently depending on the type of the key.  If the
 * key is an unsigned 32-bit integer (i.e. an index), we call such properties
 * "elements" and store them in one of a number of forms (optimized for dense
 * property storage, typed array data, and so on).  All other properties are
 * stored using shapes and shape trees.  Keys for these properties are either
 * PropertyNames (that is, atomized strings whose contents are not unsigned
 * 32-bit integers) or SpecialIds (object values for E4X and a couple other
 * things, see jsid for details); the union of these types, used in individual
 * shapes, is PropertyId.
 */
class PropertyId
{
    jsid id;

  public:
    bool isName() const {
        MOZ_ASSERT(JSID_IS_STRING(id) || JSID_IS_SPECIAL(id));
        return JSID_IS_STRING(id);
    }
    bool isSpecial() const {
        MOZ_ASSERT(JSID_IS_STRING(id) || JSID_IS_SPECIAL(id));
        return !isName();
    }

    PropertyId() {
        *this = PropertyId(SpecialId());
    }
    explicit PropertyId(PropertyName *name)
      : id(NON_INTEGER_ATOM_TO_JSID(name))
    { }
    explicit PropertyId(const SpecialId &sid)
      : id(SPECIALID_TO_JSID(sid))
    { }

    PropertyName * asName() const {
        return JSID_TO_STRING(id)->asAtom().asPropertyName();
    }
    SpecialId asSpecial() const {
        return JSID_TO_SPECIALID(id);
    }
    const jsid &asId() const {
        return id;
    }
    jsid &asId() {
        return id;
    }

    bool operator==(const PropertyId &rhs) const { return id == rhs.id; }
    bool operator!=(const PropertyId &rhs) const { return id != rhs.id; }
};

/*
 * A representation of ECMA-262 ed. 5's internal Property Descriptor data
 * structure.
 */
struct PropDesc {
  private:
    /*
     * Original object from which this descriptor derives, passed through for
     * the benefit of proxies.  FIXME: Remove this when direct proxies happen.
     */
    Value pd_;

    Value value_, get_, set_;

    /* Property descriptor boolean fields. */
    uint8_t attrs;

    /* Bits indicating which values are set. */
    bool hasGet_ : 1;
    bool hasSet_ : 1;
    bool hasValue_ : 1;
    bool hasWritable_ : 1;
    bool hasEnumerable_ : 1;
    bool hasConfigurable_ : 1;

    /* Or maybe this represents a property's absence, and it's undefined. */
    bool isUndefined_ : 1;

    PropDesc(const Value &v)
      : pd_(UndefinedValue()),
        value_(v),
        get_(UndefinedValue()), set_(UndefinedValue()),
        attrs(0),
        hasGet_(false), hasSet_(false),
        hasValue_(true), hasWritable_(false), hasEnumerable_(false), hasConfigurable_(false),
        isUndefined_(false)
    {
    }

  public:
    friend class AutoPropDescArrayRooter;
    friend void JS::AutoGCRooter::trace(JSTracer *trc);

    enum Enumerability { Enumerable = true, NonEnumerable = false };
    enum Configurability { Configurable = true, NonConfigurable = false };
    enum Writability { Writable = true, NonWritable = false };

    PropDesc();

    static PropDesc undefined() { return PropDesc(); }
    static PropDesc valueOnly(const Value &v) { return PropDesc(v); }

    PropDesc(const Value &v, Writability writable,
             Enumerability enumerable, Configurability configurable)
      : pd_(UndefinedValue()),
        value_(v),
        get_(UndefinedValue()), set_(UndefinedValue()),
        attrs((writable ? 0 : JSPROP_READONLY) |
              (enumerable ? JSPROP_ENUMERATE : 0) |
              (configurable ? 0 : JSPROP_PERMANENT)),
        hasGet_(false), hasSet_(false),
        hasValue_(true), hasWritable_(true), hasEnumerable_(true), hasConfigurable_(true),
        isUndefined_(false)
    {}

    inline PropDesc(const Value &getter, const Value &setter,
                    Enumerability enumerable, Configurability configurable);

    /*
     * 8.10.5 ToPropertyDescriptor(Obj)
     *
     * If checkAccessors is false, skip steps 7.b and 8.b, which throw a
     * TypeError if .get or .set is neither a callable object nor undefined.
     *
     * (DebuggerObject_defineProperty uses this: the .get and .set properties
     * are expected to be Debugger.Object wrappers of functions, which are not
     * themselves callable.)
     */
    bool initialize(JSContext* cx, const Value &v, bool checkAccessors = true);

    /*
     * 8.10.4 FromPropertyDescriptor(Desc)
     *
     * initFromPropertyDescriptor sets pd to undefined and populates all the
     * other fields of this PropDesc from desc.
     *
     * makeObject populates pd based on the other fields of *this, creating a
     * new property descriptor JSObject and defining properties on it.
     */
    void initFromPropertyDescriptor(const PropertyDescriptor &desc);
    bool makeObject(JSContext *cx);

    void setUndefined() { isUndefined_ = true; }

    bool isUndefined() const { return isUndefined_; }

    bool hasGet() const { MOZ_ASSERT(!isUndefined()); return hasGet_; }
    bool hasSet() const { MOZ_ASSERT(!isUndefined()); return hasSet_; }
    bool hasValue() const { MOZ_ASSERT(!isUndefined()); return hasValue_; }
    bool hasWritable() const { MOZ_ASSERT(!isUndefined()); return hasWritable_; }
    bool hasEnumerable() const { MOZ_ASSERT(!isUndefined()); return hasEnumerable_; }
    bool hasConfigurable() const { MOZ_ASSERT(!isUndefined()); return hasConfigurable_; }

    Value pd() const { MOZ_ASSERT(!isUndefined()); return pd_; }
    void clearPd() { pd_ = UndefinedValue(); }

    uint8_t attributes() const { MOZ_ASSERT(!isUndefined()); return attrs; }

    /* 8.10.1 IsAccessorDescriptor(desc) */
    bool isAccessorDescriptor() const {
        return !isUndefined() && (hasGet() || hasSet());
    }

    /* 8.10.2 IsDataDescriptor(desc) */
    bool isDataDescriptor() const {
        return !isUndefined() && (hasValue() || hasWritable());
    }

    /* 8.10.3 IsGenericDescriptor(desc) */
    bool isGenericDescriptor() const {
        return !isUndefined() && !isAccessorDescriptor() && !isDataDescriptor();
    }

    bool configurable() const {
        MOZ_ASSERT(!isUndefined());
        MOZ_ASSERT(hasConfigurable());
        return (attrs & JSPROP_PERMANENT) == 0;
    }

    bool enumerable() const {
        MOZ_ASSERT(!isUndefined());
        MOZ_ASSERT(hasEnumerable());
        return (attrs & JSPROP_ENUMERATE) != 0;
    }

    bool writable() const {
        MOZ_ASSERT(!isUndefined());
        MOZ_ASSERT(hasWritable());
        return (attrs & JSPROP_READONLY) == 0;
    }

    HandleValue value() const {
        MOZ_ASSERT(hasValue());
        return HandleValue::fromMarkedLocation(&value_);
    }

    JSObject * getterObject() const {
        MOZ_ASSERT(!isUndefined());
        MOZ_ASSERT(hasGet());
        return get_.isUndefined() ? NULL : &get_.toObject();
    }
    JSObject * setterObject() const {
        MOZ_ASSERT(!isUndefined());
        MOZ_ASSERT(hasSet());
        return set_.isUndefined() ? NULL : &set_.toObject();
    }

    HandleValue getterValue() const {
        MOZ_ASSERT(!isUndefined());
        MOZ_ASSERT(hasGet());
        return HandleValue::fromMarkedLocation(&get_);
    }
    HandleValue setterValue() const {
        MOZ_ASSERT(!isUndefined());
        MOZ_ASSERT(hasSet());
        return HandleValue::fromMarkedLocation(&set_);
    }

    /*
     * Unfortunately the values produced by these methods are used such that
     * we can't assert anything here.  :-(
     */
    PropertyOp getter() const {
        return CastAsPropertyOp(get_.isUndefined() ? NULL : &get_.toObject());
    }
    StrictPropertyOp setter() const {
        return CastAsStrictPropertyOp(set_.isUndefined() ? NULL : &set_.toObject());
    }

    /*
     * Throw a TypeError if a getter/setter is present and is neither callable
     * nor undefined. These methods do exactly the type checks that are skipped
     * by passing false as the checkAccessors parameter of initialize.
     */
    bool checkGetter(JSContext *cx);
    bool checkSetter(JSContext *cx);

    bool unwrapDebuggerObjectsInto(JSContext *cx, Debugger *dbg, JSObject *obj,
                                   PropDesc *unwrapped) const;

    bool wrapInto(JSContext *cx, JSObject *obj, const jsid &id, jsid *wrappedId,
                  PropDesc *wrappedDesc) const;

    class AutoRooter : private AutoGCRooter
    {
      public:
        explicit AutoRooter(JSContext *cx, PropDesc *pd_
                            JS_GUARD_OBJECT_NOTIFIER_PARAM)
          : AutoGCRooter(cx, PROPDESC), pd(pd_), skip(cx, pd_)
        {
            JS_GUARD_OBJECT_NOTIFIER_INIT;
        }

        friend void AutoGCRooter::trace(JSTracer *trc);

      private:
        PropDesc *pd;
        SkipRoot skip;
        JS_DECL_USE_GUARD_OBJECT_NOTIFIER
     };
};

class DenseElementsHeader;
class SparseElementsHeader;
class Uint8ElementsHeader;
class Int8ElementsHeader;
class Uint16ElementsHeader;
class Int16ElementsHeader;
class Uint32ElementsHeader;
class Int32ElementsHeader;
class Uint8ClampedElementsHeader;
class Float32ElementsHeader;
class Float64ElementsHeader;
class Uint8ClampedElementsHeader;
class ArrayBufferElementsHeader;

enum ElementsKind {
    DenseElements,
    SparseElements,

    ArrayBufferElements,

    /* These typed element types must remain contiguous. */
    Uint8Elements,
    Int8Elements,
    Uint16Elements,
    Int16Elements,
    Uint32Elements,
    Int32Elements,
    Uint8ClampedElements,
    Float32Elements,
    Float64Elements
};

class ElementsHeader
{
  protected:
    uint32_t type;
    uint32_t length; /* Array length, ArrayBuffer length, typed array length */

    union {
        class {
            friend class DenseElementsHeader;
            uint32_t initializedLength;
            uint32_t capacity;
        } dense;
        class {
            friend class SparseElementsHeader;
            Shape * shape;
        } sparse;
    };

    void staticAsserts() {
        MOZ_STATIC_ASSERT(sizeof(ElementsHeader) == ValuesPerHeader * sizeof(Value),
                          "Elements size and values-per-Elements mismatch");
    }

  public:
    ElementsKind kind() const {
        MOZ_ASSERT(type <= ArrayBufferElements);
        return ElementsKind(type);
    }

    inline bool isDenseElements() const { return kind() == DenseElements; }
    inline bool isSparseElements() const { return kind() == SparseElements; }
    inline bool isArrayBufferElements() const { return kind() == ArrayBufferElements; }
    inline bool isUint8Elements() const { return kind() == Uint8Elements; }
    inline bool isInt8Elements() const { return kind() == Int8Elements; }
    inline bool isUint16Elements() const { return kind() == Uint16Elements; }
    inline bool isInt16Elements() const { return kind() == Int16Elements; }
    inline bool isUint32Elements() const { return kind() == Uint32Elements; }
    inline bool isInt32Elements() const { return kind() == Int32Elements; }
    inline bool isUint8ClampedElements() const { return kind() == Uint8ClampedElements; }
    inline bool isFloat32Elements() const { return kind() == Float32Elements; }
    inline bool isFloat64Elements() const { return kind() == Float64Elements; }

    inline DenseElementsHeader & asDenseElements();
    inline SparseElementsHeader & asSparseElements();
    inline ArrayBufferElementsHeader & asArrayBufferElements();
    inline Uint8ElementsHeader & asUint8Elements();
    inline Int8ElementsHeader & asInt8Elements();
    inline Uint16ElementsHeader & asUint16Elements();
    inline Int16ElementsHeader & asInt16Elements();
    inline Uint32ElementsHeader & asUint32Elements();
    inline Int32ElementsHeader & asInt32Elements();
    inline Uint8ClampedElementsHeader & asUint8ClampedElements();
    inline Float32ElementsHeader & asFloat32Elements();
    inline Float64ElementsHeader & asFloat64Elements();

    static ElementsHeader * fromElements(HeapSlot *elems) {
        return reinterpret_cast<ElementsHeader *>(uintptr_t(elems) - sizeof(ElementsHeader));
    }

    static const size_t ValuesPerHeader = 2;
};

class DenseElementsHeader : public ElementsHeader
{
  public:
    uint32_t capacity() const {
        MOZ_ASSERT(ElementsHeader::isDenseElements());
        return dense.capacity;
    }

    uint32_t initializedLength() const {
        MOZ_ASSERT(ElementsHeader::isDenseElements());
        return dense.initializedLength;
    }

    uint32_t length() const {
        MOZ_ASSERT(ElementsHeader::isDenseElements());
        return ElementsHeader::length;
    }

    bool getOwnElement(JSContext *cx, Handle<ObjectImpl*> obj, uint32_t index,
                       unsigned resolveFlags, PropDesc *desc);

    bool defineElement(JSContext *cx, Handle<ObjectImpl*> obj, uint32_t index,
                       const PropDesc &desc, bool shouldThrow, unsigned resolveFlags,
                       bool *succeeded);

    bool setElement(JSContext *cx, Handle<ObjectImpl*> obj, Handle<ObjectImpl*> receiver,
                    uint32_t index, const Value &v, unsigned resolveFlags, bool *succeeded);

  private:
    inline bool isDenseElements() const MOZ_DELETE;
    inline DenseElementsHeader & asDenseElements() MOZ_DELETE;

    DenseElementsHeader(const DenseElementsHeader &other) MOZ_DELETE;
    void operator=(const DenseElementsHeader &other) MOZ_DELETE;
};

class SparseElementsHeader : public ElementsHeader
{
  public:
    Shape * shape() {
        MOZ_ASSERT(ElementsHeader::isSparseElements());
        return sparse.shape;
    }

    uint32_t length() const {
        MOZ_ASSERT(ElementsHeader::isSparseElements());
        return ElementsHeader::length;
    }

    bool getOwnElement(JSContext *cx, Handle<ObjectImpl*> obj, uint32_t index,
                       unsigned resolveFlags, PropDesc *desc);

    bool defineElement(JSContext *cx, Handle<ObjectImpl*> obj, uint32_t index,
                       const PropDesc &desc, bool shouldThrow, unsigned resolveFlags,
                       bool *succeeded);

    bool setElement(JSContext *cx, Handle<ObjectImpl*> obj, Handle<ObjectImpl*> receiver,
                    uint32_t index, const Value &v, unsigned resolveFlags, bool *succeeded);

  private:
    inline bool isSparseElements() const MOZ_DELETE;
    inline SparseElementsHeader & asSparseElements() MOZ_DELETE;

    SparseElementsHeader(const SparseElementsHeader &other) MOZ_DELETE;
    void operator=(const SparseElementsHeader &other) MOZ_DELETE;
};

extern uint32_t JS_FASTCALL
ClampDoubleToUint8(const double x);

struct uint8_clamped {
    uint8_t val;

    uint8_clamped() { }
    uint8_clamped(const uint8_clamped& other) : val(other.val) { }

    // invoke our assignment helpers for constructor conversion
    uint8_clamped(uint8_t x)    { *this = x; }
    uint8_clamped(uint16_t x)   { *this = x; }
    uint8_clamped(uint32_t x)   { *this = x; }
    uint8_clamped(int8_t x)     { *this = x; }
    uint8_clamped(int16_t x)    { *this = x; }
    uint8_clamped(int32_t x)    { *this = x; }
    uint8_clamped(double x)     { *this = x; }

    uint8_clamped& operator=(const uint8_clamped& x) {
        val = x.val;
        return *this;
    }

    uint8_clamped& operator=(uint8_t x) {
        val = x;
        return *this;
    }

    uint8_clamped& operator=(uint16_t x) {
        val = (x > 255) ? 255 : uint8_t(x);
        return *this;
    }

    uint8_clamped& operator=(uint32_t x) {
        val = (x > 255) ? 255 : uint8_t(x);
        return *this;
    }

    uint8_clamped& operator=(int8_t x) {
        val = (x >= 0) ? uint8_t(x) : 0;
        return *this;
    }

    uint8_clamped& operator=(int16_t x) {
        val = (x >= 0)
              ? ((x < 255)
                 ? uint8_t(x)
                 : 255)
              : 0;
        return *this;
    }

    uint8_clamped& operator=(int32_t x) {
        val = (x >= 0)
              ? ((x < 255)
                 ? uint8_t(x)
                 : 255)
              : 0;
        return *this;
    }

    uint8_clamped& operator=(const double x) {
        val = uint8_t(ClampDoubleToUint8(x));
        return *this;
    }

    operator uint8_t() const {
        return val;
    }

    void staticAsserts() {
        MOZ_STATIC_ASSERT(sizeof(uint8_clamped) == 1,
                          "uint8_clamped must be layout-compatible with uint8_t");
    }
};

/* Note that we can't use std::numeric_limits here due to uint8_clamped. */
template<typename T> static inline const bool TypeIsFloatingPoint() { return false; }
template<> inline const bool TypeIsFloatingPoint<float>() { return true; }
template<> inline const bool TypeIsFloatingPoint<double>() { return true; }

template<typename T> static inline const bool TypeIsUnsigned() { return false; }
template<> inline const bool TypeIsUnsigned<uint8_t>() { return true; }
template<> inline const bool TypeIsUnsigned<uint16_t>() { return true; }
template<> inline const bool TypeIsUnsigned<uint32_t>() { return true; }

template <typename T>
class TypedElementsHeader : public ElementsHeader
{
    T getElement(uint32_t index) {
        MOZ_ASSERT(index < length());
        return reinterpret_cast<T *>(this + 1)[index];
    }

    inline void assign(uint32_t index, double d);

    void setElement(uint32_t index, T value) {
        MOZ_ASSERT(index < length());
        reinterpret_cast<T *>(this + 1)[index] = value;
    }

  public:
    uint32_t length() const {
        MOZ_ASSERT(Uint8Elements <= kind());
        MOZ_ASSERT(kind() <= Float64Elements);
        return ElementsHeader::length;
    }

    bool getOwnElement(JSContext *cx, Handle<ObjectImpl*> obj, uint32_t index,
                       unsigned resolveFlags, PropDesc *desc);

    bool defineElement(JSContext *cx, Handle<ObjectImpl*> obj, uint32_t index,
                       const PropDesc &desc, bool shouldThrow, unsigned resolveFlags,
                       bool *succeeded);

    bool setElement(JSContext *cx, Handle<ObjectImpl*> obj, Handle<ObjectImpl*> receiver,
                    uint32_t index, const Value &v, unsigned resolveFlags, bool *succeeded);

  private:
    TypedElementsHeader(const TypedElementsHeader &other) MOZ_DELETE;
    void operator=(const TypedElementsHeader &other) MOZ_DELETE;
};

template<typename T> inline void
TypedElementsHeader<T>::assign(uint32_t index, double d)
{
    MOZ_NOT_REACHED("didn't specialize for this element type");
}

template<> inline void
TypedElementsHeader<uint8_clamped>::assign(uint32_t index, double d)
{
    double i = ToInteger(d);
    uint8_t u = (i <= 0)
                ? 0
                : (i >= 255)
                ? 255
                : uint8_t(i);
    setElement(index, uint8_clamped(u));
}

template<> inline void
TypedElementsHeader<uint8_t>::assign(uint32_t index, double d)
{
    setElement(index, uint8_t(ToUint32(d)));
}

template<> inline void
TypedElementsHeader<int8_t>::assign(uint32_t index, double d)
{
    /* FIXME: Casting out-of-range signed integers has undefined behavior! */
    setElement(index, int8_t(ToInt32(d)));
}

template<> inline void
TypedElementsHeader<uint16_t>::assign(uint32_t index, double d)
{
    setElement(index, uint16_t(ToUint32(d)));
}

template<> inline void
TypedElementsHeader<int16_t>::assign(uint32_t index, double d)
{
    /* FIXME: Casting out-of-range signed integers has undefined behavior! */
    setElement(index, int16_t(ToInt32(d)));
}

template<> inline void
TypedElementsHeader<uint32_t>::assign(uint32_t index, double d)
{
    setElement(index, ToUint32(d));
}

template<> inline void
TypedElementsHeader<int32_t>::assign(uint32_t index, double d)
{
    /* FIXME: Casting out-of-range signed integers has undefined behavior! */
    setElement(index, int32_t(ToInt32(d)));
}

template<> inline void
TypedElementsHeader<float>::assign(uint32_t index, double d)
{
    setElement(index, float(d));
}

template<> inline void
TypedElementsHeader<double>::assign(uint32_t index, double d)
{
    setElement(index, d);
}

class Uint8ElementsHeader : public TypedElementsHeader<uint8_t>
{
  private:
    inline bool isUint8Elements() const MOZ_DELETE;
    inline Uint8ElementsHeader & asUint8Elements() MOZ_DELETE;
    Uint8ElementsHeader(const Uint8ElementsHeader &other) MOZ_DELETE;
    void operator=(const Uint8ElementsHeader &other) MOZ_DELETE;
};
class Int8ElementsHeader : public TypedElementsHeader<int8_t>
{
  private:
    bool isInt8Elements() const MOZ_DELETE;
    Int8ElementsHeader & asInt8Elements() MOZ_DELETE;
    Int8ElementsHeader(const Int8ElementsHeader &other) MOZ_DELETE;
    void operator=(const Int8ElementsHeader &other) MOZ_DELETE;
};
class Uint16ElementsHeader : public TypedElementsHeader<uint16_t>
{
  private:
    bool isUint16Elements() const MOZ_DELETE;
    Uint16ElementsHeader & asUint16Elements() MOZ_DELETE;
    Uint16ElementsHeader(const Uint16ElementsHeader &other) MOZ_DELETE;
    void operator=(const Uint16ElementsHeader &other) MOZ_DELETE;
};
class Int16ElementsHeader : public TypedElementsHeader<int16_t>
{
  private:
    bool isInt16Elements() const MOZ_DELETE;
    Int16ElementsHeader & asInt16Elements() MOZ_DELETE;
    Int16ElementsHeader(const Int16ElementsHeader &other) MOZ_DELETE;
    void operator=(const Int16ElementsHeader &other) MOZ_DELETE;
};
class Uint32ElementsHeader : public TypedElementsHeader<uint32_t>
{
  private:
    bool isUint32Elements() const MOZ_DELETE;
    Uint32ElementsHeader & asUint32Elements() MOZ_DELETE;
    Uint32ElementsHeader(const Uint32ElementsHeader &other) MOZ_DELETE;
    void operator=(const Uint32ElementsHeader &other) MOZ_DELETE;
};
class Int32ElementsHeader : public TypedElementsHeader<int32_t>
{
  private:
    bool isInt32Elements() const MOZ_DELETE;
    Int32ElementsHeader & asInt32Elements() MOZ_DELETE;
    Int32ElementsHeader(const Int32ElementsHeader &other) MOZ_DELETE;
    void operator=(const Int32ElementsHeader &other) MOZ_DELETE;
};
class Float32ElementsHeader : public TypedElementsHeader<float>
{
  private:
    bool isFloat32Elements() const MOZ_DELETE;
    Float32ElementsHeader & asFloat32Elements() MOZ_DELETE;
    Float32ElementsHeader(const Float32ElementsHeader &other) MOZ_DELETE;
    void operator=(const Float32ElementsHeader &other) MOZ_DELETE;
};
class Float64ElementsHeader : public TypedElementsHeader<double>
{
  private:
    bool isFloat64Elements() const MOZ_DELETE;
    Float64ElementsHeader & asFloat64Elements() MOZ_DELETE;
    Float64ElementsHeader(const Float64ElementsHeader &other) MOZ_DELETE;
    void operator=(const Float64ElementsHeader &other) MOZ_DELETE;
};

class Uint8ClampedElementsHeader : public TypedElementsHeader<uint8_clamped>
{
  private:
    inline bool isUint8Clamped() const MOZ_DELETE;
    inline Uint8ClampedElementsHeader & asUint8ClampedElements() MOZ_DELETE;
    Uint8ClampedElementsHeader(const Uint8ClampedElementsHeader &other) MOZ_DELETE;
    void operator=(const Uint8ClampedElementsHeader &other) MOZ_DELETE;
};

class ArrayBufferElementsHeader : public ElementsHeader
{
  public:
    bool getOwnElement(JSContext *cx, Handle<ObjectImpl*> obj, uint32_t index,
                       unsigned resolveFlags, PropDesc *desc);

    bool defineElement(JSContext *cx, Handle<ObjectImpl*> obj, uint32_t index,
                       const PropDesc &desc, bool shouldThrow, unsigned resolveFlags,
                       bool *succeeded);

    bool setElement(JSContext *cx, Handle<ObjectImpl*> obj, Handle<ObjectImpl*> receiver,
                    uint32_t index, const Value &v, unsigned resolveFlags, bool *succeeded);

  private:
    inline bool isArrayBufferElements() const MOZ_DELETE;
    inline ArrayBufferElementsHeader & asArrayBufferElements() MOZ_DELETE;

    ArrayBufferElementsHeader(const ArrayBufferElementsHeader &other) MOZ_DELETE;
    void operator=(const ArrayBufferElementsHeader &other) MOZ_DELETE;
};

inline DenseElementsHeader &
ElementsHeader::asDenseElements()
{
    MOZ_ASSERT(isDenseElements());
    return *static_cast<DenseElementsHeader *>(this);
}

inline SparseElementsHeader &
ElementsHeader::asSparseElements()
{
    MOZ_ASSERT(isSparseElements());
    return *static_cast<SparseElementsHeader *>(this);
}

inline Uint8ElementsHeader &
ElementsHeader::asUint8Elements()
{
    MOZ_ASSERT(isUint8Elements());
    return *static_cast<Uint8ElementsHeader *>(this);
}

inline Int8ElementsHeader &
ElementsHeader::asInt8Elements()
{
    MOZ_ASSERT(isInt8Elements());
    return *static_cast<Int8ElementsHeader *>(this);
}

inline Uint16ElementsHeader &
ElementsHeader::asUint16Elements()
{
    MOZ_ASSERT(isUint16Elements());
    return *static_cast<Uint16ElementsHeader *>(this);
}

inline Int16ElementsHeader &
ElementsHeader::asInt16Elements()
{
    MOZ_ASSERT(isInt16Elements());
    return *static_cast<Int16ElementsHeader *>(this);
}

inline Uint32ElementsHeader &
ElementsHeader::asUint32Elements()
{
    MOZ_ASSERT(isUint32Elements());
    return *static_cast<Uint32ElementsHeader *>(this);
}

inline Int32ElementsHeader &
ElementsHeader::asInt32Elements()
{
    MOZ_ASSERT(isInt32Elements());
    return *static_cast<Int32ElementsHeader *>(this);
}

inline Uint8ClampedElementsHeader &
ElementsHeader::asUint8ClampedElements()
{
    MOZ_ASSERT(isUint8ClampedElements());
    return *static_cast<Uint8ClampedElementsHeader *>(this);
}

inline Float32ElementsHeader &
ElementsHeader::asFloat32Elements()
{
    MOZ_ASSERT(isFloat32Elements());
    return *static_cast<Float32ElementsHeader *>(this);
}

inline Float64ElementsHeader &
ElementsHeader::asFloat64Elements()
{
    MOZ_ASSERT(isFloat64Elements());
    return *static_cast<Float64ElementsHeader *>(this);
}

inline ArrayBufferElementsHeader &
ElementsHeader::asArrayBufferElements()
{
    MOZ_ASSERT(isArrayBufferElements());
    return *static_cast<ArrayBufferElementsHeader *>(this);
}

/*
 * Header structure for object element arrays. This structure is immediately
 * followed by an array of elements, with the elements member in an object
 * pointing to the beginning of that array (the end of this structure).
 * See below for usage of this structure.
 */
class ArrayBufferObject;
class ObjectElements
{
    friend struct ::JSObject;
    friend class ObjectImpl;
    friend class ArrayBufferObject;

    /* Number of allocated slots. */
    uint32_t capacity;

    /*
     * Number of initialized elements. This is <= the capacity, and for arrays
     * is <= the length. Memory for elements above the initialized length is
     * uninitialized, but values between the initialized length and the proper
     * length are conceptually holes.
     */
    uint32_t initializedLength;

    /* 'length' property of array objects, unused for other objects. */
    uint32_t length;

    /* :XXX: bug 586842 store state about sparse slots. */
    uint32_t unused;

    void staticAsserts() {
        MOZ_STATIC_ASSERT(sizeof(ObjectElements) == VALUES_PER_HEADER * sizeof(Value),
                          "Elements size and values-per-Elements mismatch");
    }

  public:

    ObjectElements(uint32_t capacity, uint32_t length)
      : capacity(capacity), initializedLength(0), length(length)
    {}

    HeapSlot *elements() { return (HeapSlot *)(uintptr_t(this) + sizeof(ObjectElements)); }
    static ObjectElements * fromElements(HeapSlot *elems) {
        return (ObjectElements *)(uintptr_t(elems) - sizeof(ObjectElements));
    }

    static int offsetOfCapacity() {
        return (int)offsetof(ObjectElements, capacity) - (int)sizeof(ObjectElements);
    }
    static int offsetOfInitializedLength() {
        return (int)offsetof(ObjectElements, initializedLength) - (int)sizeof(ObjectElements);
    }
    static int offsetOfLength() {
        return (int)offsetof(ObjectElements, length) - (int)sizeof(ObjectElements);
    }

    static const size_t VALUES_PER_HEADER = 2;
};

/* Shared singleton for objects with no elements. */
extern HeapSlot *emptyObjectElements;

struct Class;
struct GCMarker;
struct ObjectOps;
struct Shape;

class NewObjectCache;

inline Value
ObjectValue(ObjectImpl &obj);

/*
 * ObjectImpl specifies the internal implementation of an object.  (In contrast
 * JSObject specifies an "external" interface, at the conceptual level of that
 * exposed in ECMAScript.)
 *
 * The |shape_| member stores the shape of the object, which includes the
 * object's class and the layout of all its properties.
 *
 * The type member stores the type of the object, which contains its prototype
 * object and the possible types of its properties.
 *
 * The rest of the object stores its named properties and indexed elements.
 * These are stored separately from one another. Objects are followed by an
 * variable-sized array of values for inline storage, which may be used by
 * either properties of native objects (fixed slots) or by elements.
 *
 * Two native objects with the same shape are guaranteed to have the same
 * number of fixed slots.
 *
 * Named property storage can be split between fixed slots and a dynamically
 * allocated array (the slots member). For an object with N fixed slots, shapes
 * with slots [0..N-1] are stored in the fixed slots, and the remainder are
 * stored in the dynamic array. If all properties fit in the fixed slots, the
 * 'slots' member is NULL.
 *
 * Elements are indexed via the 'elements' member. This member can point to
 * either the shared emptyObjectElements singleton, into the inline value array
 * (the address of the third value, to leave room for a ObjectElements header;
 * in this case numFixedSlots() is zero) or to a dynamically allocated array.
 *
 * Only certain combinations of properties and elements storage are currently
 * possible. This will be changing soon :XXX: bug 586842.
 *
 * - For objects other than arrays and typed arrays, the elements are empty.
 *
 * - For 'slow' arrays, both elements and properties are used, but the
 *   elements have zero capacity --- only the length member is used.
 *
 * - For dense arrays, elements are used and properties are not used.
 *
 * - For typed array buffers, elements are used and properties are not used.
 *   The data indexed by the elements do not represent Values, but primitive
 *   unboxed integers or floating point values.
 *
 * The members of this class are currently protected; in the long run this will
 * will change so that some members are private, and only certain methods that
 * act upon them will be protected.
 */
class ObjectImpl : public gc::Cell
{
  protected:
    /*
     * Shape of the object, encodes the layout of the object's properties and
     * all other information about its structure. See jsscope.h.
     */
    HeapPtrShape shape_;

    /*
     * The object's type and prototype. For objects with the LAZY_TYPE flag
     * set, this is the prototype's default 'new' type and can only be used
     * to get that prototype.
     */
    HeapPtrTypeObject type_;

    HeapSlot *slots;     /* Slots for object properties. */
    HeapSlot *elements;  /* Slots for object elements. */

  private:
    static void staticAsserts() {
        MOZ_STATIC_ASSERT(sizeof(ObjectImpl) == sizeof(shadow::Object),
                          "shadow interface must match actual implementation");
        MOZ_STATIC_ASSERT(sizeof(ObjectImpl) % sizeof(Value) == 0,
                          "fixed slots after an object must be aligned");

        MOZ_STATIC_ASSERT(offsetof(ObjectImpl, shape_) == offsetof(shadow::Object, shape),
                          "shadow shape must match actual shape");
        MOZ_STATIC_ASSERT(offsetof(ObjectImpl, type_) == offsetof(shadow::Object, type),
                          "shadow type must match actual type");
        MOZ_STATIC_ASSERT(offsetof(ObjectImpl, slots) == offsetof(shadow::Object, slots),
                          "shadow slots must match actual slots");
        MOZ_STATIC_ASSERT(offsetof(ObjectImpl, elements) == offsetof(shadow::Object, _1),
                          "shadow placeholder must match actual elements");
    }

    JSObject * asObjectPtr() { return reinterpret_cast<JSObject *>(this); }

    friend inline Value ObjectValue(ObjectImpl &obj);

    /* These functions are public, and they should remain public. */

  public:
    JSObject * getProto() const {
        return type_->proto;
    }

    inline bool isExtensible() const;

    /*
     * XXX Once the property/element split of bug 586842 is complete, these
     *     methods should move back to JSObject.
     */
    inline bool isDenseArray() const;
    inline bool isSlowArray() const;
    inline bool isArray() const;

    inline HeapSlotArray getDenseArrayElements();
    inline const Value & getDenseArrayElement(uint32_t idx);
    inline uint32_t getDenseArrayInitializedLength();

    bool makeElementsSparse(JSContext *cx) {
        NEW_OBJECT_REPRESENTATION_ONLY();

        MOZ_NOT_REACHED("NYI");
        return false;
    }

  protected:
#ifdef DEBUG
    void checkShapeConsistency();
#else
    void checkShapeConsistency() { }
#endif

  private:
    /*
     * Get internal pointers to the range of values starting at start and
     * running for length.
     */
    inline void getSlotRangeUnchecked(uint32_t start, uint32_t length,
                                      HeapSlot **fixedStart, HeapSlot **fixedEnd,
                                      HeapSlot **slotsStart, HeapSlot **slotsEnd);
    inline void getSlotRange(uint32_t start, uint32_t length,
                             HeapSlot **fixedStart, HeapSlot **fixedEnd,
                             HeapSlot **slotsStart, HeapSlot **slotsEnd);

  protected:
    friend struct GCMarker;
    friend struct Shape;
    friend class NewObjectCache;

    inline void invalidateSlotRange(uint32_t start, uint32_t count);
    inline void initializeSlotRange(uint32_t start, uint32_t count);

    /*
     * Initialize a flat array of slots to this object at a start slot.  The
     * caller must ensure that are enough slots.
     */
    void initSlotRange(uint32_t start, const Value *vector, uint32_t length);

    /*
     * Copy a flat array of slots to this object at a start slot. Caller must
     * ensure there are enough slots in this object.
     */
    void copySlotRange(uint32_t start, const Value *vector, uint32_t length);

#ifdef DEBUG
    enum SentinelAllowed {
        SENTINEL_NOT_ALLOWED,
        SENTINEL_ALLOWED
    };

    /*
     * Check that slot is in range for the object's allocated slots.
     * If sentinelAllowed then slot may equal the slot capacity.
     */
    bool slotInRange(uint32_t slot, SentinelAllowed sentinel = SENTINEL_NOT_ALLOWED) const;
#endif

    /* Minimum size for dynamically allocated slots. */
    static const uint32_t SLOT_CAPACITY_MIN = 8;

    HeapSlot *fixedSlots() const {
        return reinterpret_cast<HeapSlot *>(uintptr_t(this) + sizeof(ObjectImpl));
    }

    friend class ElementsHeader;
    friend class DenseElementsHeader;
    friend class SparseElementsHeader;

    enum DenseElementsResult {
        Failure,
        ConvertToSparse,
        Succeeded
    };

    DenseElementsResult ensureDenseElementsInitialized(JSContext *cx, uint32_t index,
                                                       uint32_t extra)
    {
        NEW_OBJECT_REPRESENTATION_ONLY();

        MOZ_NOT_REACHED("NYI");
        return Failure;
    }

    /*
     * These functions are currently public for simplicity; in the long run
     * it may make sense to make at least some of them private.
     */

  public:
    Shape * lastProperty() const {
        MOZ_ASSERT(shape_);
        return shape_;
    }

    inline bool isNative() const;

    types::TypeObject *type() const {
        MOZ_ASSERT(!hasLazyType());
        return type_;
    }

    uint32_t numFixedSlots() const {
        return reinterpret_cast<const shadow::Object *>(this)->numFixedSlots();
    }

    /*
     * Whether this is the only object which has its specified type. This
     * object will have its type constructed lazily as needed by analysis.
     */
    bool hasSingletonType() const { return !!type_->singleton; }

    /*
     * Whether the object's type has not been constructed yet. If an object
     * might have a lazy type, use getType() below, otherwise type().
     */
    bool hasLazyType() const { return type_->lazy(); }

    inline uint32_t slotSpan() const;

    /* Compute dynamicSlotsCount() for this object. */
    inline uint32_t numDynamicSlots() const;

    Shape * nativeLookup(JSContext *cx, jsid id);
    inline Shape * nativeLookup(JSContext *cx, PropertyId pid);
    inline Shape * nativeLookup(JSContext *cx, PropertyName *name);

    Shape * nativeLookupNoAllocation(jsid id);
    inline Shape * nativeLookupNoAllocation(PropertyId pid);
    inline Shape * nativeLookupNoAllocation(PropertyName *name);

    inline Class *getClass() const;
    inline JSClass *getJSClass() const;
    inline bool hasClass(const Class *c) const;
    inline const ObjectOps *getOps() const;

    /*
     * An object is a delegate if it is on another object's prototype or scope
     * chain, and therefore the delegate might be asked implicitly to get or
     * set a property on behalf of another object. Delegates may be accessed
     * directly too, as may any object, but only those objects linked after the
     * head of any prototype or scope chain are flagged as delegates. This
     * definition helps to optimize shape-based property cache invalidation
     * (see Purge{Scope,Proto}Chain in jsobj.cpp).
     */
    inline bool isDelegate() const;

    /*
     * Return true if this object is a native one that has been converted from
     * shared-immutable prototype-rooted shape storage to dictionary-shapes in
     * a doubly-linked list.
     */
    inline bool inDictionaryMode() const;

    const Value &getSlot(uint32_t slot) const {
        MOZ_ASSERT(slotInRange(slot));
        uint32_t fixed = numFixedSlots();
        if (slot < fixed)
            return fixedSlots()[slot];
        return slots[slot - fixed];
    }

    HeapSlot *getSlotAddressUnchecked(uint32_t slot) {
        uint32_t fixed = numFixedSlots();
        if (slot < fixed)
            return fixedSlots() + slot;
        return slots + (slot - fixed);
    }

    HeapSlot *getSlotAddress(uint32_t slot) {
        /*
         * This can be used to get the address of the end of the slots for the
         * object, which may be necessary when fetching zero-length arrays of
         * slots (e.g. for callObjVarArray).
         */
        MOZ_ASSERT(slotInRange(slot, SENTINEL_ALLOWED));
        return getSlotAddressUnchecked(slot);
    }

    HeapSlot &getSlotRef(uint32_t slot) {
        MOZ_ASSERT(slotInRange(slot));
        return *getSlotAddress(slot);
    }

    inline HeapSlot &nativeGetSlotRef(uint32_t slot);
    inline const Value &nativeGetSlot(uint32_t slot) const;

    inline void setSlot(uint32_t slot, const Value &value);
    inline void initSlot(uint32_t slot, const Value &value);
    inline void initCrossCompartmentSlot(uint32_t slot, const js::Value &value);
    inline void initSlotUnchecked(uint32_t slot, const Value &value);

    /* For slots which are known to always be fixed, due to the way they are allocated. */

    HeapSlot &getFixedSlotRef(uint32_t slot) {
        MOZ_ASSERT(slot < numFixedSlots());
        return fixedSlots()[slot];
    }

    const Value &getFixedSlot(uint32_t slot) const {
        MOZ_ASSERT(slot < numFixedSlots());
        return fixedSlots()[slot];
    }

    inline void setFixedSlot(uint32_t slot, const Value &value);
    inline void initFixedSlot(uint32_t slot, const Value &value);

    /*
     * Get the number of dynamic slots to allocate to cover the properties in
     * an object with the given number of fixed slots and slot span. The slot
     * capacity is not stored explicitly, and the allocated size of the slot
     * array is kept in sync with this count.
     */
    static inline uint32_t dynamicSlotsCount(uint32_t nfixed, uint32_t span);

    /* Memory usage functions. */
    inline size_t sizeOfThis() const;

    /* Elements accessors. */

    ObjectElements * getElementsHeader() const {
        return ObjectElements::fromElements(elements);
    }

    ElementsHeader & elementsHeader() const {
        NEW_OBJECT_REPRESENTATION_ONLY();
        return *ElementsHeader::fromElements(elements);
    }

    inline HeapSlot *fixedElements() const {
        MOZ_STATIC_ASSERT(2 * sizeof(Value) == sizeof(ObjectElements),
                          "when elements are stored inline, the first two "
                          "slots will hold the ObjectElements header");
        return &fixedSlots()[2];
    }

    void setFixedElements() { this->elements = fixedElements(); }

    inline bool hasDynamicElements() const {
        /*
         * Note: for objects with zero fixed slots this could potentially give
         * a spurious 'true' result, if the end of this object is exactly
         * aligned with the end of its arena and dynamic slots are allocated
         * immediately afterwards. Such cases cannot occur for dense arrays
         * (which have at least two fixed slots) and can only result in a leak.
         */
        return elements != emptyObjectElements && elements != fixedElements();
    }

    /* GC support. */
    static inline ThingRootKind rootKind() { return THING_ROOT_OBJECT; }
    static inline void readBarrier(ObjectImpl *obj);
    static inline void writeBarrierPre(ObjectImpl *obj);
    static inline void writeBarrierPost(ObjectImpl *obj, void *addr);
    inline void privateWriteBarrierPre(void **oldval);
    inline void privateWriteBarrierPost(void **pprivate);
    void markChildren(JSTracer *trc);

    /* Private data accessors. */

    inline void *&privateRef(uint32_t nfixed) const; /* XXX should be private, not protected! */

    inline bool hasPrivate() const;
    inline void *getPrivate() const;
    inline void setPrivate(void *data);
    inline void setPrivateGCThing(gc::Cell *cell);
    inline void setPrivateUnbarriered(void *data);
    inline void initPrivate(void *data);

    /* Access private data for an object with a known number of fixed slots. */
    inline void *getPrivate(uint32_t nfixed) const;

    /* JIT Accessors */
    static size_t offsetOfShape() { return offsetof(ObjectImpl, shape_); }
    HeapPtrShape *addressOfShape() { return &shape_; }

    static size_t offsetOfType() { return offsetof(ObjectImpl, type_); }
    HeapPtrTypeObject *addressOfType() { return &type_; }

    static size_t offsetOfElements() { return offsetof(ObjectImpl, elements); }
    static size_t offsetOfFixedElements() {
        return sizeof(ObjectImpl) + sizeof(ObjectElements);
    }

    static size_t getFixedSlotOffset(size_t slot) {
        return sizeof(ObjectImpl) + slot * sizeof(Value);
    }
    static size_t getPrivateDataOffset(size_t nfixed) { return getFixedSlotOffset(nfixed); }
    static size_t offsetOfSlots() { return offsetof(ObjectImpl, slots); }
};

inline Value
ObjectValue(ObjectImpl &obj)
{
    Value v;
    v.setObject(*obj.asObjectPtr());
    return v;
}

inline Handle<JSObject*>
Downcast(Handle<ObjectImpl*> obj)
{
    return Handle<JSObject*>::fromMarkedLocation(reinterpret_cast<JSObject* const*>(obj.address()));
}

extern JSObject *
ArrayBufferDelegate(JSContext *cx, Handle<ObjectImpl*> obj);

/* Generic [[GetOwnProperty]] method. */
bool
GetOwnElement(JSContext *cx, Handle<ObjectImpl*> obj, uint32_t index, unsigned resolveFlags,
              PropDesc *desc);
extern bool
GetOwnProperty(JSContext *cx, Handle<ObjectImpl*> obj, PropertyId pid, unsigned resolveFlags,
               PropDesc *desc);
inline bool
GetOwnProperty(JSContext *cx, Handle<ObjectImpl*> obj, Handle<PropertyName*> name,
               unsigned resolveFlags, PropDesc *desc)
{
    return GetOwnProperty(cx, obj, PropertyId(name), resolveFlags, desc);
}
inline bool
GetOwnProperty(JSContext *cx, Handle<ObjectImpl*> obj, Handle<SpecialId> sid, unsigned resolveFlags,
               PropDesc *desc)
{
    return GetOwnProperty(cx, obj, PropertyId(sid), resolveFlags, desc);
}

/* Proposed default [[GetP]](Receiver, P) method. */
extern bool
GetElement(JSContext *cx, Handle<ObjectImpl*> obj, Handle<ObjectImpl*> receiver, uint32_t index,
           unsigned resolveFlags, Value *vp);

extern bool
DefineElement(JSContext *cx, Handle<ObjectImpl*> obj, uint32_t index, const PropDesc &desc,
              bool shouldThrow, unsigned resolveFlags, bool *succeeded);

/* Proposed default [[SetP]](Receiver, P, V) method. */
extern bool
SetElement(JSContext *cx, Handle<ObjectImpl*> obj, Handle<ObjectImpl*> receiver, uint32_t index,
           const Value &v, unsigned resolveFlags, bool *succeeded);

extern bool
HasElement(JSContext *cx, Handle<ObjectImpl*> obj, uint32_t index, unsigned resolveFlags,
           bool *found);

} /* namespace js */

namespace JS {
template <> struct RootMethods<js::PropertyId>
{
    static js::PropertyId initial() { return js::PropertyId(); }
    static ThingRootKind kind() { return THING_ROOT_PROPERTY_ID; }
    static bool poisoned(js::PropertyId propid) { return IsPoisonedId(propid.asId()); }
};
}

#endif /* ObjectImpl_h__ */
