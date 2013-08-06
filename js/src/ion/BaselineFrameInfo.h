/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(jsion_baseline_frameinfo_h__) && defined(JS_ION)
#define jsion_baseline_frameinfo_h__

#include "jscntxt.h"
#include "jscompartment.h"

#include "BaselineJIT.h"
#include "BaselineFrame.h"
#include "BaselineRegisters.h"
#include "BytecodeAnalysis.h"
#include "IonMacroAssembler.h"
#include "FixedList.h"

namespace js {
namespace ion {

// FrameInfo overview.
//
// FrameInfo is used by the compiler to track values stored in the frame. This
// includes locals, arguments and stack values. Locals and arguments are always
// fully synced. Stack values can either be synced, stored as constant, stored in
// a Value register or refer to a local slot. Syncing a StackValue ensures it's
// stored on the stack, e.g. kind == Stack.
//
// To see how this works, consider the following statement:
//
//    var y = x + 9;
//
// Here two values are pushed: StackValue(LocalSlot(0)) and StackValue(Int32Value(9)).
// Only when we reach the ADD op, code is generated to load the operands directly
// into the right operand registers and sync all other stack values.
//
// For stack values, the following invariants hold (and are checked between ops):
//
// (1) If a value is synced (kind == Stack), all values below it must also be synced.
//     In other words, values with kind other than Stack can only appear on top of the
//     abstract stack.
//
// (2) When we call a stub or IC, all values still on the stack must be synced.

// Represents a value pushed on the stack. Note that StackValue is not used for
// locals or arguments since these are always fully synced.
class StackValue
{
  public:
    enum Kind {
        Constant,
        Register,
        Stack,
        LocalSlot,
        ArgSlot,
        ThisSlot
#ifdef DEBUG
        // In debug builds, assert Kind is initialized.
        , Uninitialized
#endif
    };

  private:
    Kind kind_;

    union {
        struct {
            Value v;
        } constant;
        struct {
            mozilla::AlignedStorage2<ValueOperand> reg;
        } reg;
        struct {
            uint32_t slot;
        } local;
        struct {
            uint32_t slot;
        } arg;
    } data;

    JSValueType knownType_;

  public:
    StackValue() {
        reset();
    }

    Kind kind() const {
        return kind_;
    }
    bool hasKnownType() const {
        return knownType_ != JSVAL_TYPE_UNKNOWN;
    }
    bool hasKnownType(JSValueType type) const {
        JS_ASSERT(type != JSVAL_TYPE_UNKNOWN);
        return knownType_ == type;
    }
    bool isKnownBoolean() const {
        return hasKnownType(JSVAL_TYPE_BOOLEAN);
    }
    JSValueType knownType() const {
        JS_ASSERT(hasKnownType());
        return knownType_;
    }
    void reset() {
#ifdef DEBUG
        kind_ = Uninitialized;
        knownType_ = JSVAL_TYPE_UNKNOWN;
#endif
    }
    Value constant() const {
        JS_ASSERT(kind_ == Constant);
        return data.constant.v;
    }
    ValueOperand reg() const {
        JS_ASSERT(kind_ == Register);
        return *data.reg.reg.addr();
    }
    uint32_t localSlot() const {
        JS_ASSERT(kind_ == LocalSlot);
        return data.local.slot;
    }
    uint32_t argSlot() const {
        JS_ASSERT(kind_ == ArgSlot);
        return data.arg.slot;
    }

    void setConstant(const Value &v) {
        kind_ = Constant;
        data.constant.v = v;
        knownType_ = v.isDouble() ? JSVAL_TYPE_DOUBLE : v.extractNonDoubleType();
    }
    void setRegister(const ValueOperand &val, JSValueType knownType = JSVAL_TYPE_UNKNOWN) {
        kind_ = Register;
        *data.reg.reg.addr() = val;
        knownType_ = knownType;
    }
    void setLocalSlot(uint32_t slot) {
        kind_ = LocalSlot;
        data.local.slot = slot;
        knownType_ = JSVAL_TYPE_UNKNOWN;
    }
    void setArgSlot(uint32_t slot) {
        kind_ = ArgSlot;
        data.arg.slot = slot;
        knownType_ = JSVAL_TYPE_UNKNOWN;
    }
    void setThis() {
        kind_ = ThisSlot;
        knownType_ = JSVAL_TYPE_UNKNOWN;
    }
    void setStack() {
        kind_ = Stack;
        knownType_ = JSVAL_TYPE_UNKNOWN;
    }
};

enum StackAdjustment { AdjustStack, DontAdjustStack };

class BaselineCompilerShared;

class FrameInfo
{
    BaselineCompilerShared &compiler;
    RootedScript script;
    MacroAssembler &masm;

    FixedList<StackValue> stack;
    size_t spIndex;

  public:
    FrameInfo(JSContext *cx, BaselineCompilerShared &compiler, HandleScript script,
              MacroAssembler &masm)
      : compiler(compiler),
        script(cx, script),
        masm(masm),
        stack(),
        spIndex(0)
    { }

    bool init();

    uint32_t nlocals() const {
        return script->nfixed;
    }
    uint32_t nargs() const {
        return script->function()->nargs;
    }

  private:
    inline StackValue *rawPush() {
        StackValue *val = &stack[spIndex++];
        val->reset();
        return val;
    }

  public:
    inline size_t stackDepth() const {
        return spIndex;
    }
    inline void setStackDepth(uint32_t newDepth) {
        if (newDepth <= stackDepth()) {
            spIndex = newDepth;
        } else {
            uint32_t diff = newDepth - stackDepth();
            for (uint32_t i = 0; i < diff; i++) {
                StackValue *val = rawPush();
                val->setStack();
            }

            JS_ASSERT(spIndex == newDepth);
        }
    }
    inline StackValue *peek(int32_t index) const {
        JS_ASSERT(index < 0);
        return const_cast<StackValue *>(&stack[spIndex + index]);
    }

    inline void pop(StackAdjustment adjust = AdjustStack) {
        spIndex--;
        StackValue *popped = &stack[spIndex];

        if (adjust == AdjustStack && popped->kind() == StackValue::Stack)
            masm.addPtr(Imm32(sizeof(Value)), BaselineStackReg);

        // Assert when anything uses this value.
        popped->reset();
    }
    inline void popn(uint32_t n, StackAdjustment adjust = AdjustStack) {
        uint32_t poppedStack = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (peek(-1)->kind() == StackValue::Stack)
                poppedStack++;
            pop(DontAdjustStack);
        }
        if (adjust == AdjustStack && poppedStack > 0)
            masm.addPtr(Imm32(sizeof(Value) * poppedStack), BaselineStackReg);
    }
    inline void push(const Value &val) {
        StackValue *sv = rawPush();
        sv->setConstant(val);
    }
    inline void push(const ValueOperand &val, JSValueType knownType=JSVAL_TYPE_UNKNOWN) {
        StackValue *sv = rawPush();
        sv->setRegister(val, knownType);
    }
    inline void pushLocal(uint32_t local) {
        StackValue *sv = rawPush();
        sv->setLocalSlot(local);
    }
    inline void pushArg(uint32_t arg) {
        StackValue *sv = rawPush();
        sv->setArgSlot(arg);
    }
    inline void pushThis() {
        StackValue *sv = rawPush();
        sv->setThis();
    }
    inline void pushScratchValue() {
        masm.pushValue(addressOfScratchValue());
        StackValue *sv = rawPush();
        sv->setStack();
    }
    inline Address addressOfLocal(size_t local) const {
#ifdef DEBUG
        if (local >= nlocals()) {
            // GETLOCAL and SETLOCAL can be used to access stack values. This is
            // fine, as long as they are synced.
            size_t slot = local - nlocals();
            JS_ASSERT(slot < stackDepth());
            JS_ASSERT(stack[slot].kind() == StackValue::Stack);
        }
#endif
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfLocal(local));
    }
    Address addressOfArg(size_t arg) const {
        JS_ASSERT(arg < nargs());
        return Address(BaselineFrameReg, BaselineFrame::offsetOfArg(arg));
    }
    Address addressOfThis() const {
        return Address(BaselineFrameReg, BaselineFrame::offsetOfThis());
    }
    Address addressOfCallee() const {
        return Address(BaselineFrameReg, BaselineFrame::offsetOfCalleeToken());
    }
    Address addressOfScopeChain() const {
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfScopeChain());
    }
    Address addressOfBlockChain() const {
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfBlockChain());
    }
    Address addressOfFlags() const {
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFlags());
    }
    Address addressOfEvalScript() const {
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfEvalScript());
    }
    Address addressOfReturnValue() const {
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfReturnValue());
    }
    Address addressOfStackValue(const StackValue *value) const {
        JS_ASSERT(value->kind() == StackValue::Stack);
        size_t slot = value - &stack[0];
        JS_ASSERT(slot < stackDepth());
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfLocal(nlocals() + slot));
    }
    Address addressOfScratchValue() const {
        return Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfScratchValue());
    }

    void popValue(ValueOperand dest);

    void sync(StackValue *val);
    void syncStack(uint32_t uses);
    uint32_t numUnsyncedSlots();
    void popRegsAndSync(uint32_t uses);

    inline void assertSyncedStack() const {
        JS_ASSERT_IF(stackDepth() > 0, peek(-1)->kind() == StackValue::Stack);
    }

#ifdef DEBUG
    // Assert the state is valid before excuting "pc".
    void assertValidState(jsbytecode *pc);
#else
    inline void assertValidState(jsbytecode *pc) {}
#endif
};

} // namespace ion
} // namespace js

#endif

