/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsgcinlines_h___
#define jsgcinlines_h___

#include "jsgc.h"
#include "jscntxt.h"
#include "jscompartment.h"
#include "jslock.h"
#include "jsscope.h"
#include "jsxml.h"

#include "js/TemplateLib.h"

namespace js {

struct Shape;

namespace gc {

inline JSGCTraceKind
GetGCThingTraceKind(const void *thing)
{
    JS_ASSERT(thing);
    const Cell *cell = reinterpret_cast<const Cell *>(thing);
    return MapAllocToTraceKind(cell->getAllocKind());
}

/* Capacity for slotsToThingKind */
const size_t SLOTS_TO_THING_KIND_LIMIT = 17;

/* Get the best kind to use when making an object with the given slot count. */
static inline AllocKind
GetGCObjectKind(size_t numSlots)
{
    extern AllocKind slotsToThingKind[];

    if (numSlots >= SLOTS_TO_THING_KIND_LIMIT)
        return FINALIZE_OBJECT16;
    return slotsToThingKind[numSlots];
}

static inline AllocKind
GetGCObjectKind(Class *clasp)
{
    if (clasp == &FunctionClass)
        return JSFunction::FinalizeKind;
    uint32_t nslots = JSCLASS_RESERVED_SLOTS(clasp);
    if (clasp->flags & JSCLASS_HAS_PRIVATE)
        nslots++;
    return GetGCObjectKind(nslots);
}

/* As for GetGCObjectKind, but for dense array allocation. */
static inline AllocKind
GetGCArrayKind(size_t numSlots)
{
    extern AllocKind slotsToThingKind[];

    /*
     * Dense arrays can use their fixed slots to hold their elements array
     * (less two Values worth of ObjectElements header), but if more than the
     * maximum number of fixed slots is needed then the fixed slots will be
     * unused.
     */
    JS_STATIC_ASSERT(ObjectElements::VALUES_PER_HEADER == 2);
    if (numSlots > JSObject::NELEMENTS_LIMIT || numSlots + 2 >= SLOTS_TO_THING_KIND_LIMIT)
        return FINALIZE_OBJECT2;
    return slotsToThingKind[numSlots + 2];
}

static inline AllocKind
GetGCObjectFixedSlotsKind(size_t numFixedSlots)
{
    extern AllocKind slotsToThingKind[];

    JS_ASSERT(numFixedSlots < SLOTS_TO_THING_KIND_LIMIT);
    return slotsToThingKind[numFixedSlots];
}

static inline AllocKind
GetBackgroundAllocKind(AllocKind kind)
{
    JS_ASSERT(!IsBackgroundFinalized(kind));
    JS_ASSERT(kind <= FINALIZE_OBJECT_LAST);
    return (AllocKind) (kind + 1);
}

/*
 * Try to get the next larger size for an object, keeping BACKGROUND
 * consistent.
 */
static inline bool
TryIncrementAllocKind(AllocKind *kindp)
{
    size_t next = size_t(*kindp) + 2;
    if (next >= size_t(FINALIZE_OBJECT_LIMIT))
        return false;
    *kindp = AllocKind(next);
    return true;
}

/* Get the number of fixed slots and initial capacity associated with a kind. */
static inline size_t
GetGCKindSlots(AllocKind thingKind)
{
    /* Using a switch in hopes that thingKind will usually be a compile-time constant. */
    switch (thingKind) {
      case FINALIZE_OBJECT0:
      case FINALIZE_OBJECT0_BACKGROUND:
        return 0;
      case FINALIZE_OBJECT2:
      case FINALIZE_OBJECT2_BACKGROUND:
        return 2;
      case FINALIZE_OBJECT4:
      case FINALIZE_OBJECT4_BACKGROUND:
        return 4;
      case FINALIZE_OBJECT8:
      case FINALIZE_OBJECT8_BACKGROUND:
        return 8;
      case FINALIZE_OBJECT12:
      case FINALIZE_OBJECT12_BACKGROUND:
        return 12;
      case FINALIZE_OBJECT16:
      case FINALIZE_OBJECT16_BACKGROUND:
        return 16;
      default:
        JS_NOT_REACHED("Bad object finalize kind");
        return 0;
    }
}

static inline size_t
GetGCKindSlots(AllocKind thingKind, Class *clasp)
{
    size_t nslots = GetGCKindSlots(thingKind);

    /* An object's private data uses the space taken by its last fixed slot. */
    if (clasp->flags & JSCLASS_HAS_PRIVATE) {
        JS_ASSERT(nslots > 0);
        nslots--;
    }

    /*
     * Functions have a larger finalize kind than FINALIZE_OBJECT to reserve
     * space for the extra fields in JSFunction, but have no fixed slots.
     */
    if (clasp == &FunctionClass)
        nslots = 0;

    return nslots;
}

static inline void
GCPoke(JSRuntime *rt, Value oldval)
{
    /*
     * Since we're forcing a GC from JS_GC anyway, don't bother wasting cycles
     * loading oldval.  XXX remove implied force, fix jsinterp.c's "second arg
     * ignored", etc.
     */
#if 1
    rt->gcPoke = true;
#else
    rt->gcPoke = oldval.isGCThing();
#endif

#ifdef JS_GC_ZEAL
    /* Schedule a GC to happen "soon" after a GC poke. */
    if (rt->gcZeal() == js::gc::ZealPokeValue)
        rt->gcNextScheduled = 1;
#endif
}

class ArenaIter
{
    ArenaHeader *aheader;
    ArenaHeader *remainingHeader;

  public:
    ArenaIter() {
        init();
    }

    ArenaIter(JSCompartment *comp, AllocKind kind) {
        init(comp, kind);
    }

    void init() {
        aheader = NULL;
        remainingHeader = NULL;
    }

    void init(ArenaHeader *aheaderArg) {
        aheader = aheaderArg;
        remainingHeader = NULL;
    }

    void init(JSCompartment *comp, AllocKind kind) {
        aheader = comp->arenas.getFirstArena(kind);
        remainingHeader = comp->arenas.getFirstArenaToSweep(kind);
        if (!aheader) {
            aheader = remainingHeader;
            remainingHeader = NULL;
        }
    }

    bool done() {
        return !aheader;
    }

    ArenaHeader *get() {
        return aheader;
    }

    void next() {
        aheader = aheader->next;
        if (!aheader) {
            aheader = remainingHeader;
            remainingHeader = NULL;
        }
    }
};

class CellIterImpl
{
    size_t firstThingOffset;
    size_t thingSize;
    ArenaIter aiter;
    FreeSpan firstSpan;
    const FreeSpan *span;
    uintptr_t thing;
    Cell *cell;

  protected:
    CellIterImpl() {
    }

    void initSpan(JSCompartment *comp, AllocKind kind) {
        JS_ASSERT(comp->arenas.isSynchronizedFreeList(kind));
        firstThingOffset = Arena::firstThingOffset(kind);
        thingSize = Arena::thingSize(kind);
        firstSpan.initAsEmpty();
        span = &firstSpan;
        thing = span->first;
    }

    void init(ArenaHeader *singleAheader) {
        initSpan(singleAheader->compartment, singleAheader->getAllocKind());
        aiter.init(singleAheader);
        next();
        aiter.init();
    }

    void init(JSCompartment *comp, AllocKind kind) {
        initSpan(comp, kind);
        aiter.init(comp, kind);
        next();
    }

  public:
    bool done() const {
        return !cell;
    }

    template<typename T> T *get() const {
        JS_ASSERT(!done());
        return static_cast<T *>(cell);
    }

    Cell *getCell() const {
        JS_ASSERT(!done());
        return cell;
    }

    void next() {
        for (;;) {
            if (thing != span->first)
                break;
            if (JS_LIKELY(span->hasNext())) {
                thing = span->last + thingSize;
                span = span->nextSpan();
                break;
            }
            if (aiter.done()) {
                cell = NULL;
                return;
            }
            ArenaHeader *aheader = aiter.get();
            firstSpan = aheader->getFirstFreeSpan();
            span = &firstSpan;
            thing = aheader->arenaAddress() | firstThingOffset;
            aiter.next();
        }
        cell = reinterpret_cast<Cell *>(thing);
        thing += thingSize;
    }
};

class CellIterUnderGC : public CellIterImpl
{
  public:
    CellIterUnderGC(JSCompartment *comp, AllocKind kind) {
        JS_ASSERT(comp->rt->isHeapBusy());
        init(comp, kind);
    }

    CellIterUnderGC(ArenaHeader *aheader) {
        JS_ASSERT(aheader->compartment->rt->isHeapBusy());
        init(aheader);
    }
};

/*
 * When using the iterator outside the GC the caller must ensure that no GC or
 * allocations of GC things are possible and that the background finalization
 * for the given thing kind is not enabled or is done.
 */
class CellIter : public CellIterImpl
{
    ArenaLists *lists;
    AllocKind kind;
#ifdef DEBUG
    size_t *counter;
#endif
  public:
    CellIter(JSCompartment *comp, AllocKind kind)
      : lists(&comp->arenas),
        kind(kind)
    {
        /*
         * We have a single-threaded runtime, so there's no need to protect
         * against other threads iterating or allocating. However, we do have
         * background finalization; make sure people aren't using CellIter to
         * walk such allocation kinds.
         */
        JS_ASSERT(!IsBackgroundFinalized(kind));
        if (lists->isSynchronizedFreeList(kind)) {
            lists = NULL;
        } else {
            JS_ASSERT(!comp->rt->isHeapBusy());
            lists->copyFreeListToArena(kind);
        }
#ifdef DEBUG
        counter = &comp->rt->noGCOrAllocationCheck;
        ++*counter;
#endif
        init(comp, kind);
    }

    ~CellIter() {
#ifdef DEBUG
        JS_ASSERT(*counter > 0);
        --*counter;
#endif
        if (lists)
            lists->clearFreeListInArena(kind);
    }
};

/*
 * Invoke ArenaOp and CellOp on every arena and cell in a compartment which
 * have the specified thing kind.
 */
template <class ArenaOp, class CellOp>
void
ForEachArenaAndCell(JSCompartment *compartment, AllocKind thingKind,
                    ArenaOp arenaOp, CellOp cellOp)
{
    for (ArenaIter aiter(compartment, thingKind); !aiter.done(); aiter.next()) {
        ArenaHeader *aheader = aiter.get();
        arenaOp(aheader->getArena());
        for (CellIterUnderGC iter(aheader); !iter.done(); iter.next())
            cellOp(iter.getCell());
    }
}

/* Signatures for ArenaOp and CellOp above. */

inline void EmptyArenaOp(Arena *arena) {}
inline void EmptyCellOp(Cell *t) {}

class GCCompartmentsIter {
  private:
    JSCompartment **it, **end;

  public:
    GCCompartmentsIter(JSRuntime *rt) {
        JS_ASSERT(rt->isHeapBusy());
        it = rt->compartments.begin();
        end = rt->compartments.end();
        if (!(*it)->isCollecting())
            next();
    }

    bool done() const { return it == end; }

    void next() {
        JS_ASSERT(!done());
        do {
            it++;
        } while (it != end && !(*it)->isCollecting());
    }

    JSCompartment *get() const {
        JS_ASSERT(!done());
        return *it;
    }

    operator JSCompartment *() const { return get(); }
    JSCompartment *operator->() const { return get(); }
};

/*
 * Allocates a new GC thing. After a successful allocation the caller must
 * fully initialize the thing before calling any function that can potentially
 * trigger GC. This will ensure that GC tracing never sees junk values stored
 * in the partially initialized thing.
 */

template <typename T>
inline T *
NewGCThing(JSContext *cx, js::gc::AllocKind kind, size_t thingSize)
{
    JS_ASSERT(thingSize == js::gc::Arena::thingSize(kind));
    JS_ASSERT_IF(cx->compartment == cx->runtime->atomsCompartment,
                 kind == js::gc::FINALIZE_STRING || kind == js::gc::FINALIZE_SHORT_STRING);
    JS_ASSERT(!cx->runtime->isHeapBusy());
    JS_ASSERT(!cx->runtime->noGCOrAllocationCheck);

    /* For testing out of memory conditions */
    JS_OOM_POSSIBLY_FAIL_REPORT(cx);

#ifdef JS_GC_ZEAL
    if (cx->runtime->needZealousGC())
        js::gc::RunDebugGC(cx);
#endif

    MaybeCheckStackRoots(cx, /* relax = */ false);

    JSCompartment *comp = cx->compartment;
    void *t = comp->arenas.allocateFromFreeList(kind, thingSize);
    if (!t)
        t = js::gc::ArenaLists::refillFreeList(cx, kind);

    JS_ASSERT_IF(t && comp->wasGCStarted() && comp->needsBarrier(),
                 static_cast<T *>(t)->arenaHeader()->allocatedDuringIncremental);

#if defined(JSGC_GENERATIONAL) && defined(JS_GC_ZEAL)
    if (cx->runtime->gcVerifyPostData && IsNurseryAllocable(kind) && !IsAtomsCompartment(comp))
        comp->gcNursery.insertPointer(t);
#endif
    return static_cast<T *>(t);
}

/* Alternate form which allocates a GC thing if doing so cannot trigger a GC. */
template <typename T>
inline T *
TryNewGCThing(JSContext *cx, js::gc::AllocKind kind, size_t thingSize)
{
    JS_ASSERT(thingSize == js::gc::Arena::thingSize(kind));
    JS_ASSERT_IF(cx->compartment == cx->runtime->atomsCompartment,
                 kind == js::gc::FINALIZE_STRING || kind == js::gc::FINALIZE_SHORT_STRING);
    JS_ASSERT(!cx->runtime->isHeapBusy());
    JS_ASSERT(!cx->runtime->noGCOrAllocationCheck);

#ifdef JS_GC_ZEAL
    if (cx->runtime->needZealousGC())
        return NULL;
#endif

    void *t = cx->compartment->arenas.allocateFromFreeList(kind, thingSize);
    JS_ASSERT_IF(t && cx->compartment->wasGCStarted() && cx->compartment->needsBarrier(),
                 static_cast<T *>(t)->arenaHeader()->allocatedDuringIncremental);

#if defined(JSGC_GENERATIONAL) && defined(JS_GC_ZEAL)
    JSCompartment *comp = cx->compartment;
    if (cx->runtime->gcVerifyPostData && IsNurseryAllocable(kind) && !IsAtomsCompartment(comp))
        comp->gcNursery.insertPointer(t);
#endif
    return static_cast<T *>(t);
}

} /* namespace gc */
} /* namespace js */

inline JSObject *
js_NewGCObject(JSContext *cx, js::gc::AllocKind kind)
{
    JS_ASSERT(kind >= js::gc::FINALIZE_OBJECT0 && kind <= js::gc::FINALIZE_OBJECT_LAST);
    return js::gc::NewGCThing<JSObject>(cx, kind, js::gc::Arena::thingSize(kind));
}

inline JSObject *
js_TryNewGCObject(JSContext *cx, js::gc::AllocKind kind)
{
    JS_ASSERT(kind >= js::gc::FINALIZE_OBJECT0 && kind <= js::gc::FINALIZE_OBJECT_LAST);
    return js::gc::TryNewGCThing<JSObject>(cx, kind, js::gc::Arena::thingSize(kind));
}

inline JSString *
js_NewGCString(JSContext *cx)
{
    return js::gc::NewGCThing<JSString>(cx, js::gc::FINALIZE_STRING, sizeof(JSString));
}

inline JSShortString *
js_NewGCShortString(JSContext *cx)
{
    return js::gc::NewGCThing<JSShortString>(cx, js::gc::FINALIZE_SHORT_STRING, sizeof(JSShortString));
}

inline JSExternalString *
js_NewGCExternalString(JSContext *cx)
{
    return js::gc::NewGCThing<JSExternalString>(cx, js::gc::FINALIZE_EXTERNAL_STRING,
                                                sizeof(JSExternalString));
}

inline JSScript *
js_NewGCScript(JSContext *cx)
{
    return js::gc::NewGCThing<JSScript>(cx, js::gc::FINALIZE_SCRIPT, sizeof(JSScript));
}

inline js::Shape *
js_NewGCShape(JSContext *cx)
{
    return js::gc::NewGCThing<js::Shape>(cx, js::gc::FINALIZE_SHAPE, sizeof(js::Shape));
}

inline js::BaseShape *
js_NewGCBaseShape(JSContext *cx)
{
    return js::gc::NewGCThing<js::BaseShape>(cx, js::gc::FINALIZE_BASE_SHAPE, sizeof(js::BaseShape));
}

#if JS_HAS_XML_SUPPORT
extern JSXML *
js_NewGCXML(JSContext *cx);
#endif

#endif /* jsgcinlines_h___ */
