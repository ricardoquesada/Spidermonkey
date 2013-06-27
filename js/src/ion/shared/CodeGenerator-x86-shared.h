/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsion_codegen_x86_shared_h__
#define jsion_codegen_x86_shared_h__

#include "ion/shared/CodeGenerator-shared.h"

namespace js {
namespace ion {

class OutOfLineBailout;
class OutOfLineUndoALUOperation;
class MulNegativeZeroCheck;
class OutOfLineTruncate;
class OutOfLineTableSwitch;

class CodeGeneratorX86Shared : public CodeGeneratorShared
{
    friend class MoveResolverX86;

    CodeGeneratorX86Shared *thisFromCtor() {
        return this;
    }

    template <typename T>
    bool bailout(const T &t, LSnapshot *snapshot);

  protected:
    // Label for the common return path.
    HeapLabel *returnLabel_;
    HeapLabel *deoptLabel_;

    inline Operand ToOperand(const LAllocation &a) {
        if (a.isGeneralReg())
            return Operand(a.toGeneralReg()->reg());
        if (a.isFloatReg())
            return Operand(a.toFloatReg()->reg());
        return Operand(StackPointer, ToStackOffset(&a));
    }
    inline Operand ToOperand(const LAllocation *a) {
        return ToOperand(*a);
    }
    inline Operand ToOperand(const LDefinition *def) {
        return ToOperand(def->output());
    }

    MoveResolver::MoveOperand toMoveOperand(const LAllocation *a) const;

    bool bailoutIf(Assembler::Condition condition, LSnapshot *snapshot);
    bool bailoutFrom(Label *label, LSnapshot *snapshot);
    bool bailout(LSnapshot *snapshot);

  protected:
    bool generatePrologue();
    bool generateEpilogue();
    bool generateOutOfLineCode();

    Operand createArrayElementOperand(Register elements, const LAllocation *index);

    void emitCompare(MCompare::CompareType type, const LAllocation *left, const LAllocation *right);

    // Emits a branch that directs control flow to the true block if |cond| is
    // true, and the false block if |cond| is false.
    void emitBranch(Assembler::Condition cond, MBasicBlock *ifTrue, MBasicBlock *ifFalse,
                    Assembler::NaNCond ifNaN = Assembler::NaN_Unexpected);
    void emitBranch(Assembler::DoubleCondition cond, MBasicBlock *ifTrue, MBasicBlock *ifFalse);

    bool emitTableSwitchDispatch(MTableSwitch *mir, const Register &index, const Register &base);

  public:
    CodeGeneratorX86Shared(MIRGenerator *gen, LIRGraph *graph, MacroAssembler *masm);

  public:
    // Instruction visitors.
    virtual bool visitDouble(LDouble *ins);
    virtual bool visitMinMaxD(LMinMaxD *ins);
    virtual bool visitAbsD(LAbsD *ins);
    virtual bool visitSqrtD(LSqrtD *ins);
    virtual bool visitPowHalfD(LPowHalfD *ins);
    virtual bool visitAddI(LAddI *ins);
    virtual bool visitSubI(LSubI *ins);
    virtual bool visitMulI(LMulI *ins);
    virtual bool visitDivI(LDivI *ins);
    virtual bool visitModI(LModI *ins);
    virtual bool visitModPowTwoI(LModPowTwoI *ins);
    virtual bool visitBitNotI(LBitNotI *ins);
    virtual bool visitBitOpI(LBitOpI *ins);
    virtual bool visitShiftI(LShiftI *ins);
    virtual bool visitUrshD(LUrshD *ins);
    virtual bool visitMoveGroup(LMoveGroup *group);
    virtual bool visitTestIAndBranch(LTestIAndBranch *test);
    virtual bool visitTestDAndBranch(LTestDAndBranch *test);
    virtual bool visitCompare(LCompare *comp);
    virtual bool visitCompareAndBranch(LCompareAndBranch *comp);
    virtual bool visitCompareD(LCompareD *comp);
    virtual bool visitCompareDAndBranch(LCompareDAndBranch *comp);
    virtual bool visitNotI(LNotI *comp);
    virtual bool visitNotD(LNotD *comp);
    virtual bool visitMathD(LMathD *math);
    virtual bool visitFloor(LFloor *lir);
    virtual bool visitRound(LRound *lir);
    virtual bool visitGuardShape(LGuardShape *guard);
    virtual bool visitGuardClass(LGuardClass *guard);
    virtual bool visitTruncateDToInt32(LTruncateDToInt32 *ins);
    virtual bool visitEffectiveAddress(LEffectiveAddress *ins);
    virtual bool visitAsmJSDivOrMod(LAsmJSDivOrMod *ins);
    virtual bool visitAsmJSPassStackArg(LAsmJSPassStackArg *ins);

    // Out of line visitors.
    bool visitOutOfLineBailout(OutOfLineBailout *ool);
    bool visitOutOfLineUndoALUOperation(OutOfLineUndoALUOperation *ool);
    bool visitMulNegativeZeroCheck(MulNegativeZeroCheck *ool);
    bool visitOutOfLineTruncate(OutOfLineTruncate *ool);
    bool visitOutOfLineTableSwitch(OutOfLineTableSwitch *ool);
    bool generateInvalidateEpilogue();
};

// An out-of-line bailout thunk.
class OutOfLineBailout : public OutOfLineCodeBase<CodeGeneratorX86Shared>
{
    LSnapshot *snapshot_;

  public:
    OutOfLineBailout(LSnapshot *snapshot)
      : snapshot_(snapshot)
    { }

    bool accept(CodeGeneratorX86Shared *codegen);

    LSnapshot *snapshot() const {
        return snapshot_;
    }
};

} // namespace ion
} // namespace js

#endif // jsion_codegen_x86_shared_h__

