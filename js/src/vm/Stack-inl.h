/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=79 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef Stack_inl_h__
#define Stack_inl_h__

#include "jscntxt.h"
#include "jscompartment.h"

#include "methodjit/MethodJIT.h"
#include "vm/Stack.h"

#include "jsscriptinlines.h"

#include "ArgumentsObject-inl.h"
#include "ScopeObject-inl.h"


namespace js {

/*
 * We cache name lookup results only for the global object or for native
 * non-global objects without prototype or with prototype that never mutates,
 * see bug 462734 and bug 487039.
 */
static inline bool
IsCacheableNonGlobalScope(JSObject *obj)
{
    bool cacheable = (obj->isCall() || obj->isBlock() || obj->isDeclEnv());

    JS_ASSERT_IF(cacheable, !obj->getOps()->lookupProperty);
    return cacheable;
}

inline HandleObject
StackFrame::scopeChain() const
{
    JS_ASSERT_IF(!(flags_ & HAS_SCOPECHAIN), isFunctionFrame());
    if (!(flags_ & HAS_SCOPECHAIN)) {
        scopeChain_ = callee().environment();
        flags_ |= HAS_SCOPECHAIN;
    }
    return HandleObject::fromMarkedLocation(&scopeChain_);
}

inline GlobalObject &
StackFrame::global() const
{
    return scopeChain()->global();
}

inline JSObject &
StackFrame::varObj()
{
    JSObject *obj = scopeChain();
    while (!obj->isVarObj())
        obj = obj->enclosingScope();
    return *obj;
}

inline JSCompartment *
StackFrame::compartment() const
{
    JS_ASSERT(scopeChain()->compartment() == script()->compartment());
    return scopeChain()->compartment();
}

#ifdef JS_METHODJIT
inline mjit::JITScript *
StackFrame::jit()
{
    JSScript *script_ = script();
    return script_->getJIT(isConstructing(), script_->compartment()->compileBarriers());
}
#endif

inline void
StackFrame::initPrev(JSContext *cx)
{
    JS_ASSERT(flags_ & HAS_PREVPC);
    if (FrameRegs *regs = cx->maybeRegs()) {
        prev_ = regs->fp();
        prevpc_ = regs->pc;
        prevInline_ = regs->inlined();
        JS_ASSERT(uint32_t(prevpc_ - prev_->script()->code) < prev_->script()->length);
    } else {
        prev_ = NULL;
#ifdef DEBUG
        prevpc_ = (jsbytecode *)0xbadc;
        prevInline_ = (InlinedSite *)0xbadc;
#endif
    }
}

inline void
StackFrame::resetGeneratorPrev(JSContext *cx)
{
    flags_ |= HAS_PREVPC;
    initPrev(cx);
}

inline void
StackFrame::initInlineFrame(JSFunction *fun, StackFrame *prevfp, jsbytecode *prevpc)
{
    /*
     * Note: no need to ensure the scopeChain is instantiated for inline
     * frames. Functions which use the scope chain are never inlined.
     */
    flags_ = StackFrame::FUNCTION;
    exec.fun = fun;
    resetInlinePrev(prevfp, prevpc);

    if (prevfp->hasPushedSPSFrame())
        setPushedSPSFrame();
}

inline void
StackFrame::resetInlinePrev(StackFrame *prevfp, jsbytecode *prevpc)
{
    JS_ASSERT_IF(flags_ & StackFrame::HAS_PREVPC, prevInline_);
    flags_ |= StackFrame::HAS_PREVPC;
    prev_ = prevfp;
    prevpc_ = prevpc;
    prevInline_ = NULL;
}

inline void
StackFrame::initCallFrame(JSContext *cx, JSFunction &callee,
                          JSScript *script, uint32_t nactual, StackFrame::Flags flagsArg)
{
    JS_ASSERT((flagsArg & ~(CONSTRUCTING |
                            LOWERED_CALL_APPLY |
                            OVERFLOW_ARGS |
                            UNDERFLOW_ARGS)) == 0);
    JS_ASSERT(script == callee.script());

    /* Initialize stack frame members. */
    flags_ = FUNCTION | HAS_PREVPC | HAS_SCOPECHAIN | HAS_BLOCKCHAIN | flagsArg;
    exec.fun = &callee;
    u.nactual = nactual;
    scopeChain_ = callee.environment();
    ncode_ = NULL;
    initPrev(cx);
    blockChain_= NULL;
    JS_ASSERT(!hasBlockChain());
    JS_ASSERT(!hasHookData());
    JS_ASSERT(annotation() == NULL);

    initVarsToUndefined();
}

/*
 * Reinitialize the StackFrame fields that have been initialized up to the
 * point of FixupArity in the function prologue.
 */
inline void
StackFrame::initFixupFrame(StackFrame *prev, StackFrame::Flags flags, void *ncode, unsigned nactual)
{
    JS_ASSERT((flags & ~(CONSTRUCTING |
                         LOWERED_CALL_APPLY |
                         FUNCTION |
                         OVERFLOW_ARGS |
                         UNDERFLOW_ARGS)) == 0);

    flags_ = FUNCTION | flags;
    prev_ = prev;
    ncode_ = ncode;
    u.nactual = nactual;
}

inline bool
StackFrame::jitHeavyweightFunctionPrologue(JSContext *cx)
{
    JS_ASSERT(isNonEvalFunctionFrame());
    JS_ASSERT(fun()->isHeavyweight());

    CallObject *callobj = CallObject::createForFunction(cx, this);
    if (!callobj)
        return false;

    pushOnScopeChain(*callobj);
    flags_ |= HAS_CALL_OBJ;

    return true;
}

inline void
StackFrame::initVarsToUndefined()
{
    SetValueRangeToUndefined(slots(), script()->nfixed);
}

inline JSObject *
StackFrame::createRestParameter(JSContext *cx)
{
    JS_ASSERT(fun()->hasRest());
    unsigned nformal = fun()->nargs - 1, nactual = numActualArgs();
    unsigned nrest = (nactual > nformal) ? nactual - nformal : 0;
    return NewDenseCopiedArray(cx, nrest, actuals() + nformal);
}

inline Value &
StackFrame::unaliasedVar(unsigned i, MaybeCheckAliasing checkAliasing)
{
    JS_ASSERT_IF(checkAliasing, !script()->varIsAliased(i));
    JS_ASSERT(i < script()->nfixed);
    return slots()[i];
}

inline Value &
StackFrame::unaliasedLocal(unsigned i, MaybeCheckAliasing checkAliasing)
{
#ifdef DEBUG
    if (checkAliasing) {
        JS_ASSERT(i < script()->nslots);
        if (i < script()->nfixed) {
            JS_ASSERT(!script()->varIsAliased(i));
        } else {
            unsigned depth = i - script()->nfixed;
            for (StaticBlockObject *b = maybeBlockChain(); b; b = b->enclosingBlock()) {
                if (b->containsVarAtDepth(depth)) {
                    JS_ASSERT(!b->isAliased(depth - b->stackDepth()));
                    break;
                }
            }
        }
    }
#endif
    return slots()[i];
}

inline Value &
StackFrame::unaliasedFormal(unsigned i, MaybeCheckAliasing checkAliasing)
{
    JS_ASSERT(i < numFormalArgs());
    JS_ASSERT_IF(checkAliasing, !script()->argsObjAliasesFormals());
    if (checkAliasing && script()->formalIsAliased(i)) {
        while (true) {}
    }
    JS_ASSERT_IF(checkAliasing, !script()->formalIsAliased(i));
    return formals()[i];
}

inline Value &
StackFrame::unaliasedActual(unsigned i, MaybeCheckAliasing checkAliasing)
{
    JS_ASSERT(i < numActualArgs());
    JS_ASSERT_IF(checkAliasing, !script()->argsObjAliasesFormals());
    JS_ASSERT_IF(checkAliasing && i < numFormalArgs(), !script()->formalIsAliased(i));
    return i < numFormalArgs() ? formals()[i] : actuals()[i];
}

template <class Op>
inline void
StackFrame::forEachUnaliasedActual(Op op)
{
    JS_ASSERT(!script()->funHasAnyAliasedFormal);
    JS_ASSERT(!script()->needsArgsObj());

    unsigned nformal = numFormalArgs();
    unsigned nactual = numActualArgs();

    const Value *formalsEnd = (const Value *)this;
    const Value *formals = formalsEnd - nformal;

    if (nactual <= nformal) {
        const Value *actualsEnd = formals + nactual;
        for (const Value *p = formals; p < actualsEnd; ++p)
            op(*p);
    } else {
        for (const Value *p = formals; p < formalsEnd; ++p)
            op(*p);

        const Value *actualsEnd = formals - 2;
        const Value *actuals = actualsEnd - nactual;
        for (const Value *p = actuals + nformal; p < actualsEnd; ++p)
            op(*p);
    }
}

struct CopyTo
{
    Value *dst;
    CopyTo(Value *dst) : dst(dst) {}
    void operator()(const Value &src) { *dst++ = src; }
};

inline unsigned
StackFrame::numFormalArgs() const
{
    JS_ASSERT(hasArgs());
    return fun()->nargs;
}

inline unsigned
StackFrame::numActualArgs() const
{
    /*
     * u.nactual is always coherent, except for method JIT frames where the
     * callee does not access its arguments and the number of actual arguments
     * matches the number of formal arguments. The JIT requires that all frames
     * which do not have an arguments object and use their arguments have a
     * coherent u.nactual (even though the below code may not use it), as
     * JIT code may access the field directly.
     */
    JS_ASSERT(hasArgs());
    if (JS_UNLIKELY(flags_ & (OVERFLOW_ARGS | UNDERFLOW_ARGS)))
        return u.nactual;
    return numFormalArgs();
}

inline ArgumentsObject &
StackFrame::argsObj() const
{
    JS_ASSERT(script()->needsArgsObj());
    JS_ASSERT(flags_ & HAS_ARGS_OBJ);
    return *argsObj_;
}

inline void
StackFrame::initArgsObj(ArgumentsObject &argsobj)
{
    JS_ASSERT(script()->needsArgsObj());
    flags_ |= HAS_ARGS_OBJ;
    argsObj_ = &argsobj;
}

inline ScopeObject &
StackFrame::aliasedVarScope(ScopeCoordinate sc) const
{
    JSObject *scope = &scopeChain()->asScope();
    for (unsigned i = sc.hops; i; i--)
        scope = &scope->asScope().enclosingScope();
    return scope->asScope();
}

inline void
StackFrame::pushOnScopeChain(ScopeObject &scope)
{
    JS_ASSERT(*scopeChain() == scope.enclosingScope() ||
              *scopeChain() == scope.asCall().enclosingScope().asDeclEnv().enclosingScope());
    scopeChain_ = &scope;
    flags_ |= HAS_SCOPECHAIN;
}

inline void
StackFrame::popOffScopeChain()
{
    JS_ASSERT(flags_ & HAS_SCOPECHAIN);
    scopeChain_ = &scopeChain_->asScope().enclosingScope();
}

inline CallObject &
StackFrame::callObj() const
{
    JS_ASSERT(fun()->isHeavyweight());

    JSObject *pobj = scopeChain();
    while (JS_UNLIKELY(!pobj->isCall()))
        pobj = pobj->enclosingScope();
    return pobj->asCall();
}

/*****************************************************************************/

STATIC_POSTCONDITION(!return || ubound(from) >= nvals)
JS_ALWAYS_INLINE bool
StackSpace::ensureSpace(JSContext *cx, MaybeReportError report, Value *from, ptrdiff_t nvals) const
{
    assertInvariants();
    JS_ASSERT(from >= firstUnused());
#ifdef XP_WIN
    JS_ASSERT(from <= commitEnd_);
#endif
    if (JS_UNLIKELY(conservativeEnd_ - from < nvals))
        return ensureSpaceSlow(cx, report, from, nvals);
    return true;
}

inline Value *
StackSpace::getStackLimit(JSContext *cx, MaybeReportError report)
{
    FrameRegs &regs = cx->regs();
    unsigned nvals = regs.fp()->script()->nslots + STACK_JIT_EXTRA;
    return ensureSpace(cx, report, regs.sp, nvals)
           ? conservativeEnd_
           : NULL;
}

/*****************************************************************************/

JS_ALWAYS_INLINE StackFrame *
ContextStack::getCallFrame(JSContext *cx, MaybeReportError report, const CallArgs &args,
                           JSFunction *fun, JSScript *script, StackFrame::Flags *flags) const
{
    JS_ASSERT(fun->script() == script);
    unsigned nformal = fun->nargs;

    Value *firstUnused = args.end();
    JS_ASSERT(firstUnused == space().firstUnused());

    /* Include extra space to satisfy the method-jit stackLimit invariant. */
    unsigned nvals = VALUES_PER_STACK_FRAME + script->nslots + StackSpace::STACK_JIT_EXTRA;

    /* Maintain layout invariant: &formals[0] == ((Value *)fp) - nformal. */

    if (args.length() == nformal) {
        if (!space().ensureSpace(cx, report, firstUnused, nvals))
            return NULL;
        return reinterpret_cast<StackFrame *>(firstUnused);
    }

    if (args.length() < nformal) {
        *flags = StackFrame::Flags(*flags | StackFrame::UNDERFLOW_ARGS);
        unsigned nmissing = nformal - args.length();
        if (!space().ensureSpace(cx, report, firstUnused, nmissing + nvals))
            return NULL;
        SetValueRangeToUndefined(firstUnused, nmissing);
        return reinterpret_cast<StackFrame *>(firstUnused + nmissing);
    }

    *flags = StackFrame::Flags(*flags | StackFrame::OVERFLOW_ARGS);
    unsigned ncopy = 2 + nformal;
    if (!space().ensureSpace(cx, report, firstUnused, ncopy + nvals))
        return NULL;
    Value *dst = firstUnused;
    Value *src = args.base();
    PodCopy(dst, src, ncopy);
    return reinterpret_cast<StackFrame *>(firstUnused + ncopy);
}

JS_ALWAYS_INLINE bool
ContextStack::pushInlineFrame(JSContext *cx, FrameRegs &regs, const CallArgs &args,
                              JSFunction &callee, JSScript *script,
                              InitialFrameFlags initial)
{
    JS_ASSERT(onTop());
    JS_ASSERT(regs.sp == args.end());
    /* Cannot assert callee == args.callee() since this is called from LeaveTree. */
    JS_ASSERT(script == callee.script());

    StackFrame::Flags flags = ToFrameFlags(initial);
    StackFrame *fp = getCallFrame(cx, REPORT_ERROR, args, &callee, script, &flags);
    if (!fp)
        return false;

    /* Initialize frame, locals, regs. */
    fp->initCallFrame(cx, callee, script, args.length(), flags);

    /*
     * N.B. regs may differ from the active registers, if the parent is about
     * to repoint the active registers to regs. See UncachedInlineCall.
     */
    regs.prepareToRun(*fp, script);
    return true;
}

JS_ALWAYS_INLINE bool
ContextStack::pushInlineFrame(JSContext *cx, FrameRegs &regs, const CallArgs &args,
                              JSFunction &callee, JSScript *script,
                              InitialFrameFlags initial, Value **stackLimit)
{
    if (!pushInlineFrame(cx, regs, args, callee, script, initial))
        return false;
    *stackLimit = space().conservativeEnd_;
    return true;
}

JS_ALWAYS_INLINE StackFrame *
ContextStack::getFixupFrame(JSContext *cx, MaybeReportError report,
                            const CallArgs &args, JSFunction *fun, JSScript *script,
                            void *ncode, InitialFrameFlags initial, Value **stackLimit)
{
    JS_ASSERT(onTop());
    JS_ASSERT(fun->script() == args.callee().toFunction()->script());
    JS_ASSERT(fun->script() == script);

    StackFrame::Flags flags = ToFrameFlags(initial);
    StackFrame *fp = getCallFrame(cx, report, args, fun, script, &flags);
    if (!fp)
        return NULL;

    /* Do not init late prologue or regs; this is done by jit code. */
    fp->initFixupFrame(cx->fp(), flags, ncode, args.length());

    *stackLimit = space().conservativeEnd_;
    return fp;
}

JS_ALWAYS_INLINE void
ContextStack::popInlineFrame(FrameRegs &regs)
{
    JS_ASSERT(onTop());
    JS_ASSERT(&regs == &seg_->regs());

    StackFrame *fp = regs.fp();
    Value *newsp = fp->actuals() - 1;
    JS_ASSERT(newsp >= fp->prev()->base());

    newsp[-1] = fp->returnValue();
    regs.popFrame(newsp);
}

inline void
ContextStack::popFrameAfterOverflow()
{
    /* Restore the regs to what they were on entry to JSOP_CALL. */
    FrameRegs &regs = seg_->regs();
    StackFrame *fp = regs.fp();
    regs.popFrame(fp->actuals() + fp->numActualArgs());
}

inline JSScript *
ContextStack::currentScript(jsbytecode **ppc,
                            MaybeAllowCrossCompartment allowCrossCompartment) const
{
    if (ppc)
        *ppc = NULL;

    if (!hasfp())
        return NULL;

    FrameRegs &regs = this->regs();
    StackFrame *fp = regs.fp();

#ifdef JS_METHODJIT
    mjit::CallSite *inlined = regs.inlined();
    if (inlined) {
        mjit::JITChunk *chunk = fp->jit()->chunk(regs.pc);
        JS_ASSERT(inlined->inlineIndex < chunk->nInlineFrames);
        mjit::InlineFrame *frame = &chunk->inlineFrames()[inlined->inlineIndex];
        JSScript *script = frame->fun->script();
        if (!allowCrossCompartment && script->compartment() != cx_->compartment)
            return NULL;
        if (ppc)
            *ppc = script->code + inlined->pcOffset;
        return script;
    }
#endif

    JSScript *script = fp->script();
    if (!allowCrossCompartment && script->compartment() != cx_->compartment)
        return NULL;

    if (ppc)
        *ppc = fp->pcQuadratic(*this);
    return script;
}

inline HandleObject
ContextStack::currentScriptedScopeChain() const
{
    return fp()->scopeChain();
}

} /* namespace js */
#endif /* Stack_inl_h__ */
