/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsprf.h"
#include "jsscope.h"
#include "jsstr.h"
#include "jsxml.h"

#include "gc/Marking.h"
#include "methodjit/MethodJIT.h"

#include "jsobjinlines.h"
#include "jsscopeinlines.h"

#include "vm/String-inl.h"

void * const js::NullPtr::constNullValue = NULL;

/*
 * There are two mostly separate mark paths. The first is a fast path used
 * internally in the GC. The second is a slow path used for root marking and
 * for API consumers like the cycle collector or Class::trace implementations.
 *
 * The fast path uses explicit stacks. The basic marking process during a GC is
 * that all roots are pushed on to a mark stack, and then each item on the
 * stack is scanned (possibly pushing more stuff) until the stack is empty.
 *
 * PushMarkStack pushes a GC thing onto the mark stack. In some cases (shapes
 * or strings) it eagerly marks the object rather than pushing it. Popping and
 * scanning is done by the processMarkStackTop method. For efficiency reasons
 * like tail recursion elimination that method also implements the scanning of
 * objects. For other GC things it uses helper methods.
 *
 * Most of the marking code outside Marking.cpp uses functions like MarkObject,
 * MarkString, etc. These functions check if an object is in the compartment
 * currently being GCed. If it is, they call PushMarkStack. Roots are pushed
 * this way as well as pointers traversed inside trace hooks (for things like
 * PropertyIteratorObjects). It is always valid to call a MarkX function
 * instead of PushMarkStack, although it may be slower.
 *
 * The MarkX functions also handle non-GC object traversal. In this case, they
 * call a callback for each object visited. This is a recursive process; the
 * mark stacks are not involved. These callbacks may ask for the outgoing
 * pointers to be visited. Eventually, this leads to the MarkChildren functions
 * being called. These functions duplicate much of the functionality of
 * scanning functions, but they don't push onto an explicit stack.
 */

namespace js {
namespace gc {

#if JS_HAS_XML_SUPPORT
static inline void
PushMarkStack(GCMarker *gcmarker, JSXML *thing);
#endif

static inline void
PushMarkStack(GCMarker *gcmarker, JSObject *thing);

static inline void
PushMarkStack(GCMarker *gcmarker, JSFunction *thing);

static inline void
PushMarkStack(GCMarker *gcmarker, JSScript *thing);

static inline void
PushMarkStack(GCMarker *gcmarker, Shape *thing);

static inline void
PushMarkStack(GCMarker *gcmarker, JSString *thing);

static inline void
PushMarkStack(GCMarker *gcmarker, types::TypeObject *thing);

/*** Object Marking ***/

template<typename T>
static inline void
CheckMarkedThing(JSTracer *trc, T *thing)
{
    JS_ASSERT(trc);
    JS_ASSERT(thing);
    JS_ASSERT(thing->compartment());
    JS_ASSERT(thing->compartment()->rt == trc->runtime);
    JS_ASSERT(trc->debugPrinter || trc->debugPrintArg);

    DebugOnly<JSRuntime *> rt = trc->runtime;

    JS_ASSERT_IF(thing->compartment()->requireGCTracer(), IS_GC_MARKING_TRACER(trc));

    JS_ASSERT(thing->isAligned());

    JS_ASSERT_IF(rt->gcStrictCompartmentChecking,
                 thing->compartment()->isCollecting() ||
                 thing->compartment() == rt->atomsCompartment);
}

template<typename T>
void
MarkInternal(JSTracer *trc, T **thingp)
{
    JS_ASSERT(thingp);
    T *thing = *thingp;

    CheckMarkedThing(trc, thing);

    /*
     * Don't mark things outside a compartment if we are in a per-compartment
     * GC.
     */
    if (!trc->callback) {
        if (thing->compartment()->isCollecting())
            PushMarkStack(static_cast<GCMarker *>(trc), thing);
    } else {
        trc->callback(trc, (void **)thingp, GetGCThingTraceKind(thing));
        JS_SET_TRACING_LOCATION(trc, NULL);
    }

    trc->debugPrinter = NULL;
    trc->debugPrintArg = NULL;
}

#define JS_ROOT_MARKING_ASSERT(trc)                                     \
    JS_ASSERT_IF(IS_GC_MARKING_TRACER(trc),                             \
                 trc->runtime->gcIncrementalState == NO_INCREMENTAL ||  \
                 trc->runtime->gcIncrementalState == MARK_ROOTS);


template <typename T>
static void
MarkUnbarriered(JSTracer *trc, T **thingp, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkInternal(trc, thingp);
}

template <typename T>
static void
Mark(JSTracer *trc, EncapsulatedPtr<T> *thing, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkInternal(trc, thing->unsafeGet());
}

template <typename T>
static void
MarkRoot(JSTracer *trc, T **thingp, const char *name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    JS_SET_TRACING_NAME(trc, name);
    MarkInternal(trc, thingp);
}

template <typename T>
static void
MarkRange(JSTracer *trc, size_t len, HeapPtr<T> *vec, const char *name)
{
    for (size_t i = 0; i < len; ++i) {
        if (vec[i].get()) {
            JS_SET_TRACING_INDEX(trc, name, i);
            MarkInternal(trc, vec[i].unsafeGet());
        }
    }
}

template <typename T>
static void
MarkRootRange(JSTracer *trc, size_t len, T **vec, const char *name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    for (size_t i = 0; i < len; ++i) {
        if (vec[i]) {
            JS_SET_TRACING_INDEX(trc, name, i);
            MarkInternal(trc, &vec[i]);
        }
    }
}

template <typename T>
static bool
IsMarked(T **thingp)
{
    JS_ASSERT(thingp);
    JS_ASSERT(*thingp);
    if (!(*thingp)->compartment()->isCollecting())
        return true;
    return (*thingp)->isMarked();
}

#define DeclMarkerImpl(base, type)                                                                \
void                                                                                              \
Mark##base(JSTracer *trc, EncapsulatedPtr<type> *thing, const char *name)                         \
{                                                                                                 \
    Mark<type>(trc, thing, name);                                                                 \
}                                                                                                 \
                                                                                                  \
void                                                                                              \
Mark##base##Root(JSTracer *trc, type **thingp, const char *name)                                  \
{                                                                                                 \
    MarkRoot<type>(trc, thingp, name);                                                            \
}                                                                                                 \
                                                                                                  \
void                                                                                              \
Mark##base##Unbarriered(JSTracer *trc, type **thingp, const char *name)                           \
{                                                                                                 \
    MarkUnbarriered<type>(trc, thingp, name);                                                     \
}                                                                                                 \
                                                                                                  \
void Mark##base##Range(JSTracer *trc, size_t len, HeapPtr<type> *vec, const char *name)           \
{                                                                                                 \
    MarkRange<type>(trc, len, vec, name);                                                         \
}                                                                                                 \
                                                                                                  \
void Mark##base##RootRange(JSTracer *trc, size_t len, type **vec, const char *name)               \
{                                                                                                 \
    MarkRootRange<type>(trc, len, vec, name);                                                     \
}                                                                                                 \
                                                                                                  \
bool Is##base##Marked(type **thingp)                                                              \
{                                                                                                 \
    return IsMarked<type>(thingp);                                                                \
}                                                                                                 \
                                                                                                  \
bool Is##base##Marked(EncapsulatedPtr<type> *thingp)                                              \
{                                                                                                 \
    return IsMarked<type>(thingp->unsafeGet());                                                   \
}

DeclMarkerImpl(BaseShape, BaseShape)
DeclMarkerImpl(BaseShape, UnownedBaseShape)
DeclMarkerImpl(Object, ArgumentsObject)
DeclMarkerImpl(Object, DebugScopeObject)
DeclMarkerImpl(Object, GlobalObject)
DeclMarkerImpl(Object, JSObject)
DeclMarkerImpl(Object, JSFunction)
DeclMarkerImpl(Object, ScopeObject)
DeclMarkerImpl(Script, JSScript)
DeclMarkerImpl(Shape, Shape)
DeclMarkerImpl(String, JSAtom)
DeclMarkerImpl(String, JSString)
DeclMarkerImpl(String, JSFlatString)
DeclMarkerImpl(String, JSLinearString)
DeclMarkerImpl(String, PropertyName)
DeclMarkerImpl(TypeObject, types::TypeObject)
#if JS_HAS_XML_SUPPORT
DeclMarkerImpl(XML, JSXML)
#endif

/*** Externally Typed Marking ***/

void
MarkKind(JSTracer *trc, void **thingp, JSGCTraceKind kind)
{
    JS_ASSERT(thingp);
    JS_ASSERT(*thingp);
    JS_ASSERT(kind == GetGCThingTraceKind(*thingp));
    switch (kind) {
      case JSTRACE_OBJECT:
        MarkInternal(trc, reinterpret_cast<JSObject **>(thingp));
        break;
      case JSTRACE_STRING:
        MarkInternal(trc, reinterpret_cast<JSString **>(thingp));
        break;
      case JSTRACE_SCRIPT:
        MarkInternal(trc, reinterpret_cast<JSScript **>(thingp));
        break;
      case JSTRACE_SHAPE:
        MarkInternal(trc, reinterpret_cast<Shape **>(thingp));
        break;
      case JSTRACE_BASE_SHAPE:
        MarkInternal(trc, reinterpret_cast<BaseShape **>(thingp));
        break;
      case JSTRACE_TYPE_OBJECT:
        MarkInternal(trc, reinterpret_cast<types::TypeObject **>(thingp));
        break;
#if JS_HAS_XML_SUPPORT
      case JSTRACE_XML:
        MarkInternal(trc, reinterpret_cast<JSXML **>(thingp));
        break;
#endif
    }
}

void
MarkGCThingRoot(JSTracer *trc, void **thingp, const char *name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    JS_SET_TRACING_NAME(trc, name);
    JS_ASSERT(thingp);
    if (!*thingp)
        return;
    MarkKind(trc, thingp, GetGCThingTraceKind(*thingp));
}

/*** ID Marking ***/

static inline void
MarkIdInternal(JSTracer *trc, jsid *id)
{
    if (JSID_IS_STRING(*id)) {
        JSString *str = JSID_TO_STRING(*id);
        JS_SET_TRACING_LOCATION(trc, (void *)id);
        MarkInternal(trc, &str);
        *id = NON_INTEGER_ATOM_TO_JSID(reinterpret_cast<JSAtom *>(str));
    } else if (JS_UNLIKELY(JSID_IS_OBJECT(*id))) {
        JSObject *obj = JSID_TO_OBJECT(*id);
        JS_SET_TRACING_LOCATION(trc, (void *)id);
        MarkInternal(trc, &obj);
        *id = OBJECT_TO_JSID(obj);
    } else {
        /* Unset realLocation manually if we do not call MarkInternal. */
        JS_SET_TRACING_LOCATION(trc, NULL);
    }
}

void
MarkId(JSTracer *trc, EncapsulatedId *id, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkIdInternal(trc, id->unsafeGet());
}

void
MarkIdRoot(JSTracer *trc, jsid *id, const char *name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    JS_SET_TRACING_NAME(trc, name);
    MarkIdInternal(trc, id);
}

void
MarkIdUnbarriered(JSTracer *trc, jsid *id, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkIdInternal(trc, id);
}

void
MarkIdRange(JSTracer *trc, size_t len, HeapId *vec, const char *name)
{
    for (size_t i = 0; i < len; ++i) {
        JS_SET_TRACING_INDEX(trc, name, i);
        MarkIdInternal(trc, vec[i].unsafeGet());
    }
}

void
MarkIdRootRange(JSTracer *trc, size_t len, jsid *vec, const char *name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    for (size_t i = 0; i < len; ++i) {
        JS_SET_TRACING_INDEX(trc, name, i);
        MarkIdInternal(trc, &vec[i]);
    }
}

/*** Value Marking ***/

static inline void
MarkValueInternal(JSTracer *trc, Value *v)
{
    if (v->isMarkable()) {
        JS_ASSERT(v->toGCThing());
        void *thing = v->toGCThing();
        JS_SET_TRACING_LOCATION(trc, (void *)v);
        MarkKind(trc, &thing, v->gcKind());
        if (v->isString())
            v->setString((JSString *)thing);
        else
            v->setObjectOrNull((JSObject *)thing);
    } else {
        /* Unset realLocation manually if we do not call MarkInternal. */
        JS_SET_TRACING_LOCATION(trc, NULL);
    }
}

void
MarkValue(JSTracer *trc, EncapsulatedValue *v, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkValueInternal(trc, v->unsafeGet());
}

void
MarkValueRoot(JSTracer *trc, Value *v, const char *name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    JS_SET_TRACING_NAME(trc, name);
    MarkValueInternal(trc, v);
}

void
MarkTypeRoot(JSTracer *trc, types::Type *v, const char *name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    JS_SET_TRACING_NAME(trc, name);
    if (v->isSingleObject()) {
        JSObject *obj = v->singleObject();
        MarkInternal(trc, &obj);
        *v = types::Type::ObjectType(obj);
    } else if (v->isTypeObject()) {
        types::TypeObject *typeObj = v->typeObject();
        MarkInternal(trc, &typeObj);
        *v = types::Type::ObjectType(typeObj);
    }
}

void
MarkValueRange(JSTracer *trc, size_t len, EncapsulatedValue *vec, const char *name)
{
    for (size_t i = 0; i < len; ++i) {
        JS_SET_TRACING_INDEX(trc, name, i);
        MarkValueInternal(trc, vec[i].unsafeGet());
    }
}

void
MarkValueRootRange(JSTracer *trc, size_t len, Value *vec, const char *name)
{
    JS_ROOT_MARKING_ASSERT(trc);
    for (size_t i = 0; i < len; ++i) {
        JS_SET_TRACING_INDEX(trc, name, i);
        MarkValueInternal(trc, &vec[i]);
    }
}

bool
IsValueMarked(Value *v)
{
    JS_ASSERT(v->isMarkable());
    bool rv;
    if (v->isString()) {
        JSString *str = (JSString *)v->toGCThing();
        rv = IsMarked<JSString>(&str);
        v->setString(str);
    } else {
        JSObject *obj = (JSObject *)v->toGCThing();
        rv = IsMarked<JSObject>(&obj);
        v->setObject(*obj);
    }
    return rv;
}

/*** Slot Marking ***/

void
MarkSlot(JSTracer *trc, HeapSlot *s, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkValueInternal(trc, s->unsafeGet());
}

void
MarkArraySlots(JSTracer *trc, size_t len, HeapSlot *vec, const char *name)
{
    for (size_t i = 0; i < len; ++i) {
        JS_SET_TRACING_INDEX(trc, name, i);
        MarkValueInternal(trc, vec[i].unsafeGet());
    }
}

void
MarkObjectSlots(JSTracer *trc, JSObject *obj, uint32_t start, uint32_t nslots)
{
    JS_ASSERT(obj->isNative());
    for (uint32_t i = start; i < (start + nslots); ++i) {
        JS_SET_TRACING_DETAILS(trc, js_GetObjectSlotName, obj, i);
        MarkValueInternal(trc, obj->nativeGetSlotRef(i).unsafeGet());
    }
}

void
MarkCrossCompartmentObjectUnbarriered(JSTracer *trc, JSObject **obj, const char *name)
{
    if (IS_GC_MARKING_TRACER(trc) && !(*obj)->compartment()->isCollecting())
        return;

    MarkObjectUnbarriered(trc, obj, name);
}

void
MarkCrossCompartmentScriptUnbarriered(JSTracer *trc, JSScript **script, const char *name)
{
    if (IS_GC_MARKING_TRACER(trc) && !(*script)->compartment()->isCollecting())
        return;

    MarkScriptUnbarriered(trc, script, name);
}

void
MarkCrossCompartmentSlot(JSTracer *trc, HeapSlot *s, const char *name)
{
    if (s->isMarkable()) {
        Cell *cell = (Cell *)s->toGCThing();
        if (IS_GC_MARKING_TRACER(trc) && !cell->compartment()->isCollecting())
            return;

        MarkSlot(trc, s, name);
    }
}

/*** Special Marking ***/

void
MarkObject(JSTracer *trc, HeapPtr<GlobalObject, JSScript *> *thingp, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkInternal(trc, thingp->unsafeGet());
}

void
MarkValueUnbarriered(JSTracer *trc, Value *v, const char *name)
{
    JS_SET_TRACING_NAME(trc, name);
    MarkValueInternal(trc, v);
}

bool
IsCellMarked(Cell **thingp)
{
    return IsMarked<Cell>(thingp);
}

/*** Push Mark Stack ***/

#define JS_COMPARTMENT_ASSERT(rt, thing)                                \
    JS_ASSERT((thing)->compartment()->isCollecting())

#define JS_COMPARTMENT_ASSERT_STR(rt, thing)                            \
    JS_ASSERT((thing)->compartment()->isCollecting() ||                 \
              (thing)->compartment() == (rt)->atomsCompartment);

#if JS_HAS_XML_SUPPORT
static void
PushMarkStack(GCMarker *gcmarker, JSXML *thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime, thing);

    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        gcmarker->pushXML(thing);
}
#endif

static void
PushMarkStack(GCMarker *gcmarker, JSObject *thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime, thing);

    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        gcmarker->pushObject(thing);
}

static void
PushMarkStack(GCMarker *gcmarker, JSFunction *thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime, thing);

    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        gcmarker->pushObject(thing);
}

static void
PushMarkStack(GCMarker *gcmarker, types::TypeObject *thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime, thing);

    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        gcmarker->pushType(thing);
}

static void
MarkChildren(JSTracer *trc, JSScript *script);

static void
PushMarkStack(GCMarker *gcmarker, JSScript *thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime, thing);

    /*
     * We mark scripts directly rather than pushing on the stack as they can
     * refer to other scripts only indirectly (like via nested functions) and
     * we cannot get to deep recursion.
     */
    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        MarkChildren(gcmarker, thing);
}

static void
ScanShape(GCMarker *gcmarker, Shape *shape);

static void
PushMarkStack(GCMarker *gcmarker, Shape *thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime, thing);

    /* We mark shapes directly rather than pushing on the stack. */
    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        ScanShape(gcmarker, thing);
}

static inline void
ScanBaseShape(GCMarker *gcmarker, BaseShape *base);

static void
PushMarkStack(GCMarker *gcmarker, BaseShape *thing)
{
    JS_COMPARTMENT_ASSERT(gcmarker->runtime, thing);

    /* We mark base shapes directly rather than pushing on the stack. */
    if (thing->markIfUnmarked(gcmarker->getMarkColor()))
        ScanBaseShape(gcmarker, thing);
}

static void
ScanShape(GCMarker *gcmarker, Shape *shape)
{
  restart:
    PushMarkStack(gcmarker, shape->base());

    const EncapsulatedId &id = shape->propidRef();
    if (JSID_IS_STRING(id))
        PushMarkStack(gcmarker, JSID_TO_STRING(id));
    else if (JS_UNLIKELY(JSID_IS_OBJECT(id)))
        PushMarkStack(gcmarker, JSID_TO_OBJECT(id));

    shape = shape->previous();
    if (shape && shape->markIfUnmarked(gcmarker->getMarkColor()))
        goto restart;
}

static inline void
ScanBaseShape(GCMarker *gcmarker, BaseShape *base)
{
    base->assertConsistency();

    if (base->hasGetterObject())
        PushMarkStack(gcmarker, base->getterObject());

    if (base->hasSetterObject())
        PushMarkStack(gcmarker, base->setterObject());

    if (JSObject *parent = base->getObjectParent()) {
        PushMarkStack(gcmarker, parent);
    } else if (GlobalObject *global = base->compartment()->maybeGlobal()) {
        PushMarkStack(gcmarker, global);
    }

    /*
     * All children of the owned base shape are consistent with its
     * unowned one, thus we do not need to trace through children of the
     * unowned base shape.
     */
    if (base->isOwned()) {
        UnownedBaseShape *unowned = base->baseUnowned();
        JS_ASSERT(base->compartment() == unowned->compartment());
        unowned->markIfUnmarked(gcmarker->getMarkColor());
    }
}

static inline void
ScanLinearString(GCMarker *gcmarker, JSLinearString *str)
{
    JS_COMPARTMENT_ASSERT_STR(gcmarker->runtime, str);
    JS_ASSERT(str->isMarked());

    /*
     * Add extra asserts to confirm the static type to detect incorrect string
     * mutations.
     */
    JS_ASSERT(str->JSString::isLinear());
    while (str->hasBase()) {
        str = str->base();
        JS_ASSERT(str->JSString::isLinear());
        JS_COMPARTMENT_ASSERT_STR(gcmarker->runtime, str);
        if (!str->markIfUnmarked())
            break;
    }
}

/*
 * The function tries to scan the whole rope tree using the marking stack as
 * temporary storage. If that becomes full, the unscanned ropes are added to
 * the delayed marking list. When the function returns, the marking stack is
 * at the same depth as it was on entry. This way we avoid using tags when
 * pushing ropes to the stack as ropes never leaks to other users of the
 * stack. This also assumes that a rope can only point to other ropes or
 * linear strings, it cannot refer to GC things of other types.
 */
static void
ScanRope(GCMarker *gcmarker, JSRope *rope)
{
    ptrdiff_t savedPos = gcmarker->stack.position();
    for (;;) {
        JS_ASSERT(GetGCThingTraceKind(rope) == JSTRACE_STRING);
        JS_ASSERT(rope->JSString::isRope());
        JS_COMPARTMENT_ASSERT_STR(gcmarker->runtime, rope);
        JS_ASSERT(rope->isMarked());
        JSRope *next = NULL;

        JSString *right = rope->rightChild();
        if (right->markIfUnmarked()) {
            if (right->isLinear())
                ScanLinearString(gcmarker, &right->asLinear());
            else
                next = &right->asRope();
        }

        JSString *left = rope->leftChild();
        if (left->markIfUnmarked()) {
            if (left->isLinear()) {
                ScanLinearString(gcmarker, &left->asLinear());
            } else {
                /*
                 * When both children are ropes, set aside the right one to
                 * scan it later.
                 */
                if (next && !gcmarker->stack.push(reinterpret_cast<uintptr_t>(next)))
                    gcmarker->delayMarkingChildren(next);
                next = &left->asRope();
            }
        }
        if (next) {
            rope = next;
        } else if (savedPos != gcmarker->stack.position()) {
            JS_ASSERT(savedPos < gcmarker->stack.position());
            rope = reinterpret_cast<JSRope *>(gcmarker->stack.pop());
        } else {
            break;
        }
    }
    JS_ASSERT(savedPos == gcmarker->stack.position());
 }

static inline void
ScanString(GCMarker *gcmarker, JSString *str)
{
    if (str->isLinear())
        ScanLinearString(gcmarker, &str->asLinear());
    else
        ScanRope(gcmarker, &str->asRope());
}

static inline void
PushMarkStack(GCMarker *gcmarker, JSString *str)
{
    JS_COMPARTMENT_ASSERT_STR(gcmarker->runtime, str);

    /*
     * As string can only refer to other strings we fully scan its GC graph
     * using the explicit stack when navigating the rope tree to avoid
     * dealing with strings on the stack in drainMarkStack.
     */
    if (str->markIfUnmarked())
        ScanString(gcmarker, str);
}

void
MarkChildren(JSTracer *trc, JSObject *obj)
{
    obj->markChildren(trc);
}

static void
MarkChildren(JSTracer *trc, JSString *str)
{
    if (str->hasBase())
        str->markBase(trc);
    else if (str->isRope())
        str->asRope().markChildren(trc);
}

static void
MarkChildren(JSTracer *trc, JSScript *script)
{
    script->markChildren(trc);
}

static void
MarkChildren(JSTracer *trc, Shape *shape)
{
    shape->markChildren(trc);
}

static void
MarkChildren(JSTracer *trc, BaseShape *base)
{
    base->markChildren(trc);
}

/*
 * This function is used by the cycle collector to trace through the
 * children of a BaseShape (and its baseUnowned(), if any). The cycle
 * collector does not directly care about BaseShapes, so only the
 * getter, setter, and parent are marked. Furthermore, the parent is
 * marked only if it isn't the same as prevParent, which will be
 * updated to the current shape's parent.
 */
inline void
MarkCycleCollectorChildren(JSTracer *trc, BaseShape *base, JSObject **prevParent)
{
    JS_ASSERT(base);

    /*
     * The cycle collector does not need to trace unowned base shapes,
     * as they have the same getter, setter and parent as the original
     * base shape.
     */
    base->assertConsistency();

    if (base->hasGetterObject()) {
        JSObject *tmp = base->getterObject();
        MarkObjectUnbarriered(trc, &tmp, "getter");
        JS_ASSERT(tmp == base->getterObject());
    }

    if (base->hasSetterObject()) {
        JSObject *tmp = base->setterObject();
        MarkObjectUnbarriered(trc, &tmp, "setter");
        JS_ASSERT(tmp == base->setterObject());
    }

    JSObject *parent = base->getObjectParent();
    if (parent && parent != *prevParent) {
        MarkObjectUnbarriered(trc, &parent, "parent");
        JS_ASSERT(parent == base->getObjectParent());
        *prevParent = parent;
    }
}

/*
 * This function is used by the cycle collector to trace through a
 * shape. The cycle collector does not care about shapes or base
 * shapes, so those are not marked. Instead, any shapes or base shapes
 * that are encountered have their children marked. Stack space is
 * bounded. If two shapes in a row have the same parent pointer, the
 * parent pointer will only be marked once.
 */
void
MarkCycleCollectorChildren(JSTracer *trc, Shape *shape)
{
    JSObject *prevParent = NULL;
    do {
        MarkCycleCollectorChildren(trc, shape->base(), &prevParent);
        MarkId(trc, &shape->propidRef(), "propid");
        shape = shape->previous();
    } while (shape);
}

static void
ScanTypeObject(GCMarker *gcmarker, types::TypeObject *type)
{
    /* Don't mark properties for singletons. They'll be purged by the GC. */
    if (!type->singleton) {
        unsigned count = type->getPropertyCount();
        for (unsigned i = 0; i < count; i++) {
            types::Property *prop = type->getProperty(i);
            if (prop && JSID_IS_STRING(prop->id))
                PushMarkStack(gcmarker, JSID_TO_STRING(prop->id));
        }
    }

    if (type->proto)
        PushMarkStack(gcmarker, type->proto);

    if (type->singleton && !type->lazy())
        PushMarkStack(gcmarker, type->singleton);

    if (type->newScript) {
        PushMarkStack(gcmarker, type->newScript->fun);
        PushMarkStack(gcmarker, type->newScript->shape);
    }

    if (type->interpretedFunction)
        PushMarkStack(gcmarker, type->interpretedFunction);
}

static void
MarkChildren(JSTracer *trc, types::TypeObject *type)
{
    unsigned count = type->getPropertyCount();
    for (unsigned i = 0; i < count; i++) {
        types::Property *prop = type->getProperty(i);
        if (prop)
            MarkId(trc, &prop->id, "type_prop");
    }

    if (type->proto)
        MarkObject(trc, &type->proto, "type_proto");

    if (type->singleton && !type->lazy())
        MarkObject(trc, &type->singleton, "type_singleton");

    if (type->newScript) {
        MarkObject(trc, &type->newScript->fun, "type_new_function");
        MarkShape(trc, &type->newScript->shape, "type_new_shape");
    }

    if (type->interpretedFunction)
        MarkObject(trc, &type->interpretedFunction, "type_function");
}

#if JS_HAS_XML_SUPPORT
static void
MarkChildren(JSTracer *trc, JSXML *xml)
{
    js_TraceXML(trc, xml);
}
#endif

template<typename T>
void
PushArenaTyped(GCMarker *gcmarker, ArenaHeader *aheader)
{
    for (CellIterUnderGC i(aheader); !i.done(); i.next())
        PushMarkStack(gcmarker, i.get<T>());
}

void
PushArena(GCMarker *gcmarker, ArenaHeader *aheader)
{
    switch (MapAllocToTraceKind(aheader->getAllocKind())) {
      case JSTRACE_OBJECT:
        PushArenaTyped<JSObject>(gcmarker, aheader);
        break;

      case JSTRACE_STRING:
        PushArenaTyped<JSString>(gcmarker, aheader);
        break;

      case JSTRACE_SCRIPT:
        PushArenaTyped<JSScript>(gcmarker, aheader);
        break;

      case JSTRACE_SHAPE:
        PushArenaTyped<js::Shape>(gcmarker, aheader);
        break;

      case JSTRACE_BASE_SHAPE:
        PushArenaTyped<js::BaseShape>(gcmarker, aheader);
        break;

      case JSTRACE_TYPE_OBJECT:
        PushArenaTyped<js::types::TypeObject>(gcmarker, aheader);
        break;

#if JS_HAS_XML_SUPPORT
      case JSTRACE_XML:
        PushArenaTyped<JSXML>(gcmarker, aheader);
        break;
#endif
    }
}

} /* namespace gc */

using namespace js::gc;

struct SlotArrayLayout
{
    union {
        HeapSlot *end;
        js::Class *clasp;
    };
    union {
        HeapSlot *start;
        uintptr_t index;
    };
    JSObject *obj;

    static void staticAsserts() {
        /* This should have the same layout as three mark stack items. */
        JS_STATIC_ASSERT(sizeof(SlotArrayLayout) == 3 * sizeof(uintptr_t));
    }
};

/*
 * During incremental GC, we return from drainMarkStack without having processed
 * the entire stack. At that point, JS code can run and reallocate slot arrays
 * that are stored on the stack. To prevent this from happening, we replace all
 * ValueArrayTag stack items with SavedValueArrayTag. In the latter, slots
 * pointers are replaced with slot indexes.
 *
 * We also replace the slot array end pointer (which can be derived from the obj
 * pointer) with the object's class. During JS executation, array slowification
 * can cause the layout of slots to change. We can observe that slowification
 * happened if the class changed; in that case, we completely rescan the array.
 */
void
GCMarker::saveValueRanges()
{
    for (uintptr_t *p = stack.tos; p > stack.stack; ) {
        uintptr_t tag = *--p & StackTagMask;
        if (tag == ValueArrayTag) {
            p -= 2;
            SlotArrayLayout *arr = reinterpret_cast<SlotArrayLayout *>(p);
            JSObject *obj = arr->obj;

            if (obj->getClass() == &ArrayClass) {
                HeapSlot *vp = obj->getDenseArrayElements();
                JS_ASSERT(arr->start >= vp &&
                          arr->end == vp + obj->getDenseArrayInitializedLength());
                arr->index = arr->start - vp;
            } else {
                HeapSlot *vp = obj->fixedSlots();
                unsigned nfixed = obj->numFixedSlots();
                if (arr->start == arr->end) {
                    arr->index = obj->slotSpan();
                } else if (arr->start >= vp && arr->start < vp + nfixed) {
                    JS_ASSERT(arr->end == vp + Min(nfixed, obj->slotSpan()));
                    arr->index = arr->start - vp;
                } else {
                    JS_ASSERT(arr->start >= obj->slots &&
                              arr->end == obj->slots + obj->slotSpan() - nfixed);
                    arr->index = (arr->start - obj->slots) + nfixed;
                }
            }
            arr->clasp = obj->getClass();
            p[2] |= SavedValueArrayTag;
        } else if (tag == SavedValueArrayTag) {
            p -= 2;
        }
    }
}

bool
GCMarker::restoreValueArray(JSObject *obj, void **vpp, void **endp)
{
    uintptr_t start = stack.pop();
    js::Class *clasp = reinterpret_cast<js::Class *>(stack.pop());

    JS_ASSERT(obj->getClass() == clasp ||
              (clasp == &ArrayClass && obj->getClass() == &SlowArrayClass));

    if (clasp == &ArrayClass) {
        if (obj->getClass() != &ArrayClass)
            return false;

        uint32_t initlen = obj->getDenseArrayInitializedLength();
        HeapSlot *vp = obj->getDenseArrayElements();
        if (start < initlen) {
            *vpp = vp + start;
            *endp = vp + initlen;
        } else {
            /* The object shrunk, in which case no scanning is needed. */
            *vpp = *endp = vp;
        }
    } else {
        HeapSlot *vp = obj->fixedSlots();
        unsigned nfixed = obj->numFixedSlots();
        unsigned nslots = obj->slotSpan();
        if (start < nslots) {
            if (start < nfixed) {
                *vpp = vp + start;
                *endp = vp + Min(nfixed, nslots);
            } else {
                *vpp = obj->slots + start - nfixed;
                *endp = obj->slots + nslots - nfixed;
            }
        } else {
            /* The object shrunk, in which case no scanning is needed. */
            *vpp = *endp = vp;
        }
    }

    JS_ASSERT(*vpp <= *endp);
    return true;
}

void
GCMarker::processMarkStackOther(SliceBudget &budget, uintptr_t tag, uintptr_t addr)
{
    if (tag == TypeTag) {
        ScanTypeObject(this, reinterpret_cast<types::TypeObject *>(addr));
    } else if (tag == SavedValueArrayTag) {
        JS_ASSERT(!(addr & Cell::CellMask));
        JSObject *obj = reinterpret_cast<JSObject *>(addr);
        HeapValue *vp, *end;
        if (restoreValueArray(obj, (void **)&vp, (void **)&end))
            pushValueArray(obj, vp, end);
        else
            pushObject(obj);
    } else if (tag == ArenaTag) {
        ArenaHeader *aheader = reinterpret_cast<ArenaHeader *>(addr);
        AllocKind thingKind = aheader->getAllocKind();
        size_t thingSize = Arena::thingSize(thingKind);

        for ( ; aheader; aheader = aheader->next) {
            Arena *arena = aheader->getArena();
            FreeSpan firstSpan(aheader->getFirstFreeSpan());
            const FreeSpan *span = &firstSpan;

            for (uintptr_t thing = arena->thingsStart(thingKind); ; thing += thingSize) {
                JS_ASSERT(thing <= arena->thingsEnd());
                if (thing == span->first) {
                    if (!span->hasNext())
                        break;
                    thing = span->last;
                    span = span->nextSpan();
                } else {
                    JSObject *object = reinterpret_cast<JSObject *>(thing);
                    if (object->hasSingletonType() && object->markIfUnmarked(getMarkColor()))
                        pushObject(object);
                    budget.step();
                }
            }
            if (budget.isOverBudget()) {
                pushArenaList(aheader);
                return;
            }
        }
    }

#if JS_HAS_XML_SUPPORT
    else {
        JS_ASSERT(tag == XmlTag);
        MarkChildren(this, reinterpret_cast<JSXML *>(addr));
    }
#endif
}

inline void
GCMarker::processMarkStackTop(SliceBudget &budget)
{
    /*
     * The function uses explicit goto and implements the scanning of the
     * object directly. It allows to eliminate the tail recursion and
     * significantly improve the marking performance, see bug 641025.
     */
    HeapSlot *vp, *end;
    JSObject *obj;

    uintptr_t addr = stack.pop();
    uintptr_t tag = addr & StackTagMask;
    addr &= ~StackTagMask;

    if (tag == ValueArrayTag) {
        JS_STATIC_ASSERT(ValueArrayTag == 0);
        JS_ASSERT(!(addr & Cell::CellMask));
        obj = reinterpret_cast<JSObject *>(addr);
        uintptr_t addr2 = stack.pop();
        uintptr_t addr3 = stack.pop();
        JS_ASSERT(addr2 <= addr3);
        JS_ASSERT((addr3 - addr2) % sizeof(Value) == 0);
        vp = reinterpret_cast<HeapSlot *>(addr2);
        end = reinterpret_cast<HeapSlot *>(addr3);
        goto scan_value_array;
    }

    if (tag == ObjectTag) {
        obj = reinterpret_cast<JSObject *>(addr);
        JS_COMPARTMENT_ASSERT(runtime, obj);
        goto scan_obj;
    }

    processMarkStackOther(budget, tag, addr);
    return;

  scan_value_array:
    JS_ASSERT(vp <= end);
    while (vp != end) {
        const Value &v = *vp++;
        if (v.isString()) {
            JSString *str = v.toString();
            JS_COMPARTMENT_ASSERT_STR(runtime, str);
            JS_ASSERT(str->compartment() == runtime->atomsCompartment ||
                      str->compartment() == obj->compartment());
            if (str->markIfUnmarked())
                ScanString(this, str);
        } else if (v.isObject()) {
            JSObject *obj2 = &v.toObject();
            JS_COMPARTMENT_ASSERT(runtime, obj2);
            JS_ASSERT(obj->compartment() == obj2->compartment());
            if (obj2->markIfUnmarked(getMarkColor())) {
                pushValueArray(obj, vp, end);
                obj = obj2;
                goto scan_obj;
            }
        }
    }
    return;

  scan_obj:
    {
        JS_COMPARTMENT_ASSERT(runtime, obj);

        budget.step();
        if (budget.isOverBudget()) {
            pushObject(obj);
            return;
        }

        types::TypeObject *type = obj->typeFromGC();
        PushMarkStack(this, type);

        Shape *shape = obj->lastProperty();
        PushMarkStack(this, shape);

        /* Call the trace hook if necessary. */
        Class *clasp = shape->getObjectClass();
        if (clasp->trace) {
            if (clasp == &ArrayClass) {
                JS_ASSERT(!shape->isNative());
                vp = obj->getDenseArrayElements();
                end = vp + obj->getDenseArrayInitializedLength();
                goto scan_value_array;
            } else {
                JS_ASSERT_IF(runtime->gcMode == JSGC_MODE_INCREMENTAL &&
                             runtime->gcIncrementalEnabled,
                             clasp->flags & JSCLASS_IMPLEMENTS_BARRIERS);
            }
            clasp->trace(this, obj);
        }

        if (!shape->isNative())
            return;

        unsigned nslots = obj->slotSpan();
        vp = obj->fixedSlots();
        if (obj->slots) {
            unsigned nfixed = obj->numFixedSlots();
            if (nslots > nfixed) {
                pushValueArray(obj, vp, vp + nfixed);
                vp = obj->slots;
                end = vp + (nslots - nfixed);
                goto scan_value_array;
            }
        }
        JS_ASSERT(nslots <= obj->numFixedSlots());
        end = vp + nslots;
        goto scan_value_array;
    }
}

bool
GCMarker::drainMarkStack(SliceBudget &budget)
{
#ifdef DEBUG
    JSRuntime *rt = runtime;

    struct AutoCheckCompartment {
        JSRuntime *runtime;
        AutoCheckCompartment(JSRuntime *rt) : runtime(rt) {
            JS_ASSERT(!rt->gcStrictCompartmentChecking);
            runtime->gcStrictCompartmentChecking = true;
        }
        ~AutoCheckCompartment() { runtime->gcStrictCompartmentChecking = false; }
    } acc(rt);
#endif

    if (budget.isOverBudget())
        return false;

    for (;;) {
        while (!stack.isEmpty()) {
            processMarkStackTop(budget);
            if (budget.isOverBudget()) {
                saveValueRanges();
                return false;
            }
        }

        if (!hasDelayedChildren())
            break;

        /*
         * Mark children of things that caused too deep recursion during the
         * above tracing. Don't do this until we're done with everything
         * else.
         */
        if (!markDelayedChildren(budget)) {
            saveValueRanges();
            return false;
        }
    }

    return true;
}

void
TraceChildren(JSTracer *trc, void *thing, JSGCTraceKind kind)
{
    switch (kind) {
      case JSTRACE_OBJECT:
        MarkChildren(trc, static_cast<JSObject *>(thing));
        break;

      case JSTRACE_STRING:
        MarkChildren(trc, static_cast<JSString *>(thing));
        break;

      case JSTRACE_SCRIPT:
        MarkChildren(trc, static_cast<JSScript *>(thing));
        break;

      case JSTRACE_SHAPE:
        MarkChildren(trc, static_cast<Shape *>(thing));
        break;

      case JSTRACE_BASE_SHAPE:
        MarkChildren(trc, static_cast<BaseShape *>(thing));
        break;

      case JSTRACE_TYPE_OBJECT:
        MarkChildren(trc, (types::TypeObject *)thing);
        break;

#if JS_HAS_XML_SUPPORT
      case JSTRACE_XML:
        MarkChildren(trc, static_cast<JSXML *>(thing));
        break;
#endif
    }
}

void
CallTracer(JSTracer *trc, void *thing, JSGCTraceKind kind)
{
    JS_ASSERT(thing);
    void *tmp = thing;
    MarkKind(trc, &tmp, kind);
    JS_ASSERT(tmp == thing);
}

} /* namespace js */
