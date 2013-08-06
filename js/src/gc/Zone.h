/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_zone_h___
#define gc_zone_h___

#include "mozilla/Attributes.h"
#include "mozilla/GuardObjects.h"
#include "mozilla/Util.h"

#include "jscntxt.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinfer.h"
#include "jsobj.h"

#include "gc/StoreBuffer.h"
#include "gc/FindSCCs.h"
#include "vm/GlobalObject.h"
#include "vm/RegExpObject.h"
#include "vm/Shape.h"

namespace js {

/*
 * Encapsulates the data needed to perform allocation.  Typically
 * there is precisely one of these per compartment
 * (|compartment.allocator|).  However, in parallel execution mode,
 * there will be one per worker thread.  In general, if a piece of
 * code must perform execution and should work safely either in
 * parallel or sequential mode, you should make it take an
 * |Allocator*| rather than a |JSContext*|.
 */
class Allocator : public MallocProvider<Allocator>
{
    JS::Zone *zone;

  public:
    explicit Allocator(JS::Zone *zone);

    js::gc::ArenaLists arenas;

    inline void *parallelNewGCThing(gc::AllocKind thingKind, size_t thingSize);

    inline void *onOutOfMemory(void *p, size_t nbytes);
    inline void updateMallocCounter(size_t nbytes);
    inline void reportAllocationOverflow();
};

typedef Vector<JSCompartment *, 1, SystemAllocPolicy> CompartmentVector;

} /* namespace js */

namespace JS {

/*
 * A zone is a collection of compartments. Every compartment belongs to exactly
 * one zone. In Firefox, there is roughly one zone per tab along with a system
 * zone for everything else. Zones mainly serve as boundaries for garbage
 * collection. Unlike compartments, they have no special security properties.
 *
 * Every GC thing belongs to exactly one zone. GC things from the same zone but
 * different compartments can share an arena (4k page). GC things from different
 * zones cannot be stored in the same arena. The garbage collector is capable of
 * collecting one zone at a time; it cannot collect at the granularity of
 * compartments.
 *
 * GC things are tied to zones and compartments as follows:
 *
 * - JSObjects belong to a compartment and cannot be shared between
 *   compartments. If an object needs to point to a JSObject in a different
 *   compartment, regardless of zone, it must go through a cross-compartment
 *   wrapper. Each compartment keeps track of its outgoing wrappers in a table.
 *
 * - JSStrings do not belong to any particular compartment, but they do belong
 *   to a zone. Thus, two different compartments in the same zone can point to a
 *   JSString. When a string needs to be wrapped, we copy it if it's in a
 *   different zone and do nothing if it's in the same zone. Thus, transferring
 *   strings within a zone is very efficient.
 *
 * - Shapes and base shapes belong to a compartment and cannot be shared between
 *   compartments. A base shape holds a pointer to its compartment. Shapes find
 *   their compartment via their base shape. JSObjects find their compartment
 *   via their shape.
 *
 * - Scripts are also compartment-local and cannot be shared. A script points to
 *   its compartment.
 *
 * - Type objects and IonCode objects belong to a compartment and cannot be
 *   shared. However, there is no mechanism to obtain their compartments.
 *
 * A zone remains alive as long as any GC things in the zone are alive. A
 * compartment remains alive as long as any JSObjects, scripts, shapes, or base
 * shapes within it are alive.
 *
 * We always guarantee that a zone has at least one live compartment by refusing
 * to delete the last compartment in a live zone. (This could happen, for
 * example, if the conservative scanner marks a string in an otherwise dead
 * zone.)
 */

struct Zone : private JS::shadow::Zone, public js::gc::GraphNodeBase<JS::Zone>
{
    JSRuntime                    *rt;
    js::Allocator                allocator;

    js::CompartmentVector        compartments;

    bool                         hold;

  private:
    bool                         ionUsingBarriers_;

    /*
     * This flag saves the value of needsBarrier_ during minor collection,
     * since needsBarrier_ is always set to false during minor collection.
     * Outside of minor collection, the value of savedNeedsBarrier_ is
     * undefined.
     */
    bool                         savedNeedsBarrier_;

  public:
    bool                         active;  // GC flag, whether there are active frames

    void saveNeedsBarrier(bool newNeeds) {
        savedNeedsBarrier_ = needsBarrier_;
        needsBarrier_ = newNeeds;
    }

    void restoreNeedsBarrier() {
        needsBarrier_ = savedNeedsBarrier_;
    }

    bool savedNeedsBarrier() const {
        return savedNeedsBarrier_;
    }

    bool needsBarrier() const {
        return needsBarrier_;
    }

    bool compileBarriers(bool needsBarrier) const {
        return needsBarrier || rt->gcZeal() == js::gc::ZealVerifierPreValue;
    }

    bool compileBarriers() const {
        return compileBarriers(needsBarrier());
    }

    enum ShouldUpdateIon {
        DontUpdateIon,
        UpdateIon
    };

    void setNeedsBarrier(bool needs, ShouldUpdateIon updateIon);

    static size_t OffsetOfNeedsBarrier() {
        return offsetof(Zone, needsBarrier_);
    }

    js::GCMarker *barrierTracer() {
        JS_ASSERT(needsBarrier_);
        return &rt->gcMarker;
    }

  public:
    enum CompartmentGCState {
        NoGC,
        Mark,
        MarkGray,
        Sweep,
        Finished
    };

  private:
    bool                         gcScheduled;
    CompartmentGCState           gcState;
    bool                         gcPreserveCode;

  public:
    bool isCollecting() const {
        if (rt->isHeapCollecting())
            return gcState != NoGC;
        else
            return needsBarrier();
    }

    bool isPreservingCode() const {
        return gcPreserveCode;
    }

    /*
     * If this returns true, all object tracing must be done with a GC marking
     * tracer.
     */
    bool requireGCTracer() const {
        return rt->isHeapMajorCollecting() && gcState != NoGC;
    }

    void setGCState(CompartmentGCState state) {
        JS_ASSERT(rt->isHeapBusy());
        gcState = state;
    }

    void scheduleGC() {
        JS_ASSERT(!rt->isHeapBusy());
        gcScheduled = true;
    }

    void unscheduleGC() {
        gcScheduled = false;
    }

    bool isGCScheduled() const {
        return gcScheduled;
    }

    void setPreservingCode(bool preserving) {
        gcPreserveCode = preserving;
    }

    bool wasGCStarted() const {
        return gcState != NoGC;
    }

    bool isGCMarking() {
        if (rt->isHeapCollecting())
            return gcState == Mark || gcState == MarkGray;
        else
            return needsBarrier();
    }

    bool isGCMarkingBlack() {
        return gcState == Mark;
    }

    bool isGCMarkingGray() {
        return gcState == MarkGray;
    }

    bool isGCSweeping() {
        return gcState == Sweep;
    }

    bool isGCFinished() {
        return gcState == Finished;
    }

    volatile size_t              gcBytes;
    size_t                       gcTriggerBytes;
    size_t                       gcMaxMallocBytes;
    double                       gcHeapGrowthFactor;

    bool                         isSystem;

    /*
     * These flags help us to discover if a compartment that shouldn't be alive
     * manages to outlive a GC.
     */
    bool                         scheduledForDestruction;
    bool                         maybeAlive;

    /*
     * Malloc counter to measure memory pressure for GC scheduling. It runs from
     * gcMaxMallocBytes down to zero. This counter should be used only when it's
     * not possible to know the size of a free.
     */
    ptrdiff_t                    gcMallocBytes;

    /* This compartment's gray roots. */
    js::Vector<js::GrayRoot, 0, js::SystemAllocPolicy> gcGrayRoots;

    Zone(JSRuntime *rt);
    ~Zone();
    bool init(JSContext *cx);

    void findOutgoingEdges(js::gc::ComponentFinder<JS::Zone> &finder);

    void discardJitCode(js::FreeOp *fop, bool discardConstraints);

    void sizeOfIncludingThis(JSMallocSizeOfFun mallocSizeOf, size_t *typePool);

    void setGCLastBytes(size_t lastBytes, js::JSGCInvocationKind gckind);
    void reduceGCTriggerBytes(size_t amount);

    void resetGCMallocBytes();
    void setGCMaxMallocBytes(size_t value);
    void updateMallocCounter(size_t nbytes) {
        /*
         * Note: this code may be run from worker threads.  We
         * tolerate any thread races when updating gcMallocBytes.
         */
        ptrdiff_t oldCount = gcMallocBytes;
        ptrdiff_t newCount = oldCount - ptrdiff_t(nbytes);
        gcMallocBytes = newCount;
        if (JS_UNLIKELY(newCount <= 0 && oldCount > 0))
            onTooMuchMalloc();
    }

    bool isTooMuchMalloc() const {
        return gcMallocBytes <= 0;
     }

    void onTooMuchMalloc();

    void markTypes(JSTracer *trc);

    js::types::TypeZone types;

    void sweep(js::FreeOp *fop, bool releaseTypes);
};

} /* namespace JS */

namespace js {

class ZonesIter {
  private:
    JS::Zone **it, **end;

  public:
    ZonesIter(JSRuntime *rt) {
        it = rt->zones.begin();
        end = rt->zones.end();
    }

    bool done() const { return it == end; }

    void next() {
        JS_ASSERT(!done());
        it++;
    }

    JS::Zone *get() const {
        JS_ASSERT(!done());
        return *it;
    }

    operator JS::Zone *() const { return get(); }
    JS::Zone *operator->() const { return get(); }
};

struct CompartmentsInZoneIter
{
  private:
    JSCompartment **it, **end;

  public:
    CompartmentsInZoneIter(JS::Zone *zone) {
        it = zone->compartments.begin();
        end = zone->compartments.end();
    }

    bool done() const { return it == end; }
    void next() {
        JS_ASSERT(!done());
        it++;
    }

    JSCompartment *get() const { return *it; }

    operator JSCompartment *() const { return get(); }
    JSCompartment *operator->() const { return get(); }
};

/*
 * This iterator iterates over all the compartments in a given set of zones. The
 * set of zones is determined by iterating ZoneIterT.
 */
template<class ZonesIterT>
class CompartmentsIterT
{
  private:
    ZonesIterT zone;
    mozilla::Maybe<CompartmentsInZoneIter> comp;

  public:
    CompartmentsIterT(JSRuntime *rt)
      : zone(rt)
    {
        JS_ASSERT(!zone.done());
        comp.construct(zone);
    }

    bool done() const { return zone.done(); }

    void next() {
        JS_ASSERT(!done());
        JS_ASSERT(!comp.ref().done());
        comp.ref().next();
        if (comp.ref().done()) {
            comp.destroy();
            zone.next();
            if (!zone.done())
                comp.construct(zone);
        }
    }

    JSCompartment *get() const {
        JS_ASSERT(!done());
        return comp.ref();
    }

    operator JSCompartment *() const { return get(); }
    JSCompartment *operator->() const { return get(); }
};

typedef CompartmentsIterT<ZonesIter> CompartmentsIter;

} /* namespace js */

#endif /* gc_zone_h___ */
