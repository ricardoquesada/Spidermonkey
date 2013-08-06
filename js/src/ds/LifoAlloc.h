/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LifoAlloc_h__
#define LifoAlloc_h__

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/GuardObjects.h"
#include "mozilla/MemoryChecking.h"
#include "mozilla/PodOperations.h"
#include "mozilla/TypeTraits.h"

// This data structure supports stacky LIFO allocation (mark/release and
// LifoAllocScope). It does not maintain one contiguous segment; instead, it
// maintains a bunch of linked memory segments. In order to prevent malloc/free
// thrashing, unused segments are deallocated when garbage collection occurs.

#include "jsutil.h"

#include "js/TemplateLib.h"

namespace js {

namespace detail {

static const size_t LIFO_ALLOC_ALIGN = 8;

JS_ALWAYS_INLINE
char *
AlignPtr(void *orig)
{
    MOZ_STATIC_ASSERT(tl::FloorLog2<LIFO_ALLOC_ALIGN>::result ==
                      tl::CeilingLog2<LIFO_ALLOC_ALIGN>::result,
                      "LIFO_ALLOC_ALIGN must be a power of two");

    char *result = (char *) ((uintptr_t(orig) + (LIFO_ALLOC_ALIGN - 1)) & (~LIFO_ALLOC_ALIGN + 1));
    JS_ASSERT(uintptr_t(result) % LIFO_ALLOC_ALIGN == 0);
    return result;
}

// Header for a chunk of memory wrangled by the LifoAlloc.
class BumpChunk
{
    char        *bump;          // start of the available data
    char        *limit;         // end of the data
    BumpChunk   *next_;         // the next BumpChunk
    size_t      bumpSpaceSize;  // size of the data area

    char *headerBase() { return reinterpret_cast<char *>(this); }
    char *bumpBase() const { return limit - bumpSpaceSize; }

    BumpChunk *thisDuringConstruction() { return this; }

    explicit BumpChunk(size_t bumpSpaceSize)
      : bump(reinterpret_cast<char *>(thisDuringConstruction()) + sizeof(BumpChunk)),
        limit(bump + bumpSpaceSize),
        next_(NULL), bumpSpaceSize(bumpSpaceSize)
    {
        JS_ASSERT(bump == AlignPtr(bump));
    }

    void setBump(void *ptr) {
        JS_ASSERT(bumpBase() <= ptr);
        JS_ASSERT(ptr <= limit);
#if defined(DEBUG) || defined(MOZ_HAVE_MEM_CHECKS)
        char* prevBump = bump;
#endif
        bump = static_cast<char *>(ptr);
#ifdef DEBUG
        JS_ASSERT(contains(prevBump));

        // Clobber the now-free space.
        if (prevBump > bump)
            memset(bump, 0xcd, prevBump - bump);
#endif

        // Poison/Unpoison memory that we just free'd/allocated.
#if defined(MOZ_HAVE_MEM_CHECKS)
        if (prevBump > bump)
            MOZ_MAKE_MEM_NOACCESS(bump, prevBump - bump);
        else if (bump > prevBump)
            MOZ_MAKE_MEM_UNDEFINED(prevBump, bump - prevBump);
#endif
    }

  public:
    BumpChunk *next() const { return next_; }
    void setNext(BumpChunk *succ) { next_ = succ; }

    size_t used() const { return bump - bumpBase(); }

    size_t sizeOfIncludingThis(JSMallocSizeOfFun mallocSizeOf) {
        return mallocSizeOf(this);
    }

    size_t computedSizeOfIncludingThis() {
        return limit - headerBase();
    }

    void resetBump() {
        setBump(headerBase() + sizeof(BumpChunk));
    }

    void *mark() const { return bump; }

    void release(void *mark) {
        JS_ASSERT(contains(mark));
        JS_ASSERT(mark <= bump);
        setBump(mark);
    }

    bool contains(void *mark) const {
        return bumpBase() <= mark && mark <= limit;
    }

    bool canAlloc(size_t n);

    size_t unused() {
        return limit - AlignPtr(bump);
    }

    // Try to perform an allocation of size |n|, return null if not possible.
    JS_ALWAYS_INLINE
    void *tryAlloc(size_t n) {
        char *aligned = AlignPtr(bump);
        char *newBump = aligned + n;

        if (newBump > limit)
            return NULL;

        // Check for overflow.
        if (JS_UNLIKELY(newBump < bump))
            return NULL;

        JS_ASSERT(canAlloc(n)); // Ensure consistency between "can" and "try".
        setBump(newBump);
        return aligned;
    }

    void *allocInfallible(size_t n) {
        void *result = tryAlloc(n);
        JS_ASSERT(result);
        return result;
    }

    static BumpChunk *new_(size_t chunkSize);
    static void delete_(BumpChunk *chunk);
};

} // namespace detail

// LIFO bump allocator: used for phase-oriented and fast LIFO allocations.
//
// Note: |latest| is not necessary "last". We leave BumpChunks latent in the
// chain after they've been released to avoid thrashing before a GC.
class LifoAlloc
{
    typedef detail::BumpChunk BumpChunk;

    BumpChunk   *first;
    BumpChunk   *latest;
    BumpChunk   *last;
    size_t      markCount;
    size_t      defaultChunkSize_;
    size_t      curSize_;
    size_t      peakSize_;

    void operator=(const LifoAlloc &) MOZ_DELETE;
    LifoAlloc(const LifoAlloc &) MOZ_DELETE;

    // Return a BumpChunk that can perform an allocation of at least size |n|
    // and add it to the chain appropriately.
    //
    // Side effect: if retval is non-null, |first| and |latest| are initialized
    // appropriately.
    BumpChunk *getOrCreateChunk(size_t n);

    void reset(size_t defaultChunkSize) {
        JS_ASSERT(RoundUpPow2(defaultChunkSize) == defaultChunkSize);
        first = latest = last = NULL;
        defaultChunkSize_ = defaultChunkSize;
        markCount = 0;
        curSize_ = 0;
    }

    void append(BumpChunk *start, BumpChunk *end) {
        JS_ASSERT(start && end);
        if (last)
            last->setNext(start);
        else
            first = latest = start;
        last = end;
    }

    void incrementCurSize(size_t size) {
        curSize_ += size;
        if (curSize_ > peakSize_)
            peakSize_ = curSize_;
    }
    void decrementCurSize(size_t size) {
        JS_ASSERT(curSize_ >= size);
        curSize_ -= size;
    }

  public:
    explicit LifoAlloc(size_t defaultChunkSize)
      : peakSize_(0)
    {
        reset(defaultChunkSize);
    }

    // Steal allocated chunks from |other|.
    void steal(LifoAlloc *other) {
        JS_ASSERT(!other->markCount);

        // Copy everything from |other| to |this| except for |peakSize_|, which
        // requires some care.
        size_t oldPeakSize = peakSize_;
        mozilla::PodAssign(this, other);
        peakSize_ = Max(oldPeakSize, curSize_);

        other->reset(defaultChunkSize_);
    }

    // Append all chunks from |other|. They are removed from |other|.
    void transferFrom(LifoAlloc *other);

    // Append unused chunks from |other|. They are removed from |other|.
    void transferUnusedFrom(LifoAlloc *other);

    ~LifoAlloc() { freeAll(); }

    size_t defaultChunkSize() const { return defaultChunkSize_; }

    // Frees all held memory.
    void freeAll();

    static const unsigned HUGE_ALLOCATION = 50 * 1024 * 1024;
    void freeAllIfHugeAndUnused() {
        if (markCount == 0 && curSize_ > HUGE_ALLOCATION)
            freeAll();
    }

    JS_ALWAYS_INLINE
    void *alloc(size_t n) {
        JS_OOM_POSSIBLY_FAIL();

        void *result;
        if (latest && (result = latest->tryAlloc(n)))
            return result;

        if (!getOrCreateChunk(n))
            return NULL;

        return latest->allocInfallible(n);
    }

    JS_ALWAYS_INLINE
    void *allocInfallible(size_t n) {
        void *result;
        if (latest && (result = latest->tryAlloc(n)))
            return result;

        mozilla::DebugOnly<BumpChunk *> chunk = getOrCreateChunk(n);
        JS_ASSERT(chunk);

        return latest->allocInfallible(n);
    }

    // Ensures that enough space exists to satisfy N bytes worth of
    // allocation requests, not necessarily contiguous. Note that this does
    // not guarantee a successful single allocation of N bytes.
    JS_ALWAYS_INLINE
    bool ensureUnusedApproximate(size_t n) {
        size_t total = 0;
        for (BumpChunk *chunk = latest; chunk; chunk = chunk->next()) {
            total += chunk->unused();
            if (total >= n)
                return true;
        }
        BumpChunk *latestBefore = latest;
        if (!getOrCreateChunk(n))
            return false;
        if (latestBefore)
            latest = latestBefore;
        return true;
    }

    template <typename T>
    T *newArray(size_t count) {
        void *mem = alloc(sizeof(T) * count);
        if (!mem)
            return NULL;
        JS_STATIC_ASSERT(mozilla::IsPod<T>::value);
        return (T *) mem;
    }

    // Create an array with uninitialized elements of type |T|.
    // The caller is responsible for initialization.
    template <typename T>
    T *newArrayUninitialized(size_t count) {
        return static_cast<T *>(alloc(sizeof(T) * count));
    }

    class Mark {
        BumpChunk *chunk;
        void *markInChunk;
        friend class LifoAlloc;
        Mark(BumpChunk *chunk, void *markInChunk) : chunk(chunk), markInChunk(markInChunk) {}
      public:
        Mark() : chunk(NULL), markInChunk(NULL) {}
    };

    Mark mark() {
        markCount++;
        return latest ? Mark(latest, latest->mark()) : Mark();
    }

    void release(Mark mark) {
        markCount--;
        if (!mark.chunk) {
            latest = first;
            if (latest)
                latest->resetBump();
        } else {
            latest = mark.chunk;
            latest->release(mark.markInChunk);
        }
    }

    void releaseAll() {
        JS_ASSERT(!markCount);
        latest = first;
        if (latest)
            latest->resetBump();
    }

    // Get the total "used" (occupied bytes) count for the arena chunks.
    size_t used() const {
        size_t accum = 0;
        for (BumpChunk *chunk = first; chunk; chunk = chunk->next()) {
            accum += chunk->used();
            if (chunk == latest)
                break;
        }
        return accum;
    }

    // Get the total size of the arena chunks (including unused space).
    size_t sizeOfExcludingThis(JSMallocSizeOfFun mallocSizeOf) const {
        size_t n = 0;
        for (BumpChunk *chunk = first; chunk; chunk = chunk->next())
            n += chunk->sizeOfIncludingThis(mallocSizeOf);
        return n;
    }

    // Like sizeOfExcludingThis(), but includes the size of the LifoAlloc itself.
    size_t sizeOfIncludingThis(JSMallocSizeOfFun mallocSizeOf) const {
        return mallocSizeOf(this) + sizeOfExcludingThis(mallocSizeOf);
    }

    // Get the peak size of the arena chunks (including unused space and
    // bookkeeping space).
    size_t peakSizeOfExcludingThis() const { return peakSize_; }

    // Doesn't perform construction; useful for lazily-initialized POD types.
    template <typename T>
    JS_ALWAYS_INLINE
    T *newPod() {
        return static_cast<T *>(alloc(sizeof(T)));
    }

    JS_DECLARE_NEW_METHODS(new_, alloc, JS_ALWAYS_INLINE)
};

class LifoAllocScope
{
    LifoAlloc       *lifoAlloc;
    LifoAlloc::Mark mark;
    bool            shouldRelease;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

  public:
    explicit LifoAllocScope(LifoAlloc *lifoAlloc
                            MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : lifoAlloc(lifoAlloc),
        mark(lifoAlloc->mark()),
        shouldRelease(true)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    ~LifoAllocScope() {
        if (shouldRelease)
            lifoAlloc->release(mark);
    }

    LifoAlloc &alloc() {
        return *lifoAlloc;
    }

    void releaseEarly() {
        JS_ASSERT(shouldRelease);
        lifoAlloc->release(mark);
        shouldRelease = false;
    }
};

} // namespace js

#endif
