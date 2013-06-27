/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/DebugOnly.h"

#include "IonAnalysis.h"
#include "IonBuilder.h"
#include "Lowering.h"
#include "MIRGraph.h"
#include "Ion.h"
#include "IonAnalysis.h"
#include "IonSpewer.h"
#include "builtin/Eval.h"
#include "frontend/BytecodeEmitter.h"

#include "CompileInfo-inl.h"
#include "ExecutionModeInlines.h"
#include "jsscriptinlines.h"
#include "jstypedarrayinlines.h"

#ifdef JS_THREADSAFE
# include "prthread.h"
#endif

using namespace js;
using namespace js::ion;

using mozilla::DebugOnly;

IonBuilder::IonBuilder(JSContext *cx, TempAllocator *temp, MIRGraph *graph,
                       TypeOracle *oracle, CompileInfo *info, size_t inliningDepth, uint32_t loopDepth)
  : MIRGenerator(cx->compartment, temp, graph, info),
    backgroundCodegen_(NULL),
    recompileInfo(cx->compartment->types.compiledInfo),
    cx(cx),
    abortReason_(AbortReason_Disable),
    loopDepth_(loopDepth),
    callerResumePoint_(NULL),
    callerBuilder_(NULL),
    oracle(oracle),
    inliningDepth_(inliningDepth),
    failedBoundsCheck_(info->script()->failedBoundsCheck),
    failedShapeGuard_(info->script()->failedShapeGuard),
    lazyArguments_(NULL)
{
    script_.init(info->script());
    pc = info->startPC();
}

void
IonBuilder::clearForBackEnd()
{
    cx = NULL;
    oracle = NULL;
}

bool
IonBuilder::abort(const char *message, ...)
{
    // Don't call PCToLineNumber in release builds.
#ifdef DEBUG
    va_list ap;
    va_start(ap, message);
    abortFmt(message, ap);
    va_end(ap);
    IonSpew(IonSpew_Abort, "aborted @ %s:%d", script()->filename(), PCToLineNumber(script(), pc));
#endif
    return false;
}

void
IonBuilder::spew(const char *message)
{
    // Don't call PCToLineNumber in release builds.
#ifdef DEBUG
    IonSpew(IonSpew_MIR, "%s @ %s:%d", message, script()->filename(), PCToLineNumber(script(), pc));
#endif
}

static inline int32_t
GetJumpOffset(jsbytecode *pc)
{
    JS_ASSERT(js_CodeSpec[JSOp(*pc)].type() == JOF_JUMP);
    return GET_JUMP_OFFSET(pc);
}

IonBuilder::CFGState
IonBuilder::CFGState::If(jsbytecode *join, MBasicBlock *ifFalse)
{
    CFGState state;
    state.state = IF_TRUE;
    state.stopAt = join;
    state.branch.ifFalse = ifFalse;
    return state;
}

IonBuilder::CFGState
IonBuilder::CFGState::IfElse(jsbytecode *trueEnd, jsbytecode *falseEnd, MBasicBlock *ifFalse)
{
    CFGState state;
    // If the end of the false path is the same as the start of the
    // false path, then the "else" block is empty and we can devolve
    // this to the IF_TRUE case. We handle this here because there is
    // still an extra GOTO on the true path and we want stopAt to point
    // there, whereas the IF_TRUE case does not have the GOTO.
    state.state = (falseEnd == ifFalse->pc())
                  ? IF_TRUE_EMPTY_ELSE
                  : IF_ELSE_TRUE;
    state.stopAt = trueEnd;
    state.branch.falseEnd = falseEnd;
    state.branch.ifFalse = ifFalse;
    return state;
}

IonBuilder::CFGState
IonBuilder::CFGState::AndOr(jsbytecode *join, MBasicBlock *joinStart)
{
    CFGState state;
    state.state = AND_OR;
    state.stopAt = join;
    state.branch.ifFalse = joinStart;
    return state;
}

IonBuilder::CFGState
IonBuilder::CFGState::TableSwitch(jsbytecode *exitpc, MTableSwitch *ins)
{
    CFGState state;
    state.state = TABLE_SWITCH;
    state.stopAt = exitpc;
    state.tableswitch.exitpc = exitpc;
    state.tableswitch.breaks = NULL;
    state.tableswitch.ins = ins;
    state.tableswitch.currentBlock = 0;
    return state;
}

JSFunction *
IonBuilder::getSingleCallTarget(types::StackTypeSet *calleeTypes)
{
    if (!calleeTypes)
        return NULL;

    RawObject obj = calleeTypes->getSingleton();
    if (!obj || !obj->isFunction())
        return NULL;

    return obj->toFunction();
}

bool
IonBuilder::getPolyCallTargets(types::StackTypeSet *calleeTypes,
                               AutoObjectVector &targets, uint32_t maxTargets)
{
    JS_ASSERT(targets.length() == 0);

    if (!calleeTypes)
        return true;

    if (calleeTypes->baseFlags() != 0)
        return true;

    unsigned objCount = calleeTypes->getObjectCount();

    if (objCount == 0 || objCount > maxTargets)
        return true;

    if (!targets.reserve(objCount))
        return false;
    for(unsigned i = 0; i < objCount; i++) {
        JSObject *obj = calleeTypes->getSingleObject(i);
        if (!obj || !obj->isFunction()) {
            targets.clear();
            return true;
        }
        if (!targets.append(obj))
            return false;
    }

    return true;
}

bool
IonBuilder::canInlineTarget(JSFunction *target, CallInfo &callInfo)
{
    if (!target->isInterpreted()) {
        IonSpew(IonSpew_Inlining, "Cannot inline due to non-interpreted");
        return false;
    }

    if (target->getParent() != &script()->global()) {
        IonSpew(IonSpew_Inlining, "Cannot inline due to scope mismatch");
        return false;
    }

    RootedScript inlineScript(cx, target->nonLazyScript());
    ExecutionMode executionMode = info().executionMode();
    if (!CanIonCompile(inlineScript, executionMode)) {
        IonSpew(IonSpew_Inlining, "Cannot inline due to disable Ion compilation");
        return false;
    }

    // Allow inlining of recursive calls, but only one level deep.
    IonBuilder *builder = callerBuilder_;
    while (builder) {
        if (builder->script() == inlineScript) {
            IonSpew(IonSpew_Inlining, "Not inlining recursive call");
            return false;
        }
        builder = builder->callerBuilder_;
    }

    RootedScript callerScript(cx, script());

    if (!oracle->canEnterInlinedFunction(target)) {
        IonSpew(IonSpew_Inlining, "Cannot inline due to oracle veto %d", script()->lineno);
        return false;
    }

    if (!oracle->callReturnTypeSetMatches(callerScript, pc, target)) {
        IonSpew(IonSpew_Inlining, "Cannot inline due to return typeset mismatch");
        return false;
    }

    JS_ASSERT(callInfo.hasCallType());
    if (callInfo.constructing()) {
        // For constructing calls the typeset of caller should intersect the callee's typeset.
        // Except for the |this| type, because that is created during execution depending on target.
        if (!oracle->callArgsTypeSetIntersects(NULL, callInfo.argvType(), target)) {
            IonSpew(IonSpew_Inlining, "Cannot inline due to arguments typeset mismatch");
            return false;
        }
    } else if (JSOp(*pc) == JSOP_FUNAPPLY) {
        // For fun.apply() the typeset of the caller should be a subset of the callee.
        // Currently we can't loosen this, since there are no type barriers at the call boundary
        // of fun.apply. Seeing a new type will only be noticed in the inlined call and
        // result in missed types in TI.
        if (!oracle->callArgsTypeSetMatches(callInfo.thisType(), callInfo.argvType(), target)) {
            IonSpew(IonSpew_Inlining, "Cannot inline due to arguments typeset mismatch");
            return false;
        }
    } else {
        // For normal calls the typeset of caller should intersect the callee's typeset.
        if (!oracle->callArgsTypeSetIntersects(callInfo.thisType(), callInfo.argvType(), target)) {
            IonSpew(IonSpew_Inlining, "Cannot inline due to arguments typeset mismatch");
            return false;
        }
    }

    IonSpew(IonSpew_Inlining, "Inlining good to go!");
    return true;
}

void
IonBuilder::popCfgStack()
{
    if (cfgStack_.back().isLoop())
        loops_.popBack();
    if (cfgStack_.back().state == CFGState::LABEL)
        labels_.popBack();
    cfgStack_.popBack();
}

bool
IonBuilder::pushLoop(CFGState::State initial, jsbytecode *stopAt, MBasicBlock *entry,
                     jsbytecode *bodyStart, jsbytecode *bodyEnd, jsbytecode *exitpc,
                     jsbytecode *continuepc)
{
    if (!continuepc)
        continuepc = entry->pc();

    ControlFlowInfo loop(cfgStack_.length(), continuepc);
    if (!loops_.append(loop))
        return false;

    CFGState state;
    state.state = initial;
    state.stopAt = stopAt;
    state.loop.bodyStart = bodyStart;
    state.loop.bodyEnd = bodyEnd;
    state.loop.exitpc = exitpc;
    state.loop.entry = entry;
    state.loop.successor = NULL;
    state.loop.breaks = NULL;
    state.loop.continues = NULL;
    return cfgStack_.append(state);
}

bool
IonBuilder::build()
{
    current = newBlock(pc);
    if (!current)
        return false;

    IonSpew(IonSpew_Scripts, "Analyzing script %s:%d (%p) (usecount=%d) (maxloopcount=%d)",
            script()->filename(), script()->lineno, (void *)script(), (int)script()->getUseCount(),
            (int)script()->getMaxLoopCount());

    if (!graph().addScript(script()))
        return false;

    if (!initParameters())
        return false;

    // Initialize local variables.
    for (uint32_t i = 0; i < info().nlocals(); i++) {
        MConstant *undef = MConstant::New(UndefinedValue());
        current->add(undef);
        current->initSlot(info().localSlot(i), undef);
    }

    // Initialize something for the scope chain. We can bail out before the
    // start instruction, but the snapshot is encoded *at* the start
    // instruction, which means generating any code that could load into
    // registers is illegal.
    {
        MInstruction *scope = MConstant::New(UndefinedValue());
        current->add(scope);
        current->initSlot(info().scopeChainSlot(), scope);
    }

    // Emit the start instruction, so we can begin real instructions.
    current->makeStart(MStart::New(MStart::StartType_Default));
    if (instrumentedProfiling())
        current->add(MFunctionBoundary::New(script(), MFunctionBoundary::Enter));

    // Parameters have been checked to correspond to the typeset, now we unbox
    // what we can in an infallible manner.
    rewriteParameters();

    // It's safe to start emitting actual IR, so now build the scope chain.
    if (!initScopeChain())
        return false;

    // Guard against over-recursion.
    MCheckOverRecursed *check = new MCheckOverRecursed;
    current->add(check);
    check->setResumePoint(current->entryResumePoint());

    // Prevent |this| from being DCE'd: necessary for constructors.
    if (info().fun())
        current->getSlot(info().thisSlot())->setGuard();

    // The type analysis phase attempts to insert unbox operations near
    // definitions of values. It also attempts to replace uses in resume points
    // with the narrower, unboxed variants. However, we must prevent this
    // replacement from happening on values in the entry snapshot. Otherwise we
    // could get this:
    //
    //       v0 = MParameter(0)
    //       v1 = MParameter(1)
    //       --   ResumePoint(v2, v3)
    //       v2 = Unbox(v0, INT32)
    //       v3 = Unbox(v1, INT32)
    //
    // So we attach the initial resume point to each parameter, which the type
    // analysis explicitly checks (this is the same mechanism used for
    // effectful operations).
    for (uint32_t i = 0; i < CountArgSlots(info().fun()); i++) {
        MInstruction *ins = current->getEntrySlot(i)->toInstruction();
        if (ins->type() == MIRType_Value)
            ins->setResumePoint(current->entryResumePoint());
    }

    if (script()->argumentsHasVarBinding()) {
        lazyArguments_ = MConstant::New(MagicValue(JS_OPTIMIZED_ARGUMENTS));
        current->add(lazyArguments_);
    }

    if (!traverseBytecode())
        return false;

    if (!processIterators())
        return false;

    JS_ASSERT(loopDepth_ == 0);
    abortReason_ = AbortReason_NoAbort;
    return true;
}

bool
IonBuilder::processIterators()
{
    // Find phis that must directly hold an iterator live.
    Vector<MPhi *, 0, SystemAllocPolicy> worklist;
    for (size_t i = 0; i < iterators_.length(); i++) {
        MInstruction *ins = iterators_[i];
        for (MUseDefIterator iter(ins); iter; iter++) {
            if (iter.def()->isPhi()) {
                if (!worklist.append(iter.def()->toPhi()))
                    return false;
            }
        }
    }

    // Propagate the iterator and live status of phis to all other connected
    // phis.
    while (!worklist.empty()) {
        MPhi *phi = worklist.popCopy();
        phi->setIterator();
        phi->setFoldedUnchecked();

        for (MUseDefIterator iter(phi); iter; iter++) {
            if (iter.def()->isPhi()) {
                MPhi *other = iter.def()->toPhi();
                if (!other->isIterator() && !worklist.append(other))
                    return false;
            }
        }
    }

    return true;
}

bool
IonBuilder::buildInline(IonBuilder *callerBuilder, MResumePoint *callerResumePoint,
                        CallInfo &callInfo)
{
    IonSpew(IonSpew_Scripts, "Inlining script %s:%d (%p)",
            script()->filename(), script()->lineno, (void *)script());

    if (!graph().addScript(script()))
        return false;

    callerBuilder_ = callerBuilder;
    callerResumePoint_ = callerResumePoint;

    if (callerBuilder->failedBoundsCheck_)
        failedBoundsCheck_ = true;

    if (callerBuilder->failedShapeGuard_)
        failedShapeGuard_ = true;

    // Generate single entrance block.
    current = newBlock(pc);
    if (!current)
        return false;

    current->setCallerResumePoint(callerResumePoint);

    // Connect the entrance block to the last block in the caller's graph.
    MBasicBlock *predecessor = callerBuilder->current;
    JS_ASSERT(predecessor == callerResumePoint->block());

    // All further instructions generated in from this scope should be
    // considered as part of the function that we're inlining. We also need to
    // keep track of the inlining depth because all scripts inlined on the same
    // level contiguously have only one Inline_Exit node.
    if (instrumentedProfiling())
        predecessor->add(MFunctionBoundary::New(script(),
                                                MFunctionBoundary::Inline_Enter,
                                                inliningDepth_));

    predecessor->end(MGoto::New(current));
    if (!current->addPredecessorWithoutPhis(predecessor))
        return false;

    // Save the actual arguments the caller used to call this inlined call,
    // to shortcut operations on "arguments" in the inlined call.
    JS_ASSERT(inlinedArguments_.length() == 0);
    JS_ASSERT(inlinedArgumentTypes_.length() == 0);
    if (!inlinedArguments_.append(callInfo.argv().begin(), callInfo.argv().end()))
        return false;
    if (!inlinedArgumentTypes_.append(callInfo.argvType().begin(), callInfo.argvType().end()))
        return false;

    // The Oracle ensures that the inlined script does not use the scope chain.
    JS_ASSERT(!script()->analysis()->usesScopeChain());
    MInstruction *scope = MConstant::New(UndefinedValue());
    current->add(scope);
    current->initSlot(info().scopeChainSlot(), scope);
    current->initSlot(info().thisSlot(), callInfo.thisArg());

    IonSpew(IonSpew_Inlining, "Initializing %u arg slots", info().nargs());

    // Initialize actually set arguments.
    uint32_t existing_args = Min<uint32_t>(callInfo.argc(), info().nargs());
    for (size_t i = 0; i < existing_args; ++i) {
        MDefinition *arg = callInfo.getArg(i);
        current->initSlot(info().argSlot(i), arg);
    }

    // Pass Undefined for missing arguments
    for (size_t i = callInfo.argc(); i < info().nargs(); ++i) {
        MConstant *arg = MConstant::New(UndefinedValue());
        current->add(arg);
        current->initSlot(info().argSlot(i), arg);
    }

    IonSpew(IonSpew_Inlining, "Initializing %u local slots", info().nlocals());

    // Initialize local variables.
    for (uint32_t i = 0; i < info().nlocals(); i++) {
        MConstant *undef = MConstant::New(UndefinedValue());
        current->add(undef);
        current->initSlot(info().localSlot(i), undef);
    }

    IonSpew(IonSpew_Inlining, "Inline entry block MResumePoint %p, %u operands",
            (void *) current->entryResumePoint(), current->entryResumePoint()->numOperands());

    // +2 for the scope chain and |this|.
    JS_ASSERT(current->entryResumePoint()->numOperands() == info().nargs() + info().nlocals() + 2);

    if (script_->argumentsHasVarBinding()) {
        lazyArguments_ = MConstant::New(MagicValue(JS_OPTIMIZED_ARGUMENTS));
        current->add(lazyArguments_);
    }

    return traverseBytecode();
}

// Apply Type Inference information to parameters early on, unboxing them if
// they have a definitive type. The actual guards will be emitted by the code
// generator, explicitly, as part of the function prologue.
void
IonBuilder::rewriteParameters()
{
    JS_ASSERT(info().scopeChainSlot() == 0);
    static const uint32_t START_SLOT = 1;

    for (uint32_t i = START_SLOT; i < CountArgSlots(info().fun()); i++) {
        MParameter *param = current->getSlot(i)->toParameter();

        // Find the original (not cloned) type set for the MParameter, as we
        // will be adding constraints to it.
        types::StackTypeSet *types;
        if (param->index() == MParameter::THIS_SLOT)
            types = oracle->thisTypeSet(script());
        else
            types = oracle->parameterTypeSet(script(), param->index());
        if (!types)
            continue;

        JSValueType definiteType = types->getKnownTypeTag();
        if (definiteType == JSVAL_TYPE_UNKNOWN)
            continue;

        MInstruction *actual = NULL;
        switch (definiteType) {
          case JSVAL_TYPE_UNDEFINED:
            param->setFoldedUnchecked();
            actual = MConstant::New(UndefinedValue());
            break;

          case JSVAL_TYPE_NULL:
            param->setFoldedUnchecked();
            actual = MConstant::New(NullValue());
            break;

          default:
            actual = MUnbox::New(param, MIRTypeFromValueType(definiteType), MUnbox::Infallible);
            break;
        }

        // Careful! We leave the original MParameter in the entry resume point. The
        // arguments still need to be checked unless proven otherwise at the call
        // site, and these checks can bailout. We can end up:
        //   v0 = Parameter(0)
        //   v1 = Unbox(v0, INT32)
        //   --   ResumePoint(v0)
        //
        // As usual, it would be invalid for v1 to be captured in the initial
        // resume point, rather than v0.
        current->add(actual);
        current->rewriteSlot(i, actual);
    }
}

bool
IonBuilder::initParameters()
{
    if (!info().fun())
        return true;

    MParameter *param = MParameter::New(MParameter::THIS_SLOT,
                                        cloneTypeSet(oracle->thisTypeSet(script())));
    current->add(param);
    current->initSlot(info().thisSlot(), param);

    for (uint32_t i = 0; i < info().nargs(); i++) {
        param = MParameter::New(i, cloneTypeSet(oracle->parameterTypeSet(script(), i)));
        current->add(param);
        current->initSlot(info().argSlot(i), param);
    }

    return true;
}

bool
IonBuilder::initScopeChain()
{
    MInstruction *scope = NULL;

    // If the script doesn't use the scopechain, then it's already initialized
    // from earlier.
    if (!script()->analysis()->usesScopeChain())
        return true;

    // The scope chain is only tracked in scripts that have NAME opcodes which
    // will try to access the scope. For other scripts, the scope instructions
    // will be held live by resume points and code will still be generated for
    // them, so just use a constant undefined value.
    if (!script()->compileAndGo)
        return abort("non-CNG global scripts are not supported");

    if (JSFunction *fun = info().fun()) {
        MCallee *callee = MCallee::New();
        current->add(callee);

        scope = MFunctionEnvironment::New(callee);
        current->add(scope);

        // This reproduce what is done in CallObject::createForFunction
        if (fun->isHeavyweight()) {
            if (fun->isNamedLambda()) {
                scope = createDeclEnvObject(callee, scope);
                if (!scope)
                    return false;
            }

            scope = createCallObject(callee, scope);
            if (!scope)
                return false;
        }
    } else {
        scope = MConstant::New(ObjectValue(script()->global()));
        current->add(scope);
    }

    current->setScopeChain(scope);
    return true;
}

// We try to build a control-flow graph in the order that it would be built as
// if traversing the AST. This leads to a nice ordering and lets us build SSA
// in one pass, since the bytecode is structured.
//
// We traverse the bytecode iteratively, maintaining a current basic block.
// Each basic block has a mapping of local slots to instructions, as well as a
// stack depth. As we encounter instructions we mutate this mapping in the
// current block.
//
// Things get interesting when we encounter a control structure. This can be
// either an IFEQ, downward GOTO, or a decompiler hint stashed away in source
// notes. Once we encounter such an opcode, we recover the structure of the
// control flow (its branches and bounds), and push it on a stack.
//
// As we continue traversing the bytecode, we look for points that would
// terminate the topmost control flow path pushed on the stack. These are:
//  (1) The bounds of the current structure (end of a loop or join/edge of a
//      branch).
//  (2) A "return", "break", or "continue" statement.
//
// For (1), we expect that there is a current block in the progress of being
// built, and we complete the necessary edges in the CFG. For (2), we expect
// that there is no active block.
//
// For normal diamond join points, we construct Phi nodes as we add
// predecessors. For loops, care must be taken to propagate Phi nodes back
// through uses in the loop body.
bool
IonBuilder::traverseBytecode()
{
    for (;;) {
        JS_ASSERT(pc < info().limitPC());

        for (;;) {
            if (!temp().ensureBallast())
                return false;

            // Check if we've hit an expected join point or edge in the bytecode.
            // Leaving one control structure could place us at the edge of another,
            // thus |while| instead of |if| so we don't skip any opcodes.
            if (!cfgStack_.empty() && cfgStack_.back().stopAt == pc) {
                ControlStatus status = processCfgStack();
                if (status == ControlStatus_Error)
                    return false;
                if (!current)
                    return true;
                continue;
            }

            // Some opcodes need to be handled early because they affect control
            // flow, terminating the current basic block and/or instructing the
            // traversal algorithm to continue from a new pc.
            //
            //   (1) If the opcode does not affect control flow, then the opcode
            //       is inspected and transformed to IR. This is the process_opcode
            //       label.
            //   (2) A loop could be detected via a forward GOTO. In this case,
            //       we don't want to process the GOTO, but the following
            //       instruction.
            //   (3) A RETURN, STOP, BREAK, or CONTINUE may require processing the
            //       CFG stack to terminate open branches.
            //
            // Similar to above, snooping control flow could land us at another
            // control flow point, so we iterate until it's time to inspect a real
            // opcode.
            ControlStatus status;
            if ((status = snoopControlFlow(JSOp(*pc))) == ControlStatus_None)
                break;
            if (status == ControlStatus_Error)
                return false;
            if (!current)
                return true;
        }

        // Nothing in inspectOpcode() is allowed to advance the pc.
        JSOp op = JSOp(*pc);
        if (!inspectOpcode(op))
            return false;

        pc += js_CodeSpec[op].length;
#ifdef TRACK_SNAPSHOTS
        current->updateTrackedPc(pc);
#endif
    }

    return true;
}

IonBuilder::ControlStatus
IonBuilder::snoopControlFlow(JSOp op)
{
    switch (op) {
      case JSOP_NOP:
        return maybeLoop(op, info().getNote(cx, pc));

      case JSOP_POP:
        return maybeLoop(op, info().getNote(cx, pc));

      case JSOP_RETURN:
      case JSOP_STOP:
        return processReturn(op);

      case JSOP_THROW:
        return processThrow();

      case JSOP_GOTO:
      {
        jssrcnote *sn = info().getNote(cx, pc);
        switch (sn ? SN_TYPE(sn) : SRC_NULL) {
          case SRC_BREAK:
          case SRC_BREAK2LABEL:
            return processBreak(op, sn);

          case SRC_CONTINUE:
            return processContinue(op);

          case SRC_SWITCHBREAK:
            return processSwitchBreak(op);

          case SRC_WHILE:
          case SRC_FOR_IN:
            // while (cond) { }
            return whileOrForInLoop(sn);

          default:
            // Hard assert for now - make an error later.
            JS_NOT_REACHED("unknown goto case");
            break;
        }
        break;
      }

      case JSOP_TABLESWITCH:
        return tableSwitch(op, info().getNote(cx, pc));

      case JSOP_IFNE:
        // We should never reach an IFNE, it's a stopAt point, which will
        // trigger closing the loop.
        JS_NOT_REACHED("we should never reach an ifne!");
        return ControlStatus_Error;

      default:
        break;
    }
    return ControlStatus_None;
}

bool
IonBuilder::inspectOpcode(JSOp op)
{
    // Don't compile fat opcodes, run the decomposed version instead.
    if (js_CodeSpec[op].format & JOF_DECOMPOSE)
        return true;

    switch (op) {
      case JSOP_NOP:
      case JSOP_LINENO:
      case JSOP_LOOPENTRY:
        return true;

      case JSOP_LABEL:
        return jsop_label();

      case JSOP_UNDEFINED:
        return pushConstant(UndefinedValue());

      case JSOP_IFEQ:
        return jsop_ifeq(JSOP_IFEQ);

      case JSOP_CONDSWITCH:
        return jsop_condswitch();

      case JSOP_BITNOT:
        return jsop_bitnot();

      case JSOP_BITAND:
      case JSOP_BITOR:
      case JSOP_BITXOR:
      case JSOP_LSH:
      case JSOP_RSH:
      case JSOP_URSH:
        return jsop_bitop(op);

      case JSOP_ADD:
      case JSOP_SUB:
      case JSOP_MUL:
      case JSOP_DIV:
      case JSOP_MOD:
        return jsop_binary(op);

      case JSOP_POS:
        return jsop_pos();

      case JSOP_NEG:
        return jsop_neg();

      case JSOP_AND:
      case JSOP_OR:
        return jsop_andor(op);

      case JSOP_DEFVAR:
      case JSOP_DEFCONST:
        return jsop_defvar(GET_UINT32_INDEX(pc));

      case JSOP_DEFFUN:
        return jsop_deffun(GET_UINT32_INDEX(pc));

      case JSOP_EQ:
      case JSOP_NE:
      case JSOP_STRICTEQ:
      case JSOP_STRICTNE:
      case JSOP_LT:
      case JSOP_LE:
      case JSOP_GT:
      case JSOP_GE:
        return jsop_compare(op);

      case JSOP_DOUBLE:
        return pushConstant(info().getConst(pc));

      case JSOP_STRING:
        return pushConstant(StringValue(info().getAtom(pc)));

      case JSOP_ZERO:
        return pushConstant(Int32Value(0));

      case JSOP_ONE:
        return pushConstant(Int32Value(1));

      case JSOP_NULL:
        return pushConstant(NullValue());

      case JSOP_VOID:
        current->pop();
        return pushConstant(UndefinedValue());

      case JSOP_HOLE:
        return pushConstant(MagicValue(JS_ELEMENTS_HOLE));

      case JSOP_FALSE:
        return pushConstant(BooleanValue(false));

      case JSOP_TRUE:
        return pushConstant(BooleanValue(true));

      case JSOP_ARGUMENTS:
        return jsop_arguments();

      case JSOP_NOTEARG:
        return jsop_notearg();

      case JSOP_GETARG:
      case JSOP_CALLARG:
        current->pushArg(GET_SLOTNO(pc));
        return true;

      case JSOP_SETARG:
        // To handle this case, we should spill the arguments to the space where
        // actual arguments are stored. The tricky part is that if we add a MIR
        // to wrap the spilling action, we don't want the spilling to be
        // captured by the GETARG and by the resume point, only by
        // MGetArgument.
        if (info().hasArguments())
            return abort("NYI: arguments & setarg.");
        current->setArg(GET_SLOTNO(pc));
        return true;

      case JSOP_GETLOCAL:
      case JSOP_CALLLOCAL:
        current->pushLocal(GET_SLOTNO(pc));
        return true;

      case JSOP_SETLOCAL:
        current->setLocal(GET_SLOTNO(pc));
        return true;

      case JSOP_POP:
        current->pop();

        // POP opcodes frequently appear where values are killed, e.g. after
        // SET* opcodes. Place a resume point afterwards to avoid capturing
        // the dead value in later snapshots, except in places where that
        // resume point is obviously unnecessary.
        if (pc[JSOP_POP_LENGTH] == JSOP_POP)
            return true;
        return maybeInsertResume();

      case JSOP_NEWINIT:
      {
        if (GET_UINT8(pc) == JSProto_Array)
            return jsop_newarray(0);
        RootedObject baseObj(cx, NULL);
        return jsop_newobject(baseObj);
      }

      case JSOP_NEWARRAY:
        return jsop_newarray(GET_UINT24(pc));

      case JSOP_NEWOBJECT:
      {
        RootedObject baseObj(cx, info().getObject(pc));
        return jsop_newobject(baseObj);
      }

      case JSOP_INITELEM_ARRAY:
        return jsop_initelem_array();

      case JSOP_INITPROP:
      {
        RootedPropertyName name(cx, info().getAtom(pc)->asPropertyName());
        return jsop_initprop(name);
      }

      case JSOP_ENDINIT:
        return true;

      case JSOP_FUNCALL:
        return jsop_funcall(GET_ARGC(pc));

      case JSOP_FUNAPPLY:
        return jsop_funapply(GET_ARGC(pc));

      case JSOP_CALL:
      case JSOP_NEW:
        return jsop_call(GET_ARGC(pc), (JSOp)*pc == JSOP_NEW);

      case JSOP_EVAL:
        return jsop_eval(GET_ARGC(pc));

      case JSOP_INT8:
        return pushConstant(Int32Value(GET_INT8(pc)));

      case JSOP_UINT16:
        return pushConstant(Int32Value(GET_UINT16(pc)));

      case JSOP_GETGNAME:
      case JSOP_CALLGNAME:
      {
        RootedPropertyName name(cx, info().getAtom(pc)->asPropertyName());
        return jsop_getgname(name);
      }

      case JSOP_BINDGNAME:
        return pushConstant(ObjectValue(script()->global()));

      case JSOP_SETGNAME:
      {
        RootedPropertyName name(cx, info().getAtom(pc)->asPropertyName());
        return jsop_setgname(name);
      }

      case JSOP_NAME:
      case JSOP_CALLNAME:
      {
        RootedPropertyName name(cx, info().getAtom(pc)->asPropertyName());
        return jsop_getname(name);
      }

      case JSOP_GETINTRINSIC:
      case JSOP_CALLINTRINSIC:
      {
        RootedPropertyName name(cx, info().getAtom(pc)->asPropertyName());
        return jsop_intrinsic(name);
      }

      case JSOP_BINDNAME:
        return jsop_bindname(info().getName(pc));

      case JSOP_DUP:
        current->pushSlot(current->stackDepth() - 1);
        return true;

      case JSOP_DUP2:
        return jsop_dup2();

      case JSOP_SWAP:
        current->swapAt(-1);
        return true;

      case JSOP_PICK:
        current->pick(-GET_INT8(pc));
        return true;

      case JSOP_GETALIASEDVAR:
      case JSOP_CALLALIASEDVAR:
        return jsop_getaliasedvar(ScopeCoordinate(pc));

      case JSOP_SETALIASEDVAR:
        return jsop_setaliasedvar(ScopeCoordinate(pc));

      case JSOP_UINT24:
        return pushConstant(Int32Value(GET_UINT24(pc)));

      case JSOP_INT32:
        return pushConstant(Int32Value(GET_INT32(pc)));

      case JSOP_LOOPHEAD:
        // JSOP_LOOPHEAD is handled when processing the loop header.
        JS_NOT_REACHED("JSOP_LOOPHEAD outside loop");
        return true;

      case JSOP_GETELEM:
      case JSOP_CALLELEM:
        return jsop_getelem();

      case JSOP_SETELEM:
        return jsop_setelem();

      case JSOP_LENGTH:
        return jsop_length();

      case JSOP_NOT:
        return jsop_not();

      case JSOP_THIS:
        return jsop_this();

      case JSOP_CALLEE:
      {
        MInstruction *callee;
        if (inliningDepth_ == 0)
            callee = MCallee::New();
        else
            callee = MConstant::New(ObjectValue(*info().fun()));
        current->add(callee);
        current->push(callee);
        return true;
      }

      case JSOP_GETPROP:
      case JSOP_CALLPROP:
      {
        RootedPropertyName name(cx, info().getAtom(pc)->asPropertyName());
        return jsop_getprop(name);
      }

      case JSOP_SETPROP:
      case JSOP_SETNAME:
      {
        RootedPropertyName name(cx, info().getAtom(pc)->asPropertyName());
        return jsop_setprop(name);
      }

      case JSOP_DELPROP:
      {
        RootedPropertyName name(cx, info().getAtom(pc)->asPropertyName());
        return jsop_delprop(name);
      }

      case JSOP_REGEXP:
        return jsop_regexp(info().getRegExp(pc));

      case JSOP_OBJECT:
        return jsop_object(info().getObject(pc));

      case JSOP_TYPEOF:
      case JSOP_TYPEOFEXPR:
        return jsop_typeof();

      case JSOP_TOID:
        return jsop_toid();

      case JSOP_LAMBDA:
        return jsop_lambda(info().getFunction(pc));

      case JSOP_ITER:
        return jsop_iter(GET_INT8(pc));

      case JSOP_ITERNEXT:
        return jsop_iternext();

      case JSOP_MOREITER:
        return jsop_itermore();

      case JSOP_ENDITER:
        return jsop_iterend();

      case JSOP_IN:
        return jsop_in();

      case JSOP_INSTANCEOF:
        return jsop_instanceof();

      default:
#ifdef DEBUG
        return abort("Unsupported opcode: %s (line %d)", js_CodeName[op], info().lineno(cx, pc));
#else
        return abort("Unsupported opcode: %d (line %d)", op, info().lineno(cx, pc));
#endif
    }
}

// Given that the current control flow structure has ended forcefully,
// via a return, break, or continue (rather than joining), propagate the
// termination up. For example, a return nested 5 loops deep may terminate
// every outer loop at once, if there are no intervening conditionals:
//
// for (...) {
//   for (...) {
//     return x;
//   }
// }
//
// If |current| is NULL when this function returns, then there is no more
// control flow to be processed.
IonBuilder::ControlStatus
IonBuilder::processControlEnd()
{
    JS_ASSERT(!current);

    if (cfgStack_.empty()) {
        // If there is no more control flow to process, then this is the
        // last return in the function.
        return ControlStatus_Ended;
    }

    return processCfgStack();
}

// Processes the top of the CFG stack. This is used from two places:
// (1) processControlEnd(), whereby a break, continue, or return may interrupt
//     an in-progress CFG structure before reaching its actual termination
//     point in the bytecode.
// (2) traverseBytecode(), whereby we reach the last instruction in a CFG
//     structure.
IonBuilder::ControlStatus
IonBuilder::processCfgStack()
{
    ControlStatus status = processCfgEntry(cfgStack_.back());

    // If this terminated a CFG structure, act like processControlEnd() and
    // keep propagating upward.
    while (status == ControlStatus_Ended) {
        popCfgStack();
        if (cfgStack_.empty())
            return status;
        status = processCfgEntry(cfgStack_.back());
    }

    // If some join took place, the current structure is finished.
    if (status == ControlStatus_Joined)
        popCfgStack();

    return status;
}

IonBuilder::ControlStatus
IonBuilder::processCfgEntry(CFGState &state)
{
    switch (state.state) {
      case CFGState::IF_TRUE:
      case CFGState::IF_TRUE_EMPTY_ELSE:
        return processIfEnd(state);

      case CFGState::IF_ELSE_TRUE:
        return processIfElseTrueEnd(state);

      case CFGState::IF_ELSE_FALSE:
        return processIfElseFalseEnd(state);

      case CFGState::DO_WHILE_LOOP_BODY:
        return processDoWhileBodyEnd(state);

      case CFGState::DO_WHILE_LOOP_COND:
        return processDoWhileCondEnd(state);

      case CFGState::WHILE_LOOP_COND:
        return processWhileCondEnd(state);

      case CFGState::WHILE_LOOP_BODY:
        return processWhileBodyEnd(state);

      case CFGState::FOR_LOOP_COND:
        return processForCondEnd(state);

      case CFGState::FOR_LOOP_BODY:
        return processForBodyEnd(state);

      case CFGState::FOR_LOOP_UPDATE:
        return processForUpdateEnd(state);

      case CFGState::TABLE_SWITCH:
        return processNextTableSwitchCase(state);

      case CFGState::COND_SWITCH_CASE:
        return processCondSwitchCase(state);

      case CFGState::COND_SWITCH_BODY:
        return processCondSwitchBody(state);

      case CFGState::AND_OR:
        return processAndOrEnd(state);

      case CFGState::LABEL:
        return processLabelEnd(state);

      default:
        JS_NOT_REACHED("unknown cfgstate");
    }
    return ControlStatus_Error;
}

IonBuilder::ControlStatus
IonBuilder::processIfEnd(CFGState &state)
{
    if (current) {
        // Here, the false block is the join point. Create an edge from the
        // current block to the false block. Note that a RETURN opcode
        // could have already ended the block.
        current->end(MGoto::New(state.branch.ifFalse));

        if (!state.branch.ifFalse->addPredecessor(current))
            return ControlStatus_Error;
    }

    current = state.branch.ifFalse;
    graph().moveBlockToEnd(current);
    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processIfElseTrueEnd(CFGState &state)
{
    // We've reached the end of the true branch of an if-else. Don't
    // create an edge yet, just transition to parsing the false branch.
    state.state = CFGState::IF_ELSE_FALSE;
    state.branch.ifTrue = current;
    state.stopAt = state.branch.falseEnd;
    pc = state.branch.ifFalse->pc();
    current = state.branch.ifFalse;
    graph().moveBlockToEnd(current);
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processIfElseFalseEnd(CFGState &state)
{
    // Update the state to have the latest block from the false path.
    state.branch.ifFalse = current;

    // To create the join node, we need an incoming edge that has not been
    // terminated yet.
    MBasicBlock *pred = state.branch.ifTrue
                        ? state.branch.ifTrue
                        : state.branch.ifFalse;
    MBasicBlock *other = (pred == state.branch.ifTrue) ? state.branch.ifFalse : state.branch.ifTrue;

    if (!pred)
        return ControlStatus_Ended;

    // Create a new block to represent the join.
    MBasicBlock *join = newBlock(pred, state.branch.falseEnd);
    if (!join)
        return ControlStatus_Error;

    // Create edges from the true and false blocks as needed.
    pred->end(MGoto::New(join));

    if (other) {
        other->end(MGoto::New(join));
        if (!join->addPredecessor(other))
            return ControlStatus_Error;
    }

    // Ignore unreachable remainder of false block if existent.
    current = join;
    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processBrokenLoop(CFGState &state)
{
    JS_ASSERT(!current);

    JS_ASSERT(loopDepth_);
    loopDepth_--;

    // A broken loop is not a real loop (it has no header or backedge), so
    // reset the loop depth.
    for (MBasicBlockIterator i(graph().begin(state.loop.entry)); i != graph().end(); i++) {
        if (i->loopDepth() > loopDepth_)
            i->setLoopDepth(i->loopDepth() - 1);
    }

    // If the loop started with a condition (while/for) then even if the
    // structure never actually loops, the condition itself can still fail and
    // thus we must resume at the successor, if one exists.
    current = state.loop.successor;
    if (current) {
        JS_ASSERT(current->loopDepth() == loopDepth_);
        graph().moveBlockToEnd(current);
    }

    // Join the breaks together and continue parsing.
    if (state.loop.breaks) {
        MBasicBlock *block = createBreakCatchBlock(state.loop.breaks, state.loop.exitpc);
        if (!block)
            return ControlStatus_Error;

        if (current) {
            current->end(MGoto::New(block));
            if (!block->addPredecessor(current))
                return ControlStatus_Error;
        }

        current = block;
    }

    // If the loop is not gated on a condition, and has only returns, we'll
    // reach this case. For example:
    // do { ... return; } while ();
    if (!current)
        return ControlStatus_Ended;

    // Otherwise, the loop is gated on a condition and/or has breaks so keep
    // parsing at the successor.
    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::finishLoop(CFGState &state, MBasicBlock *successor)
{
    JS_ASSERT(current);

    JS_ASSERT(loopDepth_);
    loopDepth_--;
    JS_ASSERT_IF(successor, successor->loopDepth() == loopDepth_);

    // Compute phis in the loop header and propagate them throughout the loop,
    // including the successor.
    if (!state.loop.entry->setBackedge(current))
        return ControlStatus_Error;
    if (successor) {
        graph().moveBlockToEnd(successor);
        successor->inheritPhis(state.loop.entry);
    }

    if (state.loop.breaks) {
        // Propagate phis placed in the header to individual break exit points.
        DeferredEdge *edge = state.loop.breaks;
        while (edge) {
            edge->block->inheritPhis(state.loop.entry);
            edge = edge->next;
        }

        // Create a catch block to join all break exits.
        MBasicBlock *block = createBreakCatchBlock(state.loop.breaks, state.loop.exitpc);
        if (!block)
            return ControlStatus_Error;

        if (successor) {
            // Finally, create an unconditional edge from the successor to the
            // catch block.
            successor->end(MGoto::New(block));
            if (!block->addPredecessor(successor))
                return ControlStatus_Error;
        }
        successor = block;
    }

    current = successor;

    // An infinite loop (for (;;) { }) will not have a successor.
    if (!current)
        return ControlStatus_Ended;

    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processDoWhileBodyEnd(CFGState &state)
{
    if (!processDeferredContinues(state))
        return ControlStatus_Error;

    // No current means control flow cannot reach the condition, so this will
    // never loop.
    if (!current)
        return processBrokenLoop(state);

    MBasicBlock *header = newBlock(current, state.loop.updatepc);
    if (!header)
        return ControlStatus_Error;
    current->end(MGoto::New(header));

    state.state = CFGState::DO_WHILE_LOOP_COND;
    state.stopAt = state.loop.updateEnd;
    pc = state.loop.updatepc;
    current = header;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processDoWhileCondEnd(CFGState &state)
{
    JS_ASSERT(JSOp(*pc) == JSOP_IFNE);

    // We're guaranteed a |current|, it's impossible to break or return from
    // inside the conditional expression.
    JS_ASSERT(current);

    // Pop the last value, and create the successor block.
    MDefinition *vins = current->pop();
    MBasicBlock *successor = newBlock(current, GetNextPc(pc), loopDepth_ - 1);
    if (!successor)
        return ControlStatus_Error;

    // Create the test instruction and end the current block.
    MTest *test = MTest::New(vins, state.loop.entry, successor);
    current->end(test);
    return finishLoop(state, successor);
}

IonBuilder::ControlStatus
IonBuilder::processWhileCondEnd(CFGState &state)
{
    JS_ASSERT(JSOp(*pc) == JSOP_IFNE);

    // Balance the stack past the IFNE.
    MDefinition *ins = current->pop();

    // Create the body and successor blocks.
    MBasicBlock *body = newBlock(current, state.loop.bodyStart);
    state.loop.successor = newBlock(current, state.loop.exitpc, loopDepth_ - 1);
    if (!body || !state.loop.successor)
        return ControlStatus_Error;

    MTest *test = MTest::New(ins, body, state.loop.successor);
    current->end(test);

    state.state = CFGState::WHILE_LOOP_BODY;
    state.stopAt = state.loop.bodyEnd;
    pc = state.loop.bodyStart;
    current = body;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processWhileBodyEnd(CFGState &state)
{
    if (!processDeferredContinues(state))
        return ControlStatus_Error;

    if (!current)
        return processBrokenLoop(state);

    current->end(MGoto::New(state.loop.entry));
    return finishLoop(state, state.loop.successor);
}

IonBuilder::ControlStatus
IonBuilder::processForCondEnd(CFGState &state)
{
    JS_ASSERT(JSOp(*pc) == JSOP_IFNE);

    // Balance the stack past the IFNE.
    MDefinition *ins = current->pop();

    // Create the body and successor blocks.
    MBasicBlock *body = newBlock(current, state.loop.bodyStart);
    state.loop.successor = newBlock(current, state.loop.exitpc, loopDepth_ - 1);
    if (!body || !state.loop.successor)
        return ControlStatus_Error;

    MTest *test = MTest::New(ins, body, state.loop.successor);
    current->end(test);

    state.state = CFGState::FOR_LOOP_BODY;
    state.stopAt = state.loop.bodyEnd;
    pc = state.loop.bodyStart;
    current = body;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processForBodyEnd(CFGState &state)
{
    if (!processDeferredContinues(state))
        return ControlStatus_Error;

    // If there is no updatepc, just go right to processing what would be the
    // end of the update clause. Otherwise, |current| might be NULL; if this is
    // the case, the udpate is unreachable anyway.
    if (!state.loop.updatepc || !current)
        return processForUpdateEnd(state);

    pc = state.loop.updatepc;

    state.state = CFGState::FOR_LOOP_UPDATE;
    state.stopAt = state.loop.updateEnd;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processForUpdateEnd(CFGState &state)
{
    // If there is no current, we couldn't reach the loop edge and there was no
    // update clause.
    if (!current)
        return processBrokenLoop(state);

    current->end(MGoto::New(state.loop.entry));
    return finishLoop(state, state.loop.successor);
}

bool
IonBuilder::processDeferredContinues(CFGState &state)
{
    // If there are any continues for this loop, and there is an update block,
    // then we need to create a new basic block to house the update.
    if (state.loop.continues) {
        DeferredEdge *edge = state.loop.continues;

        MBasicBlock *update = newBlock(edge->block, loops_.back().continuepc);
        if (!update)
            return false;

        if (current) {
            current->end(MGoto::New(update));
            if (!update->addPredecessor(current))
                return ControlStatus_Error;
        }

        // No need to use addPredecessor for first edge,
        // because it is already predecessor.
        edge->block->end(MGoto::New(update));
        edge = edge->next;

        // Remaining edges
        while (edge) {
            edge->block->end(MGoto::New(update));
            if (!update->addPredecessor(edge->block))
                return ControlStatus_Error;
            edge = edge->next;
        }
        state.loop.continues = NULL;

        current = update;
    }

    return true;
}

MBasicBlock *
IonBuilder::createBreakCatchBlock(DeferredEdge *edge, jsbytecode *pc)
{
    // Create block, using the first break statement as predecessor
    MBasicBlock *successor = newBlock(edge->block, pc);
    if (!successor)
        return NULL;

    // No need to use addPredecessor for first edge,
    // because it is already predecessor.
    edge->block->end(MGoto::New(successor));
    edge = edge->next;

    // Finish up remaining breaks.
    while (edge) {
        edge->block->end(MGoto::New(successor));
        if (!successor->addPredecessor(edge->block))
            return NULL;
        edge = edge->next;
    }

    return successor;
}

IonBuilder::ControlStatus
IonBuilder::processNextTableSwitchCase(CFGState &state)
{
    JS_ASSERT(state.state == CFGState::TABLE_SWITCH);

    state.tableswitch.currentBlock++;

    // Test if there are still unprocessed successors (cases/default)
    if (state.tableswitch.currentBlock >= state.tableswitch.ins->numBlocks())
        return processSwitchEnd(state.tableswitch.breaks, state.tableswitch.exitpc);

    // Get the next successor
    MBasicBlock *successor = state.tableswitch.ins->getBlock(state.tableswitch.currentBlock);

    // Add current block as predecessor if available.
    // This means the previous case didn't have a break statement.
    // So flow will continue in this block.
    if (current) {
        current->end(MGoto::New(successor));
        successor->addPredecessor(current);
    }

    // Insert successor after the current block, to maintain RPO.
    graph().moveBlockToEnd(successor);

    // If this is the last successor the block should stop at the end of the tableswitch
    // Else it should stop at the start of the next successor
    if (state.tableswitch.currentBlock+1 < state.tableswitch.ins->numBlocks())
        state.stopAt = state.tableswitch.ins->getBlock(state.tableswitch.currentBlock+1)->pc();
    else
        state.stopAt = state.tableswitch.exitpc;

    current = successor;
    pc = current->pc();
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processAndOrEnd(CFGState &state)
{
    // We just processed the RHS of an && or || expression.
    // Now jump to the join point (the false block).
    current->end(MGoto::New(state.branch.ifFalse));

    if (!state.branch.ifFalse->addPredecessor(current))
        return ControlStatus_Error;

    current = state.branch.ifFalse;
    graph().moveBlockToEnd(current);
    pc = current->pc();
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processLabelEnd(CFGState &state)
{
    JS_ASSERT(state.state == CFGState::LABEL);

    // If there are no breaks and no current, controlflow is terminated.
    if (!state.label.breaks && !current)
        return ControlStatus_Ended;

    // If there are no breaks to this label, there's nothing to do.
    if (!state.label.breaks)
        return ControlStatus_Joined;

    MBasicBlock *successor = createBreakCatchBlock(state.label.breaks, state.stopAt);
    if (!successor)
        return ControlStatus_Error;

    if (current) {
        current->end(MGoto::New(successor));
        successor->addPredecessor(current);
    }

    pc = state.stopAt;
    current = successor;
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::processBreak(JSOp op, jssrcnote *sn)
{
    JS_ASSERT(op == JSOP_GOTO);

    JS_ASSERT(SN_TYPE(sn) == SRC_BREAK ||
              SN_TYPE(sn) == SRC_BREAK2LABEL);

    // Find the break target.
    jsbytecode *target = pc + GetJumpOffset(pc);
    DebugOnly<bool> found = false;

    if (SN_TYPE(sn) == SRC_BREAK2LABEL) {
        for (size_t i = labels_.length() - 1; i < labels_.length(); i--) {
            CFGState &cfg = cfgStack_[labels_[i].cfgEntry];
            JS_ASSERT(cfg.state == CFGState::LABEL);
            if (cfg.stopAt == target) {
                cfg.label.breaks = new DeferredEdge(current, cfg.label.breaks);
                found = true;
                break;
            }
        }
    } else {
        for (size_t i = loops_.length() - 1; i < loops_.length(); i--) {
            CFGState &cfg = cfgStack_[loops_[i].cfgEntry];
            JS_ASSERT(cfg.isLoop());
            if (cfg.loop.exitpc == target) {
                cfg.loop.breaks = new DeferredEdge(current, cfg.loop.breaks);
                found = true;
                break;
            }
        }
    }

    JS_ASSERT(found);

    current = NULL;
    pc += js_CodeSpec[op].length;
    return processControlEnd();
}

static inline jsbytecode *
EffectiveContinue(jsbytecode *pc)
{
    if (JSOp(*pc) == JSOP_GOTO)
        return pc + GetJumpOffset(pc);
    return pc;
}

IonBuilder::ControlStatus
IonBuilder::processContinue(JSOp op)
{
    JS_ASSERT(op == JSOP_GOTO);

    // Find the target loop.
    CFGState *found = NULL;
    jsbytecode *target = pc + GetJumpOffset(pc);
    for (size_t i = loops_.length() - 1; i < loops_.length(); i--) {
        if (loops_[i].continuepc == target ||
            EffectiveContinue(loops_[i].continuepc) == target)
        {
            found = &cfgStack_[loops_[i].cfgEntry];
            break;
        }
    }

    // There must always be a valid target loop structure. If not, there's
    // probably an off-by-something error in which pc we track.
    JS_ASSERT(found);
    CFGState &state = *found;

    state.loop.continues = new DeferredEdge(current, state.loop.continues);

    current = NULL;
    pc += js_CodeSpec[op].length;
    return processControlEnd();
}

IonBuilder::ControlStatus
IonBuilder::processSwitchBreak(JSOp op)
{
    JS_ASSERT(op == JSOP_GOTO);

    // Find the target switch.
    CFGState *found = NULL;
    jsbytecode *target = pc + GetJumpOffset(pc);
    for (size_t i = switches_.length() - 1; i < switches_.length(); i--) {
        if (switches_[i].continuepc == target) {
            found = &cfgStack_[switches_[i].cfgEntry];
            break;
        }
    }

    // There must always be a valid target loop structure. If not, there's
    // probably an off-by-something error in which pc we track.
    JS_ASSERT(found);
    CFGState &state = *found;

    DeferredEdge **breaks = NULL;
    switch (state.state) {
      case CFGState::TABLE_SWITCH:
        breaks = &state.tableswitch.breaks;
        break;
      case CFGState::COND_SWITCH_BODY:
        breaks = &state.condswitch.breaks;
        break;
      default:
        JS_NOT_REACHED("Unexpected switch state.");
        return ControlStatus_Error;
    }

    *breaks = new DeferredEdge(current, *breaks);

    current = NULL;
    pc += js_CodeSpec[op].length;
    return processControlEnd();
}

IonBuilder::ControlStatus
IonBuilder::processSwitchEnd(DeferredEdge *breaks, jsbytecode *exitpc)
{
    // No break statements, no current.
    // This means that control flow is cut-off from this point
    // (e.g. all cases have return statements).
    if (!breaks && !current)
        return ControlStatus_Ended;

    // Create successor block.
    // If there are breaks, create block with breaks as predecessor
    // Else create a block with current as predecessor
    MBasicBlock *successor = NULL;
    if (breaks)
        successor = createBreakCatchBlock(breaks, exitpc);
    else
        successor = newBlock(current, exitpc);

    if (!successor)
        return ControlStatus_Ended;

    // If there is current, the current block flows into this one.
    // So current is also a predecessor to this block
    if (current) {
        current->end(MGoto::New(successor));
        if (breaks)
            successor->addPredecessor(current);
    }

    pc = exitpc;
    current = successor;
    return ControlStatus_Joined;
}

IonBuilder::ControlStatus
IonBuilder::maybeLoop(JSOp op, jssrcnote *sn)
{
    // This function looks at the opcode and source note and tries to
    // determine the structure of the loop. For some opcodes, like
    // POP/NOP which are not explicitly control flow, this source note is
    // optional. For opcodes with control flow, like GOTO, an unrecognized
    // or not-present source note is a compilation failure.
    switch (op) {
      case JSOP_POP:
        // for (init; ; update?) ...
        if (sn && SN_TYPE(sn) == SRC_FOR) {
            current->pop();
            return forLoop(op, sn);
        }
        break;

      case JSOP_NOP:
        if (sn) {
            // do { } while (cond)
            if (SN_TYPE(sn) == SRC_WHILE)
                return doWhileLoop(op, sn);
            // Build a mapping such that given a basic block, whose successor
            // has a phi

            // for (; ; update?)
            if (SN_TYPE(sn) == SRC_FOR)
                return forLoop(op, sn);
        }
        break;

      default:
        JS_NOT_REACHED("unexpected opcode");
        return ControlStatus_Error;
    }

    return ControlStatus_None;
}

void
IonBuilder::assertValidLoopHeadOp(jsbytecode *pc)
{
#ifdef DEBUG
    JS_ASSERT(JSOp(*pc) == JSOP_LOOPHEAD);

    // Make sure this is the next opcode after the loop header,
    // unless the for loop is unconditional.
    CFGState &state = cfgStack_.back();
    JS_ASSERT_IF((JSOp)*(state.loop.entry->pc()) == JSOP_GOTO,
        GetNextPc(state.loop.entry->pc()) == pc);

    // do-while loops have a source note.
    jssrcnote *sn = info().getNote(cx, pc);
    if (sn) {
        jsbytecode *ifne = pc + js_GetSrcNoteOffset(sn, 0);

        jsbytecode *expected_ifne;
        switch (state.state) {
          case CFGState::DO_WHILE_LOOP_BODY:
            expected_ifne = state.loop.updateEnd;
            break;

          default:
            JS_NOT_REACHED("JSOP_LOOPHEAD unexpected source note");
            return;
        }

        // Make sure this loop goes to the same ifne as the loop header's
        // source notes or GOTO.
        JS_ASSERT(ifne == expected_ifne);
    } else {
        JS_ASSERT(state.state != CFGState::DO_WHILE_LOOP_BODY);
    }
#endif
}

IonBuilder::ControlStatus
IonBuilder::doWhileLoop(JSOp op, jssrcnote *sn)
{
    // do { } while() loops have the following structure:
    //    NOP         ; SRC_WHILE (offset to COND)
    //    LOOPHEAD    ; SRC_WHILE (offset to IFNE)
    //    LOOPENTRY
    //    ...         ; body
    //    ...
    //    COND        ; start of condition
    //    ...
    //    IFNE ->     ; goes to LOOPHEAD
    int condition_offset = js_GetSrcNoteOffset(sn, 0);
    jsbytecode *conditionpc = pc + condition_offset;

    jssrcnote *sn2 = info().getNote(cx, pc+1);
    int offset = js_GetSrcNoteOffset(sn2, 0);
    jsbytecode *ifne = pc + offset + 1;
    JS_ASSERT(ifne > pc);

    // Verify that the IFNE goes back to a loophead op.
    jsbytecode *loopHead = GetNextPc(pc);
    JS_ASSERT(JSOp(*loopHead) == JSOP_LOOPHEAD);
    JS_ASSERT(loopHead == ifne + GetJumpOffset(ifne));

    jsbytecode *loopEntry = GetNextPc(loopHead);
    if (info().hasOsrAt(loopEntry)) {
        MBasicBlock *preheader = newOsrPreheader(current, loopEntry);
        if (!preheader)
            return ControlStatus_Error;
        current->end(MGoto::New(preheader));
        current = preheader;
    }

    MBasicBlock *header = newPendingLoopHeader(current, pc);
    if (!header)
        return ControlStatus_Error;
    current->end(MGoto::New(header));

    jsbytecode *bodyStart = GetNextPc(GetNextPc(pc));
    jsbytecode *bodyEnd = conditionpc;
    jsbytecode *exitpc = GetNextPc(ifne);
    if (!pushLoop(CFGState::DO_WHILE_LOOP_BODY, conditionpc, header,
                  bodyStart, bodyEnd, exitpc, conditionpc))
    {
        return ControlStatus_Error;
    }

    CFGState &state = cfgStack_.back();
    state.loop.updatepc = conditionpc;
    state.loop.updateEnd = ifne;

    current = header;
    if (!jsop_loophead(GetNextPc(pc)))
        return ControlStatus_Error;

    pc = bodyStart;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::whileOrForInLoop(jssrcnote *sn)
{
    // while (cond) { } loops have the following structure:
    //    GOTO cond   ; SRC_WHILE (offset to IFNE)
    //    LOOPHEAD
    //    ...
    //  cond:
    //    LOOPENTRY
    //    ...
    //    IFNE        ; goes to LOOPHEAD
    // for (x in y) { } loops are similar; the cond will be a MOREITER.
    JS_ASSERT(SN_TYPE(sn) == SRC_FOR_IN || SN_TYPE(sn) == SRC_WHILE);
    int ifneOffset = js_GetSrcNoteOffset(sn, 0);
    jsbytecode *ifne = pc + ifneOffset;
    JS_ASSERT(ifne > pc);

    // Verify that the IFNE goes back to a loophead op.
    JS_ASSERT(JSOp(*GetNextPc(pc)) == JSOP_LOOPHEAD);
    JS_ASSERT(GetNextPc(pc) == ifne + GetJumpOffset(ifne));

    jsbytecode *loopEntry = pc + GetJumpOffset(pc);
    if (info().hasOsrAt(loopEntry)) {
        MBasicBlock *preheader = newOsrPreheader(current, loopEntry);
        if (!preheader)
            return ControlStatus_Error;
        current->end(MGoto::New(preheader));
        current = preheader;
    }

    MBasicBlock *header = newPendingLoopHeader(current, pc);
    if (!header)
        return ControlStatus_Error;
    current->end(MGoto::New(header));

    // Skip past the JSOP_LOOPHEAD for the body start.
    jsbytecode *bodyStart = GetNextPc(GetNextPc(pc));
    jsbytecode *bodyEnd = pc + GetJumpOffset(pc);
    jsbytecode *exitpc = GetNextPc(ifne);
    if (!pushLoop(CFGState::WHILE_LOOP_COND, ifne, header, bodyStart, bodyEnd, exitpc))
        return ControlStatus_Error;

    // Parse the condition first.
    current = header;
    if (!jsop_loophead(GetNextPc(pc)))
        return ControlStatus_Error;

    pc = bodyEnd;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::forLoop(JSOp op, jssrcnote *sn)
{
    // Skip the NOP or POP.
    JS_ASSERT(op == JSOP_POP || op == JSOP_NOP);
    pc = GetNextPc(pc);

    jsbytecode *condpc = pc + js_GetSrcNoteOffset(sn, 0);
    jsbytecode *updatepc = pc + js_GetSrcNoteOffset(sn, 1);
    jsbytecode *ifne = pc + js_GetSrcNoteOffset(sn, 2);
    jsbytecode *exitpc = GetNextPc(ifne);

    // for loops have the following structures:
    //
    //   NOP or POP
    //   [GOTO cond | NOP]
    //   LOOPHEAD
    // body:
    //    ; [body]
    // [increment:]
    //    ; [increment]
    // [cond:]
    //   LOOPENTRY
    //   GOTO body
    //
    // If there is a condition (condpc != ifne), this acts similar to a while
    // loop otherwise, it acts like a do-while loop.
    jsbytecode *bodyStart = pc;
    jsbytecode *bodyEnd = updatepc;
    jsbytecode *loopEntry = condpc;
    if (condpc != ifne) {
        JS_ASSERT(JSOp(*bodyStart) == JSOP_GOTO);
        JS_ASSERT(bodyStart + GetJumpOffset(bodyStart) == condpc);
        bodyStart = GetNextPc(bodyStart);
    } else {
        // No loop condition, such as for(j = 0; ; j++)
        if (op != JSOP_NOP) {
            // If the loop starts with POP, we have to skip a NOP.
            JS_ASSERT(JSOp(*bodyStart) == JSOP_NOP);
            bodyStart = GetNextPc(bodyStart);
        }
        loopEntry = GetNextPc(bodyStart);
    }
    jsbytecode *loopHead = bodyStart;
    JS_ASSERT(JSOp(*bodyStart) == JSOP_LOOPHEAD);
    JS_ASSERT(ifne + GetJumpOffset(ifne) == bodyStart);
    bodyStart = GetNextPc(bodyStart);

    if (info().hasOsrAt(loopEntry)) {
        MBasicBlock *preheader = newOsrPreheader(current, loopEntry);
        if (!preheader)
            return ControlStatus_Error;
        current->end(MGoto::New(preheader));
        current = preheader;
    }

    MBasicBlock *header = newPendingLoopHeader(current, pc);
    if (!header)
        return ControlStatus_Error;
    current->end(MGoto::New(header));

    // If there is no condition, we immediately parse the body. Otherwise, we
    // parse the condition.
    jsbytecode *stopAt;
    CFGState::State initial;
    if (condpc != ifne) {
        pc = condpc;
        stopAt = ifne;
        initial = CFGState::FOR_LOOP_COND;
    } else {
        pc = bodyStart;
        stopAt = bodyEnd;
        initial = CFGState::FOR_LOOP_BODY;
    }

    if (!pushLoop(initial, stopAt, header, bodyStart, bodyEnd, exitpc, updatepc))
        return ControlStatus_Error;

    CFGState &state = cfgStack_.back();
    state.loop.condpc = (condpc != ifne) ? condpc : NULL;
    state.loop.updatepc = (updatepc != condpc) ? updatepc : NULL;
    if (state.loop.updatepc)
        state.loop.updateEnd = condpc;

    current = header;
    if (!jsop_loophead(loopHead))
        return ControlStatus_Error;

    return ControlStatus_Jumped;
}

int
IonBuilder::CmpSuccessors(const void *a, const void *b)
{
    const MBasicBlock *a0 = * (MBasicBlock * const *)a;
    const MBasicBlock *b0 = * (MBasicBlock * const *)b;
    if (a0->pc() == b0->pc())
        return 0;

    return (a0->pc() > b0->pc()) ? 1 : -1;
}

IonBuilder::ControlStatus
IonBuilder::tableSwitch(JSOp op, jssrcnote *sn)
{
    // TableSwitch op contains the following data
    // (length between data is JUMP_OFFSET_LEN)
    //
    // 0: Offset of default case
    // 1: Lowest number in tableswitch
    // 2: Highest number in tableswitch
    // 3: Offset of case low
    // 4: Offset of case low+1
    // .: ...
    // .: Offset of case high

    JS_ASSERT(op == JSOP_TABLESWITCH);
    JS_ASSERT(SN_TYPE(sn) == SRC_TABLESWITCH);

    // Pop input.
    MDefinition *ins = current->pop();

    // Get the default and exit pc
    jsbytecode *exitpc = pc + js_GetSrcNoteOffset(sn, 0);
    jsbytecode *defaultpc = pc + GET_JUMP_OFFSET(pc);

    JS_ASSERT(defaultpc > pc && defaultpc <= exitpc);

    // Get the low and high from the tableswitch
    jsbytecode *pc2 = pc;
    pc2 += JUMP_OFFSET_LEN;
    int low = GET_JUMP_OFFSET(pc2);
    pc2 += JUMP_OFFSET_LEN;
    int high = GET_JUMP_OFFSET(pc2);
    pc2 += JUMP_OFFSET_LEN;

    // Create MIR instruction
    MTableSwitch *tableswitch = MTableSwitch::New(ins, low, high);

    // Create default case
    MBasicBlock *defaultcase = newBlock(current, defaultpc);
    if (!defaultcase)
        return ControlStatus_Error;
    tableswitch->addDefault(defaultcase);
    tableswitch->addBlock(defaultcase);

    // Create cases
    jsbytecode *casepc = NULL;
    for (int i = 0; i < high-low+1; i++) {
        casepc = pc + GET_JUMP_OFFSET(pc2);

        JS_ASSERT(casepc >= pc && casepc <= exitpc);

        MBasicBlock *caseblock = newBlock(current, casepc);
        if (!caseblock)
            return ControlStatus_Error;

        // If the casepc equals the current pc, it is not a written case,
        // but a filled gap. That way we can use a tableswitch instead of
        // condswitch, even if not all numbers are consecutive.
        // In that case this block goes to the default case
        if (casepc == pc) {
            caseblock->end(MGoto::New(defaultcase));
            defaultcase->addPredecessor(caseblock);
        }

        tableswitch->addCase(caseblock);

        // If this is an actual case (not filled gap),
        // add this block to the list that still needs to get processed
        if (casepc != pc)
            tableswitch->addBlock(caseblock);

        pc2 += JUMP_OFFSET_LEN;
    }

    // Move defaultcase to the end, to maintain RPO.
    graph().moveBlockToEnd(defaultcase);

    JS_ASSERT(tableswitch->numCases() == (uint32_t)(high - low + 1));
    JS_ASSERT(tableswitch->numSuccessors() > 0);

    // Sort the list of blocks that still needs to get processed by pc
    qsort(tableswitch->blocks(), tableswitch->numBlocks(),
          sizeof(MBasicBlock*), CmpSuccessors);

    // Create info
    ControlFlowInfo switchinfo(cfgStack_.length(), exitpc);
    if (!switches_.append(switchinfo))
        return ControlStatus_Error;

    // Use a state to retrieve some information
    CFGState state = CFGState::TableSwitch(exitpc, tableswitch);

    // Save the MIR instruction as last instruction of this block.
    current->end(tableswitch);

    // If there is only one successor the block should stop at the end of the switch
    // Else it should stop at the start of the next successor
    if (tableswitch->numBlocks() > 1)
        state.stopAt = tableswitch->getBlock(1)->pc();
    current = tableswitch->getBlock(0);

    if (!cfgStack_.append(state))
        return ControlStatus_Error;

    pc = current->pc();
    return ControlStatus_Jumped;
}

bool
IonBuilder::jsop_label()
{
    JS_ASSERT(JSOp(*pc) == JSOP_LABEL);

    jsbytecode *endpc = pc + GET_JUMP_OFFSET(pc);
    JS_ASSERT(endpc > pc);

    ControlFlowInfo label(cfgStack_.length(), endpc);
    if (!labels_.append(label))
        return false;

    return cfgStack_.append(CFGState::Label(endpc));
}

bool
IonBuilder::jsop_condswitch()
{
    // CondSwitch op looks as follows:
    //   condswitch [length +exit_pc; first case offset +next-case ]
    //   {
    //     {
    //       ... any code ...
    //       case (+jump) [pcdelta offset +next-case]
    //     }+
    //     default (+jump)
    //     ... jump targets ...
    //   }
    //
    // The default case is always emitted even if there is no default case in
    // the source.  The last case statement pcdelta source note might have a 0
    // offset on the last case (not all the time).
    //
    // A conditional evaluate the condition of each case and compare it to the
    // switch value with a strict equality.  Cases conditions are iterated
    // linearly until one is matching. If one case succeeds, the flow jumps into
    // the corresponding body block.  The body block might alias others and
    // might continue in the next body block if the body is not terminated with
    // a break.
    //
    // Algorithm:
    //  1/ Loop over the case chain to reach the default target
    //   & Estimate the number of uniq bodies.
    //  2/ Generate code for all cases (see processCondSwitchCase).
    //  3/ Generate code for all bodies (see processCondSwitchBody).

    JS_ASSERT(JSOp(*pc) == JSOP_CONDSWITCH);
    jssrcnote *sn = info().getNote(cx, pc);
    JS_ASSERT(SN_TYPE(sn) == SRC_CONDSWITCH);

    // Get the exit pc
    jsbytecode *exitpc = pc + js_GetSrcNoteOffset(sn, 0);
    jsbytecode *firstCase = pc + js_GetSrcNoteOffset(sn, 1);

    // Iterate all cases in the conditional switch.
    // - Stop at the default case. (always emitted after the last case)
    // - Estimate the number of uniq bodies. This estimation might be off by 1
    //   if the default body alias a case body.
    jsbytecode *curCase = firstCase;
    jsbytecode *lastTarget = GetJumpOffset(curCase) + curCase;
    size_t nbBodies = 2; // default target and the first body.

    JS_ASSERT(pc < curCase && curCase <= exitpc);
    while (JSOp(*curCase) == JSOP_CASE) {
        // Fetch the next case.
        jssrcnote *caseSn = info().getNote(cx, curCase);
        JS_ASSERT(caseSn && SN_TYPE(caseSn) == SRC_NEXTCASE);
        ptrdiff_t off = js_GetSrcNoteOffset(caseSn, 0);
        curCase = off ? curCase + off : GetNextPc(curCase);
        JS_ASSERT(pc < curCase && curCase <= exitpc);

        // Count non-aliased cases.
        jsbytecode *curTarget = GetJumpOffset(curCase) + curCase;
        if (lastTarget < curTarget)
            nbBodies++;
        lastTarget = curTarget;
    }

    // The current case now be the default case which jump to the body of the
    // default case, which might be behind the last target.
    JS_ASSERT(JSOp(*curCase) == JSOP_DEFAULT);
    jsbytecode *defaultTarget = GetJumpOffset(curCase) + curCase;
    JS_ASSERT(curCase < defaultTarget && defaultTarget <= exitpc);

    // Allocate the current graph state.
    CFGState state = CFGState::CondSwitch(exitpc, defaultTarget);
    if (!state.condswitch.bodies || !state.condswitch.bodies->init(nbBodies))
        return ControlStatus_Error;

    // We loop on case conditions with processCondSwitchCase.
    JS_ASSERT(JSOp(*firstCase) == JSOP_CASE);
    state.stopAt = firstCase;
    state.state = CFGState::COND_SWITCH_CASE;

    return cfgStack_.append(state);
}

IonBuilder::CFGState
IonBuilder::CFGState::CondSwitch(jsbytecode *exitpc, jsbytecode *defaultTarget)
{
    CFGState state;
    state.state = COND_SWITCH_CASE;
    state.stopAt = NULL;
    state.condswitch.bodies = (FixedList<MBasicBlock *> *)GetIonContext()->temp->allocate(
        sizeof(FixedList<MBasicBlock *>));
    state.condswitch.currentIdx = 0;
    state.condswitch.defaultTarget = defaultTarget;
    state.condswitch.defaultIdx = uint32_t(-1);
    state.condswitch.exitpc = exitpc;
    state.condswitch.breaks = NULL;
    return state;
}

IonBuilder::CFGState
IonBuilder::CFGState::Label(jsbytecode *exitpc)
{
    CFGState state;
    state.state = LABEL;
    state.stopAt = exitpc;
    state.label.breaks = NULL;
    return state;
}

IonBuilder::ControlStatus
IonBuilder::processCondSwitchCase(CFGState &state)
{
    JS_ASSERT(state.state == CFGState::COND_SWITCH_CASE);
    JS_ASSERT(!state.condswitch.breaks);
    JS_ASSERT(current);
    JS_ASSERT(JSOp(*pc) == JSOP_CASE);
    FixedList<MBasicBlock *> &bodies = *state.condswitch.bodies;
    jsbytecode *defaultTarget = state.condswitch.defaultTarget;
    uint32_t &currentIdx = state.condswitch.currentIdx;
    jsbytecode *lastTarget = currentIdx ? bodies[currentIdx - 1]->pc() : NULL;

    // Fetch the following case in which we will continue.
    jssrcnote *sn = info().getNote(cx, pc);
    ptrdiff_t off = js_GetSrcNoteOffset(sn, 0);
    jsbytecode *casePc = off ? pc + off : GetNextPc(pc);
    bool caseIsDefault = JSOp(*casePc) == JSOP_DEFAULT;
    JS_ASSERT(JSOp(*casePc) == JSOP_CASE || caseIsDefault);

    // Allocate the block of the matching case.
    bool bodyIsNew = false;
    MBasicBlock *bodyBlock = NULL;
    jsbytecode *bodyTarget = pc + GetJumpOffset(pc);
    if (lastTarget < bodyTarget) {
        // If the default body is in the middle or aliasing the current target.
        if (lastTarget < defaultTarget && defaultTarget <= bodyTarget) {
            JS_ASSERT(state.condswitch.defaultIdx == uint32_t(-1));
            state.condswitch.defaultIdx = currentIdx;
            bodies[currentIdx] = NULL;
            // If the default body does not alias any and it would be allocated
            // later and stored in the defaultIdx location.
            if (defaultTarget < bodyTarget)
                currentIdx++;
        }

        bodyIsNew = true;
        // Pop switch and case operands.
        bodyBlock = newBlockPopN(current, bodyTarget, 2);
        bodies[currentIdx++] = bodyBlock;
    } else {
        // This body alias the previous one.
        JS_ASSERT(lastTarget == bodyTarget);
        JS_ASSERT(currentIdx > 0);
        bodyBlock = bodies[currentIdx - 1];
    }

    if (!bodyBlock)
        return ControlStatus_Error;

    lastTarget = bodyTarget;

    // Allocate the block of the non-matching case.  This can either be a normal
    // case or the default case.
    bool caseIsNew = false;
    MBasicBlock *caseBlock = NULL;
    if (!caseIsDefault) {
        caseIsNew = true;
        // Pop the case operand.
        caseBlock = newBlockPopN(current, GetNextPc(pc), 1);
    } else {
        // The non-matching case is the default case, which jump directly to its
        // body. Skip the creation of a default case block and directly create
        // the default body if it does not alias any previous body.

        if (state.condswitch.defaultIdx == uint32_t(-1)) {
            // The default target is the last target.
            JS_ASSERT(lastTarget < defaultTarget);
            state.condswitch.defaultIdx = currentIdx++;
            caseIsNew = true;
        } else if (bodies[state.condswitch.defaultIdx] == NULL) {
            // The default target is in the middle and it does not alias any
            // case target.
            JS_ASSERT(defaultTarget < lastTarget);
            caseIsNew = true;
        } else {
            // The default target is in the middle and it alias a case target.
            JS_ASSERT(defaultTarget <= lastTarget);
            caseBlock = bodies[state.condswitch.defaultIdx];
        }

        // Allocate and register the default body.
        if (caseIsNew) {
            // Pop the case & switch operands.
            caseBlock = newBlockPopN(current, defaultTarget, 2);
            bodies[state.condswitch.defaultIdx] = caseBlock;
        }
    }

    if (!caseBlock)
        return ControlStatus_Error;

    // Terminate the last case condition block by emitting the code
    // corresponding to JSOP_CASE bytecode.
    if (bodyBlock != caseBlock) {
        MDefinition *caseOperand = current->pop();
        MDefinition *switchOperand = current->peek(-1);
        MCompare *cmpResult = MCompare::New(switchOperand, caseOperand, JSOP_STRICTEQ);
        TypeOracle::BinaryTypes b = oracle->binaryTypes(script(), pc);
        cmpResult->infer(b, cx);
        JS_ASSERT(!cmpResult->isEffectful());
        current->add(cmpResult);
        current->end(MTest::New(cmpResult, bodyBlock, caseBlock));

        // Add last case as predecessor of the body if the body is aliasing
        // the previous case body.
        if (!bodyIsNew && !bodyBlock->addPredecessorPopN(current, 1))
            return ControlStatus_Error;

        // Add last case as predecessor of the non-matching case if the
        // non-matching case is an aliased default case. We need to pop the
        // switch operand as we skip the default case block and use the default
        // body block directly.
        JS_ASSERT_IF(!caseIsNew, caseIsDefault);
        if (!caseIsNew && !caseBlock->addPredecessorPopN(current, 1))
            return ControlStatus_Error;
    } else {
        // The default case alias the last case body.
        JS_ASSERT(caseIsDefault);
        current->pop(); // Case operand
        current->pop(); // Switch operand
        current->end(MGoto::New(bodyBlock));
        if (!bodyIsNew && !bodyBlock->addPredecessor(current))
            return ControlStatus_Error;
    }

    if (caseIsDefault) {
        // The last case condition is finished.  Loop in processCondSwitchBody,
        // with potential stops in processSwitchBreak.  Check that the bodies
        // fixed list is over-estimate by at most 1, and shrink the size such as
        // length can be used as an upper bound while iterating bodies.
        JS_ASSERT(currentIdx == bodies.length() || currentIdx + 1 == bodies.length());
        bodies.shrink(bodies.length() - currentIdx);

        // Handle break statements in processSwitchBreak while processing
        // bodies.
        ControlFlowInfo breakInfo(cfgStack_.length() - 1, state.condswitch.exitpc);
        if (!switches_.append(breakInfo))
            return ControlStatus_Error;

        // Jump into the first body.
        currentIdx = 0;
        current = NULL;
        state.state = CFGState::COND_SWITCH_BODY;
        return processCondSwitchBody(state);
    }

    // Continue until the case condition.
    current = caseBlock;
    pc = current->pc();
    state.stopAt = casePc;
    return ControlStatus_Jumped;
}

IonBuilder::ControlStatus
IonBuilder::processCondSwitchBody(CFGState &state)
{
    JS_ASSERT(state.state == CFGState::COND_SWITCH_BODY);
    JS_ASSERT(pc <= state.condswitch.exitpc);
    FixedList<MBasicBlock *> &bodies = *state.condswitch.bodies;
    uint32_t &currentIdx = state.condswitch.currentIdx;

    JS_ASSERT(currentIdx <= bodies.length());
    if (currentIdx == bodies.length()) {
        JS_ASSERT_IF(current, pc == state.condswitch.exitpc);
        return processSwitchEnd(state.condswitch.breaks, state.condswitch.exitpc);
    }

    // Get the next body
    MBasicBlock *nextBody = bodies[currentIdx++];
    JS_ASSERT_IF(current, pc == nextBody->pc());

    // Fix the reverse post-order iteration.
    graph().moveBlockToEnd(nextBody);

    // The last body continue into the new one.
    if (current) {
        current->end(MGoto::New(nextBody));
        nextBody->addPredecessor(current);
    }

    // Continue in the next body.
    current = nextBody;
    pc = current->pc();

    if (currentIdx < bodies.length())
        state.stopAt = bodies[currentIdx]->pc();
    else
        state.stopAt = state.condswitch.exitpc;
    return ControlStatus_Jumped;
}

bool
IonBuilder::jsop_andor(JSOp op)
{
    JS_ASSERT(op == JSOP_AND || op == JSOP_OR);

    jsbytecode *rhsStart = pc + js_CodeSpec[op].length;
    jsbytecode *joinStart = pc + GetJumpOffset(pc);
    JS_ASSERT(joinStart > pc);

    // We have to leave the LHS on the stack.
    MDefinition *lhs = current->peek(-1);

    MBasicBlock *evalRhs = newBlock(current, rhsStart);
    MBasicBlock *join = newBlock(current, joinStart);
    if (!evalRhs || !join)
        return false;

    MTest *test = (op == JSOP_AND)
                  ? MTest::New(lhs, evalRhs, join)
                  : MTest::New(lhs, join, evalRhs);
    TypeOracle::UnaryTypes types = oracle->unaryTypes(script(), pc);
    test->infer(types, cx);
    current->end(test);

    if (!cfgStack_.append(CFGState::AndOr(joinStart, join)))
        return false;

    current = evalRhs;
    return true;
}

bool
IonBuilder::jsop_dup2()
{
    uint32_t lhsSlot = current->stackDepth() - 2;
    uint32_t rhsSlot = current->stackDepth() - 1;
    current->pushSlot(lhsSlot);
    current->pushSlot(rhsSlot);
    return true;
}

bool
IonBuilder::jsop_loophead(jsbytecode *pc)
{
    assertValidLoopHeadOp(pc);

    current->add(MInterruptCheck::New());

    return true;
}

bool
IonBuilder::jsop_ifeq(JSOp op)
{
    // IFEQ always has a forward offset.
    jsbytecode *trueStart = pc + js_CodeSpec[op].length;
    jsbytecode *falseStart = pc + GetJumpOffset(pc);
    JS_ASSERT(falseStart > pc);

    // We only handle cases that emit source notes.
    jssrcnote *sn = info().getNote(cx, pc);
    if (!sn)
        return abort("expected sourcenote");

    MDefinition *ins = current->pop();

    // Create true and false branches.
    MBasicBlock *ifTrue = newBlock(current, trueStart);
    MBasicBlock *ifFalse = newBlock(current, falseStart);
    if (!ifTrue || !ifFalse)
        return false;

    MTest *test = MTest::New(ins, ifTrue, ifFalse);
    current->end(test);

    // The bytecode for if/ternary gets emitted either like this:
    //
    //    IFEQ X  ; src note (IF_ELSE, COND) points to the GOTO
    //    ...
    //    GOTO Z
    // X: ...     ; else/else if
    //    ...
    // Z:         ; join
    //
    // Or like this:
    //
    //    IFEQ X  ; src note (IF) has no offset
    //    ...
    // Z: ...     ; join
    //
    // We want to parse the bytecode as if we were parsing the AST, so for the
    // IF_ELSE/COND cases, we use the source note and follow the GOTO. For the
    // IF case, the IFEQ offset is the join point.
    switch (SN_TYPE(sn)) {
      case SRC_IF:
        if (!cfgStack_.append(CFGState::If(falseStart, ifFalse)))
            return false;
        break;

      case SRC_IF_ELSE:
      case SRC_COND:
      {
        // Infer the join point from the JSOP_GOTO[X] sitting here, then
        // assert as we much we can that this is the right GOTO.
        jsbytecode *trueEnd = pc + js_GetSrcNoteOffset(sn, 0);
        JS_ASSERT(trueEnd > pc);
        JS_ASSERT(trueEnd < falseStart);
        JS_ASSERT(JSOp(*trueEnd) == JSOP_GOTO);
        JS_ASSERT(!info().getNote(cx, trueEnd));

        jsbytecode *falseEnd = trueEnd + GetJumpOffset(trueEnd);
        JS_ASSERT(falseEnd > trueEnd);
        JS_ASSERT(falseEnd >= falseStart);

        if (!cfgStack_.append(CFGState::IfElse(trueEnd, falseEnd, ifFalse)))
            return false;
        break;
      }

      default:
        JS_NOT_REACHED("unexpected source note type");
        break;
    }

    // Switch to parsing the true branch. Note that no PC update is needed,
    // it's the next instruction.
    current = ifTrue;

    return true;
}

IonBuilder::ControlStatus
IonBuilder::processReturn(JSOp op)
{
    MDefinition *def;
    switch (op) {
      case JSOP_RETURN:
        def = current->pop();
        break;

      case JSOP_STOP:
      {
        MInstruction *ins = MConstant::New(UndefinedValue());
        current->add(ins);
        def = ins;
        break;
      }

      default:
        def = NULL;
        JS_NOT_REACHED("unknown return op");
        break;
    }

    if (instrumentedProfiling())
        current->add(MFunctionBoundary::New(script(), MFunctionBoundary::Exit));
    MReturn *ret = MReturn::New(def);
    current->end(ret);

    if (!graph().addExit(current))
        return ControlStatus_Error;

    // Make sure no one tries to use this block now.
    current = NULL;
    return processControlEnd();
}

IonBuilder::ControlStatus
IonBuilder::processThrow()
{
    MDefinition *def = current->pop();

    MThrow *ins = MThrow::New(def);
    current->end(ins);

    if (!graph().addExit(current))
        return ControlStatus_Error;

    // Make sure no one tries to use this block now.
    current = NULL;
    return processControlEnd();
}

bool
IonBuilder::pushConstant(const Value &v)
{
    MConstant *ins = MConstant::New(v);
    current->add(ins);
    current->push(ins);
    return true;
}

bool
IonBuilder::jsop_bitnot()
{
    MDefinition *input = current->pop();
    MBitNot *ins = MBitNot::New(input);

    current->add(ins);
    ins->infer(oracle->unaryTypes(script(), pc));

    current->push(ins);
    if (ins->isEffectful() && !resumeAfter(ins))
        return false;
    return true;
}
bool
IonBuilder::jsop_bitop(JSOp op)
{
    // Pop inputs.
    MDefinition *right = current->pop();
    MDefinition *left = current->pop();

    MBinaryBitwiseInstruction *ins;
    switch (op) {
      case JSOP_BITAND:
        ins = MBitAnd::New(left, right);
        break;

      case JSOP_BITOR:
        ins = MBitOr::New(left, right);
        break;

      case JSOP_BITXOR:
        ins = MBitXor::New(left, right);
        break;

      case JSOP_LSH:
        ins = MLsh::New(left, right);
        break;

      case JSOP_RSH:
        ins = MRsh::New(left, right);
        break;

      case JSOP_URSH:
        ins = MUrsh::New(left, right);
        break;

      default:
        JS_NOT_REACHED("unexpected bitop");
        return false;
    }

    current->add(ins);
    TypeOracle::BinaryTypes types = oracle->binaryTypes(script(), pc);
    ins->infer(types);

    current->push(ins);
    if (ins->isEffectful() && !resumeAfter(ins))
        return false;

    return true;
}

bool
IonBuilder::jsop_binary(JSOp op, MDefinition *left, MDefinition *right)
{
    TypeOracle::Binary b = oracle->binaryOp(script(), pc);

    if (op == JSOP_ADD && b.rval == MIRType_String &&
        (b.lhs == MIRType_String || b.lhs == MIRType_Int32) &&
        (b.rhs == MIRType_String || b.rhs == MIRType_Int32))
    {
        MConcat *ins = MConcat::New(left, right);
        current->add(ins);
        current->push(ins);
        return maybeInsertResume();
    }

    MBinaryArithInstruction *ins;
    switch (op) {
      case JSOP_ADD:
        ins = MAdd::New(left, right);
        break;

      case JSOP_SUB:
        ins = MSub::New(left, right);
        break;

      case JSOP_MUL:
        ins = MMul::New(left, right);
        break;

      case JSOP_DIV:
        ins = MDiv::New(left, right);
        break;

      case JSOP_MOD:
        ins = MMod::New(left, right);
        break;

      default:
        JS_NOT_REACHED("unexpected binary opcode");
        return false;
    }

    TypeOracle::BinaryTypes types = oracle->binaryTypes(script(), pc);
    current->add(ins);
    ins->infer(types, cx);
    current->push(ins);

    if (ins->isEffectful())
        return resumeAfter(ins);
    return maybeInsertResume();
}

bool
IonBuilder::jsop_binary(JSOp op)
{
    MDefinition *right = current->pop();
    MDefinition *left = current->pop();

    return jsop_binary(op, left, right);
}

bool
IonBuilder::jsop_pos()
{
    TypeOracle::Unary types = oracle->unaryOp(script(), pc);
    if (IsNumberType(types.ival)) {
        // Already int32 or double.
        JS_ASSERT(IsNumberType(types.rval));
        return true;
    }

    // Compile +x as x * 1.
    MDefinition *value = current->pop();
    MConstant *one = MConstant::New(Int32Value(1));
    current->add(one);

    return jsop_binary(JSOP_MUL, value, one);
}

bool
IonBuilder::jsop_neg()
{
    // Since JSOP_NEG does not use a slot, we cannot push the MConstant.
    // The MConstant is therefore passed to JSOP_MUL without slot traffic.
    MConstant *negator = MConstant::New(Int32Value(-1));
    current->add(negator);

    MDefinition *right = current->pop();

    if (!jsop_binary(JSOP_MUL, negator, right))
        return false;
    return true;
}

bool
IonBuilder::jsop_notearg()
{
    // JSOP_NOTEARG notes that the value in current->pop() has just
    // been pushed onto the stack for use in calling a function.
    MDefinition *def = current->pop();
    MPassArg *arg = MPassArg::New(def);

    current->add(arg);
    current->push(arg);
    return true;
}

class AutoAccumulateExits
{
    MIRGraph &graph_;
    MIRGraphExits *prev_;

  public:
    AutoAccumulateExits(MIRGraph &graph, MIRGraphExits &exits)
      : graph_(graph)
    {
        prev_ = graph_.exitAccumulator();
        graph_.setExitAccumulator(&exits);
    }
    ~AutoAccumulateExits() {
        graph_.setExitAccumulator(prev_);
    }
};

bool
IonBuilder::inlineScriptedCall(CallInfo &callInfo, JSFunction *target)
{
    JS_ASSERT(target->isInterpreted());
    JS_ASSERT(callInfo.hasCallType());
    JS_ASSERT(types::IsInlinableCall(pc));

    // Remove any MPassArgs.
    if (callInfo.isWrapped())
        callInfo.unwrapArgs();

    // Ensure sufficient space in the slots: needed for inlining from FUNAPPLY.
    uint32_t depth = current->stackDepth() + callInfo.numFormals();
    if (depth > current->nslots()) {
        if (!current->increaseSlots(depth - current->nslots()))
            return false;
    }

    // Create new |this| on the caller-side for inlined constructors.
    if (callInfo.constructing()) {
        RootedFunction targetRoot(cx, target);
        MDefinition *thisDefn = createThis(targetRoot, callInfo.fun());
        if (!thisDefn)
            return false;
        callInfo.setThis(thisDefn);
    }

    // Capture formals in the outer resume point.
    callInfo.pushFormals(current);

    MResumePoint *outerResumePoint =
        MResumePoint::New(current, pc, callerResumePoint_, MResumePoint::Outer);
    if (!outerResumePoint)
        return false;

    // Pop formals again, except leave |fun| on stack for duration of call.
    callInfo.popFormals(current);
    current->push(callInfo.fun());

    RootedScript calleeScript(cx, target->nonLazyScript());
    TypeInferenceOracle oracle;
    if (!oracle.init(cx, calleeScript))
        return false;

    // Copy the CallInfo as the addTypeBarrier is mutating it.
    bool argsBarrier = callInfo.argsBarrier();
    CallInfo clonedCallInfo(cx, callInfo.constructing());
    CallInfo &thisCall = argsBarrier ? clonedCallInfo : callInfo;

    // Add exclude type barriers.
    if (argsBarrier) {
        if (!thisCall.init(callInfo))
            return false;

        addTypeBarrier(0, thisCall, oracle.thisTypeSet(calleeScript));
        int32_t max = (thisCall.argc() < target->nargs) ? thisCall.argc() : target->nargs;
        for (int32_t i = 1; i <= max; i++)
            addTypeBarrier(i, thisCall, oracle.parameterTypeSet(calleeScript, i - 1));
    }

    // Start inlining.
    LifoAlloc *alloc = GetIonContext()->temp->lifoAlloc();
    CompileInfo *info = alloc->new_<CompileInfo>(calleeScript.get(), target,
                                                 (jsbytecode *)NULL, thisCall.constructing(),
                                                 this->info().executionMode());
    if (!info)
        return false;

    MIRGraphExits saveExits;
    AutoAccumulateExits aae(graph(), saveExits);

    // Build the graph.
    IonBuilder inlineBuilder(cx, &temp(), &graph(), &oracle, info, inliningDepth_ + 1, loopDepth_);
    if (!inlineBuilder.buildInline(this, outerResumePoint, thisCall)) {
        JS_ASSERT(calleeScript->hasAnalysis());

        // Inlining the callee failed. Disable inlining the function
        if (inlineBuilder.abortReason_ == AbortReason_Disable)
            calleeScript->analysis()->setIonUninlineable();

        abortReason_ = AbortReason_Inlining;
        return false;
    }

    // Create return block.
    jsbytecode *postCall = GetNextPc(pc);
    MBasicBlock *returnBlock = newBlock(NULL, postCall);
    if (!returnBlock)
        return false;
    returnBlock->setCallerResumePoint(callerResumePoint_);

    // When profiling add Inline_Exit instruction to indicate end of inlined function.
    if (instrumentedProfiling())
        returnBlock->add(MFunctionBoundary::New(NULL, MFunctionBoundary::Inline_Exit));

    // Inherit the slots from current and pop |fun|.
    returnBlock->inheritSlots(current);
    returnBlock->pop();

    // Accumulate return values.
    MIRGraphExits &exits = *inlineBuilder.graph().exitAccumulator();
    MDefinition *retvalDefn = patchInlinedReturns(thisCall, exits, returnBlock);
    if (!retvalDefn)
        return false;
    returnBlock->push(retvalDefn);

    // Initialize entry slots now that the stack has been fixed up.
    if (!returnBlock->initEntrySlots())
        return false;

    current = returnBlock;
    return true;
}

void
IonBuilder::addTypeBarrier(uint32_t i, CallInfo &callinfo, types::StackTypeSet *calleeObs)
{
    MDefinition *ins = NULL;
    types::StackTypeSet *callerObs = NULL;
    types::TypeBarrier *excluded = callinfo.argsBarrier();

    if (i == 0) {
        // The |this| type get's created during execution, no need to add type barrier.
        if (callinfo.constructing())
            return;
        ins = callinfo.thisArg();
        callerObs = callinfo.thisType();
    } else {
        ins = callinfo.getArg(i - 1);
        callerObs = callinfo.getArgType(i - 1);
    }

    bool needsBarrier = false;

    // Make sure unknown inputs are always boxed.
    if (callerObs->getKnownTypeTag() == JSVAL_TYPE_UNKNOWN &&
        ins->type() != MIRType_Value)
    {
        MBox *box = MBox::New(ins);
        current->add(box);
        ins = box;
    }

    while (excluded) {
        if (excluded->target == calleeObs && callerObs->hasType(excluded->type)) {
            if (excluded->type == types::Type::DoubleType() &&
                calleeObs->hasType(types::Type::Int32Type()))
            {
                // The double type also implies int32, so this implies that
                // double should be coerced into int if possible, and other
                // types should remain.

                JSValueType callerType = callerObs->getKnownTypeTag();
                if (callerType != JSVAL_TYPE_DOUBLE && ins->type() != MIRType_Double) {
                    // We expect either an Int or a Value, this variant is not
                    // optimized and favor the int variant by filtering out all
                    // other inputs.
                    JS_ASSERT(callerType == JSVAL_TYPE_UNKNOWN);
                    JS_ASSERT(ins->type() == MIRType_Value);
                    // Bail if the input is not a number.
                    MInstruction *toDouble = MUnbox::New(ins, MIRType_Double, MUnbox::Fallible);
                    current->add(toDouble);
                    ins = toDouble;
                }
                JS_ASSERT(ins->type() == MIRType_Double ||
                          ins->type() == MIRType_Value ||
                          ins->type() == MIRType_Int32);
                // Bail if the double does not fit in an int.
                MInstruction *toInt = MToInt32::New(ins);
                current->add(toInt);
                ins = toInt;

                needsBarrier = false;
                break;
            }

            needsBarrier = true;
        }
        excluded = excluded->next;
    }

    if (needsBarrier) {
        MTypeBarrier *barrier = MTypeBarrier::New(ins, cloneTypeSet(calleeObs), Bailout_Normal);
        current->add(barrier);
    }

    if (i == 0)
        callinfo.setThis(ins);
    else
        callinfo.setArg(i - 1, ins);
}

MDefinition *
IonBuilder::patchInlinedReturn(CallInfo &callInfo, MBasicBlock *exit, MBasicBlock *bottom)
{
    // Replaces the MReturn in the exit block with an MGoto.
    MDefinition *rdef = exit->lastIns()->toReturn()->input();
    exit->discardLastIns();

    // Constructors must be patched by the caller to always return an object.
    if (callInfo.constructing()) {
        if (rdef->type() == MIRType_Value) {
            // Unknown return: dynamically detect objects.
            MReturnFromCtor *filter = MReturnFromCtor::New(rdef, callInfo.thisArg());
            exit->add(filter);
            rdef = filter;
        } else if (rdef->type() != MIRType_Object) {
            // Known non-object return: force |this|.
            rdef = callInfo.thisArg();
        }
    }

    MGoto *replacement = MGoto::New(bottom);
    exit->end(replacement);
    if (!bottom->addPredecessorWithoutPhis(exit))
        return NULL;

    return rdef;
}

MDefinition *
IonBuilder::patchInlinedReturns(CallInfo &callInfo, MIRGraphExits &exits, MBasicBlock *bottom)
{
    // Replaces MReturns with MGotos, returning the MDefinition
    // representing the return value, or NULL.
    JS_ASSERT(exits.length() > 0);

    if (exits.length() == 1)
        return patchInlinedReturn(callInfo, exits[0], bottom);

    // Accumulate multiple returns with a phi.
    MPhi *phi = MPhi::New(bottom->stackDepth());
    if (!phi->reserveLength(exits.length()))
        return NULL;

    for (size_t i = 0; i < exits.length(); i++) {
        MDefinition *rdef = patchInlinedReturn(callInfo, exits[i], bottom);
        if (!rdef)
            return NULL;
        phi->addInput(rdef);
    }

    bottom->addPhi(phi);
    return phi;
}

static bool
IsSmallFunction(JSScript *script)
{
    return script->length <= js_IonOptions.smallFunctionMaxBytecodeLength;
}

bool
IonBuilder::makeInliningDecision(JSFunction *target, CallInfo &callInfo)
{
    // Only inline when inlining is enabled.
    if (!inliningEnabled())
        return false;

    // When there is no target, inlining is impossible.
    if (target == NULL)
        return false;

    // Native functions provide their own detection in inlineNativeCall().
    if (target->isNative())
        return true;

    // Determine whether inlining is possible at callee site
    if (!canInlineTarget(target, callInfo))
        return false;

    // Determine whether inlining is possible at caller site
    RootedScript scriptRoot(cx, script());
    JSScript *targetScript = target->nonLazyScript();
    if (!oracle->canInlineCall(scriptRoot, pc)) {
        IonSpew(IonSpew_Inlining, "%s:%d - Cannot inline due to uninlineable call site",
                                  targetScript->filename(), targetScript->lineno);
        return false;
    }

    // Heuristics!

    // Cap the inlining depth.
    if (IsSmallFunction(targetScript)) {
        if (inliningDepth_ >= js_IonOptions.smallFunctionMaxInlineDepth) {
            IonSpew(IonSpew_Inlining, "%s:%d - Vetoed: exceeding allowed inline depth",
                                      targetScript->filename(), targetScript->lineno);
            return false;
        }
    } else {
        if (inliningDepth_ >= js_IonOptions.maxInlineDepth) {
            IonSpew(IonSpew_Inlining, "%s:%d - Vetoed: exceeding allowed inline depth",
                                      targetScript->filename(), targetScript->lineno);
            return false;
        }
     }

    // Always inline the empty script up to the inlining depth.
    if (targetScript->length == 1)
        return true;

    // Callee must not be excessively large.
    // This heuristic also applies to the callsite as a whole.
    if (targetScript->length > js_IonOptions.inlineMaxTotalBytecodeLength) {
        IonSpew(IonSpew_Inlining, "%s:%d - Vetoed: callee excessively large.",
                                  targetScript->filename(), targetScript->lineno);
        return false;
    }

    // Caller must be... somewhat hot.
    uint32_t callerUses = script()->getUseCount();
    if (callerUses < js_IonOptions.usesBeforeInlining()) {
        IonSpew(IonSpew_Inlining, "%s:%d - Vetoed: caller is insufficiently hot.",
                                  targetScript->filename(), targetScript->lineno);
        return false;
    }

    // Callee must be hot relative to the caller.
    if (targetScript->getUseCount() * js_IonOptions.inlineUseCountRatio < callerUses) {
        IonSpew(IonSpew_Inlining, "%s:%d - Vetoed: callee is not hot.",
                                  targetScript->filename(), targetScript->lineno);
        return false;
    }

    return true;
}

uint32_t
IonBuilder::selectInliningTargets(AutoObjectVector &targets, CallInfo &callInfo, Vector<bool> &choiceSet)
{
    uint32_t totalSize = 0;
    uint32_t numInlineable = 0;

    // For each target, ask whether it may be inlined.
    if (!choiceSet.reserve(targets.length()))
        return false;
    for (size_t i = 0; i < targets.length(); i++) {
        JSFunction *target = targets[i]->toFunction();
        bool inlineable = makeInliningDecision(target, callInfo);

        // Enforce a maximum inlined bytecode limit at the callsite.
        if (inlineable && target->isInterpreted()) {
            totalSize += target->nonLazyScript()->length;
            if (totalSize > js_IonOptions.inlineMaxTotalBytecodeLength)
                inlineable = false;
        }

        choiceSet.append(inlineable);
        if (inlineable)
            numInlineable++;
    }

    JS_ASSERT(choiceSet.length() == targets.length());
    return numInlineable;
}

static bool
CanInlineGetPropertyCache(MGetPropertyCache *cache, MDefinition *thisDef)
{
    JS_ASSERT(cache->object()->type() == MIRType_Object);
    if (cache->object() != thisDef)
        return false;

    InlinePropertyTable *table = cache->propTable();
    if (!table)
        return false;
    if (table->numEntries() == 0)
        return false;
    return true;
}

MGetPropertyCache *
IonBuilder::getInlineableGetPropertyCache(CallInfo &callInfo)
{
    if (callInfo.constructing())
        return NULL;

    MDefinition *thisDef = callInfo.thisArg();
    if (thisDef->type() != MIRType_Object)
        return NULL;

    // Unwrap thisDef for pointer comparison purposes.
    if (thisDef->isPassArg())
        thisDef = thisDef->toPassArg()->getArgument();

    MDefinition *funcDef = callInfo.fun();
    if (funcDef->type() != MIRType_Object)
        return NULL;

    // MGetPropertyCache with no uses may be optimized away.
    if (funcDef->isGetPropertyCache()) {
        MGetPropertyCache *cache = funcDef->toGetPropertyCache();
        if (cache->useCount() > 0)
            return NULL;
        if (!CanInlineGetPropertyCache(cache, thisDef))
            return NULL;
        return cache;
    }

    // Optimize away the following common pattern:
    // MUnbox[MIRType_Object, Infallible] <- MTypeBarrier <- MGetPropertyCache
    if (funcDef->isUnbox()) {
        MUnbox *unbox = funcDef->toUnbox();
        if (unbox->mode() != MUnbox::Infallible)
            return NULL;
        if (unbox->useCount() > 0)
            return NULL;
        if (!unbox->input()->isTypeBarrier())
            return NULL;

        MTypeBarrier *barrier = unbox->input()->toTypeBarrier();
        if (barrier->useCount() != 1)
            return NULL;
        if (!barrier->input()->isGetPropertyCache())
            return NULL;

        MGetPropertyCache *cache = barrier->input()->toGetPropertyCache();
        if (cache->useCount() > 1)
            return NULL;
        if (!CanInlineGetPropertyCache(cache, thisDef))
            return NULL;
        return cache;
    }

    return NULL;
}

MPolyInlineDispatch *
IonBuilder::makePolyInlineDispatch(JSContext *cx, CallInfo &callInfo,
                                   MGetPropertyCache *getPropCache, MBasicBlock *bottom,
                                   Vector<MDefinition *, 8, IonAllocPolicy> &retvalDefns)
{
    // If we're not optimizing away a GetPropertyCache, then this is pretty simple.
    if (!getPropCache)
        return MPolyInlineDispatch::New(callInfo.fun());

    InlinePropertyTable *inlinePropTable = getPropCache->propTable();

    // Take a resumepoint at this point so we can capture the state of the stack
    // immediately prior to the call operation.
    MResumePoint *preCallResumePoint =
        MResumePoint::New(current, pc, callerResumePoint_, MResumePoint::ResumeAt);
    if (!preCallResumePoint)
        return NULL;
    DebugOnly<size_t> preCallFuncDefnIdx = preCallResumePoint->numOperands() - (((size_t) callInfo.argc()) + 2);
    JS_ASSERT(preCallResumePoint->getOperand(preCallFuncDefnIdx) == callInfo.fun());

    MDefinition *targetObject = getPropCache->object();

    // If we got here, then we know the following:
    //      1. The input to the CALL is a GetPropertyCache, or a GetPropertyCache
    //         followed by a TypeBarrier followed by an Unbox.
    //      2. The GetPropertyCache has inlineable cases by guarding on the Object's type
    //      3. The GetPropertyCache (and sequence of definitions) leading to the function
    //         definition is not used by anyone else.
    //      4. Notably, this means that no resume points as of yet capture the GetPropertyCache,
    //         which implies that everything from the GetPropertyCache up to the call is
    //         repeatable.

    // If we are optimizing away a getPropCache, we replace the funcDefn
    // with a constant undefined on the stack.
    int funcDefnDepth = -((int) callInfo.argc() + 2);
    MConstant *undef = MConstant::New(UndefinedValue());
    current->add(undef);
    current->rewriteAtDepth(funcDefnDepth, undef);

    // Now construct a fallbackPrepBlock that prepares the stack state for fallback.
    // Namely it pops off all the arguments and the callee.
    MBasicBlock *fallbackPrepBlock = newBlock(current, pc);
    if (!fallbackPrepBlock)
        return NULL;

    // Pop formals (|fun|, |this| and arguments).
    callInfo.popFormals(fallbackPrepBlock);

    // Generate a fallback block that'll do the call, but the PC for this fallback block
    // is the PC for the GetPropCache.
    JS_ASSERT(inlinePropTable->pc() != NULL);
    JS_ASSERT(inlinePropTable->priorResumePoint() != NULL);
    MBasicBlock *fallbackBlock = newBlock(fallbackPrepBlock, inlinePropTable->pc(),
                                          inlinePropTable->priorResumePoint());
    if (!fallbackBlock)
        return NULL;

    fallbackPrepBlock->end(MGoto::New(fallbackBlock));

    // The fallbackBlock inherits the state of the stack right before the getprop, which
    // means we have to pop off the target of the getprop before performing it.
    DebugOnly<MDefinition *> checkTargetObject = fallbackBlock->pop();
    JS_ASSERT(checkTargetObject == targetObject);

    // Remove the instructions leading to the function definition from the current
    // block and add them to the fallback block.  Also, discard the old instructions.
    if (callInfo.fun()->isGetPropertyCache()) {
        JS_ASSERT(callInfo.fun()->toGetPropertyCache() == getPropCache);
        fallbackBlock->addFromElsewhere(getPropCache);
        fallbackBlock->push(getPropCache);
    } else {
        JS_ASSERT(callInfo.fun()->isUnbox());
        MUnbox *unbox = callInfo.fun()->toUnbox();
        JS_ASSERT(unbox->input()->isTypeBarrier());
        JS_ASSERT(unbox->type() == MIRType_Object);
        JS_ASSERT(unbox->mode() == MUnbox::Infallible);

        MTypeBarrier *typeBarrier = unbox->input()->toTypeBarrier();
        JS_ASSERT(typeBarrier->input()->isGetPropertyCache());
        JS_ASSERT(typeBarrier->input()->toGetPropertyCache() == getPropCache);

        fallbackBlock->addFromElsewhere(getPropCache);
        fallbackBlock->addFromElsewhere(typeBarrier);
        fallbackBlock->addFromElsewhere(unbox);
        fallbackBlock->push(unbox);
    }

    // Finally create a fallbackEnd block to do the actual call.  The fallbackEnd block will
    // have the |pc| restored to the current PC.
    MBasicBlock *fallbackEndBlock = newBlock(fallbackBlock, pc, preCallResumePoint);
    if (!fallbackEndBlock)
        return NULL;
    fallbackBlock->end(MGoto::New(fallbackEndBlock));

    MBasicBlock *top = current;
    current = fallbackEndBlock;

    // Make the actual call.
    CallInfo realCallInfo(cx, callInfo.constructing());
    if (!realCallInfo.init(callInfo))
        return NULL;
    realCallInfo.popFormals(current);
    realCallInfo.wrapArgs(current);

    RootedFunction target(cx, NULL);
    makeCallBarrier(target, realCallInfo,
                    oracle->getCallTarget(script(), callInfo.argc(), pc),
                    false);

    current = top;

    // Create a new MPolyInlineDispatch containing the getprop and the fallback block
    return MPolyInlineDispatch::New(targetObject, inlinePropTable,
                                    fallbackPrepBlock, fallbackBlock,
                                    fallbackEndBlock);
}

IonBuilder::InliningStatus
IonBuilder::inlineSingleCall(CallInfo &callInfo, JSFunction *target)
{
    // The inlined target must always be explicitly provided as a constant.
    JS_ASSERT(callInfo.fun()->isConstant());

    // Expects formals to be popped and wrapped.
    if (target->isNative())
        return inlineNativeCall(callInfo, target->native());

    if (!inlineScriptedCall(callInfo, target))
        return InliningStatus_Error;
    return InliningStatus_Inlined;
}

IonBuilder::InliningStatus
IonBuilder::inlineCallsite(AutoObjectVector &targets, AutoObjectVector &originals,
                           CallInfo &callInfo)
{
    if (!inliningEnabled())
        return InliningStatus_NotInlined;

    if (targets.length() == 0)
        return InliningStatus_NotInlined;

    // Is the function provided by an MGetPropertyCache?
    // If so, the cache may be movable to a fallback path, with a dispatch
    // instruction guarding on the incoming TypeObject.
    MGetPropertyCache *propCache = getInlineableGetPropertyCache(callInfo);

    // Inline single targets -- unless they derive from a cache, in which case
    // avoiding the cache and guarding is still faster.
    if (!propCache && targets.length() == 1) {
        JSFunction *target = targets[0]->toFunction();
        if (!makeInliningDecision(target, callInfo))
            return InliningStatus_NotInlined;

        // Replace the function with an MConstant.
        callInfo.fun()->setFoldedUnchecked();
        MConstant *constFun = MConstant::New(ObjectValue(*target));
        current->add(constFun);
        callInfo.setFun(constFun);

        return inlineSingleCall(callInfo, target);
    }

    // Choose a subset of the targets for polymorphic inlining.
    Vector<bool> choiceSet(cx);
    uint32_t numInlined = selectInliningTargets(targets, callInfo, choiceSet);
    if (numInlined == 0)
        return InliningStatus_NotInlined;

    // Perform a polymorphic dispatch.
    if (!inlineCalls(callInfo, targets, originals, choiceSet, propCache))
        return InliningStatus_Error;

    return InliningStatus_Inlined;
}

bool
IonBuilder::inlineGenericFallback(JSFunction *target, CallInfo &callInfo, MBasicBlock *dispatchBlock,
                                  bool clonedAtCallsite)
{
    // Generate a new block with all arguments on-stack.
    MBasicBlock *fallbackBlock = newBlock(dispatchBlock, pc);
    if (!fallbackBlock)
        return false;

    // Create a new CallInfo to track modified state within this block.
    CallInfo fallbackInfo(cx, callInfo.constructing());
    if (!fallbackInfo.init(callInfo))
        return false;
    fallbackInfo.popFormals(fallbackBlock);
    fallbackInfo.wrapArgs(fallbackBlock);

    // Generate an MCall, which uses stateful |current|.
    current = fallbackBlock;
    RootedFunction targetRooted(cx, target);
    types::StackTypeSet *calleeTypes = oracle->getCallTarget(script(), callInfo.argc(), pc);
    if (!makeCallBarrier(targetRooted, fallbackInfo, calleeTypes, clonedAtCallsite))
        return false;

    // Pass return block to caller as |current|.
    return true;
}

bool
IonBuilder::inlineTypeObjectFallback(CallInfo &callInfo, MBasicBlock *dispatchBlock,
                                     MTypeObjectDispatch *dispatch, MGetPropertyCache *cache,
                                     MBasicBlock **fallbackTarget)
{
    // Getting here implies the following:
    // 1. The call function is an MGetPropertyCache, or an MGetPropertyCache
    //    followed by an MTypeBarrier, followed by an MUnbox.
    JS_ASSERT(callInfo.fun()->isGetPropertyCache() || callInfo.fun()->isUnbox());

    // 2. The MGetPropertyCache has inlineable cases by guarding on the TypeObject.
    JS_ASSERT(dispatch->numCases() > 0);

    // 3. The MGetPropertyCache (and, if applicable, MTypeBarrier and MUnbox) only
    //    have at most a single use.
    JS_ASSERT_IF(callInfo.fun()->isGetPropertyCache(), cache->useCount() == 0);
    JS_ASSERT_IF(callInfo.fun()->isUnbox(), cache->useCount() == 1);

    // This means that no resume points yet capture the MGetPropertyCache,
    // so everything from the MGetPropertyCache up until the call is movable.
    // We now move the MGetPropertyCache and friends into a fallback path.

    // Create a new CallInfo to track modified state within the fallback path.
    CallInfo fallbackInfo(cx, callInfo.constructing());
    if (!fallbackInfo.init(callInfo))
        return false;

    // Capture stack prior to the call operation. This captures the function.
    MResumePoint *preCallResumePoint =
        MResumePoint::New(dispatchBlock, pc, callerResumePoint_, MResumePoint::ResumeAt);
    if (!preCallResumePoint)
        return false;

    DebugOnly<size_t> preCallFuncIndex = preCallResumePoint->numOperands() - callInfo.numFormals();
    JS_ASSERT(preCallResumePoint->getOperand(preCallFuncIndex) == fallbackInfo.fun());

    // In the dispatch block, replace the function's slot entry with Undefined.
    MConstant *undefined = MConstant::New(UndefinedValue());
    dispatchBlock->add(undefined);
    dispatchBlock->rewriteAtDepth(-int(callInfo.numFormals()), undefined);

    // Construct a block that does nothing but remove formals from the stack.
    // This is effectively changing the entry resume point of the later fallback block.
    MBasicBlock *prepBlock = newBlock(dispatchBlock, pc);
    if (!prepBlock)
        return false;
    fallbackInfo.popFormals(prepBlock);

    // Construct a block into which the MGetPropertyCache can be moved.
    // This is subtle: the pc and resume point are those of the MGetPropertyCache!
    InlinePropertyTable *propTable = cache->propTable();
    JS_ASSERT(propTable->pc() != NULL);
    JS_ASSERT(propTable->priorResumePoint() != NULL);
    MBasicBlock *getPropBlock = newBlock(prepBlock, propTable->pc(), propTable->priorResumePoint());
    if (!getPropBlock)
        return false;

    prepBlock->end(MGoto::New(getPropBlock));

    // Since the getPropBlock inherited the stack from right before the MGetPropertyCache,
    // the target of the MGetPropertyCache is still on the stack.
    DebugOnly<MDefinition *> checkObject = getPropBlock->pop();
    JS_ASSERT(checkObject == cache->object());

    // Move the MGetPropertyCache and friends into the getPropBlock.
    if (fallbackInfo.fun()->isGetPropertyCache()) {
        JS_ASSERT(fallbackInfo.fun()->toGetPropertyCache() == cache);
        getPropBlock->addFromElsewhere(cache);
        getPropBlock->push(cache);
    } else {
        MUnbox *unbox = callInfo.fun()->toUnbox();
        JS_ASSERT(unbox->input()->isTypeBarrier());
        JS_ASSERT(unbox->type() == MIRType_Object);
        JS_ASSERT(unbox->mode() == MUnbox::Infallible);

        MTypeBarrier *typeBarrier = unbox->input()->toTypeBarrier();
        JS_ASSERT(typeBarrier->input()->isGetPropertyCache());
        JS_ASSERT(typeBarrier->input()->toGetPropertyCache() == cache);

        getPropBlock->addFromElsewhere(cache);
        getPropBlock->addFromElsewhere(typeBarrier);
        getPropBlock->addFromElsewhere(unbox);
        getPropBlock->push(unbox);
    }

    // Construct an end block with the correct resume point.
    MBasicBlock *preCallBlock = newBlock(getPropBlock, pc, preCallResumePoint);
    if (!preCallBlock)
        return false;
    getPropBlock->end(MGoto::New(preCallBlock));

    // Now inline the MCallGeneric, using preCallBlock as the dispatch point.
    if (!inlineGenericFallback(NULL, fallbackInfo, preCallBlock, false))
        return false;

    // inlineGenericFallback() set the return block as |current|.
    preCallBlock->end(MGoto::New(current));
    *fallbackTarget = prepBlock;
    return true;
}

bool
IonBuilder::inlineCalls(CallInfo &callInfo, AutoObjectVector &targets,
                        AutoObjectVector &originals, Vector<bool> &choiceSet,
                        MGetPropertyCache *maybeCache)
{
    // Only handle polymorphic inlining.
    JS_ASSERT(types::IsInlinableCall(pc));
    JS_ASSERT(choiceSet.length() == targets.length());
    JS_ASSERT_IF(!maybeCache, targets.length() >= 2);
    JS_ASSERT_IF(maybeCache, targets.length() >= 1);

    MBasicBlock *dispatchBlock = current;

    // Unwrap the arguments.
    JS_ASSERT(callInfo.hasTypeInfo());
    JS_ASSERT(callInfo.isWrapped());
    callInfo.unwrapArgs();
    callInfo.pushFormals(dispatchBlock);

    // Patch any InlinePropertyTable to only contain functions that are inlineable.
    // Also guarantee that the table uses functions from |targets| instead of |originals|.
    // The InlinePropertyTable will also be patched at the end to exclude native functions
    // that vetoed inlining.
    if (maybeCache) {
        InlinePropertyTable *propTable = maybeCache->propTable();
        propTable->trimToAndMaybePatchTargets(targets, originals);
        if (propTable->numEntries() == 0)
            maybeCache = NULL;
    }

    // Generate a dispatch based on guard kind.
    MDispatchInstruction *dispatch;
    if (maybeCache) {
        dispatch = MTypeObjectDispatch::New(maybeCache->object(), maybeCache->propTable());
        callInfo.fun()->setFoldedUnchecked();
    } else {
        dispatch = MFunctionDispatch::New(callInfo.fun());
    }

    // Generate a return block to host the rval-collecting MPhi.
    jsbytecode *postCall = GetNextPc(pc);
    MBasicBlock *returnBlock = newBlock(NULL, postCall);
    if (!returnBlock)
        return false;
    returnBlock->setCallerResumePoint(callerResumePoint_);

    // Set up stack, used to manually create a post-call resume point.
    returnBlock->inheritSlots(dispatchBlock);
    callInfo.popFormals(returnBlock);

    MPhi *retPhi = MPhi::New(returnBlock->stackDepth());
    returnBlock->addPhi(retPhi);
    returnBlock->push(retPhi);

    // Create a resume point from current stack state.
    returnBlock->initEntrySlots();

    // Reserve the capacity for the phi.
    // Note: this is an upperbound. Unreachable targets and uninlineable natives are also counted.
    uint32_t count = 1; // Possible fallback block.
    for (uint32_t i = 0; i < targets.length(); i++) {
        if (choiceSet[i])
            count++;
    }
    retPhi->reserveLength(count);

    // Inline each of the inlineable targets.
    JS_ASSERT(targets.length() == originals.length());
    for (uint32_t i = 0; i < targets.length(); i++) {
        JSFunction *target = targets[i]->toFunction();

        // Target must be inlineable.
        if (!choiceSet[i])
            continue;

        // Target must be reachable by the MDispatchInstruction.
        if (maybeCache && !maybeCache->propTable()->hasFunction(target)) {
            choiceSet[i] = false;
            continue;
        }

        MBasicBlock *inlineBlock = newBlock(dispatchBlock, pc);
        if (!inlineBlock)
            return false;

        // Create a function MConstant to use in the entry ResumePoint.
        // Note that guarding is on the original function pointer even
        // if there is a clone, since cloning occurs at the callsite.
        JSFunction *original = originals[i]->toFunction();
        MConstant *funcDef = MConstant::New(ObjectValue(*original));
        funcDef->setFoldedUnchecked();
        dispatchBlock->add(funcDef);

        // Use the MConstant in the inline resume point and on stack.
        int funIndex = inlineBlock->entryResumePoint()->numOperands() - callInfo.numFormals();
        inlineBlock->entryResumePoint()->replaceOperand(funIndex, funcDef);
        inlineBlock->rewriteSlot(funIndex, funcDef);

        // Create a new CallInfo to track modified state within the inline block.
        CallInfo inlineInfo(cx, callInfo.constructing());
        if (!inlineInfo.init(callInfo))
            return false;
        inlineInfo.popFormals(inlineBlock);
        inlineInfo.setFun(funcDef);
        inlineInfo.wrapArgs(inlineBlock);

        // Inline the call into the inlineBlock.
        current = inlineBlock;
        InliningStatus status = inlineSingleCall(inlineInfo, target);
        if (status == InliningStatus_Error)
            return false;

        // Natives may veto inlining.
        if (status == InliningStatus_NotInlined) {
            JS_ASSERT(target->isNative());
            JS_ASSERT(current == inlineBlock);
            // Undo operations
            inlineInfo.unwrapArgs();
            inlineBlock->entryResumePoint()->replaceOperand(funIndex, callInfo.fun());
            inlineBlock->rewriteSlot(funIndex, callInfo.fun());
            inlineBlock->discard(funcDef);
            graph().removeBlock(inlineBlock);
            choiceSet[i] = false;
            continue;
        }

        // inlineSingleCall() changed |current| to the inline return block.
        MBasicBlock *inlineReturnBlock = current;
        current = dispatchBlock;

        // Connect the inline path to the returnBlock.
        dispatch->addCase(original, inlineBlock);

        MDefinition *retVal = inlineReturnBlock->peek(-1);
        retPhi->addInput(retVal);
        inlineReturnBlock->end(MGoto::New(returnBlock));
        if (!returnBlock->addPredecessorWithoutPhis(inlineReturnBlock))
            return false;
    }

    // Patch the InlinePropertyTable to not dispatch to vetoed paths.
    if (maybeCache) {
        InlinePropertyTable *propTable = maybeCache->propTable();
        propTable->trimTo(targets, choiceSet);

        // If all paths were vetoed, output only a generic fallback path.
        if (propTable->numEntries() == 0) {
            JS_ASSERT(dispatch->numCases() == 0);
            maybeCache = NULL;
        }
    }

    // If necessary, generate a fallback path.
    // MTypeObjectDispatch always uses a fallback path.
    if (maybeCache || dispatch->numCases() < targets.length()) {
        // Generate fallback blocks, and set |current| to the fallback return block.
        if (maybeCache) {
            MBasicBlock *fallbackTarget;
            if (!inlineTypeObjectFallback(callInfo, dispatchBlock, (MTypeObjectDispatch *)dispatch,
                                          maybeCache, &fallbackTarget))
            {
                return false;
            }
            dispatch->addFallback(fallbackTarget);
        } else {
            JSFunction *remaining = NULL;
            bool clonedAtCallsite = false;

            // If there is only 1 remaining case, we can annotate the fallback call
            // with the target information.
            if (dispatch->numCases() + 1 == originals.length()) {
                for (uint32_t i = 0; i < originals.length(); i++) {
                    if (choiceSet[i])
                        continue;

                    remaining = targets[i]->toFunction();
                    clonedAtCallsite = targets[i] != originals[i];
                    break;
                }
            }

            if (!inlineGenericFallback(remaining, callInfo, dispatchBlock, clonedAtCallsite))
                return false;
            dispatch->addFallback(current);
        }

        MBasicBlock *fallbackReturnBlock = current;

        // Connect fallback case to return infrastructure.
        MDefinition *retVal = fallbackReturnBlock->peek(-1);
        retPhi->addInput(retVal);
        fallbackReturnBlock->end(MGoto::New(returnBlock));
        if (!returnBlock->addPredecessorWithoutPhis(fallbackReturnBlock))
            return false;
    }

    // Finally add the dispatch instruction.
    // This must be done at the end so that add() may be called above.
    dispatchBlock->end(dispatch);

    // Check the depth change: +1 for retval
    JS_ASSERT(returnBlock->stackDepth() == dispatchBlock->stackDepth() - callInfo.numFormals() + 1);

    graph().moveBlockToEnd(returnBlock);
    current = returnBlock;
    return true;
}

MInstruction *
IonBuilder::createDeclEnvObject(MDefinition *callee, MDefinition *scope)
{
    // Create a template CallObject that we'll use to generate inline object
    // creation.

    RootedScript script(cx, script_);
    RootedFunction fun(cx, info().fun());
    RootedObject templateObj(cx, DeclEnvObject::createTemplateObject(cx, fun));
    if (!templateObj)
        return NULL;

    // Add dummy values on the slot of the template object such as we do not try
    // mark uninitialized values.
    templateObj->setFixedSlot(DeclEnvObject::enclosingScopeSlot(), MagicValue(JS_GENERIC_MAGIC));
    templateObj->setFixedSlot(DeclEnvObject::lambdaSlot(), MagicValue(JS_GENERIC_MAGIC));

    // One field is added to the function to handle its name.  This cannot be a
    // dynamic slot because there is still plenty of room on the DeclEnv object.
    JS_ASSERT(!templateObj->hasDynamicSlots());

    // Allocate the actual object. It is important that no intervening
    // instructions could potentially bailout, thus leaking the dynamic slots
    // pointer.
    MInstruction *declEnvObj = MNewDeclEnvObject::New(templateObj);
    current->add(declEnvObj);

    // Initialize the object's reserved slots.
    current->add(MStoreFixedSlot::New(declEnvObj, DeclEnvObject::enclosingScopeSlot(), scope));
    current->add(MStoreFixedSlot::New(declEnvObj, DeclEnvObject::lambdaSlot(), callee));

    return declEnvObj;
}

MInstruction *
IonBuilder::createCallObject(MDefinition *callee, MDefinition *scope)
{
    // Create a template CallObject that we'll use to generate inline object
    // creation.

    RootedScript scriptRoot(cx, script());
    RootedObject templateObj(cx, CallObject::createTemplateObject(cx, scriptRoot));
    if (!templateObj)
        return NULL;

    // If the CallObject needs dynamic slots, allocate those now.
    MInstruction *slots;
    if (templateObj->hasDynamicSlots()) {
        size_t nslots = JSObject::dynamicSlotsCount(templateObj->numFixedSlots(),
                                                    templateObj->slotSpan());
        slots = MNewSlots::New(nslots);
    } else {
        slots = MConstant::New(NullValue());
    }
    current->add(slots);

    // Allocate the actual object. It is important that no intervening
    // instructions could potentially bailout, thus leaking the dynamic slots
    // pointer.
    MInstruction *callObj = MNewCallObject::New(templateObj, slots);
    current->add(callObj);

    // Initialize the object's reserved slots.
    current->add(MStoreFixedSlot::New(callObj, CallObject::enclosingScopeSlot(), scope));
    current->add(MStoreFixedSlot::New(callObj, CallObject::calleeSlot(), callee));

    // Initialize argument slots.
    for (AliasedFormalIter i(script()); i; i++) {
        unsigned slot = i.scopeSlot();
        unsigned formal = i.frameIndex();
        MDefinition *param = current->getSlot(info().argSlot(formal));
        if (slot >= templateObj->numFixedSlots())
            current->add(MStoreSlot::New(slots, slot - templateObj->numFixedSlots(), param));
        else
            current->add(MStoreFixedSlot::New(callObj, slot, param));
    }

    return callObj;
}

MDefinition *
IonBuilder::createThisScripted(MDefinition *callee)
{
    // Get callee.prototype.
    //
    // This instruction MUST be idempotent: since it does not correspond to an
    // explicit operation in the bytecode, we cannot use resumeAfter().
    // Getters may not override |prototype| fetching, so this operation is indeed idempotent.
    // - First try an idempotent property cache.
    // - Upon failing idempotent property cache, we can't use a non-idempotent cache,
    //   therefore we fallback to CallGetProperty
    //
    // Note: both CallGetProperty and GetPropertyCache can trigger a GC,
    //       and thus invalidation.
    MInstruction *getProto;
    if (!invalidatedIdempotentCache()) {
        MGetPropertyCache *getPropCache = MGetPropertyCache::New(callee, cx->names().classPrototype);
        getPropCache->setIdempotent();
        getProto = getPropCache;
    } else {
        MCallGetProperty *callGetProp = MCallGetProperty::New(callee, cx->names().classPrototype);
        callGetProp->setIdempotent();
        getProto = callGetProp;
    }
    current->add(getProto);

    // Create this from prototype
    MCreateThisWithProto *createThis = MCreateThisWithProto::New(callee, getProto);
    current->add(createThis);

    return createThis;
}

JSObject *
IonBuilder::getSingletonPrototype(JSFunction *target)
{
    if (!target || !target->hasSingletonType())
        return NULL;
    types::TypeObject *targetType = target->getType(cx);
    if (targetType->unknownProperties())
        return NULL;

    jsid protoid = NameToId(cx->names().classPrototype);
    types::HeapTypeSet *protoTypes = targetType->getProperty(cx, protoid, false);
    if (!protoTypes)
        return NULL;

    return protoTypes->getSingleton(cx);
}

MDefinition *
IonBuilder::createThisScriptedSingleton(HandleFunction target, MDefinition *callee)
{
    // Get the singleton prototype (if exists)
    RootedObject proto(cx, getSingletonPrototype(target));
    if (!proto)
        return NULL;

    // Generate an inline path to create a new |this| object with
    // the given singleton prototype.
    types::TypeObject *type = proto->getNewType(cx, &ObjectClass, target);
    if (!type)
        return NULL;
    if (!types::TypeScript::ThisTypes(target->nonLazyScript())->hasType(types::Type::ObjectType(type)))
        return NULL;

    RootedObject templateObject(cx, CreateThisForFunctionWithProto(cx, target, proto));
    if (!templateObject)
        return NULL;

    // Trigger recompilation if the templateObject changes.
    if (templateObject->type()->newScript)
        types::HeapTypeSet::WatchObjectStateChange(cx, templateObject->type());

    MCreateThisWithTemplate *createThis = MCreateThisWithTemplate::New(templateObject);
    current->add(createThis);

    return createThis;
}

MDefinition *
IonBuilder::createThis(HandleFunction target, MDefinition *callee)
{
    // Create this for unknown target
    if (!target) {
        MCreateThis *createThis = MCreateThis::New(callee);
        current->add(createThis);
        return createThis;
    }

    // Native constructors build the new Object themselves.
    if (target->isNative()) {
        if (!target->isNativeConstructor())
            return NULL;

        MConstant *magic = MConstant::New(MagicValue(JS_IS_CONSTRUCTING));
        current->add(magic);
        return magic;
    }

    // Try baking in the prototype.
    MDefinition *createThis = createThisScriptedSingleton(target, callee);
    if (createThis)
        return createThis;

    return createThisScripted(callee);
}

bool
IonBuilder::anyFunctionIsCloneAtCallsite(types::StackTypeSet *funTypes)
{
    uint32_t count = funTypes->getObjectCount();
    if (count < 1)
        return false;

    for (uint32_t i = 0; i < count; i++) {
        JSObject *obj = funTypes->getSingleObject(i);
        if (obj->isFunction() && obj->toFunction()->isInterpreted() &&
            obj->toFunction()->nonLazyScript()->shouldCloneAtCallsite)
        {
            return true;
        }
    }
    return false;
}

bool
IonBuilder::jsop_funcall(uint32_t argc)
{
    // Stack for JSOP_FUNCALL:
    // 1:      MPassArg(arg0)
    // ...
    // argc:   MPassArg(argN)
    // argc+1: MPassArg(JSFunction *), the 'f' in |f.call()|, in |this| position.
    // argc+2: The native 'call' function.

    // If |Function.prototype.call| may be overridden, don't optimize callsite.
    types::StackTypeSet *calleeTypes = oracle->getCallTarget(script(), argc, pc);
    RootedFunction native(cx, getSingleCallTarget(calleeTypes));
    if (!native || !native->isNative() || native->native() != &js_fun_call) {
        CallInfo callInfo(cx, false);
        if (!callInfo.init(current, argc))
            return false;
        return makeCall(native, callInfo, calleeTypes, false);
    }

    // Extract call target.
    types::StackTypeSet *funTypes = oracle->getCallArg(script(), argc, 0, pc);
    RootedObject funobj(cx, (funTypes) ? funTypes->getSingleton() : NULL);
    RootedFunction target(cx, (funobj && funobj->isFunction()) ? funobj->toFunction() : NULL);

    // Unwrap the (JSFunction *) parameter.
    int funcDepth = -((int)argc + 1);
    MPassArg *passFunc = current->peek(funcDepth)->toPassArg();
    current->rewriteAtDepth(funcDepth, passFunc->getArgument());

    // Remove the MPassArg(JSFunction *).
    passFunc->replaceAllUsesWith(passFunc->getArgument());
    passFunc->block()->discard(passFunc);

    // Shimmy the slots down to remove the native 'call' function.
    current->shimmySlots(funcDepth - 1);

    // If no |this| argument was provided, explicitly pass Undefined.
    // Pushing is safe here, since one stack slot has been removed.
    if (argc == 0) {
        MConstant *undef = MConstant::New(UndefinedValue());
        current->add(undef);
        MPassArg *pass = MPassArg::New(undef);
        current->add(pass);
        current->push(pass);
    } else {
        // |this| becomes implicit in the call.
        argc -= 1;
    }

    // Call without inlining.
    CallInfo callInfo(cx, false);
    if (!callInfo.init(current, argc))
        return false;
    return makeCall(target, callInfo, funTypes, false);
}

bool
IonBuilder::jsop_funapply(uint32_t argc)
{
    types::StackTypeSet *calleeTypes = oracle->getCallTarget(script(), argc, pc);
    RootedFunction native(cx, getSingleCallTarget(calleeTypes));
    if (argc != 2) {
        CallInfo callInfo(cx, false);
        if (!callInfo.init(current, argc))
            return false;
        return makeCall(native, callInfo, calleeTypes, false);
    }

    // Disable compilation if the second argument to |apply| cannot be guaranteed
    // to be either definitely |arguments| or definitely not |arguments|.
    types::StackTypeSet *argObjTypes = oracle->getCallArg(script(), argc, 2, pc);
    LazyArgumentsType isArgObj = oracle->isArgumentObject(argObjTypes);
    if (isArgObj == MaybeArguments)
        return abort("fun.apply with MaybeArguments");

    // Fallback to regular call if arg 2 is not definitely |arguments|.
    if (isArgObj != DefinitelyArguments) {
        CallInfo callInfo(cx, false);
        if (!callInfo.init(current, argc))
            return false;
        return makeCall(native, callInfo, calleeTypes, false);
    }

    if (!native ||
        !native->isNative() ||
        native->native() != js_fun_apply)
    {
        return abort("fun.apply speculation failed");
    }

    // Use funapply that definitely uses |arguments|
    return jsop_funapplyarguments(argc);
}

bool
IonBuilder::jsop_funapplyarguments(uint32_t argc)
{
    // Stack for JSOP_FUNAPPLY:
    // 1:      MPassArg(Vp)
    // 2:      MPassArg(This)
    // argc+1: MPassArg(JSFunction *), the 'f' in |f.call()|, in |this| position.
    // argc+2: The native 'apply' function.

    // Extract call target.
    types::StackTypeSet *funTypes = oracle->getCallArg(script(), argc, 0, pc);
    RootedObject funobj(cx, (funTypes) ? funTypes->getSingleton() : NULL);
    RootedFunction target(cx, (funobj && funobj->isFunction()) ? funobj->toFunction() : NULL);

    // When this script isn't inlined, use MApplyArgs,
    // to copy the arguments from the stack and call the function
    if (inliningDepth_ == 0) {

        // Vp
        MPassArg *passVp = current->pop()->toPassArg();
        passVp->replaceAllUsesWith(passVp->getArgument());
        passVp->block()->discard(passVp);

        // This
        MPassArg *passThis = current->pop()->toPassArg();
        MDefinition *argThis = passThis->getArgument();
        passThis->replaceAllUsesWith(argThis);
        passThis->block()->discard(passThis);

        // Unwrap the (JSFunction *) parameter.
        MPassArg *passFunc = current->pop()->toPassArg();
        MDefinition *argFunc = passFunc->getArgument();
        passFunc->replaceAllUsesWith(argFunc);
        passFunc->block()->discard(passFunc);

        // Pop apply function.
        current->pop();

        MArgumentsLength *numArgs = MArgumentsLength::New();
        current->add(numArgs);

        MApplyArgs *apply = MApplyArgs::New(target, argFunc, numArgs, argThis);
        current->add(apply);
        current->push(apply);
        if (!resumeAfter(apply))
            return false;

        types::StackTypeSet *barrier;
        types::StackTypeSet *types = oracle->returnTypeSet(script(), pc, &barrier);
        return pushTypeBarrier(apply, types, barrier);
    }

    // When inlining we have the arguments the function gets called with
    // and can optimize even more, by just calling the functions with the args.
    JS_ASSERT(inliningDepth_ > 0);

    CallInfo callInfo(cx, false);

    // Vp
    MPassArg *passVp = current->pop()->toPassArg();
    passVp->replaceAllUsesWith(passVp->getArgument());
    passVp->block()->discard(passVp);

    // Arguments
    Vector<MDefinition *> args(cx);
    if (!args.append(inlinedArguments_.begin(), inlinedArguments_.end()))
        return false;
    callInfo.setArgs(&args);
    RootedScript scriptRoot(cx, script());
    RootedScript parentScriptRoot(cx, callerBuilder_->script());
    Vector<types::StackTypeSet *> argTypes(cx);
    if (!argTypes.append(inlinedArgumentTypes_.begin(), inlinedArgumentTypes_.end()))
        return false;
    if (!callInfo.initFunApplyArguments(oracle, scriptRoot, pc, &argTypes))
        return false;

    // This
    MPassArg *passThis = current->pop()->toPassArg();
    MDefinition *argThis = passThis->getArgument();
    passThis->replaceAllUsesWith(argThis);
    passThis->block()->discard(passThis);
    callInfo.setThis(argThis);

    // Unwrap the (JSFunction *) parameter.
    MPassArg *passFunc = current->pop()->toPassArg();
    MDefinition *argFunc = passFunc->getArgument();
    passFunc->replaceAllUsesWith(argFunc);
    passFunc->block()->discard(passFunc);

    callInfo.setFun(argFunc);

    // Pop apply function.
    current->pop();

    // Set type information
    types::StackTypeSet *barrier;
    types::StackTypeSet *types = oracle->returnTypeSet(script(), pc, &barrier);
    callInfo.setTypeInfo(types, barrier);

    // Try inlining call
    if (makeInliningDecision(target, callInfo) && target->isInterpreted())
        return inlineScriptedCall(callInfo, target);

    callInfo.wrapArgs(current);
    return makeCallBarrier(target, callInfo, funTypes, false);
}

bool
IonBuilder::jsop_call(uint32_t argc, bool constructing)
{
    // Acquire known call target if existent.
    AutoObjectVector originals(cx);
    types::StackTypeSet *calleeTypes = oracle->getCallTarget(script(), argc, pc);
    if (calleeTypes) {
        if (!getPolyCallTargets(calleeTypes, originals, 4))
            return false;
    }

    // If any call targets need to be cloned, clone them. Keep track of the
    // originals as we need to case on them for poly inline.
    bool hasClones = false;
    AutoObjectVector targets(cx);
    RootedFunction fun(cx);
    RootedScript scriptRoot(cx, script());
    for (uint32_t i = 0; i < originals.length(); i++) {
        fun = originals[i]->toFunction();
        if (fun->isInterpreted() && fun->nonLazyScript()->shouldCloneAtCallsite) {
            fun = CloneFunctionAtCallsite(cx, fun, scriptRoot, pc);
            if (!fun)
                return false;
            hasClones = true;
        }
        if (!targets.append(fun))
            return false;
    }

    CallInfo callInfo(cx, constructing);
    if (!callInfo.init(current, argc))
        return false;

    types::StackTypeSet *barrier;
    types::StackTypeSet *types = oracle->returnTypeSet(script(), pc, &barrier);
    callInfo.setTypeInfo(types, barrier);
    if (!callInfo.initCallType(oracle, scriptRoot, pc))
        return false;

    // Try inlining
    InliningStatus status = inlineCallsite(targets, originals, callInfo);
    if (status == InliningStatus_Inlined)
        return true;
    if (status == InliningStatus_Error)
        return false;

    // No inline, just make the call.
    RootedFunction target(cx, NULL);
    if (targets.length() == 1)
        target = targets[0]->toFunction();

    return makeCallBarrier(target, callInfo, calleeTypes, hasClones);
}

MDefinition *
IonBuilder::makeCallsiteClone(HandleFunction target, MDefinition *fun)
{
    // Bake in the clone eagerly if we have a known target. We have arrived here
    // because TI told us that the known target is a should-clone-at-callsite
    // function, which means that target already is the clone.
    if (target) {
        MConstant *constant = MConstant::New(ObjectValue(*target));
        current->add(constant);
        return constant;
    }

    // Add a callsite clone IC if we have multiple targets. Note that we
    // should have checked already that at least some targets are marked as
    // should-clone-at-callsite.
    MCallsiteCloneCache *clone = MCallsiteCloneCache::New(fun, pc);
    current->add(clone);
    return clone;
}

static bool
TestShouldDOMCall(JSContext *cx, types::TypeSet *inTypes, HandleFunction func,
                  JSJitInfo::OpType opType)
{
    if (!func->isNative() || !func->jitInfo())
        return false;
    // If all the DOM objects flowing through are legal with this
    // property, we can bake in a call to the bottom half of the DOM
    // accessor
    DOMInstanceClassMatchesProto instanceChecker =
        GetDOMCallbacks(cx->runtime)->instanceClassMatchesProto;

    const JSJitInfo *jinfo = func->jitInfo();
    if (jinfo->type != opType)
        return false;

    for (unsigned i = 0; i < inTypes->getObjectCount(); i++) {
        types::TypeObject *curType = inTypes->getTypeObject(i);

        if (!curType) {
            JSObject *curObj = inTypes->getSingleObject(i);

            if (!curObj)
                continue;

            curType = curObj->getType(cx);
            if (!curType)
                return false;
        }

        JSObject *typeProto = curType->proto;
        RootedObject proto(cx, typeProto);
        if (!instanceChecker(proto, jinfo->protoID, jinfo->depth))
            return false;
    }

    return true;
}

static bool
TestAreKnownDOMTypes(JSContext *cx, types::TypeSet *inTypes)
{
    if (inTypes->unknownObject())
        return false;

    // First iterate to make sure they all are DOM objects, then freeze all of
    // them as such if they are.
    for (unsigned i = 0; i < inTypes->getObjectCount(); i++) {
        types::TypeObject *curType = inTypes->getTypeObject(i);

        if (!curType) {
            JSObject *curObj = inTypes->getSingleObject(i);

            // Skip holes in TypeSets.
            if (!curObj)
                continue;

            curType = curObj->getType(cx);
            if (!curType)
                return false;
        }

        if (curType->unknownProperties())
            return false;

        if (!(curType->clasp->flags & JSCLASS_IS_DOMJSCLASS))
            return false;
    }

    // If we didn't check anything, no reason to say yes.
    if (inTypes->getObjectCount() > 0)
        return true;

    return false;
}

MCall *
IonBuilder::makeCallHelper(HandleFunction target, CallInfo &callInfo,
                           types::StackTypeSet *calleeTypes, bool cloneAtCallsite)
{
    JS_ASSERT(callInfo.isWrapped());

    // This function may be called with mutated stack.
    // Querying TI for popped types is invalid.

    uint32_t targetArgs = callInfo.argc();

    // Collect number of missing arguments provided that the target is
    // scripted. Native functions are passed an explicit 'argc' parameter.
    if (target && !target->isNative())
        targetArgs = Max<uint32_t>(target->nargs, callInfo.argc());

    MCall *call =
        MCall::New(target, targetArgs + 1, callInfo.argc(), callInfo.constructing(), calleeTypes);
    if (!call)
        return NULL;

    // Save the script for inspection by visitCallKnown().
    if (target && target->isInterpreted()) {
        if (!target->getOrCreateScript(cx))
            return NULL;
        call->rootTargetScript(target);
    }

    // Explicitly pad any missing arguments with |undefined|.
    // This permits skipping the argumentsRectifier.
    for (int i = targetArgs; i > (int)callInfo.argc(); i--) {
        JS_ASSERT_IF(target, !target->isNative());
        MConstant *undef = MConstant::New(UndefinedValue());
        current->add(undef);
        MPassArg *pass = MPassArg::New(undef);
        current->add(pass);
        call->addArg(i, pass);
    }

    // Add explicit arguments.
    // Skip addArg(0) because it is reserved for this
    for (int32_t i = callInfo.argc() - 1; i >= 0; i--) {
        JS_ASSERT(callInfo.getArg(i)->isPassArg());
        call->addArg(i + 1, callInfo.getArg(i)->toPassArg());
    }

    // Place an MPrepareCall before the first passed argument, before we
    // potentially perform rearrangement.
    JS_ASSERT(callInfo.thisArg()->isPassArg());
    MPassArg *thisArg = callInfo.thisArg()->toPassArg();
    MPrepareCall *start = new MPrepareCall;
    thisArg->block()->insertBefore(thisArg, start);
    call->initPrepareCall(start);

    // Inline the constructor on the caller-side.
    if (callInfo.constructing()) {
        MDefinition *create = createThis(target, callInfo.fun());
        if (!create) {
            abort("Failure inlining constructor for call.");
            return NULL;
        }

        // Unwrap the MPassArg before discarding: it may have been captured by an MResumePoint.
        thisArg->replaceAllUsesWith(thisArg->getArgument());
        thisArg->block()->discard(thisArg);

        MPassArg *newThis = MPassArg::New(create);
        current->add(newThis);

        thisArg = newThis;
    }

    // Pass |this| and function.
    call->addArg(0, thisArg);

    // Add a callsite clone IC for multiple targets which all should be
    // callsite cloned, or bake in the clone for a single target.
    if (cloneAtCallsite) {
        MDefinition *fun = makeCallsiteClone(target, callInfo.fun());
        callInfo.setFun(fun);
    }

    if (target && JSOp(*pc) == JSOP_CALL) {
        // We know we have a single call target.  Check whether the "this" types
        // are DOM types and our function a DOM function, and if so flag the
        // MCall accordingly.
        types::StackTypeSet *thisTypes = oracle->getCallArg(script(), callInfo.argc(), 0, pc);
        if (thisTypes &&
            TestAreKnownDOMTypes(cx, thisTypes) &&
            TestShouldDOMCall(cx, thisTypes, target, JSJitInfo::Method))
        {
            call->setDOMFunction();
        }
    }

    call->initFunction(callInfo.fun());

    current->add(call);
    return call;
}

static types::StackTypeSet*
AdjustTypeBarrierForDOMCall(const JSJitInfo* jitinfo, types::StackTypeSet *types,
                            types::StackTypeSet *barrier)
{
    // If the return type of our DOM native is in "types" already, we don't
    // actually need a barrier.
    if (jitinfo->returnType == JSVAL_TYPE_UNKNOWN)
        return barrier;

    // JSVAL_TYPE_OBJECT doesn't tell us much; we still have to barrier on the
    // actual type of the object.
    if (jitinfo->returnType == JSVAL_TYPE_OBJECT)
        return barrier;

    if (jitinfo->returnType != types->getKnownTypeTag())
        return barrier;

    // No need for a barrier if we're already expecting the type we'll produce.
    return NULL;
}

bool
IonBuilder::makeCallBarrier(HandleFunction target, CallInfo &callInfo,
                            types::StackTypeSet *calleeTypes, bool cloneAtCallsite)
{
    JS_ASSERT(callInfo.hasTypeInfo());

    MCall *call = makeCallHelper(target, callInfo, calleeTypes, cloneAtCallsite);
    if (!call)
        return false;

    current->push(call);
    if (!resumeAfter(call))
        return false;

    types::StackTypeSet *barrier = callInfo.barrier();
    if (call->isDOMFunction()) {
        JSFunction* target = call->getSingleTarget();
        JS_ASSERT(target && target->isNative() && target->jitInfo());
        barrier = AdjustTypeBarrierForDOMCall(target->jitInfo(), callInfo.types(), barrier);
    }

    return pushTypeBarrier(call, callInfo.types(), barrier);
}

bool
IonBuilder::makeCall(HandleFunction target, CallInfo &callInfo,
                     types::StackTypeSet *calleeTypes, bool cloneAtCallsite)
{
    JS_ASSERT(!callInfo.hasTypeInfo());

    types::StackTypeSet *barrier;
    types::StackTypeSet *types = oracle->returnTypeSet(script(), pc, &barrier);
    callInfo.setTypeInfo(types, barrier);

    return makeCallBarrier(target, callInfo, calleeTypes, cloneAtCallsite);
}

bool
IonBuilder::jsop_eval(uint32_t argc)
{
    types::StackTypeSet *calleeTypes = oracle->getCallTarget(script(), argc, pc);

    // Emit a normal call if the eval has never executed. This keeps us from
    // disabling compilation for the script when testing with --ion-eager.
    if (calleeTypes && calleeTypes->empty())
        return jsop_call(argc, /* constructing = */ false);

    RootedFunction singleton(cx, getSingleCallTarget(calleeTypes));
    if (!singleton)
        return abort("No singleton callee for eval()");

    if (IsBuiltinEvalForScope(&script()->global(), ObjectValue(*singleton))) {
        if (argc != 1)
            return abort("Direct eval with more than one argument");

        if (!info().fun())
            return abort("Direct eval in global code");

        types::StackTypeSet *thisTypes = oracle->thisTypeSet(script());

        // The 'this' value for the outer and eval scripts must be the
        // same. This is not guaranteed if a primitive string/number/etc.
        // is passed through to the eval invoke as the primitive may be
        // boxed into different objects if accessed via 'this'.
        JSValueType type = thisTypes->getKnownTypeTag();
        if (type != JSVAL_TYPE_OBJECT && type != JSVAL_TYPE_NULL && type != JSVAL_TYPE_UNDEFINED)
            return abort("Direct eval from script with maybe-primitive 'this'");

        CallInfo callInfo(cx, /* constructing = */ false);
        if (!callInfo.init(current, argc))
            return false;
        callInfo.unwrapArgs();

        MDefinition *scopeChain = current->scopeChain();
        MDefinition *string = callInfo.getArg(0);

        current->pushSlot(info().thisSlot());
        MDefinition *thisValue = current->pop();

        // Try to pattern match 'eval(v + "()")'. In this case v is likely a
        // name on the scope chain and the eval is performing a call on that
        // value. Use a dynamic scope chain lookup rather than a full eval.
        if (string->isConcat() &&
            string->getOperand(1)->isConstant() &&
            string->getOperand(1)->toConstant()->value().isString())
        {
            JSString *str = string->getOperand(1)->toConstant()->value().toString();

            JSBool match;
            if (!JS_StringEqualsAscii(cx, str, "()", &match))
                return false;
            if (match) {
                MDefinition *name = string->getOperand(0);
                MInstruction *dynamicName = MGetDynamicName::New(scopeChain, name);
                current->add(dynamicName);

                MInstruction *thisv = MPassArg::New(thisValue);
                current->add(thisv);

                current->push(dynamicName);
                current->push(thisv);

                CallInfo evalCallInfo(cx, /* constructing = */ false);
                if (!evalCallInfo.init(current, /* argc = */ 0))
                    return false;

                return makeCall(NullPtr(), evalCallInfo, NULL, false);
            }
        }

        MInstruction *filterArguments = MFilterArguments::New(string);
        current->add(filterArguments);

        MInstruction *ins = MCallDirectEval::New(scopeChain, string, thisValue, pc);
        current->add(ins);
        current->push(ins);

        types::StackTypeSet *barrier;
        types::StackTypeSet *types = oracle->returnTypeSet(script(), pc, &barrier);
        return resumeAfter(ins) && pushTypeBarrier(ins, types, barrier);
    }

    return jsop_call(argc, /* constructing = */ false);
}

bool
IonBuilder::jsop_compare(JSOp op)
{
    MDefinition *right = current->pop();
    MDefinition *left = current->pop();

    MCompare *ins = MCompare::New(left, right, op);
    current->add(ins);
    current->push(ins);

    TypeOracle::BinaryTypes b = oracle->binaryTypes(script(), pc);
    ins->infer(b, cx);

    if (ins->isEffectful() && !resumeAfter(ins))
        return false;
    return true;
}

JSObject *
IonBuilder::getNewArrayTemplateObject(uint32_t count)
{
    RootedScript scriptRoot(cx, script());
    NewObjectKind newKind = types::UseNewTypeForInitializer(cx, scriptRoot, pc, JSProto_Array);
    RootedObject templateObject(cx, NewDenseUnallocatedArray(cx, count, NULL, newKind));
    if (!templateObject)
        return NULL;

    if (newKind != SingletonObject) {
        types::TypeObject *type = types::TypeScript::InitObject(cx, scriptRoot, pc, JSProto_Array);
        if (!type)
            return NULL;
        templateObject->setType(type);
    }

    return templateObject;
}

bool
IonBuilder::jsop_newarray(uint32_t count)
{
    JS_ASSERT(script()->compileAndGo);

    JSObject *templateObject = getNewArrayTemplateObject(count);
    if (!templateObject)
        return false;

    if (oracle->arrayResultShouldHaveDoubleConversion(script(), pc))
        templateObject->setShouldConvertDoubleElements();

    MNewArray *ins = new MNewArray(count, templateObject, MNewArray::NewArray_Allocating);

    current->add(ins);
    current->push(ins);

    return true;
}

bool
IonBuilder::jsop_newobject(HandleObject baseObj)
{
    // Don't bake in the TypeObject for non-CNG scripts.
    JS_ASSERT(script()->compileAndGo);

    RootedObject templateObject(cx);

    RootedScript scriptRoot(cx, script());
    NewObjectKind newKind = types::UseNewTypeForInitializer(cx, scriptRoot, pc, JSProto_Object);
    if (baseObj) {
        templateObject = CopyInitializerObject(cx, baseObj, newKind);
    } else {
        gc::AllocKind allocKind = GuessObjectGCKind(0);
        templateObject = NewBuiltinClassInstance(cx, &ObjectClass, allocKind, newKind);
    }

    if (!templateObject)
        return false;

    if (newKind != SingletonObject) {
        types::TypeObject *type = types::TypeScript::InitObject(cx, scriptRoot, pc, JSProto_Object);
        if (!type)
            return false;
        templateObject->setType(type);
    }

    MNewObject *ins = MNewObject::New(templateObject);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_initelem_array()
{
    MDefinition *value = current->pop();
    MDefinition *obj = current->peek(-1);

    MConstant *id = MConstant::New(Int32Value(GET_UINT24(pc)));
    current->add(id);

    // Get the elements vector.
    MElements *elements = MElements::New(obj);
    current->add(elements);

    if (obj->toNewArray()->templateObject()->shouldConvertDoubleElements()) {
        MInstruction *valueDouble = MToDouble::New(value);
        current->add(valueDouble);
        value = valueDouble;
    }

    // Store the value.
    MStoreElement *store = MStoreElement::New(elements, id, value, /* needsHoleCheck = */ false);
    current->add(store);

    // Update the length.
    MSetInitializedLength *initLength = MSetInitializedLength::New(elements, id);
    current->add(initLength);

    if (!resumeAfter(initLength))
        return false;

   return true;
}

static bool
CanEffectlesslyCallLookupGenericOnObject(JSObject *obj)
{
    while (obj) {
        if (!obj->isNative())
            return false;
        if (obj->getClass()->ops.lookupProperty)
            return false;
        obj = obj->getProto();
    }
    return true;
}

bool
IonBuilder::jsop_initprop(HandlePropertyName name)
{
    MDefinition *value = current->pop();
    MDefinition *obj = current->peek(-1);

    RootedObject templateObject(cx, obj->toNewObject()->templateObject());

    if (!oracle->propertyWriteCanSpecialize(script(), pc)) {
        // This should only happen for a few names like __proto__.
        return abort("INITPROP Monitored initprop");
    }

    if (!CanEffectlesslyCallLookupGenericOnObject(templateObject))
        return abort("INITPROP template object is special");

    RootedObject holder(cx);
    RootedShape shape(cx);
    RootedId id(cx, NameToId(name));
    bool res = LookupPropertyWithFlags(cx, templateObject, id,
                                       0, &holder, &shape);
    if (!res)
        return false;

    if (!shape || holder != templateObject) {
        // JSOP_NEWINIT becomes an MNewObject without preconfigured properties.
        MInitProp *init = MInitProp::New(obj, name, value);
        current->add(init);
        return resumeAfter(init);
    }

    bool needsBarrier = true;
    TypeOracle::BinaryTypes b = oracle->binaryTypes(script(), pc);
    if (b.lhsTypes &&
        (id == types::IdToTypeId(id)) &&
        !b.lhsTypes->propertyNeedsBarrier(cx, id))
    {
        needsBarrier = false;
    }

    // In parallel execution, we never require write barriers.  See
    // forkjoin.cpp for more information.
    switch (info().executionMode()) {
      case SequentialExecution:
        break;
      case ParallelExecution:
        needsBarrier = false;
        break;
    }

    if (templateObject->isFixedSlot(shape->slot())) {
        MStoreFixedSlot *store = MStoreFixedSlot::New(obj, shape->slot(), value);
        if (needsBarrier)
            store->setNeedsBarrier();

        current->add(store);
        return resumeAfter(store);
    }

    MSlots *slots = MSlots::New(obj);
    current->add(slots);

    uint32_t slot = templateObject->dynamicSlotIndex(shape->slot());
    MStoreSlot *store = MStoreSlot::New(slots, slot, value);
    if (needsBarrier)
        store->setNeedsBarrier();

    current->add(store);
    return resumeAfter(store);
}

MBasicBlock *
IonBuilder::addBlock(MBasicBlock *block, uint32_t loopDepth)
{
    if (!block)
        return NULL;
    graph().addBlock(block);
    block->setLoopDepth(loopDepth);
    return block;
}

MBasicBlock *
IonBuilder::newBlock(MBasicBlock *predecessor, jsbytecode *pc)
{
    MBasicBlock *block = MBasicBlock::New(graph(), info(), predecessor, pc, MBasicBlock::NORMAL);
    return addBlock(block, loopDepth_);
}

MBasicBlock *
IonBuilder::newBlock(MBasicBlock *predecessor, jsbytecode *pc, MResumePoint *priorResumePoint)
{
    MBasicBlock *block = MBasicBlock::NewWithResumePoint(graph(), info(), predecessor, pc,
                                                         priorResumePoint);
    return addBlock(block, loopDepth_);
}

MBasicBlock *
IonBuilder::newBlockPopN(MBasicBlock *predecessor, jsbytecode *pc, uint32_t popped)
{
    MBasicBlock *block = MBasicBlock::NewPopN(graph(), info(), predecessor, pc, MBasicBlock::NORMAL, popped);
    return addBlock(block, loopDepth_);
}

MBasicBlock *
IonBuilder::newBlockAfter(MBasicBlock *at, MBasicBlock *predecessor, jsbytecode *pc)
{
    MBasicBlock *block = MBasicBlock::New(graph(), info(), predecessor, pc, MBasicBlock::NORMAL);
    if (!block)
        return NULL;
    graph().insertBlockAfter(at, block);
    return block;
}

MBasicBlock *
IonBuilder::newBlock(MBasicBlock *predecessor, jsbytecode *pc, uint32_t loopDepth)
{
    MBasicBlock *block = MBasicBlock::New(graph(), info(), predecessor, pc, MBasicBlock::NORMAL);
    return addBlock(block, loopDepth);
}

MBasicBlock *
IonBuilder::newOsrPreheader(MBasicBlock *predecessor, jsbytecode *loopEntry)
{
    JS_ASSERT((JSOp)*loopEntry == JSOP_LOOPENTRY);
    JS_ASSERT(loopEntry == info().osrPc());

    // Create two blocks: one for the OSR entry with no predecessors, one for
    // the preheader, which has the OSR entry block as a predecessor. The
    // OSR block is always the second block (with id 1).
    MBasicBlock *osrBlock  = newBlockAfter(*graph().begin(), loopEntry);
    MBasicBlock *preheader = newBlock(predecessor, loopEntry);
    if (!osrBlock || !preheader)
        return NULL;

    MOsrEntry *entry = MOsrEntry::New();
    osrBlock->add(entry);

    // Initialize |scopeChain|.
    {
        uint32_t slot = info().scopeChainSlot();

        MOsrScopeChain *scopev = MOsrScopeChain::New(entry);
        osrBlock->add(scopev);
        osrBlock->initSlot(slot, scopev);
    }

    if (info().fun()) {
        // Initialize |this| parameter.
        uint32_t slot = info().thisSlot();
        ptrdiff_t offset = StackFrame::offsetOfThis(info().fun());

        MOsrValue *thisv = MOsrValue::New(entry, offset);
        osrBlock->add(thisv);
        osrBlock->initSlot(slot, thisv);

        // Initialize arguments.
        for (uint32_t i = 0; i < info().nargs(); i++) {
            uint32_t slot = info().argSlot(i);
            ptrdiff_t offset = StackFrame::offsetOfFormalArg(info().fun(), i);

            MOsrValue *osrv = MOsrValue::New(entry, offset);
            osrBlock->add(osrv);
            osrBlock->initSlot(slot, osrv);
        }
    }

    // Initialize locals.
    for (uint32_t i = 0; i < info().nlocals(); i++) {
        uint32_t slot = info().localSlot(i);
        ptrdiff_t offset = StackFrame::offsetOfFixed(i);

        MOsrValue *osrv = MOsrValue::New(entry, offset);
        osrBlock->add(osrv);
        osrBlock->initSlot(slot, osrv);
    }

    // Initialize stack.
    uint32_t numSlots = preheader->stackDepth() - CountArgSlots(info().fun()) - info().nlocals();
    for (uint32_t i = 0; i < numSlots; i++) {
        uint32_t slot = info().stackSlot(i);
        ptrdiff_t offset = StackFrame::offsetOfFixed(info().nlocals() + i);

        MOsrValue *osrv = MOsrValue::New(entry, offset);
        osrBlock->add(osrv);
        osrBlock->initSlot(slot, osrv);
    }

    // Create an MStart to hold the first valid MResumePoint.
    MStart *start = MStart::New(MStart::StartType_Osr);
    osrBlock->add(start);
    graph().setOsrStart(start);

    // MOsrValue instructions are infallible, so the first MResumePoint must
    // occur after they execute, at the point of the MStart.
    if (!resumeAt(start, loopEntry))
        return NULL;

    // Link the same MResumePoint from the MStart to each MOsrValue.
    // This causes logic in ShouldSpecializeInput() to not replace Uses with
    // Unboxes in the MResumePiont, so that the MStart always sees Values.
    osrBlock->linkOsrValues(start);

    // Clone types of the other predecessor of the pre-header to the osr block,
    // such as pre-header phi's won't discard specialized type of the
    // predecessor.
    JS_ASSERT(predecessor->stackDepth() == osrBlock->stackDepth());
    JS_ASSERT(info().scopeChainSlot() == 0);
    JS_ASSERT(osrBlock->scopeChain()->type() == MIRType_Object);

    Vector<MIRType> slotTypes(cx);
    if (!slotTypes.growByUninitialized(osrBlock->stackDepth()))
        return NULL;

    // Fill slotTypes with the types of the predecessor block.
    for (uint32_t i = 0; i < osrBlock->stackDepth(); i++)
        slotTypes[i] = MIRType_Value;

    // Update slotTypes for slots that may have a different type at this join point.
    if (!oracle->getOsrTypes(loopEntry, slotTypes))
        return NULL;

    for (uint32_t i = 1; i < osrBlock->stackDepth(); i++) {
        // Unbox the MOsrValue if it is known to be unboxable.
        switch (slotTypes[i]) {
          case MIRType_Boolean:
          case MIRType_Int32:
          case MIRType_Double:
          case MIRType_String:
          case MIRType_Object:
          {
            MDefinition *def = osrBlock->getSlot(i);
            JS_ASSERT(def->type() == MIRType_Value);

            MInstruction *actual = MUnbox::New(def, slotTypes[i], MUnbox::Infallible);
            osrBlock->add(actual);
            osrBlock->rewriteSlot(i, actual);
            break;
          }

          case MIRType_Null:
          {
            MConstant *c = MConstant::New(NullValue());
            osrBlock->add(c);
            osrBlock->rewriteSlot(i, c);
            break;
          }

          case MIRType_Undefined:
          {
            MConstant *c = MConstant::New(UndefinedValue());
            osrBlock->add(c);
            osrBlock->rewriteSlot(i, c);
            break;
          }

          case MIRType_Magic:
            JS_ASSERT(lazyArguments_);
            osrBlock->rewriteSlot(i, lazyArguments_);
            break;

          default:
            break;
        }
    }

    // Finish the osrBlock.
    osrBlock->end(MGoto::New(preheader));
    preheader->addPredecessor(osrBlock);
    graph().setOsrBlock(osrBlock);

    // Wrap |this| with a guaranteed use, to prevent instruction elimination.
    // Prevent |this| from being DCE'd: necessary for constructors.
    if (info().fun())
        preheader->getSlot(info().thisSlot())->setGuard();

    return preheader;
}

MBasicBlock *
IonBuilder::newPendingLoopHeader(MBasicBlock *predecessor, jsbytecode *pc)
{
    loopDepth_++;
    MBasicBlock *block = MBasicBlock::NewPendingLoopHeader(graph(), info(), predecessor, pc);
    return addBlock(block, loopDepth_);
}

// A resume point is a mapping of stack slots to MDefinitions. It is used to
// capture the environment such that if a guard fails, and IonMonkey needs
// to exit back to the interpreter, the interpreter state can be
// reconstructed.
//
// We capture stack state at critical points:
//   * (1) At the beginning of every basic block.
//   * (2) After every effectful operation.
//
// As long as these two properties are maintained, instructions can
// be moved, hoisted, or, eliminated without problems, and ops without side
// effects do not need to worry about capturing state at precisely the
// right point in time.
//
// Effectful instructions, of course, need to capture state after completion,
// where the interpreter will not attempt to repeat the operation. For this,
// ResumeAfter must be used. The state is attached directly to the effectful
// instruction to ensure that no intermediate instructions could be injected
// in between by a future analysis pass.
//
// During LIR construction, if an instruction can bail back to the interpreter,
// we create an LSnapshot, which uses the last known resume point to request
// register/stack assignments for every live value.
bool
IonBuilder::resume(MInstruction *ins, jsbytecode *pc, MResumePoint::Mode mode)
{
    JS_ASSERT(ins->isEffectful() || !ins->isMovable());

    MResumePoint *resumePoint = MResumePoint::New(ins->block(), pc, callerResumePoint_, mode);
    if (!resumePoint)
        return false;
    ins->setResumePoint(resumePoint);
    resumePoint->setInstruction(ins);
    return true;
}

bool
IonBuilder::resumeAt(MInstruction *ins, jsbytecode *pc)
{
    return resume(ins, pc, MResumePoint::ResumeAt);
}

bool
IonBuilder::resumeAfter(MInstruction *ins)
{
    return resume(ins, pc, MResumePoint::ResumeAfter);
}

bool
IonBuilder::maybeInsertResume()
{
    // Create a resume point at the current position, without an existing
    // effectful instruction. This resume point is not necessary for correct
    // behavior (see above), but is added to avoid holding any values from the
    // previous resume point which are now dead. This shortens the live ranges
    // of such values and improves register allocation.
    //
    // This optimization is not performed outside of loop bodies, where good
    // register allocation is not as critical, in order to avoid creating
    // excessive resume points.

    if (loopDepth_ == 0)
        return true;

    MNop *ins = MNop::New();
    current->add(ins);

    return resumeAfter(ins);
}

static inline bool
TestSingletonProperty(JSContext *cx, HandleObject obj, HandleId id, bool *isKnownConstant)
{
    // We would like to completely no-op property/global accesses which can
    // produce only a particular JSObject. When indicating the access result is
    // definitely an object, type inference does not account for the
    // possibility that the property is entirely missing from the input object
    // and its prototypes (if this happens, a semantic trigger would be hit and
    // the pushed types updated, even if there is no type barrier).
    //
    // If the access definitely goes through obj, either directly or on the
    // prototype chain, then if obj has a defined property now, and the
    // property has a default or method shape, then the property is not missing
    // and the only way it can become missing in the future is if it is deleted.
    // Deletion causes type properties to be explicitly marked with undefined.

    *isKnownConstant = false;

    if (!CanEffectlesslyCallLookupGenericOnObject(obj))
        return true;

    RootedObject holder(cx);
    RootedShape shape(cx);
    if (!JSObject::lookupGeneric(cx, obj, id, &holder, &shape))
        return false;
    if (!shape)
        return true;

    if (!shape->hasDefaultGetter())
        return true;
    if (!shape->hasSlot())
        return true;
    if (holder->getSlot(shape->slot()).isUndefined())
        return true;

    *isKnownConstant = true;
    return true;
}

static inline bool
TestSingletonPropertyTypes(JSContext *cx, types::StackTypeSet *types,
                           HandleObject globalObj, HandleId id,
                           bool *isKnownConstant, bool *testObject,
                           bool *testString)
{
    // As for TestSingletonProperty, but the input is any value in a type set
    // rather than a specific object. If testObject is set then the constant
    // result can only be used after ensuring the input is an object.

    *isKnownConstant = false;
    *testObject = false;
    *testString = false;

    if (!types || types->unknownObject())
        return true;

    RootedObject singleton(cx, types->getSingleton());
    if (singleton)
        return TestSingletonProperty(cx, singleton, id, isKnownConstant);

    if (!globalObj)
        return true;

    JSProtoKey key;
    JSValueType type = types->getKnownTypeTag();
    switch (type) {
      case JSVAL_TYPE_STRING:
        key = JSProto_String;
        break;

      case JSVAL_TYPE_INT32:
      case JSVAL_TYPE_DOUBLE:
        key = JSProto_Number;
        break;

      case JSVAL_TYPE_BOOLEAN:
        key = JSProto_Boolean;
        break;

      case JSVAL_TYPE_OBJECT:
      case JSVAL_TYPE_UNKNOWN: {
        if (types->hasType(types::Type::StringType())) {
            // Do not optimize if the object is either a String or an Object.
            if (types->maybeObject())
                return true;
            key = JSProto_String;
            *testString = true;
            break;
        }

        // For property accesses which may be on many objects, we just need to
        // find a prototype common to all the objects; if that prototype
        // has the singleton property, the access will not be on a missing property.
        bool thoughtConstant = true;
        for (unsigned i = 0; i < types->getObjectCount(); i++) {
            types::TypeObject *object = types->getTypeObject(i);
            if (!object) {
                // Try to get it through the singleton.
                JSObject *curObj = types->getSingleObject(i);
                // As per the comment in jsinfer.h, there can be holes in
                // TypeSets, so just skip over them.
                if (!curObj)
                    continue;
                object = curObj->getType(cx);
                if (!object)
                    return false;
            }

            if (object->proto) {
                // Test this type.
                RootedObject proto(cx, object->proto);
                if (!TestSingletonProperty(cx, proto, id, &thoughtConstant))
                    return false;
                // Short circuit
                if (!thoughtConstant)
                    break;
            } else {
                // Can't be on the prototype chain with no prototypes...
                thoughtConstant = false;
                break;
            }
        }
        if (thoughtConstant) {
            // If this is not a known object, a test will be needed.
            *testObject = (type != JSVAL_TYPE_OBJECT);
        }
        *isKnownConstant = thoughtConstant;
        return true;
      }
      default:
        return true;
    }

    RootedObject proto(cx);
    if (!js_GetClassPrototype(cx, key, &proto, NULL))
        return false;

    return TestSingletonProperty(cx, proto, id, isKnownConstant);
}

// Given an actual and observed type set, annotates the IR as much as possible:
// (1) If no type information is provided, the value on the top of the stack is
//     left in place.
// (2) If a single type definitely exists, and no type barrier is in place,
//     then an infallible unbox instruction replaces the value on the top of
//     the stack.
// (3) If a type barrier is in place, but has an unknown type set, leave the
//     value at the top of the stack.
// (4) If a type barrier is in place, and has a single type, an unbox
//     instruction replaces the top of the stack.
// (5) Lastly, a type barrier instruction replaces the top of the stack.
bool
IonBuilder::pushTypeBarrier(MInstruction *ins, types::StackTypeSet *actual,
                            types::StackTypeSet *observed)
{
    // If the instruction has no side effects, we'll resume the entire operation.
    // The actual type barrier will occur in the interpreter. If the
    // instruction is effectful, even if it has a singleton type, there
    // must be a resume point capturing the original def, and resuming
    // to that point will explicitly monitor the new type.

    if (!actual) {
        JS_ASSERT(!observed);
        return true;
    }

    if (!observed) {
        JSValueType type = actual->getKnownTypeTag();
        MInstruction *replace = NULL;
        switch (type) {
          case JSVAL_TYPE_UNDEFINED:
            ins->setFoldedUnchecked();
            replace = MConstant::New(UndefinedValue());
            break;
          case JSVAL_TYPE_NULL:
            ins->setFoldedUnchecked();
            replace = MConstant::New(NullValue());
            break;
          case JSVAL_TYPE_UNKNOWN:
            break;
          default: {
            MIRType replaceType = MIRTypeFromValueType(type);
            if (ins->type() == MIRType_Value)
                replace = MUnbox::New(ins, replaceType, MUnbox::Infallible);
            else
                JS_ASSERT(ins->type() == replaceType);
            break;
          }
        }
        if (replace) {
            current->pop();
            current->add(replace);
            current->push(replace);
            if (replace->acceptsTypeSet())
                replace->setTypeSet(cloneTypeSet(actual));
        } else {
            if (ins->acceptsTypeSet())
                ins->setTypeSet(cloneTypeSet(actual));
        }
        return true;
    }

    if (observed->unknown())
        return true;

    current->pop();

    MInstruction *barrier;
    JSValueType type = observed->getKnownTypeTag();

    // An unbox instruction isn't enough to capture JSVAL_TYPE_OBJECT. Use a type
    // barrier followed by an infallible unbox.
    bool isObject = false;
    if (type == JSVAL_TYPE_OBJECT && !observed->hasType(types::Type::AnyObjectType())) {
        type = JSVAL_TYPE_UNKNOWN;
        isObject = true;
    }

    switch (type) {
      case JSVAL_TYPE_UNKNOWN:
      case JSVAL_TYPE_UNDEFINED:
      case JSVAL_TYPE_NULL:
        barrier = MTypeBarrier::New(ins, cloneTypeSet(observed));
        current->add(barrier);

        if (type == JSVAL_TYPE_UNDEFINED)
            return pushConstant(UndefinedValue());
        if (type == JSVAL_TYPE_NULL)
            return pushConstant(NullValue());
        if (isObject) {
            barrier = MUnbox::New(barrier, MIRType_Object, MUnbox::Infallible);
            current->add(barrier);
        }
        break;
      default:
        MUnbox::Mode mode = ins->isEffectful() ? MUnbox::TypeBarrier : MUnbox::TypeGuard;
        barrier = MUnbox::New(ins, MIRTypeFromValueType(type), mode);
        current->add(barrier);
    }
    current->push(barrier);
    return true;
}

// Test the type of values returned by a VM call. This is an optimized version
// of calling TypeScript::Monitor inside such stubs.
void
IonBuilder::monitorResult(MInstruction *ins, types::TypeSet *barrier, types::StackTypeSet *types)
{
    // MonitorTypes is redundant if we will also add a type barrier.
    if (barrier)
        return;

    if (!types || types->unknown())
        return;

    MInstruction *monitor = MMonitorTypes::New(ins, cloneTypeSet(types));
    current->add(monitor);
}

bool
IonBuilder::jsop_getgname(HandlePropertyName name)
{
    // Optimize undefined, NaN, and Infinity.
    if (name == cx->names().undefined)
        return pushConstant(UndefinedValue());
    if (name == cx->names().NaN)
        return pushConstant(cx->runtime->NaNValue);
    if (name == cx->names().Infinity)
        return pushConstant(cx->runtime->positiveInfinityValue);

    RootedObject globalObj(cx, &script()->global());
    JS_ASSERT(globalObj->isNative());

    RootedId id(cx, NameToId(name));

    // For the fastest path, the property must be found, and it must be found
    // as a normal data property on exactly the global object.
    RootedShape shape(cx, globalObj->nativeLookup(cx, id));
    if (!shape || !shape->hasDefaultGetter() || !shape->hasSlot())
        return jsop_getname(name);

    types::HeapTypeSet *propertyTypes = oracle->globalPropertyTypeSet(script(), pc, id);
    types::TypeObject *globalType = globalObj->getType(cx);
    if (!globalType)
        return false;
    if (propertyTypes && propertyTypes->isOwnProperty(cx, globalType, true)) {
        // The property has been reconfigured as non-configurable, non-enumerable
        // or non-writable.
        return jsop_getname(name);
    }

    // If the property is permanent, a shape guard isn't necessary.
    JSValueType knownType = JSVAL_TYPE_UNKNOWN;

    RootedScript scriptRoot(cx, script());
    types::StackTypeSet *barrier = oracle->propertyReadBarrier(scriptRoot, pc);
    types::StackTypeSet *types = oracle->propertyRead(script(), pc);
    if (types) {
        JSObject *singleton = types->getSingleton();

        knownType = types->getKnownTypeTag();
        if (!barrier) {
            if (singleton) {
                // Try to inline a known constant value.
                bool isKnownConstant;
                if (!TestSingletonProperty(cx, globalObj, id, &isKnownConstant))
                    return false;
                if (isKnownConstant)
                    return pushConstant(ObjectValue(*singleton));
            }
            if (knownType == JSVAL_TYPE_UNDEFINED)
                return pushConstant(UndefinedValue());
            if (knownType == JSVAL_TYPE_NULL)
                return pushConstant(NullValue());
        }
    }

    MInstruction *global = MConstant::New(ObjectValue(*globalObj));
    current->add(global);

    // If we have a property typeset, the isOwnProperty call will trigger recompilation if
    // the property is deleted or reconfigured.
    if (!propertyTypes && shape->configurable())
        global = addShapeGuard(global, globalObj->lastProperty(), Bailout_ShapeGuard);

    JS_ASSERT(shape->slot() >= globalObj->numFixedSlots());

    MSlots *slots = MSlots::New(global);
    current->add(slots);
    MLoadSlot *load = MLoadSlot::New(slots, shape->slot() - globalObj->numFixedSlots());
    current->add(load);

    // Slot loads can be typed, if they have a single, known, definitive type.
    if (knownType != JSVAL_TYPE_UNKNOWN && !barrier)
        load->setResultType(MIRTypeFromValueType(knownType));

    current->push(load);
    return pushTypeBarrier(load, types, barrier);
}

bool
IonBuilder::jsop_setgname(HandlePropertyName name)
{
    RootedObject globalObj(cx, &script()->global());
    RootedId id(cx, NameToId(name));

    JS_ASSERT(globalObj->isNative());

    bool canSpecialize;
    types::HeapTypeSet *propertyTypes = oracle->globalPropertyWrite(script(), pc, id, &canSpecialize);

    // This should only happen for a few names like __proto__.
    if (!canSpecialize || globalObj->watched())
        return jsop_setprop(name);

    // For the fastest path, the property must be found, and it must be found
    // as a normal data property on exactly the global object.
    RootedShape shape(cx, globalObj->nativeLookup(cx, id));
    if (!shape || !shape->hasDefaultSetter() || !shape->writable() || !shape->hasSlot())
        return jsop_setprop(name);

    types::TypeObject *globalType = globalObj->getType(cx);
    if (!globalType)
        return false;
    if (propertyTypes && propertyTypes->isOwnProperty(cx, globalType, true)) {
        // The property has been reconfigured as non-configurable, non-enumerable
        // or non-writable.
        return jsop_setprop(name);
    }

    MInstruction *global = MConstant::New(ObjectValue(*globalObj));
    current->add(global);

    // If we have a property type set, the isOwnProperty call will trigger recompilation
    // if the property is deleted or reconfigured. Without TI, we always need a shape guard
    // to guard against the property being reconfigured as non-writable.
    if (!propertyTypes)
        global = addShapeGuard(global, globalObj->lastProperty(), Bailout_ShapeGuard);

    JS_ASSERT(shape->slot() >= globalObj->numFixedSlots());

    MSlots *slots = MSlots::New(global);
    current->add(slots);

    MDefinition *value = current->pop();
    MStoreSlot *store = MStoreSlot::New(slots, shape->slot() - globalObj->numFixedSlots(), value);
    current->add(store);

    // Determine whether write barrier is required.
    if (!propertyTypes || propertyTypes->needsBarrier(cx))
        store->setNeedsBarrier();

    // Pop the global object pushed by bindgname.
    DebugOnly<MDefinition *> pushedGlobal = current->pop();
    JS_ASSERT(&pushedGlobal->toConstant()->value().toObject() == globalObj);

    // If the property has a known type, we may be able to optimize typed stores by not
    // storing the type tag. This only works if the property does not have its initial
    // |undefined| value; if |undefined| is assigned at a later point, it will be added
    // to the type set.
    if (propertyTypes && !globalObj->getSlot(shape->slot()).isUndefined()) {
        JSValueType knownType = propertyTypes->getKnownTypeTag(cx);
        if (knownType != JSVAL_TYPE_UNKNOWN)
            store->setSlotType(MIRTypeFromValueType(knownType));
    }

    JS_ASSERT_IF(store->needsBarrier(), store->slotType() != MIRType_None);

    current->push(value);
    return resumeAfter(store);
}

bool
IonBuilder::jsop_getname(HandlePropertyName name)
{
    MDefinition *object;
    if (js_CodeSpec[*pc].format & JOF_GNAME) {
        MInstruction *global = MConstant::New(ObjectValue(script()->global()));
        current->add(global);
        object = global;
    } else {
        current->push(current->scopeChain());
        object = current->pop();
    }

    MGetNameCache *ins;
    if (JSOp(*GetNextPc(pc)) == JSOP_TYPEOF)
        ins = MGetNameCache::New(object, name, MGetNameCache::NAMETYPEOF);
    else
        ins = MGetNameCache::New(object, name, MGetNameCache::NAME);

    current->add(ins);
    current->push(ins);

    if (!resumeAfter(ins))
        return false;

    RootedScript scriptRoot(cx, script());
    types::StackTypeSet *barrier = oracle->propertyReadBarrier(scriptRoot, pc);
    types::StackTypeSet *types = oracle->propertyRead(script(), pc);

    monitorResult(ins, barrier, types);
    return pushTypeBarrier(ins, types, barrier);
}

bool
IonBuilder::jsop_intrinsic(HandlePropertyName name)
{
    types::StackTypeSet *types = oracle->propertyRead(script(), pc);
    JSValueType type = types->getKnownTypeTag();

    // If we haven't executed this opcode yet, we need to get the intrinsic
    // value and monitor the result.
    if (type == JSVAL_TYPE_UNKNOWN) {
        MCallGetIntrinsicValue *ins = MCallGetIntrinsicValue::New(name);

        current->add(ins);
        current->push(ins);

        if (!resumeAfter(ins))
            return false;

        RootedScript scriptRoot(cx, script());
        types::StackTypeSet *barrier = oracle->propertyReadBarrier(scriptRoot, pc);
        monitorResult(ins, barrier, types);
        return pushTypeBarrier(ins, types, barrier);
    }

    // Bake in the intrinsic. Make sure that TI agrees with us on the type.
    RootedValue vp(cx, UndefinedValue());
    if (!cx->global()->getIntrinsicValue(cx, name, &vp))
        return false;

    JS_ASSERT(types->hasType(types::GetValueType(cx, vp)));

    MConstant *ins = MConstant::New(vp);
    current->add(ins);
    current->push(ins);

    return true;
}

bool
IonBuilder::jsop_bindname(PropertyName *name)
{
    JS_ASSERT(script()->analysis()->usesScopeChain());

    MDefinition *scopeChain = current->scopeChain();
    MBindNameCache *ins = MBindNameCache::New(scopeChain, name, script(), pc);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

static JSValueType
GetElemKnownType(bool needsHoleCheck, types::StackTypeSet *types)
{
    JSValueType knownType = types->getKnownTypeTag();

    // Null and undefined have no payload so they can't be specialized.
    // Since folding null/undefined while building SSA is not safe (see the
    // comment in IsPhiObservable), we just add an untyped load instruction
    // and rely on pushTypeBarrier and DCE to replace it with a null/undefined
    // constant.
    if (knownType == JSVAL_TYPE_UNDEFINED || knownType == JSVAL_TYPE_NULL)
        knownType = JSVAL_TYPE_UNKNOWN;

    // Different architectures may want typed element reads which require
    // hole checks to be done as either value or typed reads.
    if (needsHoleCheck && !LIRGenerator::allowTypedElementHoleCheck())
        knownType = JSVAL_TYPE_UNKNOWN;

    return knownType;
}

bool
IonBuilder::jsop_getelem()
{
    RootedScript script(cx, this->script());

    if (oracle->elementReadIsDenseNative(script, pc))
        return jsop_getelem_dense();

    int arrayType = TypedArray::TYPE_MAX;
    if (oracle->elementReadIsTypedArray(script, pc, &arrayType))
        return jsop_getelem_typed(arrayType);

    if (oracle->elementReadIsString(script, pc))
        return jsop_getelem_string();

    LazyArgumentsType isArguments = oracle->elementReadMagicArguments(script, pc);
    if (isArguments == MaybeArguments)
        return abort("Type is not definitely lazy arguments.");
    if (isArguments == DefinitelyArguments)
        return jsop_arguments_getelem();

    MDefinition *rhs = current->pop();
    MDefinition *lhs = current->pop();

    MInstruction *ins;

    // TI does not account for GETELEM with string indexes, so we have to monitor
    // the result of MGetElementCache if it's expected to access string properties.
    // If the result of MGetElementCache is not monitored, we won't generate any
    // getprop stubs.
    bool mustMonitorResult = false;
    bool cacheable = false;
    bool intIndex = false;

    oracle->elementReadGeneric(script, pc, &cacheable, &mustMonitorResult, &intIndex);

    if (cacheable)
        ins = MGetElementCache::New(lhs, rhs, mustMonitorResult);
    else
        ins = MCallGetElement::New(lhs, rhs);

    current->add(ins);
    current->push(ins);

    if (!resumeAfter(ins))
        return false;

    types::StackTypeSet *barrier = oracle->propertyReadBarrier(script, pc);
    types::StackTypeSet *types = oracle->propertyRead(script, pc);

    if (cacheable && intIndex && !barrier && !mustMonitorResult) {
        bool needHoleCheck = !oracle->elementReadIsPacked(script, pc);
        JSValueType knownType = GetElemKnownType(needHoleCheck, types);

        if (knownType != JSVAL_TYPE_UNKNOWN && knownType != JSVAL_TYPE_DOUBLE)
            ins->setResultType(MIRTypeFromValueType(knownType));
    }

    if (mustMonitorResult)
        monitorResult(ins, barrier, types);
    return pushTypeBarrier(ins, types, barrier);
}

bool
IonBuilder::jsop_getelem_dense()
{
    RootedScript scriptRoot(cx, script());
    types::StackTypeSet *barrier = oracle->propertyReadBarrier(scriptRoot, pc);
    types::StackTypeSet *types = oracle->propertyRead(script(), pc);
    bool needsHoleCheck = !oracle->elementReadIsPacked(script(), pc);

    // Reads which are on holes in the object do not have to bail out if
    // undefined values have been observed at this access site and the access
    // cannot hit another indexed property on the object or its prototypes.
    bool readOutOfBounds =
        types->hasType(types::Type::UndefinedType()) &&
        !oracle->elementReadHasExtraIndexedProperty(script(), pc);

    MDefinition *id = current->pop();
    MDefinition *obj = current->pop();

    JSValueType knownType = JSVAL_TYPE_UNKNOWN;
    if (!barrier)
        knownType = GetElemKnownType(needsHoleCheck, types);

    // Ensure id is an integer.
    MInstruction *idInt32 = MToInt32::New(id);
    current->add(idInt32);
    id = idInt32;

    // Get the elements vector.
    MInstruction *elements = MElements::New(obj);
    current->add(elements);

    // If we can load the element as a definite double, make sure to check that
    // the array has been converted to homogenous doubles first.
    bool loadDouble = !barrier &&
                      loopDepth_ &&
                      !readOutOfBounds &&
                      !needsHoleCheck &&
                      knownType == JSVAL_TYPE_DOUBLE &&
                      oracle->elementReadShouldAlwaysLoadDoubles(script(), pc);
    if (loadDouble)
        elements = addConvertElementsToDoubles(elements);

    MInitializedLength *initLength = MInitializedLength::New(elements);
    current->add(initLength);

    MInstruction *load;

    if (!readOutOfBounds) {
        // This load should not return undefined, so likely we're reading
        // in-bounds elements, and the array is packed or its holes are not
        // read. This is the best case: we can separate the bounds check for
        // hoisting.
        id = addBoundsCheck(id, initLength);

        load = MLoadElement::New(elements, id, needsHoleCheck, loadDouble);
        current->add(load);
    } else {
        // This load may return undefined, so assume that we *can* read holes,
        // or that we can read out-of-bounds accesses. In this case, the bounds
        // check is part of the opcode.
        load = MLoadElementHole::New(elements, id, initLength, needsHoleCheck);
        current->add(load);

        // If maybeUndefined was true, the typeset must have undefined, and
        // then either additional types or a barrier. This means we should
        // never have a typed version of LoadElementHole.
        JS_ASSERT(knownType == JSVAL_TYPE_UNKNOWN);
    }

    if (knownType != JSVAL_TYPE_UNKNOWN)
        load->setResultType(MIRTypeFromValueType(knownType));

    current->push(load);
    return pushTypeBarrier(load, types, barrier);
}

MInstruction *
IonBuilder::getTypedArrayLength(MDefinition *obj)
{
    if (obj->isConstant() && obj->toConstant()->value().isObject()) {
        JSObject *array = &obj->toConstant()->value().toObject();
        int32_t length = (int32_t) TypedArray::length(array);
        obj->setFoldedUnchecked();
        return MConstant::New(Int32Value(length));
    }
    return MTypedArrayLength::New(obj);
}

MInstruction *
IonBuilder::getTypedArrayElements(MDefinition *obj)
{
    if (obj->isConstant() && obj->toConstant()->value().isObject()) {
        JSObject *array = &obj->toConstant()->value().toObject();
        void *data = TypedArray::viewData(array);

        // The 'data' pointer can change in rare circumstances
        // (ArrayBufferObject::changeContents).
        types::HeapTypeSet::WatchObjectStateChange(cx, array->getType(cx));

        obj->setFoldedUnchecked();
        return MConstantElements::New(data);
    }
    return MTypedArrayElements::New(obj);
}

bool
IonBuilder::jsop_getelem_typed(int arrayType)
{
    RootedScript scriptRoot(cx, script());
    types::StackTypeSet *barrier = oracle->propertyReadBarrier(scriptRoot, pc);
    types::StackTypeSet *types = oracle->propertyRead(script(), pc);

    MDefinition *id = current->pop();
    MDefinition *obj = current->pop();

    bool maybeUndefined = types->hasType(types::Type::UndefinedType());

    // Reading from an Uint32Array will result in a double for values
    // that don't fit in an int32. We have to bailout if this happens
    // and the instruction is not known to return a double.
    bool allowDouble = types->hasType(types::Type::DoubleType());

    // Ensure id is an integer.
    MInstruction *idInt32 = MToInt32::New(id);
    current->add(idInt32);
    id = idInt32;

    if (!maybeUndefined) {
        // Assume the index is in range, so that we can hoist the length,
        // elements vector and bounds check.

        // If we are reading in-bounds elements, we can use knowledge about
        // the array type to determine the result type. This may be more
        // precise than the known pushed type.
        MIRType knownType;
        switch (arrayType) {
          case TypedArray::TYPE_INT8:
          case TypedArray::TYPE_UINT8:
          case TypedArray::TYPE_UINT8_CLAMPED:
          case TypedArray::TYPE_INT16:
          case TypedArray::TYPE_UINT16:
          case TypedArray::TYPE_INT32:
            knownType = MIRType_Int32;
            break;
          case TypedArray::TYPE_UINT32:
            knownType = allowDouble ? MIRType_Double : MIRType_Int32;
            break;
          case TypedArray::TYPE_FLOAT32:
          case TypedArray::TYPE_FLOAT64:
            knownType = MIRType_Double;
            break;
          default:
            JS_NOT_REACHED("Unknown typed array type");
            return false;
        }

        // Get the length.
        MInstruction *length = getTypedArrayLength(obj);
        current->add(length);

        // Bounds check.
        id = addBoundsCheck(id, length);

        // Get the elements vector.
        MInstruction *elements = getTypedArrayElements(obj);
        current->add(elements);

        // Load the element.
        MLoadTypedArrayElement *load = MLoadTypedArrayElement::New(elements, id, arrayType);
        current->add(load);
        current->push(load);

        load->setResultType(knownType);

        // Note: we can ignore the type barrier here, we know the type must
        // be valid and unbarriered.
        JS_ASSERT_IF(knownType == MIRType_Int32, types->hasType(types::Type::Int32Type()));
        JS_ASSERT_IF(knownType == MIRType_Double, types->hasType(types::Type::DoubleType()));
        return true;
    } else {
        // Assume we will read out-of-bound values. In this case the
        // bounds check will be part of the instruction, and the instruction
        // will always return a Value.
        MLoadTypedArrayElementHole *load =
            MLoadTypedArrayElementHole::New(obj, id, arrayType, allowDouble);
        current->add(load);
        current->push(load);

        return resumeAfter(load) && pushTypeBarrier(load, types, barrier);
    }
}

bool
IonBuilder::jsop_getelem_string()
{
    MDefinition *id = current->pop();
    MDefinition *str = current->pop();

    MInstruction *idInt32 = MToInt32::New(id);
    current->add(idInt32);
    id = idInt32;

    MStringLength *length = MStringLength::New(str);
    current->add(length);

    // This will cause an invalidation of this script once the 'undefined' type
    // is monitored by the interpreter.
    JS_ASSERT(oracle->propertyRead(script(), pc)->getKnownTypeTag() == JSVAL_TYPE_STRING);
    id = addBoundsCheck(id, length);

    MCharCodeAt *charCode = MCharCodeAt::New(str, id);
    current->add(charCode);

    MFromCharCode *result = MFromCharCode::New(charCode);
    current->add(result);
    current->push(result);
    return true;
}

bool
IonBuilder::jsop_setelem()
{
    RootedScript script(cx, this->script());

    if (oracle->propertyWriteCanSpecialize(script, pc)) {
        if (oracle->elementWriteIsDenseNative(script, pc))
            return jsop_setelem_dense();

        int arrayType = TypedArray::TYPE_MAX;
        if (oracle->elementWriteIsTypedArray(script, pc, &arrayType))
            return jsop_setelem_typed(arrayType);
    }

    LazyArgumentsType isArguments = oracle->elementWriteMagicArguments(script, pc);
    if (isArguments == MaybeArguments)
        return abort("Type is not definitely lazy arguments.");
    if (isArguments == DefinitelyArguments)
        return jsop_arguments_setelem();

    MDefinition *value = current->pop();
    MDefinition *index = current->pop();
    MDefinition *object = current->pop();

    MInstruction *ins = MCallSetElement::New(object, index, value);
    current->add(ins);
    current->push(value);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_setelem_dense()
{
    MIRType elementType = oracle->elementWrite(script(), pc);
    bool packed = oracle->elementWriteIsPacked(script(), pc);

    // Writes which are on holes in the object do not have to bail out if they
    // cannot hit another indexed property on the object or its prototypes.
    bool writeOutOfBounds = !oracle->elementWriteHasExtraIndexedProperty(script(), pc);

    MDefinition *value = current->pop();
    MDefinition *id = current->pop();
    MDefinition *obj = current->pop();

    // Ensure id is an integer.
    MInstruction *idInt32 = MToInt32::New(id);
    current->add(idInt32);
    id = idInt32;

    // Ensure the value is a double, if double conversion might be needed.
    MDefinition *newValue = value;
    if (oracle->elementWriteNeedsDoubleConversion(script(), pc)) {
        MInstruction *valueDouble = MToDouble::New(value);
        current->add(valueDouble);
        newValue = valueDouble;
    }

    // Get the elements vector.
    MElements *elements = MElements::New(obj);
    current->add(elements);

    // Use MStoreElementHole if this SETELEM has written to out-of-bounds
    // indexes in the past. Otherwise, use MStoreElement so that we can hoist
    // the initialized length and bounds check.
    MStoreElementCommon *store;
    if (oracle->setElementHasWrittenHoles(script(), pc) && writeOutOfBounds) {
        MStoreElementHole *ins = MStoreElementHole::New(obj, elements, id, newValue);
        store = ins;

        current->add(ins);
        current->push(value);

        if (!resumeAfter(ins))
            return false;
    } else {
        MInitializedLength *initLength = MInitializedLength::New(elements);
        current->add(initLength);

        id = addBoundsCheck(id, initLength);

        bool needsHoleCheck = !packed && !writeOutOfBounds;

        MStoreElement *ins = MStoreElement::New(elements, id, newValue, needsHoleCheck);
        store = ins;

        current->add(ins);
        current->push(value);

        if (!resumeAfter(ins))
            return false;
    }

    if (oracle->elementWriteNeedsBarrier(script(), pc))
        store->setNeedsBarrier();

    if (elementType != MIRType_None && packed)
        store->setElementType(elementType);

    return true;
}

bool
IonBuilder::jsop_setelem_typed(int arrayType)
{
    MDefinition *value = current->pop();
    MDefinition *id = current->pop();
    MDefinition *obj = current->pop();

    // Ensure id is an integer.
    MInstruction *idInt32 = MToInt32::New(id);
    current->add(idInt32);
    id = idInt32;

    // Get the length.
    MInstruction *length = getTypedArrayLength(obj);
    current->add(length);

    // Bounds check.
    id = addBoundsCheck(id, length);

    // Get the elements vector.
    MInstruction *elements = getTypedArrayElements(obj);
    current->add(elements);

    // Clamp value to [0, 255] for Uint8ClampedArray.
    MDefinition *unclampedValue = value;
    if (arrayType == TypedArray::TYPE_UINT8_CLAMPED) {
        value = MClampToUint8::New(value);
        current->add(value->toInstruction());
    }

    // Store the value.
    MStoreTypedArrayElement *store = MStoreTypedArrayElement::New(elements, id, value, arrayType);
    current->add(store);

    current->push(unclampedValue);
    return resumeAfter(store);
}

bool
IonBuilder::jsop_length()
{
    if (jsop_length_fastPath())
        return true;

    RootedPropertyName name(cx, info().getAtom(pc)->asPropertyName());
    return jsop_getprop(name);
}

bool
IonBuilder::jsop_length_fastPath()
{
    TypeOracle::UnaryTypes sig = oracle->unaryTypes(script(), pc);
    if (!sig.inTypes || !sig.outTypes)
        return false;

    if (sig.outTypes->getKnownTypeTag() != JSVAL_TYPE_INT32)
        return false;

    switch (sig.inTypes->getKnownTypeTag()) {
      case JSVAL_TYPE_STRING: {
        MDefinition *obj = current->pop();
        MStringLength *ins = MStringLength::New(obj);
        current->add(ins);
        current->push(ins);
        return true;
      }

      case JSVAL_TYPE_OBJECT: {
        if (sig.inTypes->getKnownClass() == &ArrayClass &&
            !sig.inTypes->hasObjectFlags(cx, types::OBJECT_FLAG_LENGTH_OVERFLOW))
        {
            MDefinition *obj = current->pop();
            MElements *elements = MElements::New(obj);
            current->add(elements);

            // Read length.
            MArrayLength *length = new MArrayLength(elements);
            current->add(length);
            current->push(length);
            return true;
        }

        if (sig.inTypes->getTypedArrayType() != TypedArray::TYPE_MAX) {
            MDefinition *obj = current->pop();
            MInstruction *length = getTypedArrayLength(obj);
            current->add(length);
            current->push(length);
            return true;
        }

        return false;
      }

      default:
        break;
    }

    return false;
}

bool
IonBuilder::jsop_arguments()
{
    JS_ASSERT(lazyArguments_);
    current->push(lazyArguments_);
    return true;
}

bool
IonBuilder::jsop_arguments_length()
{
    // Type Inference has guaranteed this is an optimized arguments object.
    MDefinition *args = current->pop();
    args->setFoldedUnchecked();

    // We don't know anything from the callee
    if (inliningDepth_ == 0) {
        MInstruction *ins = MArgumentsLength::New();
        current->add(ins);
        current->push(ins);
        return true;
    }

    // We are inlining and know the number of arguments the callee pushed
    return pushConstant(Int32Value(inlinedArguments_.length()));
}

bool
IonBuilder::jsop_arguments_getelem()
{
    if (inliningDepth_ != 0)
        return abort("NYI inlined get argument element");

    RootedScript scriptRoot(cx, script());
    types::StackTypeSet *barrier = oracle->propertyReadBarrier(scriptRoot, pc);
    types::StackTypeSet *types = oracle->propertyRead(script(), pc);

    MDefinition *idx = current->pop();

    // Type Inference has guaranteed this is an optimized arguments object.
    MDefinition *args = current->pop();
    args->setFoldedUnchecked();

    // To ensure that we are not looking above the number of actual arguments.
    MArgumentsLength *length = MArgumentsLength::New();
    current->add(length);

    // Ensure idx is an integer.
    MInstruction *index = MToInt32::New(idx);
    current->add(index);

    // Bailouts if we read more than the number of actual arguments.
    index = addBoundsCheck(index, length);

    // Load the argument from the actual arguments.
    MGetArgument *load = MGetArgument::New(index);
    current->add(load);
    current->push(load);

    return pushTypeBarrier(load, types, barrier);
}

bool
IonBuilder::jsop_arguments_setelem()
{
    return abort("NYI arguments[]=");
}

inline types::HeapTypeSet *
GetDefiniteSlot(JSContext *cx, types::StackTypeSet *types, JSAtom *atom)
{
    if (!types || types->unknownObject() || types->getObjectCount() != 1)
        return NULL;

    types::TypeObject *type = types->getTypeObject(0);
    if (!type || type->unknownProperties())
        return NULL;

    RawId id = AtomToId(atom);
    if (id != types::IdToTypeId(id))
        return NULL;

    types::HeapTypeSet *propertyTypes = type->getProperty(cx, id, false);
    if (!propertyTypes ||
        !propertyTypes->definiteProperty() ||
        propertyTypes->isOwnProperty(cx, type, true))
    {
        return NULL;
    }

    return propertyTypes;
}

bool
IonBuilder::jsop_not()
{
    MDefinition *value = current->pop();

    MNot *ins = new MNot(value);
    current->add(ins);
    current->push(ins);
    TypeOracle::UnaryTypes types = oracle->unaryTypes(script(), pc);
    ins->infer(types, cx);
    return true;
}


inline bool
IonBuilder::TestCommonPropFunc(JSContext *cx, types::StackTypeSet *types, HandleId id,
                               JSFunction **funcp, bool isGetter, bool *isDOM,
                               MDefinition **guardOut)
{
    JSObject *found = NULL;
    JSObject *foundProto = NULL;

    *funcp = NULL;
    *isDOM = false;

    // No sense looking if we don't know what's going on.
    if (!types || types->unknownObject())
        return true;

    // Iterate down all the types to see if they all have the same getter or
    // setter.
    for (unsigned i = 0; i < types->getObjectCount(); i++) {
        RootedObject curObj(cx, types->getSingleObject(i));

        // Non-Singleton type
        if (!curObj) {
            types::TypeObject *typeObj = types->getTypeObject(i);

            if (!typeObj)
                continue;

            if (typeObj->unknownProperties())
                return true;

            // If the type has an own property, we can't be sure we don't shadow
            // the chain.
            types::HeapTypeSet *propSet = typeObj->getProperty(cx, types::IdToTypeId(id), false);
            if (!propSet)
                return false;
            if (propSet->ownProperty(false))
                return true;

            // Otherwise try using the prototype.
            curObj = typeObj->proto;
        } else {
            // We can't optimize setters on watched singleton objects. A getter
            // on an own property can be protected with the prototype
            // shapeguard, though.
            if (!isGetter && curObj->watched())
                return true;
        }

        // Turns out that we need to check for a property lookup op, else we
        // will end up calling it mid-compilation.
        if (!CanEffectlesslyCallLookupGenericOnObject(curObj))
            return true;

        RootedObject proto(cx);
        RootedShape shape(cx);
        if (!JSObject::lookupGeneric(cx, curObj, id, &proto, &shape))
            return false;

        if (!shape)
            return true;

        // We want to optimize specialized getters/setters. The defaults will
        // hit the slot optimization.
        if (isGetter) {
            if (shape->hasDefaultGetter() || !shape->hasGetterValue())
                return true;
        } else {
            if (shape->hasDefaultSetter() || !shape->hasSetterValue())
                return true;
        }

        JSObject * curFound = isGetter ? shape->getterObject():
                                         shape->setterObject();

        // Save the first seen, or verify uniqueness.
        if (!found) {
            if (!curFound->isFunction())
                return true;
            found = curFound;
        } else if (found != curFound) {
            return true;
        }

        // We only support cases with a single prototype shared. This is
        // overwhelmingly more likely than having multiple different prototype
        // chains with the same custom property function.
        if (!foundProto)
            foundProto = proto;
        else if (foundProto != proto)
            return true;

        // Check here to make sure that everyone has Type Objects with known
        // properties between them and the proto we found the accessor on. We
        // need those to add freezes safely. NOTE: We do not do this above, as
        // we may be able to freeze all the types up to where we found the
        // property, even if there are unknown types higher in the prototype
        // chain.
        while (curObj != foundProto) {
            types::TypeObject *typeObj = curObj->getType(cx);
            if (!typeObj)
                return false;

            if (typeObj->unknownProperties())
                return true;

            // Check here to make sure that nobody on the prototype chain is
            // marked as having the property as an "own property". This can
            // happen in cases of |delete| having been used, or cases with
            // watched objects. If TI ever decides to be more accurate about
            // |delete| handling, this should go back to curObj->watched().

            // Even though we are not directly accessing the properties on the whole
            // prototype chain, we need to fault in the sets anyway, as we need
            // to freeze on them.
            types::HeapTypeSet *propSet = typeObj->getProperty(cx, types::IdToTypeId(id), false);
            if (!propSet)
                return false;
            if (propSet->ownProperty(false))
                return true;

            curObj = curObj->getProto();
        }
    }

    // No need to add a freeze if we didn't find anything
    if (!found)
        return true;

    JS_ASSERT(foundProto);

    // Add a shape guard on the prototype we found the property on. The rest of
    // the prototype chain is guarded by TI freezes. Note that a shape guard is
    // good enough here, even in the proxy case, because we have ensured there
    // are no lookup hooks for this property.
    MInstruction *wrapper = MConstant::New(ObjectValue(*foundProto));
    current->add(wrapper);
    wrapper = addShapeGuard(wrapper, foundProto->lastProperty(), Bailout_ShapeGuard);

    // Pass the guard back so it can be an operand.
    if (isGetter) {
        JS_ASSERT(wrapper->isGuardShape());
        *guardOut = wrapper;
    }

    // Now we have to freeze all the property typesets to ensure there isn't a
    // lower shadowing getter or setter installed in the future.
    types::TypeObject *curType;
    for (unsigned i = 0; i < types->getObjectCount(); i++) {
        curType = types->getTypeObject(i);
        JSObject *obj = NULL;
        if (!curType) {
            obj = types->getSingleObject(i);
            if (!obj)
                continue;

            curType = obj->getType(cx);
            if (!curType)
                return false;
        }

        // If we found a Singleton object's own-property, there's nothing to
        // freeze.
        if (obj != foundProto) {
            // Walk the prototype chain. Everyone has to have the property, since we
            // just checked, so propSet cannot be NULL.
            RawId typeId = types::IdToTypeId(id);
            while (true) {
                types::HeapTypeSet *propSet = curType->getProperty(cx, typeId, false);
                // This assert is now assured, since we have faulted them in
                // above.
                JS_ASSERT(propSet);
                // Asking, freeze by asking.
                DebugOnly<bool> isOwn = propSet->isOwnProperty(cx, curType, false);
                JS_ASSERT(!isOwn);
                // Don't mark the proto. It will be held down by the shape
                // guard. This allows us tp use properties found on prototypes
                // with properties unknown to TI.
                if (curType->proto == foundProto)
                    break;
                curType = curType->proto->getType(cx);
                if (!curType)
                    return false;
            }
        }
    }

    *funcp = found->toFunction();
    *isDOM = types->isDOMClass();

    return true;
}

bool
IonBuilder::annotateGetPropertyCache(JSContext *cx, MDefinition *obj, MGetPropertyCache *getPropCache,
                                    types::StackTypeSet *objTypes, types::StackTypeSet *pushedTypes)
{
    RootedId id(cx, NameToId(getPropCache->name()));
    if (id != types::IdToTypeId(id))
        return true;

    // Ensure every pushed value is a singleton.
    if (pushedTypes->unknownObject() || pushedTypes->baseFlags() != 0)
        return true;

    for (unsigned i = 0; i < pushedTypes->getObjectCount(); i++) {
        if (pushedTypes->getTypeObject(i) != NULL)
            return true;
    }

    // Object's typeset should be a proper object
    if (objTypes->baseFlags() || objTypes->unknownObject())
        return true;

    unsigned int objCount = objTypes->getObjectCount();
    if (objCount == 0)
        return true;

    InlinePropertyTable *inlinePropTable = getPropCache->initInlinePropertyTable(pc);
    if (!inlinePropTable)
        return false;

    // Ensure that the relevant property typeset for each type object is
    // is a single-object typeset containing a JSFunction
    for (unsigned int i = 0; i < objCount; i++) {
        types::TypeObject *typeObj = objTypes->getTypeObject(i);
        if (!typeObj || typeObj->unknownProperties() || !typeObj->proto)
            continue;

        types::HeapTypeSet *ownTypes = typeObj->getProperty(cx, id, false);
        if (!ownTypes)
            continue;

        if (ownTypes->isOwnProperty(cx, typeObj, false))
            continue;

        bool knownConstant = false;
        Rooted<JSObject*> proto(cx, typeObj->proto);
        if (!TestSingletonProperty(cx, proto, id, &knownConstant))
            return false;

        types::TypeObject *protoType = proto->getType(cx);
        if (!protoType)
            return false;
        if (!knownConstant || protoType->unknownProperties())
            continue;

        types::HeapTypeSet *protoTypes = protoType->getProperty(cx, id, false);
        if (!protoTypes)
            continue;

        JSObject *obj = protoTypes->getSingleton(cx);
        if (!obj || !obj->isFunction())
            continue;

        // Don't add cases corresponding to non-observed pushes
        if (!pushedTypes->hasType(types::Type::ObjectType(obj)))
            continue;

        if (!inlinePropTable->addEntry(typeObj, obj->toFunction()))
            return false;
    }

    if (inlinePropTable->numEntries() == 0) {
        getPropCache->clearInlinePropertyTable();
        return true;
    }

#ifdef DEBUG
    if (inlinePropTable->numEntries() > 0)
        IonSpew(IonSpew_Inlining, "Annotated GetPropertyCache with %d/%d inline cases",
                                    (int) inlinePropTable->numEntries(), (int) objCount);
#endif

    // If we successfully annotated the GetPropertyCache and there are inline cases,
    // then keep a resume point of the state right before this instruction for use
    // later when we have to bail out to this point in the fallback case of a
    // PolyInlineDispatch.
    if (inlinePropTable->numEntries() > 0) {
        // Push the object back onto the stack temporarily to capture the resume point.
        current->push(obj);
        MResumePoint *resumePoint = MResumePoint::New(current, pc, callerResumePoint_,
                                                      MResumePoint::ResumeAt);
        if (!resumePoint)
            return false;
        inlinePropTable->setPriorResumePoint(resumePoint);
        current->pop();
    }
    return true;
}

// Returns true if an idempotent cache has ever invalidated this script
// or an outer script.
bool
IonBuilder::invalidatedIdempotentCache()
{
    IonBuilder *builder = this;
    do {
        if (builder->script()->invalidatedIdempotentCache)
            return true;
        builder = builder->callerBuilder_;
    } while (builder);

    return false;
}

bool
IonBuilder::loadSlot(MDefinition *obj, HandleShape shape, MIRType rvalType)
{
    JS_ASSERT(shape->hasDefaultGetter());
    JS_ASSERT(shape->hasSlot());

    RootedScript scriptRoot(cx, script());
    types::StackTypeSet *barrier = oracle->propertyReadBarrier(scriptRoot, pc);
    types::StackTypeSet *types = oracle->propertyRead(script(), pc);

    if (shape->slot() < shape->numFixedSlots()) {
        MLoadFixedSlot *load = MLoadFixedSlot::New(obj, shape->slot());
        current->add(load);
        current->push(load);

        load->setResultType(rvalType);
        return pushTypeBarrier(load, types, barrier);
    }

    MSlots *slots = MSlots::New(obj);
    current->add(slots);

    MLoadSlot *load = MLoadSlot::New(slots, shape->slot() - shape->numFixedSlots());
    current->add(load);
    current->push(load);

    load->setResultType(rvalType);
    return pushTypeBarrier(load, types, barrier);
}

bool
IonBuilder::storeSlot(MDefinition *obj, RawShape shape, MDefinition *value, bool needsBarrier)
{
    JS_ASSERT(shape->hasDefaultSetter());
    JS_ASSERT(shape->writable());
    JS_ASSERT(shape->hasSlot());

    if (shape->slot() < shape->numFixedSlots()) {
        MStoreFixedSlot *store = MStoreFixedSlot::New(obj, shape->slot(), value);
        current->add(store);
        current->push(value);
        if (needsBarrier)
            store->setNeedsBarrier();
        return resumeAfter(store);
    }

    MSlots *slots = MSlots::New(obj);
    current->add(slots);

    MStoreSlot *store = MStoreSlot::New(slots, shape->slot() - shape->numFixedSlots(), value);
    current->add(store);
    current->push(value);
    if (needsBarrier)
        store->setNeedsBarrier();
    return resumeAfter(store);
}

bool
IonBuilder::jsop_getprop(HandlePropertyName name)
{
    RootedId id(cx, NameToId(name));

    RootedScript scriptRoot(cx, script());
    types::StackTypeSet *barrier = oracle->propertyReadBarrier(scriptRoot, pc);
    types::StackTypeSet *types = oracle->propertyRead(script(), pc);
    TypeOracle::Unary unary = oracle->unaryOp(script(), pc);
    TypeOracle::UnaryTypes uTypes = oracle->unaryTypes(script(), pc);

    bool emitted = false;

    // Try to optimize arguments.length.
    if (!getPropTryArgumentsLength(&emitted) || emitted)
        return emitted;

    // Try to hardcode known constants.
    if (!getPropTryConstant(&emitted, id, barrier, types, uTypes) || emitted)
        return emitted;

    // Try to emit loads from definite slots.
    if (!getPropTryDefiniteSlot(&emitted, name, barrier, types, unary, uTypes) || emitted)
        return emitted;

    // Try to inline a common property getter, or make a call.
    if (!getPropTryCommonGetter(&emitted, id, barrier, types, uTypes) || emitted)
        return emitted;

    // Try to emit a monomorphic cache based on data in JM caches.
    if (!getPropTryMonomorphic(&emitted, id, barrier, unary, uTypes) || emitted)
        return emitted;

    // Try to emit a polymorphic cache.
    if (!getPropTryPolymorphic(&emitted, name, id, barrier, types, unary, uTypes) || emitted)
        return emitted;

    // Emit a call.
    MDefinition *obj = current->pop();
    MCallGetProperty *call = MCallGetProperty::New(obj, name);
    current->add(call);
    current->push(call);
    if (!resumeAfter(call))
        return false;

    monitorResult(call, barrier, types);
    return pushTypeBarrier(call, types, barrier);
}

bool
IonBuilder::getPropTryArgumentsLength(bool *emitted)
{
    JS_ASSERT(*emitted == false);
    LazyArgumentsType isArguments = oracle->propertyReadMagicArguments(script(), pc);

    if (isArguments == MaybeArguments)
        return abort("Type is not definitely lazy arguments.");
    if (isArguments != DefinitelyArguments)
        return true;
    if (JSOp(*pc) != JSOP_LENGTH)
        return true;

    *emitted = true;
    return jsop_arguments_length();
}

bool
IonBuilder::getPropTryConstant(bool *emitted, HandleId id, types::StackTypeSet *barrier,
                               types::StackTypeSet *types, TypeOracle::UnaryTypes unaryTypes)
{
    JS_ASSERT(*emitted == false);
    JSObject *singleton = types ? types->getSingleton() : NULL;
    if (!singleton || barrier)
        return true;

    RootedObject global(cx, &script()->global());

    bool isConstant, testObject, testString;
    if (!TestSingletonPropertyTypes(cx, unaryTypes.inTypes, global, id,
                                    &isConstant, &testObject, &testString))
        return false;

    if (!isConstant)
        return true;

    MDefinition *obj = current->pop();

    // Property access is a known constant -- safe to emit.
    JS_ASSERT(!testString || !testObject);
    if (testObject)
        current->add(MGuardObject::New(obj));
    else if (testString)
        current->add(MGuardString::New(obj));
    else
        obj->setFoldedUnchecked();

    MConstant *known = MConstant::New(ObjectValue(*singleton));

    current->add(known);
    current->push(known);

    *emitted = true;
    return true;
}

bool
IonBuilder::getPropTryDefiniteSlot(bool *emitted, HandlePropertyName name,
                                   types::StackTypeSet *barrier, types::StackTypeSet *types,
                                   TypeOracle::Unary unary, TypeOracle::UnaryTypes unaryTypes)
{
    JS_ASSERT(*emitted == false);
    types::TypeSet *propTypes = GetDefiniteSlot(cx, unaryTypes.inTypes, name);
    if (!propTypes)
        return true;

    MDefinition *obj = current->pop();
    MDefinition *useObj = obj;
    if (unaryTypes.inTypes && unaryTypes.inTypes->baseFlags()) {
        MGuardObject *guard = MGuardObject::New(obj);
        current->add(guard);
        useObj = guard;
    }

    MLoadFixedSlot *fixed = MLoadFixedSlot::New(useObj, propTypes->definiteSlot());
    if (!barrier)
        fixed->setResultType(unary.rval);

    current->add(fixed);
    current->push(fixed);

    if (!pushTypeBarrier(fixed, types, barrier))
        return false;

    *emitted = true;
    return true;
}

bool
IonBuilder::getPropTryCommonGetter(bool *emitted, HandleId id, types::StackTypeSet *barrier,
                                   types::StackTypeSet *types, TypeOracle::UnaryTypes unaryTypes)
{
    JS_ASSERT(*emitted == false);
    JSFunction *commonGetter;
    bool isDOM;
    MDefinition *guard;

    if (!TestCommonPropFunc(cx, unaryTypes.inTypes, id, &commonGetter, true,
                            &isDOM, &guard))
    {
        return false;
    }
    if (!commonGetter)
        return true;

    MDefinition *obj = current->pop();
    RootedFunction getter(cx, commonGetter);

    if (isDOM && TestShouldDOMCall(cx, unaryTypes.inTypes, getter, JSJitInfo::Getter)) {
        const JSJitInfo *jitinfo = getter->jitInfo();
        MGetDOMProperty *get = MGetDOMProperty::New(jitinfo, obj, guard);
        current->add(get);
        current->push(get);

        if (get->isEffectful() && !resumeAfter(get))
            return false;
        barrier = AdjustTypeBarrierForDOMCall(jitinfo, types, barrier);
        if (!pushTypeBarrier(get, types, barrier))
            return false;

        *emitted = true;
        return true;
    }

    // Don't call the getter with a primitive value.
    if (unaryTypes.inTypes->getKnownTypeTag() != JSVAL_TYPE_OBJECT) {
        MGuardObject *guardObj = MGuardObject::New(obj);
        current->add(guardObj);
        obj = guardObj;
    }

    // Spoof stack to expected state for call.
    pushConstant(ObjectValue(*commonGetter));

    MPassArg *wrapper = MPassArg::New(obj);
    current->add(wrapper);
    current->push(wrapper);

    CallInfo callInfo(cx, false, types, barrier);
    if (!callInfo.init(current, 0))
        return false;
    if (!makeCallBarrier(getter, callInfo, unaryTypes.inTypes, false))
        return false;

    *emitted = true;
    return true;
}

bool
IonBuilder::getPropTryMonomorphic(bool *emitted, HandleId id, types::StackTypeSet *barrier,
                                  TypeOracle::Unary unary, TypeOracle::UnaryTypes unaryTypes)
{
    JS_ASSERT(*emitted == false);
    bool accessGetter = oracle->propertyReadAccessGetter(script(), pc);

    if (unary.ival != MIRType_Object)
        return true;

    RootedShape objShape(cx, mjit::GetPICSingleShape(cx, script(), pc, info().constructing()));
    if (!objShape || objShape->inDictionary()) {
        spew("GETPROP not monomorphic");
        return true;
    }

    MDefinition *obj = current->pop();

    // The JM IC was monomorphic, so we inline the property access as long as
    // the shape is not in dictionary made. We cannot be sure that the shape is
    // still a lastProperty, and calling Shape::search() on dictionary mode
    // shapes that aren't lastProperty is invalid.
    obj = addShapeGuard(obj, objShape, Bailout_CachedShapeGuard);

    spew("Inlining monomorphic GETPROP");
    RootedShape shape(cx, objShape->search(cx, id));
    JS_ASSERT(shape);

    MIRType rvalType = unary.rval;
    if (barrier || IsNullOrUndefined(unary.rval) || accessGetter)
        rvalType = MIRType_Value;

    if (!loadSlot(obj, shape, rvalType))
        return false;

    *emitted = true;
    return true;
}

bool
IonBuilder::getPropTryPolymorphic(bool *emitted, HandlePropertyName name, HandleId id,
                                  types::StackTypeSet *barrier, types::StackTypeSet *types,
                                  TypeOracle::Unary unary, TypeOracle::UnaryTypes unaryTypes)
{
    JS_ASSERT(*emitted == false);
    bool accessGetter = oracle->propertyReadAccessGetter(script(), pc);

    // The input value must either be an object, or we should have strong suspicions
    // that it can be safely unboxed to an object.
    if (unary.ival != MIRType_Object && !unaryTypes.inTypes->objectOrSentinel())
        return true;

    MIRType rvalType = unary.rval;
    if (barrier || IsNullOrUndefined(unary.rval) || accessGetter)
        rvalType = MIRType_Value;

    MDefinition *obj = current->pop();
    MGetPropertyCache *load = MGetPropertyCache::New(obj, name);
    load->setResultType(rvalType);

    // Try to mark the cache as idempotent. We only do this if JM is enabled
    // (its ICs are used to mark property reads as likely non-idempotent) or
    // if we are compiling eagerly (to improve test coverage).
    if (unary.ival == MIRType_Object &&
        (cx->methodJitEnabled || js_IonOptions.eagerCompilation) &&
        !invalidatedIdempotentCache())
    {
        RootedScript scriptRoot(cx, script());
        if (oracle->propertyReadIdempotent(scriptRoot, pc, id))
            load->setIdempotent();
    }

    if (JSOp(*pc) == JSOP_CALLPROP) {
        if (!annotateGetPropertyCache(cx, obj, load, unaryTypes.inTypes, types))
            return false;
    }

    // If the cache is known to access getters, then enable generation of getter stubs.
    if (accessGetter)
        load->setAllowGetters();

    current->add(load);
    current->push(load);

    if (load->isEffectful() && !resumeAfter(load))
        return false;

    if (accessGetter)
        monitorResult(load, barrier, types);

    if (!pushTypeBarrier(load, types, barrier))
        return false;

    *emitted = true;
    return true;
}

bool
IonBuilder::jsop_setprop(HandlePropertyName name)
{
    MDefinition *value = current->pop();
    MDefinition *obj = current->pop();

    bool monitored = !oracle->propertyWriteCanSpecialize(script(), pc);

    TypeOracle::BinaryTypes binaryTypes = oracle->binaryTypes(script(), pc);

    if (!monitored) {
        if (types::HeapTypeSet *propTypes = GetDefiniteSlot(cx, binaryTypes.lhsTypes, name)) {
            MStoreFixedSlot *fixed = MStoreFixedSlot::New(obj, propTypes->definiteSlot(), value);
            current->add(fixed);
            current->push(value);
            if (propTypes->needsBarrier(cx))
                fixed->setNeedsBarrier();
            return resumeAfter(fixed);
        }
    }

    RootedId id(cx, NameToId(name));
    types::StackTypeSet *types = binaryTypes.lhsTypes;

    JSFunction *commonSetter;
    bool isDOM;
    if (!TestCommonPropFunc(cx, types, id, &commonSetter, false, &isDOM, NULL))
        return false;
    if (!monitored && commonSetter) {
        RootedFunction setter(cx, commonSetter);
        if (isDOM && TestShouldDOMCall(cx, types, setter, JSJitInfo::Setter)) {
            MSetDOMProperty *set = MSetDOMProperty::New(setter->jitInfo()->op, obj, value);
            if (!set)
                return false;

            current->add(set);
            current->push(value);

            return resumeAfter(set);
        }

        // Don't call the setter with a primitive value.
        if (types->getKnownTypeTag() != JSVAL_TYPE_OBJECT) {
            MGuardObject *guardObj = MGuardObject::New(obj);
            current->add(guardObj);
            obj = guardObj;
        }

        // Dummy up the stack, as in getprop
        pushConstant(ObjectValue(*setter));

        MPassArg *wrapper = MPassArg::New(obj);
        current->push(wrapper);
        current->add(wrapper);

        MPassArg *arg = MPassArg::New(value);
        current->push(arg);
        current->add(arg);

        // Call the setter. Note that we have to push the original value, not
        // the setter's return value.
        CallInfo callInfo(cx, false);
        if (!callInfo.init(current, 1))
            return false;
        MCall *call = makeCallHelper(setter, callInfo, types, false);
        if (!call)
            return false;

        current->push(value);
        return resumeAfter(call);
    }

    oracle->binaryOp(script(), pc);

    MSetPropertyInstruction *ins;
    if (monitored) {
        ins = MCallSetProperty::New(obj, value, name, script()->strict);
    } else {
        RawShape objShape = mjit::GetPICSingleShape(cx, script(), pc, info().constructing());
        if (objShape && !objShape->inDictionary()) {
            // The JM IC was monomorphic, so we inline the property access as
            // long as the shape is not in dictionary mode. We cannot be sure
            // that the shape is still a lastProperty, and calling Shape::search
            // on dictionary mode shapes that aren't lastProperty is invalid.
            obj = addShapeGuard(obj, objShape, Bailout_CachedShapeGuard);

            RootedShape shape(cx, objShape->search(cx, NameToId(name)));
            JS_ASSERT(shape);

            spew("Inlining monomorphic SETPROP");

            RawId typeId = types::IdToTypeId(id);
            bool needsBarrier = oracle->propertyWriteNeedsBarrier(script(), pc, typeId);

            return storeSlot(obj, shape, value, needsBarrier);
        }

        spew("SETPROP not monomorphic");

        ins = MSetPropertyCache::New(obj, value, name, script()->strict);

        if (!binaryTypes.lhsTypes || binaryTypes.lhsTypes->propertyNeedsBarrier(cx, id))
            ins->setNeedsBarrier();
    }

    current->add(ins);
    current->push(value);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_delprop(HandlePropertyName name)
{
    MDefinition *obj = current->pop();

    MInstruction *ins = MDeleteProperty::New(obj, name);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_regexp(RegExpObject *reobj)
{
    JSObject *prototype = script()->global().getOrCreateRegExpPrototype(cx);
    if (!prototype)
        return false;

    MRegExp *regexp = MRegExp::New(reobj, prototype);
    current->add(regexp);
    current->push(regexp);

    regexp->setMovable();

    // The MRegExp is set to be movable.
    // That would be incorrect for global/sticky, because lastIndex could be wrong.
    // Therefore setting the lastIndex to 0. That is faster than removing the movable flag.
    if (reobj->sticky() || reobj->global()) {
        MConstant *zero = MConstant::New(Int32Value(0));
        current->add(zero);

        MStoreFixedSlot *lastIndex =
            MStoreFixedSlot::New(regexp, RegExpObject::lastIndexSlot(), zero);
        current->add(lastIndex);
    }

    return true;
}

bool
IonBuilder::jsop_object(JSObject *obj)
{
    MConstant *ins = MConstant::New(ObjectValue(*obj));
    current->add(ins);
    current->push(ins);

    return true;
}

bool
IonBuilder::jsop_lambda(JSFunction *fun)
{
    JS_ASSERT(script()->analysis()->usesScopeChain());
    if (fun->isArrow())
        return abort("bound arrow function");
    if (fun->isNative() && IsAsmJSModuleNative(fun->native()))
        return abort("asm.js module function");

    MLambda *ins = MLambda::New(current->scopeChain(), fun);
    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_defvar(uint32_t index)
{
    JS_ASSERT(JSOp(*pc) == JSOP_DEFVAR || JSOp(*pc) == JSOP_DEFCONST);

    RootedPropertyName name(cx, script()->getName(index));

    // Bake in attrs.
    unsigned attrs = JSPROP_ENUMERATE | JSPROP_PERMANENT;
    if (JSOp(*pc) == JSOP_DEFCONST)
        attrs |= JSPROP_READONLY;

    // Pass the ScopeChain.
    JS_ASSERT(script()->analysis()->usesScopeChain());

    // Bake the name pointer into the MDefVar.
    MDefVar *defvar = MDefVar::New(name, attrs, current->scopeChain());
    current->add(defvar);

    return resumeAfter(defvar);
}

bool
IonBuilder::jsop_deffun(uint32_t index)
{
    RootedFunction fun(cx, script()->getFunction(index));
    if (fun->isNative() && IsAsmJSModuleNative(fun->native()))
        return abort("asm.js module function");

    JS_ASSERT(script()->analysis()->usesScopeChain());

    MDefFun *deffun = MDefFun::New(fun, current->scopeChain());
    current->add(deffun);

    return resumeAfter(deffun);
}

bool
IonBuilder::jsop_this()
{
    if (!info().fun())
        return abort("JSOP_THIS outside of a JSFunction.");

    if (script()->strict) {
        current->pushSlot(info().thisSlot());
        return true;
    }

    types::StackTypeSet *types = oracle->thisTypeSet(script());
    if (types && types->getKnownTypeTag() == JSVAL_TYPE_OBJECT) {
        // This is safe, because if the entry type of |this| is an object, it
        // will necessarily be an object throughout the entire function. OSR
        // can introduce a phi, but this phi will be specialized.
        current->pushSlot(info().thisSlot());
        return true;
    }

    return abort("JSOP_THIS hard case not yet handled");
}

bool
IonBuilder::jsop_typeof()
{
    TypeOracle::Unary unary = oracle->unaryOp(script(), pc);

    MDefinition *input = current->pop();
    MTypeOf *ins = MTypeOf::New(input, unary.ival);

    current->add(ins);
    current->push(ins);

    if (ins->isEffectful() && !resumeAfter(ins))
        return false;
    return true;
}

bool
IonBuilder::jsop_toid()
{
    // No-op if the index is an integer.
    TypeOracle::Unary unary = oracle->unaryOp(script(), pc);
    if (unary.ival == MIRType_Int32)
        return true;

    MDefinition *index = current->pop();
    MToId *ins = MToId::New(current->peek(-1), index);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_iter(uint8_t flags)
{
    MDefinition *obj = current->pop();
    MInstruction *ins = MIteratorStart::New(obj, flags);

    if (!iterators_.append(ins))
        return false;

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_iternext()
{
    MDefinition *iter = current->peek(-1);
    MInstruction *ins = MIteratorNext::New(iter);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_itermore()
{
    MDefinition *iter = current->peek(-1);
    MInstruction *ins = MIteratorMore::New(iter);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_iterend()
{
    MDefinition *iter = current->pop();
    MInstruction *ins = MIteratorEnd::New(iter);

    current->add(ins);

    return resumeAfter(ins);
}

MDefinition *
IonBuilder::walkScopeChain(unsigned hops)
{
    MDefinition *scope = current->getSlot(info().scopeChainSlot());

    for (unsigned i = 0; i < hops; i++) {
        MInstruction *ins = MEnclosingScope::New(scope);
        current->add(ins);
        scope = ins;
    }

    return scope;
}

bool
IonBuilder::jsop_getaliasedvar(ScopeCoordinate sc)
{
    types::StackTypeSet *barrier;
    types::StackTypeSet *actual = oracle->aliasedVarBarrier(script(), pc, &barrier);

    MDefinition *obj = walkScopeChain(sc.hops);

    RootedShape shape(cx, ScopeCoordinateToStaticScopeShape(cx, script(), pc));

    MInstruction *load;
    if (shape->numFixedSlots() <= sc.slot) {
        MInstruction *slots = MSlots::New(obj);
        current->add(slots);

        load = MLoadSlot::New(slots, sc.slot - shape->numFixedSlots());
    } else {
        load = MLoadFixedSlot::New(obj, sc.slot);
    }

    if (!barrier) {
        JSValueType type = actual->getKnownTypeTag();
        if (type != JSVAL_TYPE_UNKNOWN &&
            type != JSVAL_TYPE_UNDEFINED &&
            type != JSVAL_TYPE_NULL)
        {
            load->setResultType(MIRTypeFromValueType(type));
        }
    }

    current->add(load);
    current->push(load);

    return pushTypeBarrier(load, actual, barrier);
}

bool
IonBuilder::jsop_setaliasedvar(ScopeCoordinate sc)
{
    MDefinition *rval = current->peek(-1);
    MDefinition *obj = walkScopeChain(sc.hops);

    RootedShape shape(cx, ScopeCoordinateToStaticScopeShape(cx, script(), pc));

    MInstruction *store;
    if (shape->numFixedSlots() <= sc.slot) {
        MInstruction *slots = MSlots::New(obj);
        current->add(slots);

        store = MStoreSlot::NewBarriered(slots, sc.slot - shape->numFixedSlots(), rval);
    } else {
        store = MStoreFixedSlot::NewBarriered(obj, sc.slot, rval);
    }

    current->add(store);
    return resumeAfter(store);
}

bool
IonBuilder::jsop_in()
{
    RootedScript scriptRoot(cx, script());
    if (oracle->inObjectIsDenseNativeWithoutExtraIndexedProperties(scriptRoot, pc))
        return jsop_in_dense();

    MDefinition *obj = current->pop();
    MDefinition *id = current->pop();
    MIn *ins = new MIn(id, obj);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

bool
IonBuilder::jsop_in_dense()
{
    bool needsHoleCheck = !oracle->inArrayIsPacked(script(), pc);

    MDefinition *obj = current->pop();
    MDefinition *id = current->pop();

    // Ensure id is an integer.
    MInstruction *idInt32 = MToInt32::New(id);
    current->add(idInt32);
    id = idInt32;

    // Get the elements vector.
    MElements *elements = MElements::New(obj);
    current->add(elements);

    MInitializedLength *initLength = MInitializedLength::New(elements);
    current->add(initLength);

    // Check if id < initLength and elem[id] not a hole.
    MInArray *ins = MInArray::New(elements, id, initLength, obj, needsHoleCheck);

    current->add(ins);
    current->push(ins);

    return true;
}

bool
IonBuilder::jsop_instanceof()
{
    MDefinition *rhs = current->pop();
    MDefinition *obj = current->pop();

    TypeOracle::BinaryTypes types = oracle->binaryTypes(script(), pc);

    // If this is an 'x instanceof function' operation and we can determine the
    // exact function and prototype object being tested for, use a typed path.
    do {
        RawObject rhsObject = types.rhsTypes ? types.rhsTypes->getSingleton() : NULL;
        if (!rhsObject || !rhsObject->isFunction() || rhsObject->isBoundFunction())
            break;

        types::TypeObject *rhsType = rhsObject->getType(cx);
        if (!rhsType || rhsType->unknownProperties())
            break;

        types::HeapTypeSet *protoTypes =
            rhsType->getProperty(cx, NameToId(cx->names().classPrototype), false);
        RawObject protoObject = protoTypes ? protoTypes->getSingleton(cx) : NULL;
        if (!protoObject)
            break;

        MInstanceOf *ins = new MInstanceOf(obj, protoObject);

        current->add(ins);
        current->push(ins);

        return resumeAfter(ins);
    } while (false);

    MCallInstanceOf *ins = new MCallInstanceOf(obj, rhs);

    current->add(ins);
    current->push(ins);

    return resumeAfter(ins);
}

MInstruction *
IonBuilder::addConvertElementsToDoubles(MDefinition *elements)
{
    MInstruction *convert = MConvertElementsToDoubles::New(elements);
    current->add(convert);
    return convert;
}

MInstruction *
IonBuilder::addBoundsCheck(MDefinition *index, MDefinition *length)
{
    MInstruction *check = MBoundsCheck::New(index, length);
    current->add(check);

    // If a bounds check failed in the past, don't optimize bounds checks.
    if (failedBoundsCheck_)
        check->setNotMovable();

    return check;
}

MInstruction *
IonBuilder::addShapeGuard(MDefinition *obj, const RawShape shape, BailoutKind bailoutKind)
{
    MGuardShape *guard = MGuardShape::New(obj, shape, bailoutKind);
    current->add(guard);

    // If a shape guard failed in the past, don't optimize shape guard.
    if (failedShapeGuard_)
        guard->setNotMovable();

    return guard;
}

const types::StackTypeSet *
IonBuilder::cloneTypeSet(const types::StackTypeSet *types)
{
    if (!js_IonOptions.parallelCompilation)
        return types;

    // Clone a type set so that it can be stored into the MIR and accessed
    // during off thread compilation. This is necessary because main thread
    // updates to type sets can race with reads in the compiler backend, and
    // after bug 804676 this code can be removed.
    return types->clone(GetIonContext()->temp->lifoAlloc());
}
