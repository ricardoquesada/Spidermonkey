/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef DEBUG

#ifndef jsion_c1spewer_h__
#define jsion_c1spewer_h__

#include "jsscript.h"

#include "js/RootingAPI.h"

namespace js {
namespace ion {

class MDefinition;
class MInstruction;
class MBasicBlock;
class MIRGraph;
class LinearScanAllocator;
class LInstruction;

class C1Spewer
{
    MIRGraph *graph;
    HandleScript script;
    FILE *spewout_;

  public:
    C1Spewer()
      : graph(NULL), script(NullPtr()), spewout_(NULL)
    { }

    bool init(const char *path);
    void beginFunction(MIRGraph *graph, HandleScript script);
    void spewPass(const char *pass);
    void spewIntervals(const char *pass, LinearScanAllocator *regalloc);
    void endFunction();
    void finish();

  private:
    void spewPass(FILE *fp, MBasicBlock *block);
    void spewIntervals(FILE *fp, LinearScanAllocator *regalloc, LInstruction *ins, size_t &nextId);
    void spewIntervals(FILE *fp, MBasicBlock *block, LinearScanAllocator *regalloc, size_t &nextId);
};

} // namespace ion
} // namespace js

#endif // jsion_c1spewer_h__

#endif /* DEBUG */

