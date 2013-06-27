/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(jsion_ion_h__) && defined(JS_ION)
#define jsion_ion_h__

#include "jscntxt.h"
#include "jscompartment.h"
#include "IonCode.h"
#include "CompileInfo.h"
#include "jsinfer.h"
#include "jsinterp.h"

namespace js {
namespace ion {

class TempAllocator;
class ParallelCompileContext; // in ParallelArrayAnalysis.h

// Possible register allocators which may be used.
enum IonRegisterAllocator {
    RegisterAllocator_LSRA,
    RegisterAllocator_Backtracking,
    RegisterAllocator_Stupid
};

struct IonOptions
{
    // Toggles whether global value numbering is used.
    //
    // Default: true
    bool gvn;

    // Toggles whether global value numbering is optimistic (true) or
    // pessimistic (false).
    //
    // Default: true
    bool gvnIsOptimistic;

    // Toggles whether loop invariant code motion is performed.
    //
    // Default: true
    bool licm;

    // Toggles whether functions may be entered at loop headers.
    //
    // Default: true
    bool osr;

    // Toggles whether large scripts are rejected.
    //
    // Default: true
    bool limitScriptSize;

    // Describes which register allocator to use.
    //
    // Default: LSRA
    IonRegisterAllocator registerAllocator;

    // Toggles whether inlining is performed.
    //
    // Default: true
    bool inlining;

    // Toggles whether Edge Case Analysis is used.
    //
    // Default: true
    bool edgeCaseAnalysis;

    // Toggles whether Range Analysis is used.
    //
    // Default: true
    bool rangeAnalysis;

    // Toggles whether Unreachable Code Elimination is performed.
    //
    // Default: true
    bool uce;

    // Toggles whether Effective Address Analysis is performed.
    //
    // Default: true
    bool eaa;

    // Toggles whether compilation occurs off the main thread.
    //
    // Default: true iff there are at least two CPUs available
    bool parallelCompilation;

    // How many invocations or loop iterations are needed before functions
    // are compiled.
    //
    // Default: 10,240
    uint32_t usesBeforeCompile;

    // How many invocations or loop iterations are needed before functions
    // are compiled when JM is disabled.
    //
    // Default: 40
    uint32_t usesBeforeCompileNoJaeger;

    // How many invocations or loop iterations are needed before calls
    // are inlined, as a fraction of usesBeforeCompile.
    //
    // Default: .125
    double usesBeforeInliningFactor;

    // How many actual arguments are accepted on the C stack.
    //
    // Default: 4,096
    uint32_t maxStackArgs;

    // The maximum inlining depth.
    //
    // Default: 3
    uint32_t maxInlineDepth;

    // The maximum inlining depth for functions.
    //
    // Inlining small functions has almost no compiling overhead
    // and removes the otherwise needed call overhead.
    // The value is currently very low.
    // Actually it is only needed to make sure we don't blow out the stack.
    //
    // Default: 10
    uint32_t smallFunctionMaxInlineDepth;

    // The bytecode length limit for small function.
    //
    // The default for this was arrived at empirically via benchmarking.
    // We may want to tune it further after other optimizations have gone
    // in.
    //
    // Default: 100
    uint32_t smallFunctionMaxBytecodeLength;

    // The maximum number of functions to polymorphically inline at a call site.
    //
    // Default: 4
    uint32_t polyInlineMax;

    // The maximum total bytecode size of an inline call site.
    //
    // Default: 1000
    uint32_t inlineMaxTotalBytecodeLength;

    // Minimal ratio between the use counts of the caller and the callee to
    // enable inlining of functions.
    //
    // Default: 128
    uint32_t inlineUseCountRatio;

    // Whether functions are compiled immediately.
    //
    // Default: false
    bool eagerCompilation;

    // If a function has attempted to make this many calls to
    // functions that are marked "uncompileable", then
    // stop running this function in IonMonkey. (default 512)
    uint32_t slowCallLimit;

    // When caller runs in IM, but callee not, we take a slow path to the interpreter.
    // This has a significant overhead. In order to decrease the number of times this happens,
    // the useCount gets incremented faster to compile this function in IM and use the fastpath.
    //
    // Default: 5
    uint32_t slowCallIncUseCount;

    // How many uses of a parallel kernel before we attempt compilation.
    //
    // Default: 1
    uint32_t usesBeforeCompileParallel;

    void setEagerCompilation() {
        eagerCompilation = true;
        usesBeforeCompile = usesBeforeCompileNoJaeger = 0;

        parallelCompilation = false;
    }

    IonOptions()
      : gvn(true),
        gvnIsOptimistic(true),
        licm(true),
        osr(true),
        limitScriptSize(true),
        registerAllocator(RegisterAllocator_LSRA),
        inlining(true),
        edgeCaseAnalysis(true),
        rangeAnalysis(true),
        uce(true),
        eaa(true),
        parallelCompilation(false),
        usesBeforeCompile(10240),
        usesBeforeCompileNoJaeger(40),
        usesBeforeInliningFactor(.125),
        maxStackArgs(4096),
        maxInlineDepth(3),
        smallFunctionMaxInlineDepth(10),
        smallFunctionMaxBytecodeLength(100),
        polyInlineMax(4),
        inlineMaxTotalBytecodeLength(1000),
        inlineUseCountRatio(128),
        eagerCompilation(false),
        slowCallLimit(512),
        slowCallIncUseCount(5),
        usesBeforeCompileParallel(1)
    {
    }

    uint32_t usesBeforeInlining() {
        return usesBeforeCompile * usesBeforeInliningFactor;
    }
};

enum MethodStatus
{
    Method_Error,
    Method_CantCompile,
    Method_Skipped,
    Method_Compiled
};

enum AbortReason {
    AbortReason_Alloc,
    AbortReason_Inlining,
    AbortReason_Disable,
    AbortReason_NoAbort
};

// An Ion context is needed to enter into either an Ion method or an instance
// of the Ion compiler. It points to a temporary allocator and the active
// JSContext, either of which may be NULL, and the active compartment, which
// will not be NULL.

class IonContext
{
  public:
    IonContext(JSContext *cx, TempAllocator *temp);
    IonContext(JSCompartment *comp, TempAllocator *temp);
    IonContext(JSRuntime *rt);
    ~IonContext();

    JSRuntime *runtime;
    JSContext *cx;
    JSCompartment *compartment;
    TempAllocator *temp;
    int getNextAssemblerId() {
        return assemblerCount_++;
    }
  private:
    IonContext *prev_;
    int assemblerCount_;
};

extern IonOptions js_IonOptions;

// Initialize Ion statically for all JSRuntimes.
bool InitializeIon();

// Get and set the current Ion context.
IonContext *GetIonContext();

bool SetIonContext(IonContext *ctx);

MethodStatus CanEnterAtBranch(JSContext *cx, JSScript *script,
                              AbstractFramePtr fp, jsbytecode *pc, bool isConstructing);
MethodStatus CanEnter(JSContext *cx, JSScript *script, AbstractFramePtr fp, bool isConstructing);
MethodStatus CanEnterUsingFastInvoke(JSContext *cx, HandleScript script, uint32_t numActualArgs);

enum IonExecStatus
{
    // The method call had to be aborted due to a stack limit check. This
    // error indicates that Ion never attempted to clean up frames.
    IonExec_Aborted,

    // The method call resulted in an error, and IonMonkey has cleaned up
    // frames.
    IonExec_Error,

    // The method call succeeed and returned a value.
    IonExec_Ok,

    // A guard triggered in IonMonkey and we must resume execution in
    // the interpreter.
    IonExec_Bailout
};

static inline bool
IsErrorStatus(IonExecStatus status)
{
    return status == IonExec_Error || status == IonExec_Aborted;
}

IonExecStatus Cannon(JSContext *cx, StackFrame *fp);
IonExecStatus SideCannon(JSContext *cx, StackFrame *fp, jsbytecode *pc);

// Used to enter Ion from C++ natives like Array.map. Called from FastInvokeGuard.
IonExecStatus FastInvoke(JSContext *cx, HandleFunction fun, CallArgsList &args);

// Walk the stack and invalidate active Ion frames for the invalid scripts.
void Invalidate(types::TypeCompartment &types, FreeOp *fop,
                const Vector<types::RecompileInfo> &invalid, bool resetUses = true);
void Invalidate(JSContext *cx, const Vector<types::RecompileInfo> &invalid, bool resetUses = true);
bool Invalidate(JSContext *cx, RawScript script, ExecutionMode mode, bool resetUses = true);
bool Invalidate(JSContext *cx, RawScript script, bool resetUses = true);

void MarkValueFromIon(JSRuntime *rt, Value *vp);
void MarkShapeFromIon(JSRuntime *rt, Shape **shapep);

void ToggleBarriers(JS::Zone *zone, bool needs);

class IonBuilder;
class MIRGenerator;
class LIRGraph;
class CodeGenerator;

bool OptimizeMIR(MIRGenerator *mir);
LIRGraph *GenerateLIR(MIRGenerator *mir);
CodeGenerator *GenerateCode(MIRGenerator *mir, LIRGraph *lir, MacroAssembler *maybeMasm = NULL);
CodeGenerator *CompileBackEnd(MIRGenerator *mir, MacroAssembler *maybeMasm = NULL);

void AttachFinishedCompilations(JSContext *cx);
void FinishOffThreadBuilder(IonBuilder *builder);

static inline bool IsEnabled(JSContext *cx)
{
    return cx->hasOption(JSOPTION_ION) && cx->typeInferenceEnabled();
}

void ForbidCompilation(JSContext *cx, RawScript script);
void ForbidCompilation(JSContext *cx, RawScript script, ExecutionMode mode);
uint32_t UsesBeforeIonRecompile(RawScript script, jsbytecode *pc);

void PurgeCaches(RawScript script, JS::Zone *zone);
size_t MemoryUsed(RawScript script, JSMallocSizeOfFun mallocSizeOf);
void DestroyIonScripts(FreeOp *fop, RawScript script);
void TraceIonScripts(JSTracer* trc, RawScript script);

} // namespace ion
} // namespace js

#endif // jsion_ion_h__

