/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
 *
 * Copyright (C) 2008 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "ExecutableAllocator.h"

#include "js/MemoryMetrics.h"

#if ENABLE_ASSEMBLER

#include "prmjtime.h"

namespace JSC {

size_t ExecutableAllocator::pageSize = 0;
size_t ExecutableAllocator::largeAllocSize = 0;

ExecutablePool::~ExecutablePool()
{
    m_allocator->releasePoolPages(this);
}

void
ExecutableAllocator::sizeOfCode(JS::CodeSizes *sizes) const
{
    *sizes = JS::CodeSizes();

    if (m_pools.initialized()) {
        for (ExecPoolHashSet::Range r = m_pools.all(); !r.empty(); r.popFront()) {
            ExecutablePool* pool = r.front();
            sizes->jaeger   += pool->m_jaegerCodeBytes;
            sizes->ion      += pool->m_ionCodeBytes;
            sizes->baseline += pool->m_baselineCodeBytes;
            sizes->asmJS    += pool->m_asmJSCodeBytes;
            sizes->regexp   += pool->m_regexpCodeBytes;
            sizes->other    += pool->m_otherCodeBytes;
            sizes->unused   += pool->m_allocation.size - pool->m_jaegerCodeBytes
                                                       - pool->m_ionCodeBytes
                                                       - pool->m_baselineCodeBytes
                                                       - pool->m_asmJSCodeBytes
                                                       - pool->m_regexpCodeBytes
                                                       - pool->m_otherCodeBytes;
        }
    }
}

}

#endif // HAVE(ASSEMBLER)
