/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ion/BytecodeAnalysis.h"
#include "jsopcode.h"
#include "jsopcodeinlines.h"
#include "jsobjinlines.h"

using namespace js;
using namespace js::ion;

BytecodeAnalysis::BytecodeAnalysis(JSScript *script)
  : script_(script),
    infos_()
{
}

bool
BytecodeAnalysis::init()
{
    if (!infos_.growByUninitialized(script_->length))
        return false;

    jsbytecode *end = script_->code + script_->length;

    // Clear all BytecodeInfo.
    mozilla::PodZero(infos_.begin(), infos_.length());
    infos_[0].init(/*stackDepth=*/0);

    for (jsbytecode *pc = script_->code; pc < end; pc += GetBytecodeLength(pc)) {
        JSOp op = JSOp(*pc);
        unsigned offset = pc - script_->code;

        IonSpew(IonSpew_BaselineOp, "Analyzing op @ %d (end=%d): %s",
                int(pc - script_->code), int(end - script_->code), js_CodeName[op]);

        // If this bytecode info has not yet been initialized, it's not reachable.
        if (!infos_[offset].initialized)
            continue;


        unsigned stackDepth = infos_[offset].stackDepth;
#ifdef DEBUG
        for (jsbytecode *chkpc = pc + 1; chkpc < (pc + GetBytecodeLength(pc)); chkpc++)
            JS_ASSERT(!infos_[chkpc - script_->code].initialized);
#endif

        // Treat decompose ops as no-ops which do not adjust the stack. We will
        // pick up the stack depths as we go through the decomposed version.
        if (!(js_CodeSpec[op].format & JOF_DECOMPOSE)) {
            unsigned nuses = GetUseCount(script_, offset);
            unsigned ndefs = GetDefCount(script_, offset);

            JS_ASSERT(stackDepth >= nuses);
            stackDepth -= nuses;
            stackDepth += ndefs;

            // If stack depth exceeds max allowed by analysis, fail fast.
            JS_ASSERT(stackDepth <= BytecodeInfo::MAX_STACK_DEPTH);
        }

        if (op == JSOP_TABLESWITCH) {
            unsigned defaultOffset = offset + GET_JUMP_OFFSET(pc);
            jsbytecode *pc2 = pc + JUMP_OFFSET_LEN;
            int32_t low = GET_JUMP_OFFSET(pc2);
            pc2 += JUMP_OFFSET_LEN;
            int32_t high = GET_JUMP_OFFSET(pc2);
            pc2 += JUMP_OFFSET_LEN;

            infos_[defaultOffset].init(stackDepth);
            infos_[defaultOffset].jumpTarget = true;

            for (int32_t i = low; i <= high; i++) {
                unsigned targetOffset = offset + GET_JUMP_OFFSET(pc2);
                if (targetOffset != offset) {
                    infos_[targetOffset].init(stackDepth);
                    infos_[targetOffset].jumpTarget = true;
                }
                pc2 += JUMP_OFFSET_LEN;
            }
        } else if (op == JSOP_TRY) {
            JSTryNote *tn = script_->trynotes()->vector;
            JSTryNote *tnlimit = tn + script_->trynotes()->length;
            for (; tn < tnlimit; tn++) {
                unsigned startOffset = script_->mainOffset + tn->start;
                if (startOffset == offset + 1) {
                    unsigned catchOffset = startOffset + tn->length;

                    if (tn->kind != JSTRY_ITER) {
                        infos_[catchOffset].init(stackDepth);
                        infos_[catchOffset].jumpTarget = true;
                    }
                }
            }
        }

        bool jump = IsJumpOpcode(op);
        if (jump) {
            // Case instructions do not push the lvalue back when branching.
            unsigned newStackDepth = stackDepth;
            if (op == JSOP_CASE)
                newStackDepth--;

            unsigned targetOffset = offset + GET_JUMP_OFFSET(pc);

            // If this is a a backedge to an un-analyzed segment, analyze from there.
            bool jumpBack = (targetOffset < offset) && !infos_[targetOffset].initialized;

            infos_[targetOffset].init(newStackDepth);
            infos_[targetOffset].jumpTarget = true;

            if (jumpBack)
                pc = script_->code + targetOffset;
        }

        // Handle any fallthrough from this opcode.
        if (BytecodeFallsThrough(op)) {
            jsbytecode *nextpc = pc + GetBytecodeLength(pc);
            JS_ASSERT(nextpc < end);
            unsigned nextOffset = nextpc - script_->code;

            infos_[nextOffset].init(stackDepth);

            if (jump)
                infos_[nextOffset].jumpFallthrough = true;

            // Treat the fallthrough of a branch instruction as a jump target.
            if (jump)
                infos_[nextOffset].jumpTarget = true;
            else
                infos_[nextOffset].fallthrough = true;
        }
    }

    return true;
}
