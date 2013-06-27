/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "EffectiveAddressAnalysis.h"

using namespace js;
using namespace ion;

#ifdef JS_ASMJS
static void
AnalyzeLsh(MBasicBlock *block, MLsh *lsh)
{
    if (lsh->specialization() != MIRType_Int32)
        return;

    MDefinition *index = lsh->lhs();
    JS_ASSERT(index->type() == MIRType_Int32);

    MDefinition *shift = lsh->rhs();
    if (!shift->isConstant())
        return;

    Value shiftValue = shift->toConstant()->value();
    if (!shiftValue.isInt32() || !IsShiftInScaleRange(shiftValue.toInt32()))
        return;

    Scale scale = ShiftToScale(shiftValue.toInt32());

    int32_t displacement = 0;
    MInstruction *last = lsh;
    MDefinition *base = NULL;
    while (true) {
        if (last->useCount() != 1)
            break;

        MUseIterator use = last->usesBegin();
        if (!use->consumer()->isDefinition() || !use->consumer()->toDefinition()->isAdd())
            break;

        MAdd *add = use->consumer()->toDefinition()->toAdd();
        if (add->specialization() != MIRType_Int32 || !add->isTruncated())
            break;

        MDefinition *other = add->getOperand(1 - use->index());

        if (other->isConstant()) {
            displacement += other->toConstant()->value().toInt32();
        } else {
            if (base)
                break;
            base = other;
        }

        last = add;
    }

    if (!base) {
        uint32_t elemSize = 1 << ScaleToShift(scale);
        if (displacement % elemSize != 0)
            return;

        if (last->useCount() != 1)
            return;

        MUseIterator use = last->usesBegin();
        if (!use->consumer()->isDefinition() || !use->consumer()->toDefinition()->isBitAnd())
            return;

        MBitAnd *bitAnd = use->consumer()->toDefinition()->toBitAnd();
        MDefinition *other = bitAnd->getOperand(1 - use->index());
        if (!other->isConstant() || !other->toConstant()->value().isInt32())
            return;

        uint32_t bitsClearedByShift = elemSize - 1;
        uint32_t bitsClearedByMask = ~uint32_t(other->toConstant()->value().toInt32());
        if ((bitsClearedByShift & bitsClearedByMask) != bitsClearedByMask)
            return;

        bitAnd->replaceAllUsesWith(last);
        return;
    }

    MEffectiveAddress *eaddr = MEffectiveAddress::New(base, index, scale, displacement);
    last->replaceAllUsesWith(eaddr);
    block->insertAfter(last, eaddr);
}
#endif

// This analysis converts patterns of the form:
//   truncate(x + (y << {0,1,2,3}))
//   truncate(x + (y << {0,1,2,3}) + imm32)
// into a single lea instruction, and patterns of the form:
//   asmload(x + imm32)
//   asmload(x << {0,1,2,3})
//   asmload((x << {0,1,2,3}) + imm32)
//   asmload((x << {0,1,2,3}) & mask)            (where mask is redundant with shift)
//   asmload(((x << {0,1,2,3}) + imm32) & mask)  (where mask is redundant with shift + imm32)
// into a single asmload instruction (and for asmstore too).
//
// Additionally, we should consider the general forms:
//   truncate(x + y + imm32)
//   truncate((y << {0,1,2,3}) + imm32)
bool
EffectiveAddressAnalysis::analyze()
{
#if defined(JS_ASMJS)
    for (ReversePostorderIterator block(graph_.rpoBegin()); block != graph_.rpoEnd(); block++) {
        for (MInstructionIterator i = block->begin(); i != block->end(); i++) {
            if (i->isLsh())
                AnalyzeLsh(*block, i->toLsh());
        }
    }
#endif
    return true;
}
