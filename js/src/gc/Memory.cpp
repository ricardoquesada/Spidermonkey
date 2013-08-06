/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"

#include "jsapi.h"

#include "js/HeapAPI.h"
#include "js/Utility.h"
#include "gc/Memory.h"

using namespace js;
using namespace js::gc;

/* Unused memory decommiting requires the arena size match the page size. */
static bool
DecommitEnabled()
{
    return PageSize == ArenaSize;
}

#if defined(XP_WIN)
#include "jswin.h"
#include <psapi.h>

static size_t AllocationGranularity = 0;

void
gc::InitMemorySubsystem()
{
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    if (sysinfo.dwPageSize != PageSize) {
        fprintf(stderr,"SpiderMonkey compiled with incorrect page size; please update js/public/HeapAPI.h.\n");
        MOZ_CRASH();
    }
    AllocationGranularity = sysinfo.dwAllocationGranularity;
}

void *
gc::MapAlignedPages(size_t size, size_t alignment)
{
    JS_ASSERT(size >= alignment);
    JS_ASSERT(size % alignment == 0);
    JS_ASSERT(size % PageSize == 0);
    JS_ASSERT(alignment % AllocationGranularity == 0);

    /* Special case: If we want allocation alignment, no further work is needed. */
    if (alignment == AllocationGranularity) {
        return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }

    /*
     * Windows requires that there be a 1:1 mapping between VM allocation
     * and deallocation operations.  Therefore, take care here to acquire the
     * final result via one mapping operation.  This means unmapping any
     * preliminary result that is not correctly aligned.
     */
    void *p = NULL;
    while (!p) {
        /*
         * Over-allocate in order to map a memory region that is
         * definitely large enough then deallocate and allocate again the
         * correct sizee, within the over-sized mapping.
         *
         * Since we're going to unmap the whole thing anyway, the first
         * mapping doesn't have to commit pages.
         */
        p = VirtualAlloc(NULL, size * 2, MEM_RESERVE, PAGE_READWRITE);
        if (!p)
            return NULL;
        void *chunkStart = (void *)(uintptr_t(p) + (alignment - (uintptr_t(p) % alignment)));
        UnmapPages(p, size * 2);
        p = VirtualAlloc(chunkStart, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

        /* Failure here indicates a race with another thread, so try again. */
    }

    JS_ASSERT(uintptr_t(p) % alignment == 0);
    return p;
}

void
gc::UnmapPages(void *p, size_t size)
{
    JS_ALWAYS_TRUE(VirtualFree(p, 0, MEM_RELEASE));
}

bool
gc::MarkPagesUnused(void *p, size_t size)
{
    if (!DecommitEnabled())
        return false;

    JS_ASSERT(uintptr_t(p) % PageSize == 0);
    LPVOID p2 = VirtualAlloc(p, size, MEM_RESET, PAGE_READWRITE);
    return p2 == p;
}

bool
gc::MarkPagesInUse(void *p, size_t size)
{
    JS_ASSERT(uintptr_t(p) % PageSize == 0);
    return true;
}

size_t
gc::GetPageFaultCount()
{
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return 0;
    return pmc.PageFaultCount;
}

#elif defined(XP_OS2)

#define INCL_DOSMEMMGR
#include <os2.h>

#define JS_GC_HAS_MAP_ALIGN 1
#define OS2_MAX_RECURSIONS  16

void
gc::InitMemorySubsystem()
{
}

void
gc::UnmapPages(void *addr, size_t size)
{
    if (!DosFreeMem(addr))
        return;

    /*
     * If DosFreeMem() failed, 'addr' is probably part of an "expensive"
     * allocation, so calculate the base address and try again.
     */
    unsigned long cb = 2 * size;
    unsigned long flags;
    if (DosQueryMem(addr, &cb, &flags) || cb < size)
        return;

    uintptr_t base = reinterpret_cast<uintptr_t>(addr) - ((2 * size) - cb);
    DosFreeMem(reinterpret_cast<void*>(base));

    return;
}

static void *
gc::MapAlignedPagesRecursively(size_t size, size_t alignment, int& recursions)
{
    if (++recursions >= OS2_MAX_RECURSIONS)
        return NULL;

    void *tmp;
    if (DosAllocMem(&tmp, size,
                    OBJ_ANY | PAG_COMMIT | PAG_READ | PAG_WRITE)) {
        JS_ALWAYS_TRUE(DosAllocMem(&tmp, size,
                                   PAG_COMMIT | PAG_READ | PAG_WRITE) == 0);
    }
    size_t offset = reinterpret_cast<uintptr_t>(tmp) & (alignment - 1);
    if (!offset)
        return tmp;

    /*
     * If there are 'filler' bytes of free space above 'tmp', free 'tmp',
     * then reallocate it as a 'filler'-sized block;  assuming we're not
     * in a race with another thread, the next recursion should succeed.
     */
    size_t filler = size + alignment - offset;
    unsigned long cb = filler;
    unsigned long flags = 0;
    unsigned long rc = DosQueryMem(&(static_cast<char*>(tmp))[size],
                                   &cb, &flags);
    if (!rc && (flags & PAG_FREE) && cb >= filler) {
        UnmapPages(tmp, 0);
        if (DosAllocMem(&tmp, filler,
                        OBJ_ANY | PAG_COMMIT | PAG_READ | PAG_WRITE)) {
            JS_ALWAYS_TRUE(DosAllocMem(&tmp, filler,
                                       PAG_COMMIT | PAG_READ | PAG_WRITE) == 0);
        }
    }

    void *p = MapAlignedPagesRecursively(size, alignment, recursions);
    UnmapPages(tmp, 0);

    return p;
}

void *
gc::MapAlignedPages(size_t size, size_t alignment)
{
    JS_ASSERT(size >= alignment);
    JS_ASSERT(size % alignment == 0);
    JS_ASSERT(size % PageSize == 0);
    JS_ASSERT(alignment % PageSize == 0);

    int recursions = -1;

    /*
     * Make up to OS2_MAX_RECURSIONS attempts to get an aligned block
     * of the right size by recursively allocating blocks of unaligned
     * free memory until only an aligned allocation is possible.
     */
    void *p = MapAlignedPagesRecursively(size, alignment, recursions);
    if (p)
        return p;

    /*
     * If memory is heavily fragmented, the recursive strategy may fail;
     * instead, use the "expensive" strategy:  allocate twice as much
     * as requested and return an aligned address within this block.
     */
    if (DosAllocMem(&p, 2 * size,
                    OBJ_ANY | PAG_COMMIT | PAG_READ | PAG_WRITE)) {
        JS_ALWAYS_TRUE(DosAllocMem(&p, 2 * size,
                                   PAG_COMMIT | PAG_READ | PAG_WRITE) == 0);
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    addr = (addr + (alignment - 1)) & ~(alignment - 1);

    return reinterpret_cast<void *>(addr);
}

bool
gc::MarkPagesUnused(void *p, size_t size)
{
    JS_ASSERT(uintptr_t(p) % PageSize == 0);
    return true;
}

bool
gc::MarkPagesInUse(void *p, size_t size)
{
    JS_ASSERT(uintptr_t(p) % PageSize == 0);
    return true;
}

size_t
gc::GetPageFaultCount()
{
    return 0;
}

#elif defined(SOLARIS)

#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_NOSYNC
# define MAP_NOSYNC 0
#endif

void
gc::InitMemorySubsystem()
{
}

void *
gc::MapAlignedPages(size_t size, size_t alignment)
{
    JS_ASSERT(size >= alignment);
    JS_ASSERT(size % alignment == 0);
    JS_ASSERT(size % PageSize == 0);
    JS_ASSERT(alignment % PageSize == 0);

    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_PRIVATE | MAP_ANON | MAP_ALIGN | MAP_NOSYNC;

    void *p = mmap((caddr_t)alignment, size, prot, flags, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    return p;
}

void
gc::UnmapPages(void *p, size_t size)
{
    JS_ALWAYS_TRUE(0 == munmap((caddr_t)p, size));
}

bool
gc::MarkPagesUnused(void *p, size_t size)
{
    JS_ASSERT(uintptr_t(p) % PageSize == 0);
    return true;
}

bool
gc::MarkPagesInUse(void *p, size_t size)
{
    JS_ASSERT(uintptr_t(p) % PageSize == 0);
    return true;
}

size_t
gc::GetPageFaultCount()
{
    return 0;
}

#elif defined(XP_UNIX) || defined(XP_MACOSX) || defined(DARWIN)

#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>

void
gc::InitMemorySubsystem()
{
    if (size_t(sysconf(_SC_PAGESIZE)) != PageSize) {
        fprintf(stderr,"SpiderMonkey compiled with incorrect page size; please update js/public/HeapAPI.h.\n");
        MOZ_CRASH();
    }
}

void *
gc::MapAlignedPages(size_t size, size_t alignment)
{
    JS_ASSERT(size >= alignment);
    JS_ASSERT(size % alignment == 0);
    JS_ASSERT(size % PageSize == 0);
    JS_ASSERT(alignment % PageSize == 0);

    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_PRIVATE | MAP_ANON;

    /* Special case: If we want page alignment, no further work is needed. */
    if (alignment == PageSize) {
        return mmap(NULL, size, prot, flags, -1, 0);
    }

    /* Overallocate and unmap the region's edges. */
    size_t reqSize = Min(size + 2 * alignment, 2 * size);
    void *region = mmap(NULL, reqSize, prot, flags, -1, 0);
    if (region == MAP_FAILED)
        return NULL;

    uintptr_t regionEnd = uintptr_t(region) + reqSize;
    uintptr_t offset = uintptr_t(region) % alignment;
    JS_ASSERT(offset < reqSize - size);

    void *front = (void *)(uintptr_t(region) + (alignment - offset));
    void *end = (void *)(uintptr_t(front) + size);
    if (front != region)
        JS_ALWAYS_TRUE(0 == munmap(region, alignment - offset));
    if (uintptr_t(end) != regionEnd)
        JS_ALWAYS_TRUE(0 == munmap(end, regionEnd - uintptr_t(end)));

    JS_ASSERT(uintptr_t(front) % alignment == 0);
    return front;
}

void
gc::UnmapPages(void *p, size_t size)
{
    JS_ALWAYS_TRUE(0 == munmap(p, size));
}

bool
gc::MarkPagesUnused(void *p, size_t size)
{
    if (!DecommitEnabled())
        return false;

    JS_ASSERT(uintptr_t(p) % PageSize == 0);
    int result = madvise(p, size, MADV_DONTNEED);
    return result != -1;
}

bool
gc::MarkPagesInUse(void *p, size_t size)
{
    JS_ASSERT(uintptr_t(p) % PageSize == 0);
    return true;
}

size_t
gc::GetPageFaultCount()
{
    struct rusage usage;
    int err = getrusage(RUSAGE_SELF, &usage);
    if (err)
        return 0;
    return usage.ru_majflt;
}

#else
#error "Memory mapping functions are not defined for your OS."
#endif
