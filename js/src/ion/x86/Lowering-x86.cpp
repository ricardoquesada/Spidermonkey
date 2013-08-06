/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Lowering-x86.h"

#include "ion/MIR.h"
#include "Assembler-x86.h"
#include "ion/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::ion;

bool
LIRGeneratorX86::useBox(LInstruction *lir, size_t n, MDefinition *mir,
                        LUse::Policy policy, bool useAtStart)
{
    JS_ASSERT(mir->type() == MIRType_Value);

    if (!ensureDefined(mir))
        return false;
    lir->setOperand(n, LUse(mir->virtualRegister(), policy, useAtStart));
    lir->setOperand(n + 1, LUse(VirtualRegisterOfPayload(mir), policy, useAtStart));
    return true;
}

bool
LIRGeneratorX86::useBoxFixed(LInstruction *lir, size_t n, MDefinition *mir, Register reg1,
                             Register reg2)
{
    JS_ASSERT(mir->type() == MIRType_Value);
    JS_ASSERT(reg1 != reg2);

    if (!ensureDefined(mir))
        return false;
    lir->setOperand(n, LUse(reg1, mir->virtualRegister()));
    lir->setOperand(n + 1, LUse(reg2, VirtualRegisterOfPayload(mir)));
    return true;
}

bool
LIRGeneratorX86::visitBox(MBox *box)
{
    MDefinition *inner = box->getOperand(0);

    // If the box wrapped a double, it needs a new register.
    if (inner->type() == MIRType_Double)
        return defineBox(new LBoxDouble(useRegisterAtStart(inner), tempCopy(inner, 0)), box);

    if (box->canEmitAtUses())
        return emitAtUses(box);

    if (inner->isConstant())
        return defineBox(new LValue(inner->toConstant()->value()), box);

    LBox *lir = new LBox(use(inner), inner->type());

    // Otherwise, we should not define a new register for the payload portion
    // of the output, so bypass defineBox().
    uint32_t vreg = getVirtualRegister();
    if (vreg >= MAX_VIRTUAL_REGISTERS)
        return false;

    // Note that because we're using PASSTHROUGH, we do not change the type of
    // the definition. We also do not define the first output as "TYPE",
    // because it has no corresponding payload at (vreg + 1). Also note that
    // although we copy the input's original type for the payload half of the
    // definition, this is only for clarity. PASSTHROUGH definitions are
    // ignored.
    lir->setDef(0, LDefinition(vreg, LDefinition::GENERAL));
    lir->setDef(1, LDefinition(inner->virtualRegister(), LDefinition::TypeFrom(inner->type()),
                               LDefinition::PASSTHROUGH));
    box->setVirtualRegister(vreg);
    return add(lir);
}

bool
LIRGeneratorX86::visitUnbox(MUnbox *unbox)
{
    // An unbox on x86 reads in a type tag (either in memory or a register) and
    // a payload. Unlike most instructions conusming a box, we ask for the type
    // second, so that the result can re-use the first input.
    MDefinition *inner = unbox->getOperand(0);

    if (!ensureDefined(inner))
        return false;

    if (unbox->type() == MIRType_Double) {
        LUnboxDouble *lir = new LUnboxDouble;
        if (unbox->fallible() && !assignSnapshot(lir, unbox->bailoutKind()))
            return false;
        if (!useBox(lir, LUnboxDouble::Input, inner))
            return false;
        return define(lir, unbox);
    }

    // Swap the order we use the box pieces so we can re-use the payload register.
    LUnbox *lir = new LUnbox;
    lir->setOperand(0, usePayloadInRegisterAtStart(inner));
    lir->setOperand(1, useType(inner, LUse::ANY));

    if (unbox->fallible() && !assignSnapshot(lir, unbox->bailoutKind()))
        return false;

    // Note that PASSTHROUGH here is illegal, since types and payloads form two
    // separate intervals. If the type becomes dead before the payload, it
    // could be used as a Value without the type being recoverable. Unbox's
    // purpose is to eagerly kill the definition of a type tag, so keeping both
    // alive (for the purpose of gcmaps) is unappealing. Instead, we create a
    // new virtual register.
    return defineReuseInput(lir, unbox, 0);
}

bool
LIRGeneratorX86::visitReturn(MReturn *ret)
{
    MDefinition *opd = ret->getOperand(0);
    JS_ASSERT(opd->type() == MIRType_Value);

    LReturn *ins = new LReturn;
    ins->setOperand(0, LUse(JSReturnReg_Type));
    ins->setOperand(1, LUse(JSReturnReg_Data));
    return fillBoxUses(ins, 0, opd) && add(ins);
}

bool
LIRGeneratorX86::lowerForFPU(LInstructionHelper<1, 2, 0> *ins, MDefinition *mir, MDefinition *lhs, MDefinition *rhs)
{
    ins->setOperand(0, useRegisterAtStart(lhs));
    ins->setOperand(1, use(rhs));
    return defineReuseInput(ins, mir, 0);
}

bool
LIRGeneratorX86::defineUntypedPhi(MPhi *phi, size_t lirIndex)
{
    LPhi *type = current->getPhi(lirIndex + VREG_TYPE_OFFSET);
    LPhi *payload = current->getPhi(lirIndex + VREG_DATA_OFFSET);

    uint32_t typeVreg = getVirtualRegister();
    if (typeVreg >= MAX_VIRTUAL_REGISTERS)
        return false;

    phi->setVirtualRegister(typeVreg);

    uint32_t payloadVreg = getVirtualRegister();
    if (payloadVreg >= MAX_VIRTUAL_REGISTERS)
        return false;
    JS_ASSERT(typeVreg + 1 == payloadVreg);

    type->setDef(0, LDefinition(typeVreg, LDefinition::TYPE));
    payload->setDef(0, LDefinition(payloadVreg, LDefinition::PAYLOAD));
    annotate(type);
    annotate(payload);
    return true;
}

void
LIRGeneratorX86::lowerUntypedPhiInput(MPhi *phi, uint32_t inputPosition, LBlock *block, size_t lirIndex)
{
    MDefinition *operand = phi->getOperand(inputPosition);
    LPhi *type = block->getPhi(lirIndex + VREG_TYPE_OFFSET);
    LPhi *payload = block->getPhi(lirIndex + VREG_DATA_OFFSET);
    type->setOperand(inputPosition, LUse(operand->virtualRegister() + VREG_TYPE_OFFSET, LUse::ANY));
    payload->setOperand(inputPosition, LUse(VirtualRegisterOfPayload(operand), LUse::ANY));
}

bool
LIRGeneratorX86::visitStoreTypedArrayElement(MStoreTypedArrayElement *ins)
{
    JS_ASSERT(ins->elements()->type() == MIRType_Elements);
    JS_ASSERT(ins->index()->type() == MIRType_Int32);

    if (ins->isFloatArray())
        JS_ASSERT(ins->value()->type() == MIRType_Double);
    else
        JS_ASSERT(ins->value()->type() == MIRType_Int32);

    LUse elements = useRegister(ins->elements());
    LAllocation index = useRegisterOrConstant(ins->index());
    LAllocation value;

    // For byte arrays, the value has to be in a byte register on x86.
    if (ins->isByteArray())
        value = useFixed(ins->value(), eax);
    else
        value = useRegisterOrNonDoubleConstant(ins->value());
    return add(new LStoreTypedArrayElement(elements, index, value), ins);
}

bool
LIRGeneratorX86::visitStoreTypedArrayElementHole(MStoreTypedArrayElementHole *ins)
{
    JS_ASSERT(ins->elements()->type() == MIRType_Elements);
    JS_ASSERT(ins->index()->type() == MIRType_Int32);
    JS_ASSERT(ins->length()->type() == MIRType_Int32);

    if (ins->isFloatArray())
        JS_ASSERT(ins->value()->type() == MIRType_Double);
    else
        JS_ASSERT(ins->value()->type() == MIRType_Int32);

    LUse elements = useRegister(ins->elements());
    LAllocation length = useAnyOrConstant(ins->length());
    LAllocation index = useRegisterOrConstant(ins->index());
    LAllocation value;

    // For byte arrays, the value has to be in a byte register on x86.
    if (ins->isByteArray())
        value = useFixed(ins->value(), eax);
    else
        value = useRegisterOrNonDoubleConstant(ins->value());
    return add(new LStoreTypedArrayElementHole(elements, length, index, value), ins);
}

bool
LIRGeneratorX86::visitAsmJSUnsignedToDouble(MAsmJSUnsignedToDouble *ins)
{
    JS_ASSERT(ins->input()->type() == MIRType_Int32);
    LUInt32ToDouble *lir = new LUInt32ToDouble(useRegisterAtStart(ins->input()), temp());
    return define(lir, ins);
}

bool
LIRGeneratorX86::visitAsmJSStoreHeap(MAsmJSStoreHeap *ins)
{
    LAsmJSStoreHeap *lir;
    switch (ins->viewType()) {
      case ArrayBufferView::TYPE_INT8: case ArrayBufferView::TYPE_UINT8:
        // It's a trap! On x86, the 1-byte store can only use one of
        // {al,bl,cl,dl,ah,bh,ch,dh}. That means if the register allocator
        // gives us one of {edi,esi,ebp,esp}, we're out of luck. (The formatter
        // will assert on us.) Ideally, we'd just ask the register allocator to
        // give us one of {al,bl,cl,dl}. For now, just useFixed(al).
        lir = new LAsmJSStoreHeap(useRegister(ins->ptr()),
                                  useFixed(ins->value(), eax));
        break;
      case ArrayBufferView::TYPE_INT16: case ArrayBufferView::TYPE_UINT16:
      case ArrayBufferView::TYPE_INT32: case ArrayBufferView::TYPE_UINT32:
      case ArrayBufferView::TYPE_FLOAT32: case ArrayBufferView::TYPE_FLOAT64:
        // For now, don't allow constants. The immediate operand affects
        // instruction layout which affects patching.
        lir = new LAsmJSStoreHeap(useRegisterAtStart(ins->ptr()),
                                  useRegisterAtStart(ins->value()));
        break;
      default: JS_NOT_REACHED("unexpected array type");
    }

    return add(lir, ins);
}

bool
LIRGeneratorX86::visitStoreTypedArrayElementStatic(MStoreTypedArrayElementStatic *ins)
{
    // The code generated for StoreTypedArrayElementStatic is identical to that
    // for AsmJSStoreHeap, and the same concerns apply.
    LStoreTypedArrayElementStatic *lir;
    switch (ins->viewType()) {
      case ArrayBufferView::TYPE_INT8: case ArrayBufferView::TYPE_UINT8:
      case ArrayBufferView::TYPE_UINT8_CLAMPED:
        lir = new LStoreTypedArrayElementStatic(useRegister(ins->ptr()),
                                                useFixed(ins->value(), eax));
        break;
      case ArrayBufferView::TYPE_INT16: case ArrayBufferView::TYPE_UINT16:
      case ArrayBufferView::TYPE_INT32: case ArrayBufferView::TYPE_UINT32:
      case ArrayBufferView::TYPE_FLOAT32: case ArrayBufferView::TYPE_FLOAT64:
        lir = new LStoreTypedArrayElementStatic(useRegisterAtStart(ins->ptr()),
                                                useRegisterAtStart(ins->value()));
        break;
      default: JS_NOT_REACHED("unexpected array type");
    }

    return add(lir, ins);
}

bool
LIRGeneratorX86::visitAsmJSLoadFuncPtr(MAsmJSLoadFuncPtr *ins)
{
    return define(new LAsmJSLoadFuncPtr(useRegisterAtStart(ins->index())), ins);
}

LGetPropertyCacheT *
LIRGeneratorX86::newLGetPropertyCacheT(MGetPropertyCache *ins)
{
    // Since x86 doesn't have a scratch register and we need one for the
    // indirect jump for dispatch-style ICs, we need a temporary in the case
    // of a double output type as we can't get a scratch from the output.
    LDefinition scratch;
    if (ins->type() == MIRType_Double)
        scratch = temp();
    else
        scratch = LDefinition::BogusTemp();
    return new LGetPropertyCacheT(useRegister(ins->object()), scratch);
}
