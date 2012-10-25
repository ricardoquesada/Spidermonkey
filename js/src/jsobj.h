/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsobj_h___
#define jsobj_h___

/*
 * JS object definitions.
 *
 * A JS object consists of a possibly-shared object descriptor containing
 * ordered property names, called the map; and a dense vector of property
 * values, called slots.  The map/slot pointer pair is GC'ed, while the map
 * is reference counted and the slot vector is malloc'ed.
 */
#include "jsapi.h"
#include "jsatom.h"
#include "jsclass.h"
#include "jsfriendapi.h"
#include "jsinfer.h"
#include "jspubtd.h"
#include "jsprvtd.h"
#include "jslock.h"

#include "gc/Barrier.h"
#include "gc/Heap.h"

#include "vm/ObjectImpl.h"
#include "vm/String.h"

namespace js {

class AutoPropDescArrayRooter;
class BaseProxyHandler;
class CallObject;
struct GCMarker;
struct NativeIterator;

namespace mjit { class Compiler; }

inline JSObject *
CastAsObject(PropertyOp op)
{
    return JS_FUNC_TO_DATA_PTR(JSObject *, op);
}

inline JSObject *
CastAsObject(StrictPropertyOp op)
{
    return JS_FUNC_TO_DATA_PTR(JSObject *, op);
}

inline Value
CastAsObjectJsval(PropertyOp op)
{
    return ObjectOrNullValue(CastAsObject(op));
}

inline Value
CastAsObjectJsval(StrictPropertyOp op)
{
    return ObjectOrNullValue(CastAsObject(op));
}

/*
 * JSPropertySpec uses JSAPI JSPropertyOp and JSStrictPropertyOp in function
 * signatures, but with JSPROP_NATIVE_ACCESSORS the actual values must be
 * JSNatives. To avoid widespread casting, have JS_PSG and JS_PSGS perform
 * type-safe casts.
 */
#define JS_PSG(name,getter,flags)                                               \
    {name, 0, (flags) | JSPROP_SHARED | JSPROP_NATIVE_ACCESSORS,                \
     JSOP_WRAPPER((JSPropertyOp)getter), JSOP_NULLWRAPPER}
#define JS_PSGS(name,getter,setter,flags)                                       \
    {name, 0, (flags) | JSPROP_SHARED | JSPROP_NATIVE_ACCESSORS,                \
     JSOP_WRAPPER((JSPropertyOp)getter), JSOP_WRAPPER((JSStrictPropertyOp)setter)}
#define JS_PS_END {0, 0, 0, JSOP_NULLWRAPPER, JSOP_NULLWRAPPER}

/******************************************************************************/

typedef Vector<PropDesc, 1> PropDescArray;

/*
 * The baseops namespace encapsulates the default behavior when performing
 * various operations on an object, irrespective of hooks installed in the
 * object's class. In general, instance methods on the object itself should be
 * called instead of calling these methods directly.
 */
namespace baseops {

/*
 * On success, and if id was found, return true with *objp non-null and with a
 * property of *objp stored in *propp. If successful but id was not found,
 * return true with both *objp and *propp null.
 */
extern JS_FRIEND_API(JSBool)
LookupProperty(JSContext *cx, HandleObject obj, HandleId id, MutableHandleObject objp,
               MutableHandleShape propp);

inline bool
LookupProperty(JSContext *cx, HandleObject obj, PropertyName *name,
               MutableHandleObject objp, MutableHandleShape propp)
{
    Rooted<jsid> id(cx, NameToId(name));
    return LookupProperty(cx, obj, id, objp, propp);
}

extern JS_FRIEND_API(JSBool)
LookupElement(JSContext *cx, HandleObject obj, uint32_t index,
              MutableHandleObject objp, MutableHandleShape propp);

extern JSBool
DefineGeneric(JSContext *cx, HandleObject obj, HandleId id, HandleValue value,
               JSPropertyOp getter, JSStrictPropertyOp setter, unsigned attrs);

inline JSBool
DefineProperty(JSContext *cx, HandleObject obj, PropertyName *name, HandleValue value,
               JSPropertyOp getter, JSStrictPropertyOp setter, unsigned attrs)
{
    Rooted<jsid> id(cx, NameToId(name));
    return DefineGeneric(cx, obj, id, value, getter, setter, attrs);
}

extern JSBool
DefineElement(JSContext *cx, HandleObject obj, uint32_t index, HandleValue value,
              JSPropertyOp getter, JSStrictPropertyOp setter, unsigned attrs);

extern JSBool
GetProperty(JSContext *cx, HandleObject obj, HandleObject receiver, HandleId id, MutableHandleValue vp);

extern JSBool
GetElement(JSContext *cx, HandleObject obj, HandleObject receiver, uint32_t index, MutableHandleValue vp);

inline JSBool
GetProperty(JSContext *cx, HandleObject obj, HandleId id, MutableHandleValue vp)
{
    return GetProperty(cx, obj, obj, id, vp);
}

inline JSBool
GetElement(JSContext *cx, HandleObject obj, uint32_t index, MutableHandleValue vp)
{
    return GetElement(cx, obj, obj, index, vp);
}

extern JSBool
GetPropertyDefault(JSContext *cx, HandleObject obj, HandleId id, HandleValue def, MutableHandleValue vp);

extern JSBool
SetPropertyHelper(JSContext *cx, HandleObject obj, HandleObject receiver, HandleId id,
                  unsigned defineHow, MutableHandleValue vp, JSBool strict);

inline bool
SetPropertyHelper(JSContext *cx, HandleObject obj, HandleObject receiver, PropertyName *name,
                  unsigned defineHow, MutableHandleValue vp, JSBool strict)
{
    Rooted<jsid> id(cx, NameToId(name));
    return SetPropertyHelper(cx, obj, receiver, id, defineHow, vp, strict);
}

extern JSBool
SetElementHelper(JSContext *cx, HandleObject obj, HandleObject Receiver, uint32_t index,
                 unsigned defineHow, MutableHandleValue vp, JSBool strict);

extern JSType
TypeOf(JSContext *cx, HandleObject obj);

extern JSBool
GetAttributes(JSContext *cx, HandleObject obj, HandleId id, unsigned *attrsp);

extern JSBool
SetAttributes(JSContext *cx, HandleObject obj, HandleId id, unsigned *attrsp);

extern JSBool
GetElementAttributes(JSContext *cx, HandleObject obj, uint32_t index, unsigned *attrsp);

extern JSBool
SetElementAttributes(JSContext *cx, HandleObject obj, uint32_t index, unsigned *attrsp);

extern JSBool
DeleteProperty(JSContext *cx, HandleObject obj, HandlePropertyName name, MutableHandleValue rval, JSBool strict);

extern JSBool
DeleteElement(JSContext *cx, HandleObject obj, uint32_t index, MutableHandleValue rval, JSBool strict);

extern JSBool
DeleteSpecial(JSContext *cx, HandleObject obj, HandleSpecialId sid, MutableHandleValue rval, JSBool strict);

extern JSBool
DeleteGeneric(JSContext *cx, HandleObject obj, HandleId id, MutableHandleValue rval, JSBool strict);

} /* namespace js::baseops */

/* ES5 8.12.8. */
extern JSBool
DefaultValue(JSContext *cx, HandleObject obj, JSType hint, MutableHandleValue vp);

extern Class ArrayClass;
extern Class ArrayBufferClass;
extern Class BlockClass;
extern Class BooleanClass;
extern Class CallableObjectClass;
extern Class DataViewClass;
extern Class DateClass;
extern Class ErrorClass;
extern Class ElementIteratorClass;
extern Class GeneratorClass;
extern Class JSONClass;
extern Class MapIteratorClass;
extern Class MathClass;
extern Class NumberClass;
extern Class NormalArgumentsObjectClass;
extern Class ObjectClass;
extern Class ProxyClass;
extern Class RegExpClass;
extern Class RegExpStaticsClass;
extern Class SetIteratorClass;
extern Class SlowArrayClass;
extern Class StopIterationClass;
extern Class StringClass;
extern Class StrictArgumentsObjectClass;
extern Class WeakMapClass;
extern Class WithClass;
extern Class XMLFilterClass;

class ArgumentsObject;
class ArrayBufferObject;
class BlockObject;
class BooleanObject;
class ClonedBlockObject;
class DataViewObject;
class DebugScopeObject;
class DeclEnvObject;
class ElementIteratorObject;
class GlobalObject;
class MapObject;
class MapIteratorObject;
class NestedScopeObject;
class NewObjectCache;
class NormalArgumentsObject;
class NumberObject;
class PropertyIteratorObject;
class ScopeObject;
class SetObject;
class SetIteratorObject;
class StaticBlockObject;
class StrictArgumentsObject;
class StringObject;
class RegExpObject;
class WithObject;

}  /* namespace js */

/*
 * The public interface for an object.
 *
 * Implementation of the underlying structure occurs in ObjectImpl, from which
 * this struct inherits.  This inheritance is currently public, but it will
 * eventually be made protected.  For full details, see vm/ObjectImpl.{h,cpp}.
 *
 * The JSFunction struct is an extension of this struct allocated from a larger
 * GC size-class.
 */
struct JSObject : public js::ObjectImpl
{
  private:
    friend struct js::Shape;
    friend struct js::GCMarker;
    friend class  js::NewObjectCache;

    /* Make the type object to use for LAZY_TYPE objects. */
    js::types::TypeObject *makeLazyType(JSContext *cx);

  public:
    /*
     * Update the last property, keeping the number of allocated slots in sync
     * with the object's new slot span.
     */
    bool setLastProperty(JSContext *cx, js::Shape *shape);

    /* As above, but does not change the slot span. */
    inline void setLastPropertyInfallible(js::Shape *shape);

    /* Make a non-array object with the specified initial state. */
    static inline JSObject *create(JSContext *cx,
                                   js::gc::AllocKind kind,
                                   js::HandleShape shape,
                                   js::HandleTypeObject type,
                                   js::HeapSlot *slots);

    /* Make a dense array object with the specified initial state. */
    static inline JSObject *createDenseArray(JSContext *cx,
                                             js::gc::AllocKind kind,
                                             js::HandleShape shape,
                                             js::HandleTypeObject type,
                                             uint32_t length);

    /*
     * Remove the last property of an object, provided that it is safe to do so
     * (the shape and previous shape do not carry conflicting information about
     * the object itself).
     */
    inline void removeLastProperty(JSContext *cx);
    inline bool canRemoveLastProperty();

    /*
     * Update the slot span directly for a dictionary object, and allocate
     * slots to cover the new span if necessary.
     */
    bool setSlotSpan(JSContext *cx, uint32_t span);

    inline bool nativeContains(JSContext *cx, js::HandleId id);
    inline bool nativeContains(JSContext *cx, js::HandleShape shape);

    inline bool nativeContainsNoAllocation(jsid id);
    inline bool nativeContainsNoAllocation(const js::Shape &shape);

    /* Upper bound on the number of elements in an object. */
    static const uint32_t NELEMENTS_LIMIT = JS_BIT(28);

  public:
    inline bool setDelegate(JSContext *cx);

    inline bool isBoundFunction() const;

    inline bool hasSpecialEquality() const;

    inline bool watched() const;
    inline bool setWatched(JSContext *cx);

    /* See StackFrame::varObj. */
    inline bool isVarObj();
    inline bool setVarObj(JSContext *cx);

    /*
     * Objects with an uncacheable proto can have their prototype mutated
     * without inducing a shape change on the object. Property cache entries
     * and JIT inline caches should not be filled for lookups across prototype
     * lookups on the object.
     */
    inline bool hasUncacheableProto() const;
    inline bool setUncacheableProto(JSContext *cx);

    bool generateOwnShape(JSContext *cx, js::Shape *newShape = NULL) {
        return replaceWithNewEquivalentShape(cx, lastProperty(), newShape);
    }

  private:
    js::Shape *replaceWithNewEquivalentShape(JSContext *cx, js::Shape *existingShape,
                                             js::Shape *newShape = NULL);

    enum GenerateShape {
        GENERATE_NONE,
        GENERATE_SHAPE
    };

    bool setFlag(JSContext *cx, /*BaseShape::Flag*/ uint32_t flag,
                 GenerateShape generateShape = GENERATE_NONE);

  public:
    inline bool nativeEmpty() const;

    bool shadowingShapeChange(JSContext *cx, const js::Shape &shape);

    /* Whether there may be indexed properties on this object. */
    inline bool isIndexed() const;

    inline uint32_t propertyCount() const;

    inline bool hasShapeTable() const;

    inline size_t computedSizeOfThisSlotsElements() const;

    inline void sizeOfExcludingThis(JSMallocSizeOfFun mallocSizeOf,
                                    size_t *slotsSize, size_t *elementsSize,
                                    size_t *miscSize) const;

    static const uint32_t MAX_FIXED_SLOTS = 16;

  public:

    /* Accessors for properties. */

    /* Whether a slot is at a fixed offset from this object. */
    inline bool isFixedSlot(size_t slot);

    /* Index into the dynamic slots array to use for a dynamic slot. */
    inline size_t dynamicSlotIndex(size_t slot);

    /* Get a raw pointer to the object's properties. */
    inline const js::HeapSlot *getRawSlots();

    /*
     * Grow or shrink slots immediately before changing the slot span.
     * The number of allocated slots is not stored explicitly, and changes to
     * the slots must track changes in the slot span.
     */
    bool growSlots(JSContext *cx, uint32_t oldCount, uint32_t newCount);
    void shrinkSlots(JSContext *cx, uint32_t oldCount, uint32_t newCount);

    bool hasDynamicSlots() const { return slots != NULL; }

  protected:
    inline bool updateSlotsForSpan(JSContext *cx, size_t oldSpan, size_t newSpan);

  public:
    /*
     * Trigger the write barrier on a range of slots that will no longer be
     * reachable.
     */
    inline void prepareSlotRangeForOverwrite(size_t start, size_t end);
    inline void prepareElementRangeForOverwrite(size_t start, size_t end);

    void rollbackProperties(JSContext *cx, uint32_t slotSpan);

    inline void nativeSetSlot(unsigned slot, const js::Value &value);
    inline void nativeSetSlotWithType(JSContext *cx, js::Shape *shape, const js::Value &value);

    inline const js::Value &getReservedSlot(unsigned index) const;
    inline js::HeapSlot &getReservedSlotRef(unsigned index);
    inline void initReservedSlot(unsigned index, const js::Value &v);
    inline void setReservedSlot(unsigned index, const js::Value &v);

    /*
     * Marks this object as having a singleton type, and leave the type lazy.
     * Constructs a new, unique shape for the object.
     */
    static inline bool setSingletonType(JSContext *cx, js::HandleObject obj);

    inline js::types::TypeObject *getType(JSContext *cx);

    const js::HeapPtr<js::types::TypeObject> &typeFromGC() const {
        /* Direct field access for use by GC. */
        return type_;
    }

    inline void setType(js::types::TypeObject *newType);

    js::types::TypeObject *getNewType(JSContext *cx, JSFunction *fun = NULL,
                                      bool isDOM = false);

#ifdef DEBUG
    bool hasNewType(js::types::TypeObject *newType);
#endif

    /*
     * Mark an object that has been iterated over and is a singleton. We need
     * to recover this information in the object's type information after it
     * is purged on GC.
     */
    inline bool setIteratedSingleton(JSContext *cx);

    /*
     * Mark an object as requiring its default 'new' type to have unknown
     * properties.
     */
    bool setNewTypeUnknown(JSContext *cx);

    /* Set a new prototype for an object with a singleton type. */
    bool splicePrototype(JSContext *cx, JSObject *proto);

    /*
     * For bootstrapping, whether to splice a prototype for Function.prototype
     * or the global object.
     */
    bool shouldSplicePrototype(JSContext *cx);

    /*
     * Parents and scope chains.
     *
     * All script-accessible objects with a NULL parent are global objects,
     * and all global objects have a NULL parent. Some builtin objects which
     * are not script-accessible also have a NULL parent, such as parser
     * created functions for non-compileAndGo scripts.
     *
     * Except for the non-script-accessible builtins, the global with which an
     * object is associated can be reached by following parent links to that
     * global (see global()).
     *
     * The scope chain of an object is the link in the search path when a
     * script does a name lookup on a scope object. For JS internal scope
     * objects --- Call, DeclEnv and block --- the chain is stored in
     * the first fixed slot of the object, and the object's parent is the
     * associated global. For other scope objects, the chain is stored in the
     * object's parent.
     *
     * In compileAndGo code, scope chains can contain only internal scope
     * objects with a global object at the root as the scope of the outermost
     * non-function script. In non-compileAndGo code, the scope of the
     * outermost non-function script might not be a global object, and can have
     * a mix of other objects above it before the global object is reached.
     */

    /* Access the parent link of an object. */
    inline JSObject *getParent() const;
    static bool setParent(JSContext *cx, js::HandleObject obj, js::HandleObject newParent);

    /*
     * Get the enclosing scope of an object. When called on non-scope object,
     * this will just be the global (the name "enclosing scope" still applies
     * in this situation because non-scope objects can be on the scope chain).
     */
    inline JSObject *enclosingScope();

    inline js::GlobalObject &global() const;

    /* Remove the type (and prototype) or parent from a new object. */
    static inline bool clearType(JSContext *cx, js::HandleObject obj);
    static bool clearParent(JSContext *cx, js::HandleObject obj);

    /*
     * ES5 meta-object properties and operations.
     */

  private:
    enum ImmutabilityType { SEAL, FREEZE };

    /*
     * The guts of Object.seal (ES5 15.2.3.8) and Object.freeze (ES5 15.2.3.9): mark the
     * object as non-extensible, and adjust each property's attributes appropriately: each
     * property becomes non-configurable, and if |freeze|, data properties become
     * read-only as well.
     */
    static bool sealOrFreeze(JSContext *cx, js::HandleObject obj, ImmutabilityType it);

    static bool isSealedOrFrozen(JSContext *cx, js::HandleObject obj, ImmutabilityType it, bool *resultp);

    static inline unsigned getSealedOrFrozenAttributes(unsigned attrs, ImmutabilityType it);

  public:
    bool preventExtensions(JSContext *cx);

    /* ES5 15.2.3.8: non-extensible, all props non-configurable */
    static inline bool seal(JSContext *cx, js::HandleObject obj) { return sealOrFreeze(cx, obj, SEAL); }
    /* ES5 15.2.3.9: non-extensible, all properties non-configurable, all data props read-only */
    static inline bool freeze(JSContext *cx, js::HandleObject obj) { return sealOrFreeze(cx, obj, FREEZE); }

    static inline bool isSealed(JSContext *cx, js::HandleObject obj, bool *resultp) {
        return isSealedOrFrozen(cx, obj, SEAL, resultp);
    }
    static inline bool isFrozen(JSContext *cx, js::HandleObject obj, bool *resultp) {
        return isSealedOrFrozen(cx, obj, FREEZE, resultp);
    }

    /* Accessors for elements. */

    inline bool ensureElements(JSContext *cx, unsigned cap);
    bool growElements(JSContext *cx, unsigned cap);
    void shrinkElements(JSContext *cx, unsigned cap);


    /*
     * Array-specific getters and setters (for both dense and slow arrays).
     */

    bool allocateSlowArrayElements(JSContext *cx);

    inline uint32_t getArrayLength() const;
    inline void setArrayLength(JSContext *cx, uint32_t length);

    inline uint32_t getDenseArrayCapacity();
    inline void setDenseArrayLength(uint32_t length);
    inline void setDenseArrayInitializedLength(uint32_t length);
    inline void ensureDenseArrayInitializedLength(JSContext *cx, unsigned index, unsigned extra);
    inline void setDenseArrayElement(unsigned idx, const js::Value &val);
    inline void initDenseArrayElement(unsigned idx, const js::Value &val);
    inline void setDenseArrayElementWithType(JSContext *cx, unsigned idx, const js::Value &val);
    inline void initDenseArrayElementWithType(JSContext *cx, unsigned idx, const js::Value &val);
    inline void copyDenseArrayElements(unsigned dstStart, const js::Value *src, unsigned count);
    inline void initDenseArrayElements(unsigned dstStart, const js::Value *src, unsigned count);
    inline void moveDenseArrayElements(unsigned dstStart, unsigned srcStart, unsigned count);
    inline void moveDenseArrayElementsUnbarriered(unsigned dstStart, unsigned srcStart, unsigned count);
    inline bool denseArrayHasInlineSlots() const;

    /* Packed information for this array. */
    inline void markDenseArrayNotPacked(JSContext *cx);

    /*
     * ensureDenseArrayElements ensures that the dense array can hold at least
     * index + extra elements. It returns ED_OK on success, ED_FAILED on
     * failure to grow the array, ED_SPARSE when the array is too sparse to
     * grow (this includes the case of index + extra overflow). In the last
     * two cases the array is kept intact.
     */
    enum EnsureDenseResult { ED_OK, ED_FAILED, ED_SPARSE };
    inline EnsureDenseResult ensureDenseArrayElements(JSContext *cx, unsigned index, unsigned extra);

    /*
     * Check if after growing the dense array will be too sparse.
     * newElementsHint is an estimated number of elements to be added.
     */
    bool willBeSparseDenseArray(unsigned requiredCapacity, unsigned newElementsHint);

    static bool makeDenseArraySlow(JSContext *cx, js::HandleObject obj);

    /*
     * If this array object has a data property with index i, set *vp to its
     * value and return true. If not, do vp->setMagic(JS_ARRAY_HOLE) and return
     * true. On OOM, report it and return false.
     */
    bool arrayGetOwnDataElement(JSContext *cx, size_t i, js::Value *vp);

  public:
    /*
     * Date-specific getters and setters.
     */

    static const uint32_t JSSLOT_DATE_UTC_TIME = 0;
    static const uint32_t JSSLOT_DATE_TZA = 1;

    /*
     * Cached slots holding local properties of the date.
     * These are undefined until the first actual lookup occurs
     * and are reset to undefined whenever the date's time is modified.
     */
    static const uint32_t JSSLOT_DATE_COMPONENTS_START = 2;

    static const uint32_t JSSLOT_DATE_LOCAL_TIME    = JSSLOT_DATE_COMPONENTS_START + 0;
    static const uint32_t JSSLOT_DATE_LOCAL_YEAR    = JSSLOT_DATE_COMPONENTS_START + 1;
    static const uint32_t JSSLOT_DATE_LOCAL_MONTH   = JSSLOT_DATE_COMPONENTS_START + 2;
    static const uint32_t JSSLOT_DATE_LOCAL_DATE    = JSSLOT_DATE_COMPONENTS_START + 3;
    static const uint32_t JSSLOT_DATE_LOCAL_DAY     = JSSLOT_DATE_COMPONENTS_START + 4;
    static const uint32_t JSSLOT_DATE_LOCAL_HOURS   = JSSLOT_DATE_COMPONENTS_START + 5;
    static const uint32_t JSSLOT_DATE_LOCAL_MINUTES = JSSLOT_DATE_COMPONENTS_START + 6;
    static const uint32_t JSSLOT_DATE_LOCAL_SECONDS = JSSLOT_DATE_COMPONENTS_START + 7;

    static const uint32_t DATE_CLASS_RESERVED_SLOTS = JSSLOT_DATE_LOCAL_SECONDS + 1;

    inline const js::Value &getDateUTCTime() const;
    inline void setDateUTCTime(const js::Value &pthis);

    /*
     * Function-specific getters and setters.
     */

    friend struct JSFunction;

    inline JSFunction *toFunction();
    inline const JSFunction *toFunction() const;

  public:
    /*
     * Iterator-specific getters and setters.
     */

    static const uint32_t ITER_CLASS_NFIXED_SLOTS = 1;

    /*
     * XML-related getters and setters.
     */

    /*
     * Slots for XML-related classes are as follows:
     * - NamespaceClass.base reserves the *_NAME_* and *_NAMESPACE_* slots.
     * - QNameClass.base, AttributeNameClass, AnyNameClass reserve
     *   the *_NAME_* and *_QNAME_* slots.
     * - Others (XMLClass, js_XMLFilterClass) don't reserve any slots.
     */
  private:
    static const uint32_t JSSLOT_NAME_PREFIX          = 0;   // shared
    static const uint32_t JSSLOT_NAME_URI             = 1;   // shared

    static const uint32_t JSSLOT_NAMESPACE_DECLARED   = 2;

    static const uint32_t JSSLOT_QNAME_LOCAL_NAME     = 2;

  public:
    static const uint32_t NAMESPACE_CLASS_RESERVED_SLOTS = 3;
    static const uint32_t QNAME_CLASS_RESERVED_SLOTS     = 3;

    inline JSLinearString *getNamePrefix() const;
    inline jsval getNamePrefixVal() const;
    inline void setNamePrefix(JSLinearString *prefix);
    inline void clearNamePrefix();

    inline JSLinearString *getNameURI() const;
    inline jsval getNameURIVal() const;
    inline void setNameURI(JSLinearString *uri);

    inline jsval getNamespaceDeclared() const;
    inline void setNamespaceDeclared(jsval decl);

    inline JSAtom *getQNameLocalName() const;
    inline jsval getQNameLocalNameVal() const;
    inline void setQNameLocalName(JSAtom *name);

    /*
     * Back to generic stuff.
     */
    inline bool isCallable();

    inline void finish(js::FreeOp *fop);
    JS_ALWAYS_INLINE void finalize(js::FreeOp *fop);

    static inline bool hasProperty(JSContext *cx, js::HandleObject obj,
                                   js::HandleId id, bool *foundp, unsigned flags = 0);

    /*
     * Allocate and free an object slot.
     *
     * FIXME: bug 593129 -- slot allocation should be done by object methods
     * after calling object-parameter-free shape methods, avoiding coupling
     * logic across the object vs. shape module wall.
     */
    bool allocSlot(JSContext *cx, uint32_t *slotp);
    void freeSlot(JSContext *cx, uint32_t slot);

  public:
    static bool reportReadOnly(JSContext *cx, jsid id, unsigned report = JSREPORT_ERROR);
    bool reportNotConfigurable(JSContext* cx, jsid id, unsigned report = JSREPORT_ERROR);
    bool reportNotExtensible(JSContext *cx, unsigned report = JSREPORT_ERROR);

    /*
     * Get the property with the given id, then call it as a function with the
     * given arguments, providing this object as |this|. If the property isn't
     * callable a TypeError will be thrown. On success the value returned by
     * the call is stored in *vp.
     */
    bool callMethod(JSContext *cx, js::HandleId id, unsigned argc, js::Value *argv,
                    js::MutableHandleValue vp);

  private:
    js::Shape *getChildProperty(JSContext *cx, js::Shape *parent, js::StackShape &child);

  protected:
    /*
     * Internal helper that adds a shape not yet mapped by this object.
     *
     * Notes:
     * 1. getter and setter must be normalized based on flags (see jsscope.cpp).
     * 2. !isExtensible() checking must be done by callers.
     */
    js::Shape *addPropertyInternal(JSContext *cx, jsid id,
                                   JSPropertyOp getter, JSStrictPropertyOp setter,
                                   uint32_t slot, unsigned attrs,
                                   unsigned flags, int shortid, js::Shape **spp,
                                   bool allowDictionary);

  private:
    bool toDictionaryMode(JSContext *cx);

    struct TradeGutsReserved;
    static bool ReserveForTradeGuts(JSContext *cx, JSObject *a, JSObject *b,
                                    TradeGutsReserved &reserved);

    static void TradeGuts(JSContext *cx, JSObject *a, JSObject *b,
                          TradeGutsReserved &reserved);

  public:
    /* Add a property whose id is not yet in this scope. */
    js::Shape *addProperty(JSContext *cx, jsid id,
                           JSPropertyOp getter, JSStrictPropertyOp setter,
                           uint32_t slot, unsigned attrs,
                           unsigned flags, int shortid, bool allowDictionary = true);

    /* Add a data property whose id is not yet in this scope. */
    js::Shape *addDataProperty(JSContext *cx, jsid id, uint32_t slot, unsigned attrs) {
        JS_ASSERT(!(attrs & (JSPROP_GETTER | JSPROP_SETTER)));
        return addProperty(cx, id, NULL, NULL, slot, attrs, 0, 0);
    }

    /* Add or overwrite a property for id in this scope. */
    js::Shape *putProperty(JSContext *cx, jsid id,
                           JSPropertyOp getter, JSStrictPropertyOp setter,
                           uint32_t slot, unsigned attrs,
                           unsigned flags, int shortid);
    inline js::Shape *
    putProperty(JSContext *cx, js::PropertyName *name,
                JSPropertyOp getter, JSStrictPropertyOp setter,
                uint32_t slot, unsigned attrs, unsigned flags, int shortid) {
        return putProperty(cx, js::NameToId(name), getter, setter, slot, attrs, flags, shortid);
    }

    /* Change the given property into a sibling with the same id in this scope. */
    static js::Shape *changeProperty(JSContext *cx, js::HandleObject obj,
                                     js::Shape *shape, unsigned attrs, unsigned mask,
                                     JSPropertyOp getter, JSStrictPropertyOp setter);

    static inline bool changePropertyAttributes(JSContext *cx, js::HandleObject obj,
                                                js::Shape *shape, unsigned attrs);

    /* Remove the property named by id from this object. */
    bool removeProperty(JSContext *cx, jsid id);

    /* Clear the scope, making it empty. */
    void clear(JSContext *cx);

    static inline JSBool lookupGeneric(JSContext *cx, js::HandleObject obj,
                                       js::HandleId id,
                                       js::MutableHandleObject objp, js::MutableHandleShape propp);
    static inline JSBool lookupProperty(JSContext *cx, js::HandleObject obj,
                                        js::PropertyName *name,
                                        js::MutableHandleObject objp, js::MutableHandleShape propp);
    static inline JSBool lookupElement(JSContext *cx, js::HandleObject obj,
                                       uint32_t index,
                                       js::MutableHandleObject objp, js::MutableHandleShape propp);
    static inline JSBool lookupSpecial(JSContext *cx, js::HandleObject obj,
                                       js::SpecialId sid,
                                       js::MutableHandleObject objp, js::MutableHandleShape propp);

    static inline JSBool defineGeneric(JSContext *cx, js::HandleObject obj,
                                       js::HandleId id, js::HandleValue value,
                                       JSPropertyOp getter = JS_PropertyStub,
                                       JSStrictPropertyOp setter = JS_StrictPropertyStub,
                                       unsigned attrs = JSPROP_ENUMERATE);
    static inline JSBool defineProperty(JSContext *cx, js::HandleObject obj,
                                        js::PropertyName *name, js::HandleValue value,
                                        JSPropertyOp getter = JS_PropertyStub,
                                        JSStrictPropertyOp setter = JS_StrictPropertyStub,
                                        unsigned attrs = JSPROP_ENUMERATE);

    static inline JSBool defineElement(JSContext *cx, js::HandleObject obj,
                                       uint32_t index, js::HandleValue value,
                                       JSPropertyOp getter = JS_PropertyStub,
                                       JSStrictPropertyOp setter = JS_StrictPropertyStub,
                                       unsigned attrs = JSPROP_ENUMERATE);
    static inline JSBool defineSpecial(JSContext *cx, js::HandleObject obj,
                                       js::SpecialId sid, js::HandleValue value,
                                       JSPropertyOp getter = JS_PropertyStub,
                                       JSStrictPropertyOp setter = JS_StrictPropertyStub,
                                       unsigned attrs = JSPROP_ENUMERATE);

    static inline JSBool getGeneric(JSContext *cx, js::HandleObject obj,
                                    js::HandleObject receiver,
                                    js::HandleId id, js::MutableHandleValue vp);
    static inline JSBool getProperty(JSContext *cx, js::HandleObject obj,
                                     js::HandleObject receiver,
                                     js::PropertyName *name, js::MutableHandleValue vp);
    static inline JSBool getElement(JSContext *cx, js::HandleObject obj,
                                    js::HandleObject receiver,
                                    uint32_t index, js::MutableHandleValue vp);
    /* If element is not present (e.g. array hole) *present is set to
       false and the contents of *vp are unusable garbage. */
    static inline JSBool getElementIfPresent(JSContext *cx, js::HandleObject obj,
                                             js::HandleObject receiver, uint32_t index,
                                             js::MutableHandleValue vp, bool *present);
    static inline JSBool getSpecial(JSContext *cx, js::HandleObject obj,
                                    js::HandleObject receiver, js::SpecialId sid,
                                    js::MutableHandleValue vp);

    static inline JSBool setGeneric(JSContext *cx, js::HandleObject obj, js::HandleObject receiver,
                                    js::HandleId id,
                                    js::MutableHandleValue vp, JSBool strict);
    static inline JSBool setProperty(JSContext *cx, js::HandleObject obj, js::HandleObject receiver,
                                     js::PropertyName *name,
                                     js::MutableHandleValue vp, JSBool strict);
    static inline JSBool setElement(JSContext *cx, js::HandleObject obj, js::HandleObject receiver,
                                    uint32_t index,
                                    js::MutableHandleValue vp, JSBool strict);
    static inline JSBool setSpecial(JSContext *cx, js::HandleObject obj, js::HandleObject receiver,
                                    js::SpecialId sid,
                                    js::MutableHandleValue vp, JSBool strict);

    static JSBool nonNativeSetProperty(JSContext *cx, js::HandleObject obj,
                                       js::HandleId id, js::MutableHandleValue vp, JSBool strict);
    static JSBool nonNativeSetElement(JSContext *cx, js::HandleObject obj,
                                      uint32_t index, js::MutableHandleValue vp, JSBool strict);

    static inline JSBool getGenericAttributes(JSContext *cx, js::HandleObject obj,
                                              js::HandleId id, unsigned *attrsp);
    static inline JSBool getPropertyAttributes(JSContext *cx, js::HandleObject obj,
                                               js::PropertyName *name, unsigned *attrsp);
    static inline JSBool getElementAttributes(JSContext *cx, js::HandleObject obj,
                                              uint32_t index, unsigned *attrsp);
    static inline JSBool getSpecialAttributes(JSContext *cx, js::HandleObject obj,
                                              js::SpecialId sid, unsigned *attrsp);

    static inline JSBool setGenericAttributes(JSContext *cx, js::HandleObject obj,
                                              js::HandleId id, unsigned *attrsp);
    static inline JSBool setPropertyAttributes(JSContext *cx, js::HandleObject obj,
                                               js::PropertyName *name, unsigned *attrsp);
    static inline JSBool setElementAttributes(JSContext *cx, js::HandleObject obj,
                                              uint32_t index, unsigned *attrsp);
    static inline JSBool setSpecialAttributes(JSContext *cx, js::HandleObject obj,
                                              js::SpecialId sid, unsigned *attrsp);

    static inline bool deleteProperty(JSContext *cx, js::HandleObject obj,
                                      js::HandlePropertyName name,
                                      js::MutableHandleValue rval, bool strict);
    static inline bool deleteElement(JSContext *cx, js::HandleObject obj,
                                     uint32_t index,
                                     js::MutableHandleValue rval, bool strict);
    static inline bool deleteSpecial(JSContext *cx, js::HandleObject obj,
                                     js::HandleSpecialId sid,
                                     js::MutableHandleValue rval, bool strict);
    static bool deleteByValue(JSContext *cx, js::HandleObject obj,
                              const js::Value &property, js::MutableHandleValue rval, bool strict);

    static inline bool enumerate(JSContext *cx, js::HandleObject obj,
                                 JSIterateOp iterop, js::Value *statep, jsid *idp);
    static inline bool defaultValue(JSContext *cx, js::HandleObject obj,
                                    JSType hint, js::MutableHandleValue vp);
    static inline JSType typeOf(JSContext *cx, js::HandleObject obj);
    static inline JSObject *thisObject(JSContext *cx, js::HandleObject obj);

    static bool thisObject(JSContext *cx, const js::Value &v, js::Value *vp);

    bool swap(JSContext *cx, JSObject *other);

    inline void initArrayClass();

    /*
     * In addition to the generic object interface provided by JSObject,
     * specific types of objects may provide additional operations. To access,
     * these addition operations, callers should use the pattern:
     *
     *   if (obj.isX()) {
     *     XObject &x = obj.asX();
     *     x.foo();
     *   }
     *
     * These XObject classes form a hierarchy. For example, for a cloned block
     * object, the following predicates are true: isClonedBlock, isBlock,
     * isNestedScope and isScope. Each of these has a respective class that
     * derives and adds operations.
     *
     * A class XObject is defined in a vm/XObject{.h, .cpp, -inl.h} file
     * triplet (along with any class YObject that derives XObject).
     *
     * Note that X represents a low-level representation and does not query the
     * [[Class]] property of object defined by the spec (for this, see
     * js::ObjectClassIs).
     *
     * SpiderMonkey has not been completely switched to the isX/asX/XObject
     * pattern so in some cases there is no XObject class and the engine
     * instead pokes directly at reserved slots and getPrivate. In such cases,
     * consider adding the missing XObject class.
     */

    /* Direct subtypes of JSObject: */
    inline bool isArguments() const;
    inline bool isArrayBuffer() const;
    inline bool isDataView() const;
    inline bool isDate() const;
    inline bool isElementIterator() const;
    inline bool isError() const;
    inline bool isFunction() const;
    inline bool isGenerator() const;
    inline bool isGlobal() const;
    inline bool isMapIterator() const;
    inline bool isObject() const;
    inline bool isPrimitive() const;
    inline bool isPropertyIterator() const;
    inline bool isProxy() const;
    inline bool isRegExp() const;
    inline bool isRegExpStatics() const;
    inline bool isScope() const;
    inline bool isScript() const;
    inline bool isSetIterator() const;
    inline bool isStopIteration() const;
    inline bool isTypedArray() const;
    inline bool isWeakMap() const;
#if JS_HAS_XML_SUPPORT
    inline bool isNamespace() const;
    inline bool isQName() const;
    inline bool isXML() const;
    inline bool isXMLId() const;
#endif

    /* Subtypes of ScopeObject. */
    inline bool isBlock() const;
    inline bool isCall() const;
    inline bool isDeclEnv() const;
    inline bool isNestedScope() const;
    inline bool isWith() const;
    inline bool isClonedBlock() const;
    inline bool isStaticBlock() const;

    /* Subtypes of PrimitiveObject. */
    inline bool isBoolean() const;
    inline bool isNumber() const;
    inline bool isString() const;

    /* Subtypes of ArgumentsObject. */
    inline bool isNormalArguments() const;
    inline bool isStrictArguments() const;

    /* Subtypes of Proxy. */
    inline bool isDebugScope() const;
    inline bool isWrapper() const;
    inline bool isFunctionProxy() const;
    inline bool isCrossCompartmentWrapper() const;

    inline js::ArgumentsObject &asArguments();
    inline js::ArrayBufferObject &asArrayBuffer();
    inline const js::ArgumentsObject &asArguments() const;
    inline js::BlockObject &asBlock();
    inline js::BooleanObject &asBoolean();
    inline js::CallObject &asCall();
    inline js::ClonedBlockObject &asClonedBlock();
    inline js::DataViewObject &asDataView();
    inline js::DeclEnvObject &asDeclEnv();
    inline js::DebugScopeObject &asDebugScope();
    inline js::GlobalObject &asGlobal();
    inline js::MapObject &asMap();
    inline js::MapIteratorObject &asMapIterator();
    inline js::NestedScopeObject &asNestedScope();
    inline js::NormalArgumentsObject &asNormalArguments();
    inline js::NumberObject &asNumber();
    inline js::PropertyIteratorObject &asPropertyIterator();
    inline js::RegExpObject &asRegExp();
    inline js::ScopeObject &asScope();
    inline js::SetObject &asSet();
    inline js::SetIteratorObject &asSetIterator();
    inline js::StrictArgumentsObject &asStrictArguments();
    inline js::StaticBlockObject &asStaticBlock();
    inline js::StringObject &asString();
    inline js::WithObject &asWith();

    static inline js::ThingRootKind rootKind() { return js::THING_ROOT_OBJECT; }

#ifdef DEBUG
    void dump();
#endif

  private:
    static void staticAsserts() {
        MOZ_STATIC_ASSERT(sizeof(JSObject) == sizeof(js::shadow::Object),
                          "shadow interface must match actual interface");
        MOZ_STATIC_ASSERT(sizeof(JSObject) == sizeof(js::ObjectImpl),
                          "JSObject itself must not have any fields");
        MOZ_STATIC_ASSERT(sizeof(JSObject) % sizeof(js::Value) == 0,
                          "fixed slots after an object must be aligned");
    }

    JSObject() MOZ_DELETE;
    JSObject(const JSObject &other) MOZ_DELETE;
    void operator=(const JSObject &other) MOZ_DELETE;
};

/*
 * The only sensible way to compare JSObject with == is by identity. We use
 * const& instead of * as a syntactic way to assert non-null. This leads to an
 * abundance of address-of operators to identity. Hence this overload.
 */
static JS_ALWAYS_INLINE bool
operator==(const JSObject &lhs, const JSObject &rhs)
{
    return &lhs == &rhs;
}

static JS_ALWAYS_INLINE bool
operator!=(const JSObject &lhs, const JSObject &rhs)
{
    return &lhs != &rhs;
}

struct JSObject_Slots2 : JSObject { js::Value fslots[2]; };
struct JSObject_Slots4 : JSObject { js::Value fslots[4]; };
struct JSObject_Slots8 : JSObject { js::Value fslots[8]; };
struct JSObject_Slots12 : JSObject { js::Value fslots[12]; };
struct JSObject_Slots16 : JSObject { js::Value fslots[16]; };

#define JSSLOT_FREE(clasp)  JSCLASS_RESERVED_SLOTS(clasp)

class JSValueArray {
  public:
    jsval *array;
    size_t length;

    JSValueArray(jsval *v, size_t c) : array(v), length(c) {}
};

class ValueArray {
  public:
    js::Value *array;
    size_t length;

    ValueArray(js::Value *v, size_t c) : array(v), length(c) {}
};

/* For manipulating JSContext::sharpObjectMap. */
extern bool
js_EnterSharpObject(JSContext *cx, js::HandleObject obj, JSIdArray **idap, bool *alreadySeen, bool *isSharp);

extern void
js_LeaveSharpObject(JSContext *cx, JSIdArray **idap);

/*
 * Mark objects stored in map if GC happens between js_EnterSharpObject
 * and js_LeaveSharpObject. GC calls this when map->depth > 0.
 */
extern void
js_TraceSharpMap(JSTracer *trc, JSSharpObjectMap *map);

extern JSBool
js_HasOwnPropertyHelper(JSContext *cx, js::LookupGenericOp lookup, js::HandleObject obj,
                        js::HandleId id, js::MutableHandleValue rval);

extern JSBool
js_HasOwnProperty(JSContext *cx, js::LookupGenericOp lookup, js::HandleObject obj, js::HandleId id,
                  js::MutableHandleObject objp, js::MutableHandleShape propp);

extern JSBool
js_PropertyIsEnumerable(JSContext *cx, js::HandleObject obj, js::HandleId id, js::Value *vp);

extern JSFunctionSpec object_methods[];
extern JSFunctionSpec object_static_methods[];

namespace js {

bool
IsStandardClassResolved(JSObject *obj, js::Class *clasp);

void
MarkStandardClassInitializedNoProto(JSObject *obj, js::Class *clasp);

} /* namespace js */

/*
 * Select Object.prototype method names shared between jsapi.cpp and jsobj.cpp.
 */
extern const char js_watch_str[];
extern const char js_unwatch_str[];
extern const char js_hasOwnProperty_str[];
extern const char js_isPrototypeOf_str[];
extern const char js_propertyIsEnumerable_str[];

#ifdef OLD_GETTER_SETTER_METHODS
extern const char js_defineGetter_str[];
extern const char js_defineSetter_str[];
extern const char js_lookupGetter_str[];
extern const char js_lookupSetter_str[];
#endif

extern JSBool
js_PopulateObject(JSContext *cx, js::HandleObject newborn, js::HandleObject props);

/*
 * Fast access to immutable standard objects (constructors and prototypes).
 */
extern bool
js_GetClassObject(JSContext *cx, js::RawObject obj, JSProtoKey key,
                  js::MutableHandleObject objp);

/*
 * Determine if the given object is a prototype for a standard class. If so,
 * return the associated JSProtoKey. If not, return JSProto_Null.
 */
extern JSProtoKey
js_IdentifyClassPrototype(JSObject *obj);

/*
 * If protoKey is not JSProto_Null, then clasp is ignored. If protoKey is
 * JSProto_Null, clasp must non-null.
 */
bool
js_FindClassObject(JSContext *cx, JSProtoKey protoKey, js::MutableHandleValue vp,
                   js::Class *clasp = NULL);

// Specialized call for constructing |this| with a known function callee,
// and a known prototype.
extern JSObject *
js_CreateThisForFunctionWithProto(JSContext *cx, js::HandleObject callee, JSObject *proto);

// Specialized call for constructing |this| with a known function callee.
extern JSObject *
js_CreateThisForFunction(JSContext *cx, js::HandleObject callee, bool newType);

// Generic call for constructing |this|.
extern JSObject *
js_CreateThis(JSContext *cx, js::Class *clasp, js::HandleObject callee);

/*
 * Find or create a property named by id in obj's scope, with the given getter
 * and setter, slot, attributes, and other members.
 */
extern js::Shape *
js_AddNativeProperty(JSContext *cx, js::HandleObject obj, jsid id,
                     JSPropertyOp getter, JSStrictPropertyOp setter, uint32_t slot,
                     unsigned attrs, unsigned flags, int shortid);

extern JSBool
js_DefineOwnProperty(JSContext *cx, js::HandleObject obj, js::HandleId id,
                     const js::Value &descriptor, JSBool *bp);

namespace js {

/*
 * Flags for the defineHow parameter of js_DefineNativeProperty.
 */
const unsigned DNP_CACHE_RESULT = 1;   /* an interpreter call from JSOP_INITPROP */
const unsigned DNP_DONT_PURGE   = 2;   /* suppress js_PurgeScopeChain */
const unsigned DNP_UNQUALIFIED  = 4;   /* Unqualified property set.  Only used in
                                       the defineHow argument of
                                       js_SetPropertyHelper. */
const unsigned DNP_SKIP_TYPE    = 8;   /* Don't update type information */

/*
 * Return successfully added or changed shape or NULL on error.
 */
extern Shape *
DefineNativeProperty(JSContext *cx, HandleObject obj, HandleId id, HandleValue value,
                     PropertyOp getter, StrictPropertyOp setter, unsigned attrs,
                     unsigned flags, int shortid, unsigned defineHow = 0);

inline Shape *
DefineNativeProperty(JSContext *cx, HandleObject obj, PropertyName *name, HandleValue value,
                     PropertyOp getter, StrictPropertyOp setter, unsigned attrs,
                     unsigned flags, int shortid, unsigned defineHow = 0)
{
    Rooted<jsid> id(cx, NameToId(name));
    return DefineNativeProperty(cx, obj, id, value, getter, setter, attrs, flags,
                                shortid, defineHow);
}

/*
 * Specialized subroutine that allows caller to preset JSRESOLVE_* flags.
 */
extern bool
LookupPropertyWithFlags(JSContext *cx, HandleObject obj, HandleId id, unsigned flags,
                        js::MutableHandleObject objp, js::MutableHandleShape propp);

inline bool
LookupPropertyWithFlags(JSContext *cx, HandleObject obj, PropertyName *name, unsigned flags,
                        js::MutableHandleObject objp, js::MutableHandleShape propp)
{
    Rooted<jsid> id(cx, NameToId(name));
    return LookupPropertyWithFlags(cx, obj, id, flags, objp, propp);
}

/*
 * Call the [[DefineOwnProperty]] internal method of obj.
 *
 * If obj is an array, this follows ES5 15.4.5.1.
 * If obj is any other native object, this follows ES5 8.12.9.
 * If obj is a proxy, this calls the proxy handler's defineProperty method.
 * Otherwise, this reports an error and returns false.
 */
extern bool
DefineProperty(JSContext *cx, js::HandleObject obj,
               js::HandleId id, const PropDesc &desc, bool throwError,
               bool *rval);

/*
 * Read property descriptors from props, as for Object.defineProperties. See
 * ES5 15.2.3.7 steps 3-5.
 */
extern bool
ReadPropertyDescriptors(JSContext *cx, HandleObject props, bool checkAccessors,
                        AutoIdVector *ids, AutoPropDescArrayRooter *descs);

/*
 * Constant to pass to js_LookupPropertyWithFlags to infer bits from current
 * bytecode.
 */
static const unsigned RESOLVE_INFER = 0xffff;

/* Read the name using a dynamic lookup on the scopeChain. */
extern bool
LookupName(JSContext *cx, HandlePropertyName name, HandleObject scopeChain,
           MutableHandleObject objp, MutableHandleObject pobjp, MutableHandleShape propp);

/*
 * Like LookupName except returns the global object if 'name' is not found in
 * any preceding non-global scope.
 *
 * Additionally, pobjp and propp are not needed by callers so they are not
 * returned.
 */
extern bool
LookupNameWithGlobalDefault(JSContext *cx, HandlePropertyName name, HandleObject scopeChain,
                            MutableHandleObject objp);

}

extern JSObject *
js_FindVariableScope(JSContext *cx, JSFunction **funp);

/* JSGET_CACHE_RESULT is the analogue of DNP_CACHE_RESULT for GetMethod. */
const unsigned JSGET_CACHE_RESULT = 1; // from a caching interpreter opcode

/*
 * NB: js_NativeGet and js_NativeSet are called with the scope containing shape
 * (pobj's scope for Get, obj's for Set) locked, and on successful return, that
 * scope is again locked.  But on failure, both functions return false with the
 * scope containing shape unlocked.
 */
extern JSBool
js_NativeGet(JSContext *cx, js::Handle<JSObject*> obj, js::Handle<JSObject*> pobj,
             js::Shape *shape, unsigned getHow, js::Value *vp);

extern JSBool
js_NativeSet(JSContext *cx, js::Handle<JSObject*> obj, js::Handle<JSObject*> receiver,
             js::Shape *shape, bool added, bool strict, js::Value *vp);

namespace js {

bool
GetPropertyHelper(JSContext *cx, HandleObject obj, HandleId id, uint32_t getHow, MutableHandleValue vp);

inline bool
GetPropertyHelper(JSContext *cx, HandleObject obj, PropertyName *name, uint32_t getHow, MutableHandleValue vp)
{
    RootedId id(cx, NameToId(name));
    return GetPropertyHelper(cx, obj, id, getHow, vp);
}

bool
GetOwnPropertyDescriptor(JSContext *cx, HandleObject obj, HandleId id, PropertyDescriptor *desc);

bool
GetOwnPropertyDescriptor(JSContext *cx, HandleObject obj, HandleId id, Value *vp);

bool
NewPropertyDescriptorObject(JSContext *cx, const PropertyDescriptor *desc, Value *vp);

extern JSBool
GetMethod(JSContext *cx, HandleObject obj, HandleId id, unsigned getHow, MutableHandleValue vp);

inline bool
GetMethod(JSContext *cx, HandleObject obj, PropertyName *name, unsigned getHow, MutableHandleValue vp)
{
    Rooted<jsid> id(cx, NameToId(name));
    return GetMethod(cx, obj, id, getHow, vp);
}

/*
 * If obj has an already-resolved data property for id, return true and
 * store the property value in *vp.
 */
extern bool
HasDataProperty(JSContext *cx, HandleObject obj, jsid id, Value *vp);

inline bool
HasDataProperty(JSContext *cx, HandleObject obj, PropertyName *name, Value *vp)
{
    return HasDataProperty(cx, obj, NameToId(name), vp);
}

extern JSBool
CheckAccess(JSContext *cx, JSObject *obj, HandleId id, JSAccessMode mode,
            js::Value *vp, unsigned *attrsp);

} /* namespace js */

extern bool
js_IsDelegate(JSContext *cx, JSObject *obj, const js::Value &v);

/*
 * Wrap boolean, number or string as Boolean, Number or String object.
 * *vp must not be an object, null or undefined.
 */
extern JSBool
js_PrimitiveToObject(JSContext *cx, js::Value *vp);

extern JSBool
js_ValueToObjectOrNull(JSContext *cx, const js::Value &v, JS::MutableHandleObject objp);

/* Throws if v could not be converted to an object. */
extern JSObject *
js_ValueToNonNullObject(JSContext *cx, const js::Value &v);

namespace js {

/*
 * Invokes the ES5 ToObject algorithm on vp, returning the result. If vp might
 * already be an object, use ToObject. reportCantConvert controls how null and
 * undefined errors are reported.
 */
extern JSObject *
ToObjectSlow(JSContext *cx, HandleValue vp, bool reportScanStack);

/* For object conversion in e.g. native functions. */
JS_ALWAYS_INLINE JSObject *
ToObject(JSContext *cx, HandleValue vp)
{
    if (vp.isObject())
        return &vp.toObject();
    return ToObjectSlow(cx, vp, false);
}

/* For converting stack values to objects. */
JS_ALWAYS_INLINE JSObject *
ToObjectFromStack(JSContext *cx, HandleValue vp)
{
    if (vp.isObject())
        return &vp.toObject();
    return ToObjectSlow(cx, vp, true);
}

} /* namespace js */

extern void
js_GetObjectSlotName(JSTracer *trc, char *buf, size_t bufsize);

extern bool
js_ClearNative(JSContext *cx, JSObject *obj);

extern JSBool
js_ReportGetterOnlyAssignment(JSContext *cx);

extern unsigned
js_InferFlags(JSContext *cx, unsigned defaultFlags);

/* Object constructor native. Exposed only so the JIT can know its address. */
JSBool
js_Object(JSContext *cx, unsigned argc, js::Value *vp);

/*
 * If protoKey is not JSProto_Null, then clasp is ignored. If protoKey is
 * JSProto_Null, clasp must non-null.
 *
 * If protoKey is constant and scope is non-null, use GlobalObject's prototype
 * methods instead.
 */
extern JS_FRIEND_API(bool)
js_GetClassPrototype(JSContext *cx, JSProtoKey protoKey, js::MutableHandleObject protop,
                     js::Class *clasp = NULL);

namespace js {

extern bool
SetProto(JSContext *cx, HandleObject obj, HandleObject proto, bool checkForCycles);

extern JSString *
obj_toStringHelper(JSContext *cx, JSObject *obj);

extern JSObject *
NonNullObject(JSContext *cx, const Value &v);

extern const char *
InformalValueTypeName(const Value &v);

inline void
DestroyIdArray(FreeOp *fop, JSIdArray *ida);

extern bool
GetFirstArgumentAsObject(JSContext *cx, unsigned argc, Value *vp, const char *method,
                         MutableHandleObject objp);

/* Helpers for throwing. These always return false. */
extern bool
Throw(JSContext *cx, jsid id, unsigned errorNumber);

extern bool
Throw(JSContext *cx, JSObject *obj, unsigned errorNumber);

/*
 * Helper function. To approximate a call to the [[DefineOwnProperty]] internal
 * method described in ES5, first call this, then call JS_DefinePropertyById.
 *
 * JS_DefinePropertyById by itself does not enforce the invariants on
 * non-configurable properties when obj->isNative(). This function performs the
 * relevant checks (specified in ES5 8.12.9 [[DefineOwnProperty]] steps 1-11),
 * but only if obj is native.
 *
 * The reason for the messiness here is that ES5 uses [[DefineOwnProperty]] as
 * a sort of extension point, but there is no hook in js::Class,
 * js::ProxyHandler, or the JSAPI with precisely the right semantics for it.
 */
extern bool
CheckDefineProperty(JSContext *cx, HandleObject obj, HandleId id, HandleValue value,
                    PropertyOp getter, StrictPropertyOp setter, unsigned attrs);

}  /* namespace js */

#endif /* jsobj_h___ */
