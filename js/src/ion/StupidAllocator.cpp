/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StupidAllocator.h"

using namespace js;
using namespace js::ion;

static inline uint32_t
DefaultStackSlot(uint32_t vreg)
{
#if JS_BITS_PER_WORD == 32
    return vreg * 2 + 2;
#else
    return vreg + 1;
#endif
}

LAllocation *
StupidAllocator::stackLocation(uint32_t vreg)
{
    LDefinition *def = virtualRegisters[vreg];
    if (def->policy() == LDefinition::PRESET && def->output()->isArgument())
        return def->output();

    return new LStackSlot(DefaultStackSlot(vreg), def->type() == LDefinition::DOUBLE);
}

StupidAllocator::RegisterIndex
StupidAllocator::registerIndex(AnyRegister reg)
{
    for (size_t i = 0; i < registerCount; i++) {
        if (reg == registers[i].reg)
            return i;
    }
    JS_NOT_REACHED("Bad register");
    return UINT32_MAX;
}

bool
StupidAllocator::init()
{
    if (!RegisterAllocator::init())
        return false;

    if (!virtualRegisters.reserve(graph.numVirtualRegisters()))
        return false;
    for (size_t i = 0; i < graph.numVirtualRegisters(); i++)
        virtualRegisters.infallibleAppend((LDefinition *)NULL);

    for (size_t i = 0; i < graph.numBlocks(); i++) {
        LBlock *block = graph.getBlock(i);
        for (LInstructionIterator ins = block->begin(); ins != block->end(); ins++) {
            for (size_t j = 0; j < ins->numDefs(); j++) {
                LDefinition *def = ins->getDef(j);
                if (def->policy() != LDefinition::PASSTHROUGH)
                    virtualRegisters[def->virtualRegister()] = def;
            }

            for (size_t j = 0; j < ins->numTemps(); j++) {
                LDefinition *def = ins->getTemp(j);
                if (def->isBogusTemp())
                    continue;
                virtualRegisters[def->virtualRegister()] = def;
            }
        }
        for (size_t j = 0; j < block->numPhis(); j++) {
            LPhi *phi = block->getPhi(j);
            LDefinition *def = phi->getDef(0);
            uint32_t vreg = def->virtualRegister();

            virtualRegisters[vreg] = def;
        }
    }

    // Assign physical registers to the tracked allocation.
    {
        registerCount = 0;
        RegisterSet remainingRegisters(allRegisters_);
        while (!remainingRegisters.empty(/* float = */ false))
            registers[registerCount++].reg = AnyRegister(remainingRegisters.takeGeneral());
        while (!remainingRegisters.empty(/* float = */ true))
            registers[registerCount++].reg = AnyRegister(remainingRegisters.takeFloat());
        JS_ASSERT(registerCount <= MAX_REGISTERS);
    }

    return true;
}

static inline bool
AllocationRequiresRegister(const LAllocation *alloc, AnyRegister reg)
{
    if (alloc->isRegister() && alloc->toRegister() == reg)
        return true;
    if (alloc->isUse()) {
        const LUse *use = alloc->toUse();
        if (use->policy() == LUse::FIXED && AnyRegister::FromCode(use->registerCode()) == reg)
            return true;
    }
    return false;
}

static inline bool
RegisterIsReserved(LInstruction *ins, AnyRegister reg)
{
    // Whether reg is already reserved for an input or output of ins.
    for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next()) {
        if (AllocationRequiresRegister(*alloc, reg))
            return true;
    }
    for (size_t i = 0; i < ins->numTemps(); i++) {
        if (AllocationRequiresRegister(ins->getTemp(i)->output(), reg))
            return true;
    }
    for (size_t i = 0; i < ins->numDefs(); i++) {
        if (AllocationRequiresRegister(ins->getDef(i)->output(), reg))
            return true;
    }
    return false;
}

AnyRegister
StupidAllocator::ensureHasRegister(LInstruction *ins, uint32_t vreg)
{
    // Ensure that vreg is held in a register before ins.

    // Check if the virtual register is already held in a physical register.
    RegisterIndex existing = findExistingRegister(vreg);
    if (existing != UINT32_MAX) {
        if (RegisterIsReserved(ins, registers[existing].reg)) {
            evictRegister(ins, existing);
        } else {
            registers[existing].age = ins->id();
            return registers[existing].reg;
        }
    }

    RegisterIndex best = allocateRegister(ins, vreg);
    loadRegister(ins, vreg, best);

    return registers[best].reg;
}

StupidAllocator::RegisterIndex
StupidAllocator::allocateRegister(LInstruction *ins, uint32_t vreg)
{
    // Pick a register for vreg, evicting an existing register if necessary.
    // Spill code will be placed before ins, and no existing allocated input
    // for ins will be touched.
    JS_ASSERT(ins);

    LDefinition *def = virtualRegisters[vreg];
    JS_ASSERT(def);

    RegisterIndex best = UINT32_MAX;

    for (size_t i = 0; i < registerCount; i++) {
        AnyRegister reg = registers[i].reg;

        if (reg.isFloat() != (def->type() == LDefinition::DOUBLE))
            continue;

        // Skip the register if it is in use for an allocated input or output.
        if (RegisterIsReserved(ins, reg))
            continue;

        if (registers[i].vreg == MISSING_ALLOCATION ||
            best == UINT32_MAX ||
            registers[best].age > registers[i].age)
        {
            best = i;
        }
    }

    evictRegister(ins, best);
    return best;
}

void
StupidAllocator::syncRegister(LInstruction *ins, RegisterIndex index)
{
    if (registers[index].dirty) {
        LMoveGroup *input = getInputMoveGroup(ins->id());
        LAllocation *source = new LAllocation(registers[index].reg);

        uint32_t existing = registers[index].vreg;
        LAllocation *dest = stackLocation(existing);
        input->addAfter(source, dest);

        registers[index].dirty = false;
    }
}

void
StupidAllocator::evictRegister(LInstruction *ins, RegisterIndex index)
{
    syncRegister(ins, index);
    registers[index].set(MISSING_ALLOCATION);
}

void
StupidAllocator::loadRegister(LInstruction *ins, uint32_t vreg, RegisterIndex index)
{
    // Load a vreg from its stack location to a register.
    LMoveGroup *input = getInputMoveGroup(ins->id());
    LAllocation *source = stackLocation(vreg);
    LAllocation *dest = new LAllocation(registers[index].reg);
    input->addAfter(source, dest);
    registers[index].set(vreg, ins);
}

StupidAllocator::RegisterIndex
StupidAllocator::findExistingRegister(uint32_t vreg)
{
    for (size_t i = 0; i < registerCount; i++) {
        if (registers[i].vreg == vreg)
            return i;
    }
    return UINT32_MAX;
}

bool
StupidAllocator::go()
{
    // This register allocator is intended to be as simple as possible, while
    // still being complicated enough to share properties with more complicated
    // allocators. Namely, physical registers may be used to carry virtual
    // registers across LIR instructions, but not across basic blocks.
    //
    // This algorithm does not pay any attention to liveness. It is performed
    // as a single forward pass through the basic blocks in the program. As
    // virtual registers and temporaries are defined they are assigned physical
    // registers, evicting existing allocations in an LRU fashion.

    // For virtual registers not carried in a register, a canonical spill
    // location is used. Each vreg has a different spill location; since we do
    // not track liveness we cannot determine that two vregs have disjoint
    // lifetimes. Thus, the maximum stack height is the number of vregs (scaled
    // by two on 32 bit platforms to allow storing double values).
    graph.setLocalSlotCount(DefaultStackSlot(graph.numVirtualRegisters() - 1) + 1);

    if (!init())
        return false;

    for (size_t blockIndex = 0; blockIndex < graph.numBlocks(); blockIndex++) {
        LBlock *block = graph.getBlock(blockIndex);
        JS_ASSERT(block->mir()->id() == blockIndex);

        for (size_t i = 0; i < registerCount; i++)
            registers[i].set(MISSING_ALLOCATION);

        for (LInstructionIterator iter = block->begin(); iter != block->end(); iter++) {
            LInstruction *ins = *iter;

            if (ins == *block->rbegin())
                syncForBlockEnd(block, ins);

            allocateForInstruction(ins);
        }
    }

    return true;
}

void
StupidAllocator::syncForBlockEnd(LBlock *block, LInstruction *ins)
{
    // Sync any dirty registers, and update the synced state for phi nodes at
    // each successor of a block. We cannot conflate the storage for phis with
    // that of their inputs, as we cannot prove the live ranges of the phi and
    // its input do not overlap. The values for the two may additionally be
    // different, as the phi could be for the value of the input in a previous
    // loop iteration.

    for (size_t i = 0; i < registerCount; i++)
        syncRegister(ins, i);

    LMoveGroup *group = NULL;

    MBasicBlock *successor = block->mir()->successorWithPhis();
    if (successor) {
        uint32_t position = block->mir()->positionInPhiSuccessor();
        LBlock *lirsuccessor = graph.getBlock(successor->id());
        for (size_t i = 0; i < lirsuccessor->numPhis(); i++) {
            LPhi *phi = lirsuccessor->getPhi(i);

            uint32_t sourcevreg = phi->getOperand(position)->toUse()->virtualRegister();
            uint32_t destvreg = phi->getDef(0)->virtualRegister();

            if (sourcevreg == destvreg)
                continue;

            LAllocation *source = stackLocation(sourcevreg);
            LAllocation *dest = stackLocation(destvreg);

            if (!group) {
                // The moves we insert here need to happen simultaneously with
                // each other, yet after any existing moves before the instruction.
                LMoveGroup *input = getInputMoveGroup(ins->id());
                if (input->numMoves() == 0) {
                    group = input;
                } else {
                    group = new LMoveGroup;
                    block->insertAfter(input, group);
                }
            }

            group->add(source, dest);
        }
    }
}

void
StupidAllocator::allocateForInstruction(LInstruction *ins)
{
    // Sync all registers before making a call.
    if (ins->isCall()) {
        for (size_t i = 0; i < registerCount; i++)
            syncRegister(ins, i);
    }

    // Allocate for inputs which are required to be in registers.
    for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next()) {
        if (!alloc->isUse())
            continue;
        LUse *use = alloc->toUse();
        uint32_t vreg = use->virtualRegister();
        if (use->policy() == LUse::REGISTER) {
            AnyRegister reg = ensureHasRegister(ins, vreg);
            alloc.replace(LAllocation(reg));
        } else if (use->policy() == LUse::FIXED) {
            AnyRegister reg = AnyRegister::FromCode(use->registerCode());
            RegisterIndex index = registerIndex(reg);
            if (registers[index].vreg != vreg) {
                evictRegister(ins, index);
                RegisterIndex existing = findExistingRegister(vreg);
                if (existing != UINT32_MAX)
                    evictRegister(ins, existing);
                loadRegister(ins, vreg, index);
            }
            alloc.replace(LAllocation(reg));
        } else {
            // Inputs which are not required to be in a register are not
            // allocated until after temps/definitions, as the latter may need
            // to evict registers which hold these inputs.
        }
    }

    // Find registers to hold all temporaries and outputs of the instruction.
    for (size_t i = 0; i < ins->numTemps(); i++) {
        LDefinition *def = ins->getTemp(i);
        if (!def->isBogusTemp())
            allocateForDefinition(ins, def);
    }
    for (size_t i = 0; i < ins->numDefs(); i++) {
        LDefinition *def = ins->getDef(i);
        if (def->policy() != LDefinition::PASSTHROUGH)
            allocateForDefinition(ins, def);
    }

    // Allocate for remaining inputs which do not need to be in registers.
    for (LInstruction::InputIterator alloc(*ins); alloc.more(); alloc.next()) {
        if (!alloc->isUse())
            continue;
        LUse *use = alloc->toUse();
        uint32_t vreg = use->virtualRegister();
        JS_ASSERT(use->policy() != LUse::REGISTER && use->policy() != LUse::FIXED);

        RegisterIndex index = findExistingRegister(vreg);
        if (index == UINT32_MAX) {
            LAllocation *stack = stackLocation(use->virtualRegister());
            alloc.replace(*stack);
        } else {
            registers[index].age = ins->id();
            alloc.replace(LAllocation(registers[index].reg));
        }
    }

    // If this is a call, evict all registers except for those holding outputs.
    if (ins->isCall()) {
        for (size_t i = 0; i < registerCount; i++) {
            if (!registers[i].dirty)
                registers[i].set(MISSING_ALLOCATION);
        }
    }
}

void
StupidAllocator::allocateForDefinition(LInstruction *ins, LDefinition *def)
{
    uint32_t vreg = def->virtualRegister();

    CodePosition from;
    if ((def->output()->isRegister() && def->policy() == LDefinition::PRESET) ||
        def->policy() == LDefinition::MUST_REUSE_INPUT)
    {
        // Result will be in a specific register, spill any vreg held in
        // that register before the instruction.
        RegisterIndex index =
            registerIndex(def->policy() == LDefinition::PRESET
                          ? def->output()->toRegister()
                          : ins->getOperand(def->getReusedInput())->toRegister());
        evictRegister(ins, index);
        registers[index].set(vreg, ins, true);
        def->setOutput(LAllocation(registers[index].reg));
    } else if (def->policy() == LDefinition::PRESET) {
        // The result must be a stack location.
        def->setOutput(*stackLocation(vreg));
    } else {
        // Find a register to hold the result of the instruction.
        RegisterIndex best = allocateRegister(ins, vreg);
        registers[best].set(vreg, ins, true);
        def->setOutput(LAllocation(registers[best].reg));
    }
}
