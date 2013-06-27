/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=79 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/DebugOnly.h"

#include "jscntxt.h"
#include "gc/Marking.h"
#include "methodjit/MethodJIT.h"
#ifdef JS_ION
#include "ion/IonFrames.h"
#include "ion/IonCompartment.h"
#include "ion/Bailouts.h"
#endif
#include "Stack.h"
#include "ForkJoin.h"

#include "jsgcinlines.h"
#include "jsobjinlines.h"
#include "jsinterpinlines.h"

#include "jsopcode.h"

#include "Stack-inl.h"

/* Includes to get to low-level memory-mapping functionality. */
#ifdef XP_WIN
# include "jswin.h"
#elif defined(XP_OS2)
# define INCL_DOSMEMMGR
# include <os2.h>
#else
# include <unistd.h>
# include <sys/mman.h>
# if !defined(MAP_ANONYMOUS)
#  if defined(MAP_ANON)
#   define MAP_ANONYMOUS MAP_ANON
#  else
#   define MAP_ANONYMOUS 0
#  endif
# endif
#endif

using namespace js;

using mozilla::DebugOnly;

/*****************************************************************************/

void
StackFrame::initExecuteFrame(RawScript script, StackFrame *prevLink, AbstractFramePtr prev,
                             FrameRegs *regs, const Value &thisv, JSObject &scopeChain,
                             ExecuteType type)
{
     /*
     * If |prev| is an interpreter frame, we can always prev-link to it.
     * If |prev| is a baseline JIT frame, we prev-link to its entry frame.
     */
    JS_ASSERT_IF(prev.isStackFrame(), prev.asStackFrame() == prevLink);
    JS_ASSERT_IF(prev, prevLink != NULL);

    /*
     * See encoding of ExecuteType. When GLOBAL isn't set, we are executing a
     * script in the context of another frame and the frame type is determined
     * by the context.
     */
    flags_ = type | HAS_SCOPECHAIN | HAS_BLOCKCHAIN | HAS_PREVPC;
    if (!(flags_ & GLOBAL)) {
        JS_ASSERT(prev.isFunctionFrame() || prev.isGlobalFrame());
        flags_ |= prev.isFunctionFrame() ? FUNCTION : GLOBAL;
    }

    Value *dstvp = (Value *)this - 2;
    dstvp[1] = thisv;

    if (isFunctionFrame()) {
        dstvp[0] = prev.calleev();
        exec.fun = prev.fun();
        u.evalScript = script;
    } else {
        JS_ASSERT(isGlobalFrame());
        dstvp[0] = NullValue();
        exec.script = script;
#ifdef DEBUG
        u.evalScript = (RawScript)0xbad;
#endif
    }

    scopeChain_ = &scopeChain;
    prev_ = prevLink;
    prevpc_ = regs ? regs->pc : (jsbytecode *)0xbad;
    prevInline_ = regs ? regs->inlined() : NULL;
    blockChain_ = NULL;

#ifdef DEBUG
    ncode_ = (void *)0xbad;
    Debug_SetValueRangeToCrashOnTouch(&rval_, 1);
    hookData_ = (void *)0xbad;
#endif
}

template <StackFrame::TriggerPostBarriers doPostBarrier>
void
StackFrame::copyFrameAndValues(JSContext *cx, Value *vp, StackFrame *otherfp,
                               const Value *othervp, Value *othersp)
{
    JS_ASSERT(vp == (Value *)this - ((Value *)otherfp - othervp));
    JS_ASSERT(othervp == otherfp->generatorArgsSnapshotBegin());
    JS_ASSERT(othersp >= otherfp->slots());
    JS_ASSERT(othersp <= otherfp->generatorSlotsSnapshotBegin() + otherfp->script()->nslots);
    JS_ASSERT((Value *)this - vp == (Value *)otherfp - othervp);

    /* Copy args, StackFrame, and slots. */
    const Value *srcend = otherfp->generatorArgsSnapshotEnd();
    Value *dst = vp;
    for (const Value *src = othervp; src < srcend; src++, dst++) {
        *dst = *src;
        if (doPostBarrier)
            HeapValue::writeBarrierPost(*dst, dst);
    }

    *this = *otherfp;
    unsetPushedSPSFrame();
    if (doPostBarrier)
        writeBarrierPost();

    srcend = othersp;
    dst = slots();
    for (const Value *src = otherfp->slots(); src < srcend; src++, dst++) {
        *dst = *src;
        if (doPostBarrier)
            HeapValue::writeBarrierPost(*dst, dst);
    }

    if (cx->compartment->debugMode())
        DebugScopes::onGeneratorFrameChange(otherfp, this, cx);
}

/* Note: explicit instantiation for js_NewGenerator located in jsiter.cpp. */
template
void StackFrame::copyFrameAndValues<StackFrame::NoPostBarrier>(
                                    JSContext *, Value *, StackFrame *, const Value *, Value *);
template
void StackFrame::copyFrameAndValues<StackFrame::DoPostBarrier>(
                                    JSContext *, Value *, StackFrame *, const Value *, Value *);

void
StackFrame::writeBarrierPost()
{
    /* This needs to follow the same rules as in StackFrame::mark. */
    if (scopeChain_)
        JSObject::writeBarrierPost(scopeChain_, (void *)&scopeChain_);
    if (flags_ & HAS_ARGS_OBJ)
        JSObject::writeBarrierPost(argsObj_, (void *)&argsObj_);
    if (isFunctionFrame()) {
        JSFunction::writeBarrierPost(exec.fun, (void *)&exec.fun);
        if (isEvalFrame())
            JSScript::writeBarrierPost(u.evalScript, (void *)&u.evalScript);
    } else {
        JSScript::writeBarrierPost(exec.script, (void *)&exec.script);
    }
    if (hasReturnValue())
        HeapValue::writeBarrierPost(rval_, &rval_);
}

JSGenerator *
StackFrame::maybeSuspendedGenerator(JSRuntime *rt)
{
    /*
     * A suspended generator's frame is embedded inside the JSGenerator object
     * instead of on the contiguous stack like all active frames.
     */
    if (!isGeneratorFrame() || rt->stackSpace.containsFast(this))
        return NULL;

    /*
     * Once we know we have a suspended generator frame, there is a static
     * offset from the frame's snapshot to beginning of the JSGenerator.
     */
    char *vp = reinterpret_cast<char *>(generatorArgsSnapshotBegin());
    char *p = vp - offsetof(JSGenerator, stackSnapshot);
    JSGenerator *gen = reinterpret_cast<JSGenerator *>(p);
    JS_ASSERT(gen->fp == this);
    return gen;
}

jsbytecode *
StackFrame::prevpcSlow(InlinedSite **pinlined)
{
    JS_ASSERT(!(flags_ & HAS_PREVPC));
#if defined(JS_METHODJIT) && defined(JS_MONOIC)
    StackFrame *p = prev();
    mjit::JITScript *jit = p->script()->getJIT(p->isConstructing(),
                                               p->compartment()->zone()->compileBarriers());
    prevpc_ = jit->nativeToPC(ncode_, &prevInline_);
    flags_ |= HAS_PREVPC;
    if (pinlined)
        *pinlined = prevInline_;
    return prevpc_;
#else
    JS_NOT_REACHED("Unknown PC for frame");
    return NULL;
#endif
}

jsbytecode *
StackFrame::pcQuadratic(const ContextStack &stack, size_t maxDepth)
{
    StackSegment &seg = stack.space().containingSegment(this);
    FrameRegs &regs = seg.regs();

    /*
     * This isn't just an optimization; seg->computeNextFrame(fp) is only
     * defined if fp != seg->regs->fp.
     */
    if (regs.fp() == this)
        return regs.pc;

    /*
     * To compute fp's pc, we need the next frame (where next->prev == fp).
     * This requires a linear search which we allow the caller to limit (in
     * cases where we do not have a hard requirement to find the correct pc).
     */
    if (StackFrame *next = seg.computeNextFrame(this, maxDepth))
        return next->prevpc();

    /* If we hit the limit, just return the beginning of the script. */
    return regs.fp()->script()->code;
}

bool
StackFrame::copyRawFrameSlots(AutoValueVector *vec)
{
    if (!vec->resize(numFormalArgs() + script()->nfixed))
        return false;
    PodCopy(vec->begin(), formals(), numFormalArgs());
    PodCopy(vec->begin() + numFormalArgs(), slots(), script()->nfixed);
    return true;
}

static void
CleanupTornValue(StackFrame *fp, Value *vp)
{
    if (vp->isObject() && !vp->toGCThing())
        vp->setObject(fp->global());
    if (vp->isString() && !vp->toGCThing())
        vp->setString(fp->compartment()->rt->emptyString);
}

void
StackFrame::cleanupTornValues()
{
    for (size_t i = 0; i < numFormalArgs(); i++)
        CleanupTornValue(this, &formals()[i]);
    for (size_t i = 0; i < script()->nfixed; i++)
        CleanupTornValue(this, &slots()[i]);
}

static inline void
AssertDynamicScopeMatchesStaticScope(JSContext *cx, JSScript *script, JSObject *scope)
{
#ifdef DEBUG
    RootedObject enclosingScope(cx, script->enclosingStaticScope());
    for (StaticScopeIter i(cx, enclosingScope); !i.done(); i++) {
        if (i.hasDynamicScopeObject()) {
            /*
             * 'with' does not participate in the static scope of the script,
             * but it does in the dynamic scope, so skip them here.
             */
            while (scope->isWith())
                scope = &scope->asWith().enclosingScope();

            switch (i.type()) {
              case StaticScopeIter::BLOCK:
                JS_ASSERT(i.block() == scope->asClonedBlock().staticBlock());
                scope = &scope->asClonedBlock().enclosingScope();
                break;
              case StaticScopeIter::FUNCTION:
                JS_ASSERT(scope->asCall().callee().nonLazyScript() == i.funScript());
                scope = &scope->asCall().enclosingScope();
                break;
              case StaticScopeIter::NAMED_LAMBDA:
                scope = &scope->asDeclEnv().enclosingScope();
                break;
            }
        }
    }

    /*
     * Ideally, we'd JS_ASSERT(!scope->isScope()) but the enclosing lexical
     * scope chain stops at eval() boundaries. See StaticScopeIter comment.
     */
#endif
}

bool
StackFrame::initFunctionScopeObjects(JSContext *cx)
{
    CallObject *callobj = CallObject::createForFunction(cx, this);
    if (!callobj)
        return false;
    pushOnScopeChain(*callobj);
    flags_ |= HAS_CALL_OBJ;
    return true;
}

bool
StackFrame::prologue(JSContext *cx)
{
    RootedScript script(cx, this->script());

    JS_ASSERT(!isGeneratorFrame());
    JS_ASSERT(cx->regs().pc == script->code);

    if (isEvalFrame()) {
        if (script->strict) {
            CallObject *callobj = CallObject::createForStrictEval(cx, this);
            if (!callobj)
                return false;
            pushOnScopeChain(*callobj);
            flags_ |= HAS_CALL_OBJ;
        }
        Probes::enterScript(cx, script, NULL, this);
        return true;
    }

    if (isGlobalFrame()) {
        Probes::enterScript(cx, script, NULL, this);
        return true;
    }

    JS_ASSERT(isNonEvalFunctionFrame());
    AssertDynamicScopeMatchesStaticScope(cx, script, scopeChain());

    if (fun()->isHeavyweight() && !initFunctionScopeObjects(cx))
        return false;

    if (isConstructing()) {
        RootedObject callee(cx, &this->callee());
        JSObject *obj = CreateThisForFunction(cx, callee, useNewType());
        if (!obj)
            return false;
        functionThis() = ObjectValue(*obj);
    }

    Probes::enterScript(cx, script, script->function(), this);
    return true;
}

void
StackFrame::epilogue(JSContext *cx)
{
    JS_ASSERT(!isYielding());
    JS_ASSERT(!hasBlockChain());

    RootedScript script(cx, this->script());
    Probes::exitScript(cx, script, script->function(), this);

    if (isEvalFrame()) {
        if (isStrictEvalFrame()) {
            JS_ASSERT_IF(hasCallObj(), scopeChain()->asCall().isForEval());
            if (cx->compartment->debugMode())
                DebugScopes::onPopStrictEvalScope(this);
        } else if (isDirectEvalFrame()) {
            if (isDebuggerFrame())
                JS_ASSERT(!scopeChain()->isScope());
        } else {
            /*
             * Debugger.Object.prototype.evalInGlobal creates indirect eval
             * frames scoped to the given global;
             * Debugger.Object.prototype.evalInGlobalWithBindings creates
             * indirect eval frames scoped to an object carrying the introduced
             * bindings.
             */
            if (isDebuggerFrame())
                JS_ASSERT(scopeChain()->isGlobal() || scopeChain()->enclosingScope()->isGlobal());
            else
                JS_ASSERT(scopeChain()->isGlobal());
        }
        return;
    }

    if (isGlobalFrame()) {
        JS_ASSERT(!scopeChain()->isScope());
        return;
    }

    JS_ASSERT(isNonEvalFunctionFrame());

    if (fun()->isHeavyweight())
        JS_ASSERT_IF(hasCallObj(), scopeChain()->asCall().callee().nonLazyScript() == script);
    else
        AssertDynamicScopeMatchesStaticScope(cx, script, scopeChain());

    if (cx->compartment->debugMode())
        DebugScopes::onPopCall(this, cx);


    if (isConstructing() && returnValue().isPrimitive())
        setReturnValue(ObjectValue(constructorThis()));
}

bool
StackFrame::jitStrictEvalPrologue(JSContext *cx)
{
    JS_ASSERT(isStrictEvalFrame());
    CallObject *callobj = CallObject::createForStrictEval(cx, this);
    if (!callobj)
        return false;

    pushOnScopeChain(*callobj);
    flags_ |= HAS_CALL_OBJ;
    return true;
}

bool
StackFrame::pushBlock(JSContext *cx, StaticBlockObject &block)
{
    JS_ASSERT_IF(hasBlockChain(), blockChain_ == block.enclosingBlock());

    if (block.needsClone()) {
        Rooted<StaticBlockObject *> blockHandle(cx, &block);
        ClonedBlockObject *clone = ClonedBlockObject::create(cx, blockHandle, this);
        if (!clone)
            return false;

        pushOnScopeChain(*clone);

        blockChain_ = blockHandle;
    } else {
        blockChain_ = &block;
    }

    flags_ |= HAS_BLOCKCHAIN;
    return true;
}

void
StackFrame::popBlock(JSContext *cx)
{
    JS_ASSERT(hasBlockChain());

    if (cx->compartment->debugMode())
        DebugScopes::onPopBlock(cx, this);

    if (blockChain_->needsClone()) {
        JS_ASSERT(scopeChain_->asClonedBlock().staticBlock() == *blockChain_);
        popOffScopeChain();
    }

    blockChain_ = blockChain_->enclosingBlock();
}

void
StackFrame::popWith(JSContext *cx)
{
    if (cx->compartment->debugMode())
        DebugScopes::onPopWith(this);

    JS_ASSERT(scopeChain()->isWith());
    popOffScopeChain();
}

void
StackFrame::mark(JSTracer *trc)
{
    /*
     * Normally we would use MarkRoot here, except that generators also take
     * this path. However, generators use a special write barrier when the stack
     * frame is copied to the floating frame. Therefore, no barrier is needed.
     */
    if (flags_ & HAS_SCOPECHAIN)
        gc::MarkObjectUnbarriered(trc, &scopeChain_, "scope chain");
    if (flags_ & HAS_ARGS_OBJ)
        gc::MarkObjectUnbarriered(trc, &argsObj_, "arguments");
    if (isFunctionFrame()) {
        gc::MarkObjectUnbarriered(trc, &exec.fun, "fun");
        if (isEvalFrame())
            gc::MarkScriptUnbarriered(trc, &u.evalScript, "eval script");
    } else {
        gc::MarkScriptUnbarriered(trc, &exec.script, "script");
    }
    if (IS_GC_MARKING_TRACER(trc))
        script()->compartment()->zone()->active = true;
    gc::MarkValueUnbarriered(trc, &returnValue(), "rval");
}

/*****************************************************************************/

bool
StackSegment::contains(const StackFrame *fp) const
{
    /* NB: this depends on the continuity of segments in memory. */
    return (Value *)fp >= slotsBegin() && (Value *)fp <= (Value *)maybefp();
}

bool
StackSegment::contains(const FrameRegs *regs) const
{
    return regs && contains(regs->fp());
}

bool
StackSegment::contains(const CallArgsList *call) const
{
    if (!call || !calls_)
        return false;

    /* NB: this depends on the continuity of segments in memory. */
    Value *vp = call->array();
    return vp > slotsBegin() && vp <= calls_->array();
}

StackFrame *
StackSegment::computeNextFrame(const StackFrame *f, size_t maxDepth) const
{
    JS_ASSERT(contains(f) && f != fp());

    StackFrame *next = fp();
    for (size_t i = 0; i <= maxDepth; ++i) {
        if (next->prev() == f)
            return next;
        next = next->prev();
    }

    return NULL;
}

Value *
StackSegment::end() const
{
    /* NB: this depends on the continuity of segments in memory. */
    JS_ASSERT_IF(calls_ || regs_, contains(calls_) || contains(regs_));
    Value *p = calls_
               ? regs_
                 ? Max(regs_->sp, calls_->end())
                 : calls_->end()
               : regs_
                 ? regs_->sp
                 : slotsBegin();
    JS_ASSERT(p >= slotsBegin());
    return p;
}

FrameRegs *
StackSegment::pushRegs(FrameRegs &regs)
{
    JS_ASSERT_IF(contains(regs_), regs.fp()->prev() == regs_->fp());
    FrameRegs *prev = regs_;
    regs_ = &regs;
    return prev;
}

void
StackSegment::popRegs(FrameRegs *regs)
{
    JS_ASSERT_IF(regs && contains(regs->fp()), regs->fp() == regs_->fp()->prev());
    regs_ = regs;
}

void
StackSegment::pushCall(CallArgsList &callList)
{
    callList.prev_ = calls_;
    calls_ = &callList;
}

void
StackSegment::pointAtCall(CallArgsList &callList)
{
    calls_ = &callList;
}

void
StackSegment::popCall()
{
    calls_ = calls_->prev_;
}

/*****************************************************************************/

StackSpace::StackSpace()
  : seg_(NULL),
    base_(NULL),
    conservativeEnd_(NULL),
#ifdef XP_WIN
    commitEnd_(NULL),
#endif
    defaultEnd_(NULL),
    trustedEnd_(NULL)
{
    assertInvariants();
}

bool
StackSpace::init()
{
    void *p;
#ifdef XP_WIN
    p = VirtualAlloc(NULL, CAPACITY_BYTES, MEM_RESERVE, PAGE_READWRITE);
    if (!p)
        return false;
    void *check = VirtualAlloc(p, COMMIT_BYTES, MEM_COMMIT, PAGE_READWRITE);
    if (p != check)
        return false;
    base_ = reinterpret_cast<Value *>(p);
    conservativeEnd_ = commitEnd_ = base_ + COMMIT_VALS;
    trustedEnd_ = base_ + CAPACITY_VALS;
    defaultEnd_ = trustedEnd_ - BUFFER_VALS;
    Debug_SetValueRangeToCrashOnTouch(base_, commitEnd_);
#elif defined(XP_OS2)
    if (DosAllocMem(&p, CAPACITY_BYTES, PAG_COMMIT | PAG_READ | PAG_WRITE | OBJ_ANY) &&
        DosAllocMem(&p, CAPACITY_BYTES, PAG_COMMIT | PAG_READ | PAG_WRITE))
        return false;
    base_ = reinterpret_cast<Value *>(p);
    trustedEnd_ = base_ + CAPACITY_VALS;
    conservativeEnd_ = defaultEnd_ = trustedEnd_ - BUFFER_VALS;
    Debug_SetValueRangeToCrashOnTouch(base_, trustedEnd_);
#else
    JS_ASSERT(CAPACITY_BYTES % getpagesize() == 0);
    p = mmap(NULL, CAPACITY_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return false;
    base_ = reinterpret_cast<Value *>(p);
    trustedEnd_ = base_ + CAPACITY_VALS;
    conservativeEnd_ = defaultEnd_ = trustedEnd_ - BUFFER_VALS;
    Debug_SetValueRangeToCrashOnTouch(base_, trustedEnd_);
#endif
    assertInvariants();
    return true;
}

StackSpace::~StackSpace()
{
    assertInvariants();
    JS_ASSERT(!seg_);
    if (!base_)
        return;
#ifdef XP_WIN
    VirtualFree(base_, (commitEnd_ - base_) * sizeof(Value), MEM_DECOMMIT);
    VirtualFree(base_, 0, MEM_RELEASE);
#elif defined(XP_OS2)
    DosFreeMem(base_);
#else
#ifdef SOLARIS
    munmap((caddr_t)base_, CAPACITY_BYTES);
#else
    munmap(base_, CAPACITY_BYTES);
#endif
#endif
}

StackSegment &
StackSpace::containingSegment(const StackFrame *target) const
{
    for (StackSegment *s = seg_; s; s = s->prevInMemory()) {
        if (s->contains(target))
            return *s;
    }
    JS_NOT_REACHED("frame not in stack space");
    return *(StackSegment *)NULL;
}

void
StackSpace::markFrame(JSTracer *trc, StackFrame *fp, Value *slotsEnd)
{
    /*
     * JM may leave values with object/string type but a null payload on the
     * stack. This can happen if the script was initially compiled by Ion,
     * which replaced dead values with undefined, and later ran under JM which
     * assumed values were of the original type.
     */
    Value *slotsBegin = fp->slots();
    gc::MarkValueRootRangeMaybeNullPayload(trc, slotsEnd - slotsBegin, slotsBegin, "vm_stack");
}

void
StackSpace::mark(JSTracer *trc)
{
    /* NB: this depends on the continuity of segments in memory. */
    Value *nextSegEnd = firstUnused();
    for (StackSegment *seg = seg_; seg; seg = seg->prevInMemory()) {
        /*
         * A segment describes a linear region of memory that contains a stack
         * of native and interpreted calls. For marking purposes, though, we
         * only need to distinguish between frames and values and mark
         * accordingly. Since native calls only push values on the stack, we
         * can effectively lump them together and just iterate over interpreted
         * calls. Thus, marking can view the stack as the regex:
         *   (segment slots (frame slots)*)*
         * which gets marked in reverse order.
         */
        Value *slotsEnd = nextSegEnd;
        for (StackFrame *fp = seg->maybefp(); (Value *)fp > (Value *)seg; fp = fp->prev()) {
            /* Mark from fp->slots() to slotsEnd. */
            markFrame(trc, fp, slotsEnd);

            fp->mark(trc);
            slotsEnd = (Value *)fp;

            InlinedSite *site;
            fp->prevpc(&site);
            JS_ASSERT_IF(fp->prev(), !site);
        }
        gc::MarkValueRootRange(trc, seg->slotsBegin(), slotsEnd, "vm_stack");
        nextSegEnd = (Value *)seg;
    }
}

void
StackSpace::markActiveCompartments()
{
    for (StackSegment *seg = seg_; seg; seg = seg->prevInMemory()) {
        for (StackFrame *fp = seg->maybefp(); (Value *)fp > (Value *)seg; fp = fp->prev())
            MarkCompartmentActive(fp);
    }
}

JS_FRIEND_API(bool)
StackSpace::ensureSpaceSlow(JSContext *cx, MaybeReportError report, Value *from, ptrdiff_t nvals) const
{
    assertInvariants();

    JSCompartment *dest = cx->compartment;
    bool trusted = dest->principals == cx->runtime->trustedPrincipals();
    Value *end = trusted ? trustedEnd_ : defaultEnd_;

    /*
     * conservativeEnd_ must stay below defaultEnd_: if conservativeEnd_ were
     * to be bumped past defaultEnd_, untrusted JS would be able to consume the
     * buffer space at the end of the stack reserved for trusted JS.
     */

    if (end - from < nvals) {
        if (report)
            js_ReportOverRecursed(cx);
        return false;
    }

#ifdef XP_WIN
    if (commitEnd_ - from < nvals) {
        Value *newCommit = commitEnd_;
        Value *request = from + nvals;

        /* Use a dumb loop; will probably execute once. */
        JS_ASSERT((trustedEnd_ - newCommit) % COMMIT_VALS == 0);
        do {
            newCommit += COMMIT_VALS;
            JS_ASSERT((trustedEnd_ - newCommit) >= 0);
        } while (newCommit < request);

        /* The cast is safe because CAPACITY_BYTES is small. */
        int32_t size = static_cast<int32_t>(newCommit - commitEnd_) * sizeof(Value);

        if (!VirtualAlloc(commitEnd_, size, MEM_COMMIT, PAGE_READWRITE)) {
            if (report)
                js_ReportOverRecursed(cx);
            return false;
        }

        Debug_SetValueRangeToCrashOnTouch(commitEnd_, newCommit);

        commitEnd_ = newCommit;
        conservativeEnd_ = Min(commitEnd_, defaultEnd_);
        assertInvariants();
    }
#endif

    return true;
}

bool
StackSpace::tryBumpLimit(JSContext *cx, Value *from, unsigned nvals, Value **limit)
{
    if (!ensureSpace(cx, REPORT_ERROR, from, nvals))
        return false;
    *limit = conservativeEnd_;
    return true;
}

size_t
StackSpace::sizeOf()
{
#if defined(XP_UNIX)
    /*
     * Measure how many of our pages are resident in RAM using mincore, and
     * return that as our size.  This is slow, but hopefully nobody expects
     * this method to be fast.
     *
     * Note that using mincore means that we don't count pages of the stack
     * which are swapped out to disk.  We really should, but what we have here
     * is better than counting the whole stack!
     */

    const int pageSize = getpagesize();
    size_t numBytes = (trustedEnd_ - base_) * sizeof(Value);
    size_t numPages = (numBytes + pageSize - 1) / pageSize;

    // On Linux, mincore's third argument has type unsigned char*.
#ifdef __linux__
    typedef unsigned char MincoreArgType;
#else
    typedef char MincoreArgType;
#endif

    MincoreArgType *vec = (MincoreArgType *) js_malloc(numPages);
    int result = mincore(base_, numBytes, vec);
    if (result) {
        js_free(vec);
        /*
         * If mincore fails us, return the vsize (like we do below if we're not
         * on Windows or Unix).
         */
        return (trustedEnd_ - base_) * sizeof(Value);
    }

    size_t residentBytes = 0;
    for (size_t i = 0; i < numPages; i++) {
        /* vec[i] has its least-significant bit set iff page i is in RAM. */
        if (vec[i] & 0x1)
            residentBytes += pageSize;
    }
    js_free(vec);
    return residentBytes;

#elif defined(XP_WIN)
    return (commitEnd_ - base_) * sizeof(Value);
#else
    /*
     * Return the stack's virtual size, which is at least an upper bound on its
     * resident size.
     */
    return (trustedEnd_ - base_) * sizeof(Value);
#endif
}

#ifdef DEBUG
bool
StackSpace::containsSlow(StackFrame *fp)
{
    if (!seg_)
        return false;
    for (AllFramesIter i(seg_->cx()->runtime); !i.done(); ++i) {
        /*
         * Debug-mode currently disables Ion compilation in the compartment of
         * the debuggee.
         */
        if (i.isIon())
            continue;
        if (i.interpFrame() == fp)
            return true;
    }
    return false;
}
#endif

/*****************************************************************************/

ContextStack::ContextStack(JSContext *cx)
  : seg_(NULL),
    space_(&cx->runtime->stackSpace),
    cx_(cx)
{}

ContextStack::~ContextStack()
{
    JS_ASSERT(!seg_);
}

bool
ContextStack::onTop() const
{
    return seg_ && seg_ == space().seg_;
}

/*
 * This helper function brings the ContextStack to the top of the thread stack
 * (so that it can be extended to push a frame and/or arguments) by potentially
 * pushing a StackSegment. The 'pushedSeg' outparam indicates whether such a
 * segment was pushed (and hence whether the caller needs to call popSegment).
 *
 * Additionally, to minimize calls to ensureSpace, ensureOnTop ensures that
 * there is space for nvars slots on top of the stack.
 */
Value *
ContextStack::ensureOnTop(JSContext *cx, MaybeReportError report, unsigned nvars,
                          MaybeExtend extend, bool *pushedSeg)
{
    Value *firstUnused = space().firstUnused();
    FrameRegs *regs = cx->maybeRegs();

#ifdef JS_METHODJIT
    /*
     * The only calls made by inlined methodjit frames can be to other JIT
     * frames associated with the same VMFrame. If we try to Invoke(),
     * Execute() or so forth, any topmost inline frame will need to be
     * expanded (along with other inline frames in the compartment).
     * To avoid pathological behavior here, make sure to mark any topmost
     * function as uninlineable, which will expand inline frames if there are
     * any and prevent the function from being inlined in the future.
     *
     * Note: When called from pushBailoutFrame, error = DONT_REPORT_ERROR. Use
     * this to deny potential invalidation, which would read from
     * runtime->ionTop.
     */
    if (regs && report != DONT_REPORT_ERROR) {
        RootedFunction fun(cx);
        if (InlinedSite *site = regs->inlined()) {
            mjit::JITChunk *chunk = regs->fp()->jit()->chunk(regs->pc);
            fun = chunk->inlineFrames()[site->inlineIndex].fun;
        } else {
            StackFrame *fp = regs->fp();
            if (fp->isFunctionFrame()) {
                JSFunction *f = fp->fun();
                if (f->isInterpreted())
                    fun = f;
            }
        }

        if (fun) {
            AutoCompartment ac(cx, fun);
            fun->nonLazyScript()->uninlineable = true;
            types::MarkTypeObjectFlags(cx, fun, types::OBJECT_FLAG_UNINLINEABLE);
        }
    }
    JS_ASSERT_IF(cx->hasfp(), !cx->regs().inlined());
#endif

    if (onTop() && extend) {
        if (!space().ensureSpace(cx, report, firstUnused, nvars))
            return NULL;
        return firstUnused;
    }

    if (!space().ensureSpace(cx, report, firstUnused, VALUES_PER_STACK_SEGMENT + nvars))
        return NULL;

    CallArgsList *calls;
    if (seg_ && extend) {
        regs = seg_->maybeRegs();
        calls = seg_->maybeCalls();
    } else {
        regs = NULL;
        calls = NULL;
    }

    seg_ = new(firstUnused) StackSegment(cx, seg_, space().seg_, regs, calls);
    space().seg_ = seg_;
    *pushedSeg = true;
    return seg_->slotsBegin();
}

void
ContextStack::popSegment()
{
    space().seg_ = seg_->prevInMemory();
    seg_ = seg_->prevInContext();

    if (!seg_)
        cx_->maybeMigrateVersionOverride();
}

bool
ContextStack::pushInvokeArgs(JSContext *cx, unsigned argc, InvokeArgsGuard *iag,
                             MaybeReportError report)
{
    JS_ASSERT(argc <= StackSpace::ARGS_LENGTH_MAX);

    unsigned nvars = 2 + argc;
    Value *firstUnused = ensureOnTop(cx, report, nvars, CAN_EXTEND, &iag->pushedSeg_);
    if (!firstUnused)
        return false;

    MakeRangeGCSafe(firstUnused, nvars);

    ImplicitCast<CallArgs>(*iag) = CallArgsFromVp(argc, firstUnused);

    seg_->pushCall(*iag);
    JS_ASSERT(space().firstUnused() == iag->end());
    iag->setPushed(*this);
    return true;
}

void
ContextStack::popInvokeArgs(const InvokeArgsGuard &iag)
{
    JS_ASSERT(iag.pushed());
    JS_ASSERT(onTop());
    JS_ASSERT(space().firstUnused() == seg_->calls().end());

    Value *oldend = seg_->end();

    seg_->popCall();
    if (iag.pushedSeg_)
        popSegment();

    Debug_SetValueRangeToCrashOnTouch(space().firstUnused(), oldend);
}

StackFrame *
ContextStack::pushInvokeFrame(JSContext *cx, MaybeReportError report,
                              const CallArgs &args, JSFunction *funArg,
                              InitialFrameFlags initial, FrameGuard *fg)
{
    JS_ASSERT(onTop());
    JS_ASSERT(space().firstUnused() == args.end());

    RootedFunction fun(cx, funArg);
    RootedScript script(cx, fun->nonLazyScript());

    StackFrame::Flags flags = ToFrameFlags(initial);
    StackFrame *fp = getCallFrame(cx, report, args, fun, script, &flags);
    if (!fp)
        return NULL;

    fp->initCallFrame(cx, *fun, script, args.length(), flags);
    fg->regs_.prepareToRun(*fp, script);

    fg->prevRegs_ = seg_->pushRegs(fg->regs_);
    JS_ASSERT(space().firstUnused() == fg->regs_.sp);
    fg->setPushed(*this);
    return fp;
}

bool
ContextStack::pushInvokeFrame(JSContext *cx, const CallArgs &args,
                              InitialFrameFlags initial, InvokeFrameGuard *ifg)
{
    JSObject &callee = args.callee();
    JSFunction *fun = callee.toFunction();
    if (!pushInvokeFrame(cx, REPORT_ERROR, args, fun, initial, ifg))
        return false;
    return true;
}

bool
ContextStack::pushExecuteFrame(JSContext *cx, HandleScript script, const Value &thisv,
                               HandleObject scopeChain, ExecuteType type,
                               AbstractFramePtr evalInFrame, ExecuteFrameGuard *efg)
{
    /*
     * Even though global code and indirect eval do not execute in the context
     * of the current frame, prev-link these to the current frame so that the
     * callstack looks right to the debugger (via CAN_EXTEND). This is safe
     * since the scope chain is what determines name lookup and access, not
     * prev-links.
     *
     * Eval-in-frame is the exception since it prev-links to an arbitrary frame
     * (possibly in the middle of some previous segment). Thus pass CANT_EXTEND
     * (to start a new segment) and link the frame and call chain manually
     * below. If |evalInFrame| is a baseline JIT frame, prev-link to its entry
     * frame.
     */
    CallArgsList *evalInFrameCalls = NULL;  /* quell overwarning */
    MaybeExtend extend;
    StackFrame *prevLink;
    if (evalInFrame) {
        /* First, find the right segment. */
        AllFramesIter frameIter(cx->runtime);
        while (frameIter.isIon() || frameIter.abstractFramePtr() != evalInFrame)
            ++frameIter;
        JS_ASSERT(frameIter.abstractFramePtr() == evalInFrame);

        StackSegment &seg = *frameIter.seg();

        StackIter iter(cx->runtime, seg);
        /* Debug-mode currently disables Ion compilation. */
        JS_ASSERT_IF(evalInFrame.isStackFrame(), !evalInFrame.asStackFrame()->runningInIon());
        JS_ASSERT_IF(evalInFrame.compartment() == iter.compartment(), !iter.isIon());
        while (!iter.isScript() || iter.isIon() || iter.abstractFramePtr() != evalInFrame) {
            ++iter;
            JS_ASSERT_IF(evalInFrame.compartment() == iter.compartment(), !iter.isIon());
        }
        JS_ASSERT(iter.abstractFramePtr() == evalInFrame);
        evalInFrameCalls = iter.data_.calls_;
        prevLink = iter.data_.fp_;
        extend = CANT_EXTEND;
    } else {
        prevLink = maybefp();
        extend = CAN_EXTEND;
    }

    unsigned nvars = 2 /* callee, this */ + VALUES_PER_STACK_FRAME + script->nslots;
    Value *firstUnused = ensureOnTop(cx, REPORT_ERROR, nvars, extend, &efg->pushedSeg_);
    if (!firstUnused)
        return false;

    AbstractFramePtr prev = evalInFrame ? evalInFrame : maybefp();
    StackFrame *fp = reinterpret_cast<StackFrame *>(firstUnused + 2);
    fp->initExecuteFrame(script, prevLink, prev, seg_->maybeRegs(), thisv, *scopeChain, type);
    fp->initVarsToUndefined();
    efg->regs_.prepareToRun(*fp, script);

    /* pushRegs() below links the prev-frame; manually link the prev-call. */
    if (evalInFrame && evalInFrameCalls)
        seg_->pointAtCall(*evalInFrameCalls);

    efg->prevRegs_ = seg_->pushRegs(efg->regs_);
    JS_ASSERT(space().firstUnused() == efg->regs_.sp);
    efg->setPushed(*this);
    return true;
}

#ifdef JS_ION
bool
ContextStack::pushBailoutArgs(JSContext *cx, const ion::IonBailoutIterator &it, InvokeArgsGuard *iag)
{
    unsigned argc = it.numActualArgs();

    if (!pushInvokeArgs(cx, argc, iag, DONT_REPORT_ERROR))
        return false;

    ion::SnapshotIterator s(it);
    JSFunction *fun = it.callee();
    iag->setCallee(ObjectValue(*fun));

    CopyTo dst(iag->array());
    Value *src = it.actualArgs();
    Value thisv = iag->thisv();
    s.readFrameArgs(dst, src, NULL, &thisv, 0, fun->nargs, argc);
    return true;
}

StackFrame *
ContextStack::pushBailoutFrame(JSContext *cx, const ion::IonBailoutIterator &it,
                               const CallArgs &args, BailoutFrameGuard *bfg)
{
    JSFunction *fun = it.callee();
    return pushInvokeFrame(cx, DONT_REPORT_ERROR, args, fun, INITIAL_NONE, bfg);
}
#endif

void
ContextStack::popFrame(const FrameGuard &fg)
{
    JS_ASSERT(fg.pushed());
    JS_ASSERT(onTop());
    JS_ASSERT(space().firstUnused() == fg.regs_.sp);
    JS_ASSERT(&fg.regs_ == &seg_->regs());

    Value *oldend = seg_->end();

    seg_->popRegs(fg.prevRegs_);
    if (fg.pushedSeg_)
        popSegment();

    Debug_SetValueRangeToCrashOnTouch(space().firstUnused(), oldend);
}

bool
ContextStack::pushGeneratorFrame(JSContext *cx, JSGenerator *gen, GeneratorFrameGuard *gfg)
{
    HeapValue *genvp = gen->stackSnapshot;
    JS_ASSERT(genvp == HeapValueify(gen->fp->generatorArgsSnapshotBegin()));
    unsigned vplen = HeapValueify(gen->fp->generatorArgsSnapshotEnd()) - genvp;

    unsigned nvars = vplen + VALUES_PER_STACK_FRAME + gen->fp->script()->nslots;
    Value *firstUnused = ensureOnTop(cx, REPORT_ERROR, nvars, CAN_EXTEND, &gfg->pushedSeg_);
    if (!firstUnused)
        return false;

    StackFrame *stackfp = reinterpret_cast<StackFrame *>(firstUnused + vplen);
    Value *stackvp = (Value *)stackfp - vplen;

    /* Save this for popGeneratorFrame. */
    gfg->gen_ = gen;
    gfg->stackvp_ = stackvp;

    /*
     * Trigger incremental barrier on the floating frame's generator object.
     * This is normally traced through only by associated arguments/call
     * objects, but only when the generator is not actually on the stack.
     * We don't need to worry about generational barriers as the generator
     * object has a trace hook and cannot be nursery allocated.
     */
    JS_ASSERT(gen->obj->getClass()->trace);
    JSObject::writeBarrierPre(gen->obj);

    /* Copy from the generator's floating frame to the stack. */
    stackfp->copyFrameAndValues<StackFrame::NoPostBarrier>(cx, stackvp, gen->fp,
                                                           Valueify(genvp), gen->regs.sp);
    stackfp->resetGeneratorPrev(cx);
    gfg->regs_.rebaseFromTo(gen->regs, *stackfp);

    gfg->prevRegs_ = seg_->pushRegs(gfg->regs_);
    JS_ASSERT(space().firstUnused() == gfg->regs_.sp);
    gfg->setPushed(*this);
    return true;
}

void
ContextStack::popGeneratorFrame(const GeneratorFrameGuard &gfg)
{
    JSGenerator *gen = gfg.gen_;
    HeapValue *genvp = gen->stackSnapshot;
    JS_ASSERT(genvp == HeapValueify(gen->fp->generatorArgsSnapshotBegin()));

    const FrameRegs &stackRegs = gfg.regs_;
    StackFrame *stackfp = stackRegs.fp();
    Value *stackvp = gfg.stackvp_;

    /* Copy from the stack to the generator's floating frame. */
    if (stackfp->isYielding()) {
        /*
         * Assert that the frame is not markable so that we don't need an
         * incremental write barrier when updating the generator's saved slots.
         */
        JS_ASSERT(!GeneratorHasMarkableFrame(gen));

        gen->regs.rebaseFromTo(stackRegs, *gen->fp);
        gen->fp->copyFrameAndValues<StackFrame::DoPostBarrier>(cx_, (Value *)genvp, stackfp,
                                                               stackvp, stackRegs.sp);
    }

    /* ~FrameGuard/popFrame will finish the popping. */
    JS_ASSERT(ImplicitCast<const FrameGuard>(gfg).pushed());
}

bool
ContextStack::saveFrameChain()
{
    bool pushedSeg;
    if (!ensureOnTop(cx_, REPORT_ERROR, 0, CANT_EXTEND, &pushedSeg))
        return false;

    JS_ASSERT(pushedSeg);
    JS_ASSERT(!hasfp());
    JS_ASSERT(onTop());
    JS_ASSERT(seg_->isEmpty());
    return true;
}

void
ContextStack::restoreFrameChain()
{
    JS_ASSERT(!hasfp());
    JS_ASSERT(onTop());
    JS_ASSERT(seg_->isEmpty());

    popSegment();
}

/*****************************************************************************/

void
StackIter::poisonRegs()
{
    data_.pc_ = (jsbytecode *)0xbad;
}

void
StackIter::popFrame()
{
    StackFrame *oldfp = data_.fp_;
    JS_ASSERT(data_.seg_->contains(oldfp));
    data_.fp_ = data_.fp_->prev();

    if (data_.seg_->contains(data_.fp_)) {
        InlinedSite *inline_;
        data_.pc_ = oldfp->prevpc(&inline_);
        JS_ASSERT(!inline_);
    } else {
        poisonRegs();
    }
}

void
StackIter::popCall()
{
    DebugOnly<CallArgsList*> oldCall = data_.calls_;
    JS_ASSERT(data_.seg_->contains(oldCall));
    data_.calls_ = data_.calls_->prev();
    if (!data_.seg_->contains(data_.fp_))
        poisonRegs();
}

void
StackIter::settleOnNewSegment()
{
    if (FrameRegs *regs = data_.seg_->maybeRegs())
        data_.pc_ = regs->pc;
    else
        poisonRegs();
}

void
StackIter::startOnSegment(StackSegment *seg)
{
    data_.seg_ = seg;
    data_.fp_ = data_.seg_->maybefp();
    data_.calls_ = data_.seg_->maybeCalls();
    settleOnNewSegment();
}

/*
 * Given that the iterator's current value of fp_ and calls_ (initialized on
 * construction or after operator++ popped the previous scripted/native call),
 * "settle" the iterator on a new StackIter::State value. The goal is to
 * present the client a simple linear sequence of native/scripted calls while
 * covering up unpleasant stack implementation details:
 *  - The frame change can be "saved" and "restored" (see JS_SaveFrameChain).
 *    This artificially cuts the call chain and the StackIter client may want
 *    to continue through this cut to the previous frame by passing
 *    GO_THROUGH_SAVED.
 *  - fp->prev can be in a different contiguous segment from fp. In this case,
 *    the current values of sp/pc after calling popFrame/popCall are incorrect
 *    and should be recovered from fp->prev's segment.
 *  - there is no explicit relationship to determine whether fp_ or calls_ is
 *    the innermost invocation so implicit memory ordering is used since both
 *    push values on the stack.
 *  - a native call's 'callee' argument is clobbered on return while the
 *    CallArgsList element is still visible.
 */
void
StackIter::settleOnNewState()
{
    /* Reset whether or we popped a call last time we settled. */
    data_.poppedCallDuringSettle_ = false;

    /*
     * There are elements of the calls_ and fp_ chains that we want to skip
     * over so iterate until we settle on one or until there are no more.
     */
    while (true) {
        if (!data_.fp_ && !data_.calls_) {
            if (data_.savedOption_ == GO_THROUGH_SAVED && data_.seg_->prevInContext()) {
                startOnSegment(data_.seg_->prevInContext());
                continue;
            }
            data_.state_ = DONE;
            return;
        }

        /* Check if popFrame/popCall changed segment. */
        bool containsFrame = data_.seg_->contains(data_.fp_);
        bool containsCall = data_.seg_->contains(data_.calls_);
        while (!containsFrame && !containsCall) {
            /* Eval-in-frame can cross contexts, so use prevInMemory. */
            data_.seg_ = data_.seg_->prevInMemory();
            containsFrame = data_.seg_->contains(data_.fp_);
            containsCall = data_.seg_->contains(data_.calls_);

            /* Eval-in-frame allows jumping into the middle of a segment. */
            if (containsFrame &&
                (data_.seg_->fp() != data_.fp_ || data_.seg_->maybeCalls() != data_.calls_))
            {
                /* Avoid duplicating logic; seg_ contains fp_, so no iloop. */
                StackIter tmp = *this;
                tmp.startOnSegment(data_.seg_);
                while (!tmp.isScript() || tmp.data_.fp_ != data_.fp_)
                    ++tmp;
                JS_ASSERT(tmp.isScript() &&
                          tmp.data_.seg_ == data_.seg_ &&
                          tmp.data_.fp_ == data_.fp_);
                *this = tmp;
                return;
            }

            /* There is no eval-in-frame equivalent for native calls. */
            JS_ASSERT_IF(containsCall, &data_.seg_->calls() == data_.calls_);

            settleOnNewSegment();
        }

        /*
         * In case of both a scripted frame and call record, use linear memory
         * ordering to decide which was the most recent.
         */
        if (containsFrame && (!containsCall || (Value *)data_.fp_ >= data_.calls_->array())) {
#ifdef JS_ION
            if (data_.fp_->beginsIonActivation()) {
                data_.ionFrames_ = ion::IonFrameIterator(data_.ionActivations_);

                if (data_.ionFrames_.isNative()) {
                    data_.state_ = ION;
                    return;
                }

                while (!data_.ionFrames_.isScripted() && !data_.ionFrames_.done())
                    ++data_.ionFrames_;

                // When invoked from JM, we don't re-use the entryfp, so we
                // may have an empty Ion activation.
                if (data_.ionFrames_.done()) {
                    data_.state_ = SCRIPTED;
                    return;
                }

                data_.state_ = ION;
                ionInlineFrames_.resetOn(&data_.ionFrames_);
                data_.pc_ = ionInlineFrames_.pc();
                return;
            }
#endif /* JS_ION */

            data_.state_ = SCRIPTED;
            return;
        }

        /*
         * A CallArgsList element is pushed for any call to Invoke, regardless
         * of whether the callee is a scripted function or even a callable
         * object. Thus, it is necessary to filter calleev for natives.
         *
         * Second, stuff can happen after the args are pushed but before/after
         * the actual call, so only consider "active" calls. (Since Invoke
         * necessarily clobbers the callee, "active" is also necessary to
         * ensure that the callee slot is valid.)
         */
        if (data_.calls_->active() && IsNativeFunction(data_.calls_->calleev())) {
            data_.state_ = NATIVE;
            data_.args_ = *data_.calls_;
            return;
        }

        /* Pop the call and keep looking. */
        popCall();
        data_.poppedCallDuringSettle_ = true;
    }
}

StackIter::Data::Data(JSContext *cx, PerThreadData *perThread, SavedOption savedOption)
  : perThread_(perThread),
    cx_(cx),
    savedOption_(savedOption),
    poppedCallDuringSettle_(false)
#ifdef JS_ION
    , ionActivations_(cx),
    ionFrames_((uint8_t *)NULL)
#endif
{
}

StackIter::Data::Data(JSContext *cx, JSRuntime *rt, StackSegment *seg)
  : perThread_(&rt->mainThread),
    cx_(cx),
    savedOption_(STOP_AT_SAVED),
    poppedCallDuringSettle_(false)
#ifdef JS_ION
    , ionActivations_(rt),
    ionFrames_((uint8_t *)NULL)
#endif
{
}

StackIter::Data::Data(const StackIter::Data &other)
  : perThread_(other.perThread_),
    cx_(other.cx_),
    savedOption_(other.savedOption_),
    state_(other.state_),
    fp_(other.fp_),
    calls_(other.calls_),
    seg_(other.seg_),
    pc_(other.pc_),
    args_(other.args_),
    poppedCallDuringSettle_(other.poppedCallDuringSettle_)
#ifdef JS_ION
    , ionActivations_(other.ionActivations_),
    ionFrames_(other.ionFrames_)
#endif
{
}

StackIter::StackIter(JSContext *cx, SavedOption savedOption)
  : data_(cx, &cx->runtime->mainThread, savedOption)
#ifdef JS_ION
    , ionInlineFrames_(cx, (js::ion::IonFrameIterator*) NULL)
#endif
{
#ifdef JS_METHODJIT
    for (ZonesIter zone(cx->runtime); !zone.done(); zone.next())
        mjit::ExpandInlineFrames(zone);
#endif

    if (StackSegment *seg = cx->stack.seg_) {
        startOnSegment(seg);
        settleOnNewState();
    } else {
        data_.state_ = DONE;
    }
}

StackIter::StackIter(JSRuntime *rt, StackSegment &seg)
  : data_(seg.cx(), rt, &seg)
#ifdef JS_ION
    , ionInlineFrames_(seg.cx(), (js::ion::IonFrameIterator*) NULL)
#endif
{
#ifdef JS_METHODJIT
    for (ZonesIter zone(rt); !zone.done(); zone.next())
        mjit::ExpandInlineFrames(zone);
#endif
    startOnSegment(&seg);
    settleOnNewState();
}

StackIter::StackIter(const StackIter &other)
  : data_(other.data_)
#ifdef JS_ION
    , ionInlineFrames_(other.data_.seg_->cx(),
                       data_.ionFrames_.isScripted() ? &other.ionInlineFrames_ : NULL)
#endif
{
}

StackIter::StackIter(const Data &data)
  : data_(data)
#ifdef JS_ION
    , ionInlineFrames_(data.cx_, data_.ionFrames_.isScripted() ? &data_.ionFrames_ : NULL)
#endif
{
    JS_ASSERT(data.cx_);
}

#ifdef JS_ION
void
StackIter::popIonFrame()
{
    // Keep fp which describes all ion frames.
    poisonRegs();
    if (data_.ionFrames_.isScripted() && ionInlineFrames_.more()) {
        ++ionInlineFrames_;
        data_.pc_ = ionInlineFrames_.pc();
    } else {
        ++data_.ionFrames_;
        while (!data_.ionFrames_.done() && !data_.ionFrames_.isScripted())
            ++data_.ionFrames_;

        if (!data_.ionFrames_.done()) {
            ionInlineFrames_.resetOn(&data_.ionFrames_);
            data_.pc_ = ionInlineFrames_.pc();
            return;
        }

        // The activation has no other frames. If entryfp is NULL, it was invoked
        // by a native written in C++, using FastInvoke, on top of another activation.
        ion::IonActivation *activation = data_.ionActivations_.activation();
        if (!activation->entryfp()) {
            JS_ASSERT(activation->prevpc());
            JS_ASSERT(data_.fp_->beginsIonActivation());
            ++data_.ionActivations_;
            settleOnNewState();
            return;
        }

        if (data_.fp_->runningInIon()) {
            ++data_.ionActivations_;
            popFrame();
            settleOnNewState();
        } else {
            JS_ASSERT(data_.fp_->callingIntoIon());
            data_.state_ = SCRIPTED;
            data_.pc_ = data_.ionActivations_.activation()->prevpc();
            ++data_.ionActivations_;
        }
    }
}
#endif

StackIter &
StackIter::operator++()
{
    switch (data_.state_) {
      case DONE:
        JS_NOT_REACHED("Unexpected state");
      case SCRIPTED:
        popFrame();
        settleOnNewState();
        break;
      case NATIVE:
        popCall();
        settleOnNewState();
        break;
      case ION:
#ifdef JS_ION
        popIonFrame();
        break;
#else
        JS_NOT_REACHED("Unexpected state");
#endif
    }
    return *this;
}

bool
StackIter::operator==(const StackIter &rhs) const
{
    return done() == rhs.done() &&
           (done() ||
            (isScript() == rhs.isScript() &&
             ((isScript() && data_.fp_ == rhs.data_.fp_) ||
              (!isScript() && nativeArgs().base() == rhs.nativeArgs().base()))));
}

StackIter::Data *
StackIter::copyData() const
{
#ifdef JS_ION
    /*
     * This doesn't work for optimized Ion frames since ionInlineFrames_ is
     * not copied.
     */
    JS_ASSERT(data_.ionFrames_.type() != ion::IonFrame_OptimizedJS);
#endif
    return data_.cx_->new_<Data>(data_);
}

JSCompartment *
StackIter::compartment() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case SCRIPTED:
        return data_.fp_->compartment();
      case ION:
#ifdef  JS_ION
        return data_.ionActivations_.activation()->compartment();
#else
        break;
#endif
      case NATIVE:
        return data_.calls_->callee().compartment();
    }
    JS_NOT_REACHED("Unexpected state");
    return NULL;
}

bool
StackIter::isFunctionFrame() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case SCRIPTED:
        return interpFrame()->isFunctionFrame();
      case ION:
#ifdef  JS_ION
        return ionInlineFrames_.isFunctionFrame();
#else
        break;
#endif
      case NATIVE:
        return false;
    }
    JS_NOT_REACHED("Unexpected state");
    return false;
}

bool
StackIter::isGlobalFrame() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case SCRIPTED:
        return interpFrame()->isGlobalFrame();
      case ION:
        JS_ASSERT(!script()->isForEval());
        return !script()->function();
      case NATIVE:
        return false;
    }
    JS_NOT_REACHED("Unexpected state");
    return false;
}

bool
StackIter::isEvalFrame() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case SCRIPTED:
        return interpFrame()->isEvalFrame();
      case ION:
      case NATIVE:
        return false;
    }
    JS_NOT_REACHED("Unexpected state");
    return false;
}

bool
StackIter::isNonEvalFunctionFrame() const
{
    JS_ASSERT(!done());
    switch (data_.state_) {
      case DONE:
        break;
      case SCRIPTED:
        return interpFrame()->isNonEvalFunctionFrame();
      case ION:
      case NATIVE:
        return !isEvalFrame() && isFunctionFrame();
    }
    JS_NOT_REACHED("Unexpected state");
    return false;
}

bool
StackIter::isGeneratorFrame() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case SCRIPTED:
        return interpFrame()->isGeneratorFrame();
      case ION:
      case NATIVE:
        return false;
    }
    JS_NOT_REACHED("Unexpected state");
    return false;
}

bool
StackIter::isConstructing() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case ION:
#ifdef JS_ION
        return ionInlineFrames_.isConstructing();
#else
        break;
#endif        
      case SCRIPTED:
      case NATIVE:
        return interpFrame()->isConstructing();
    }
    JS_NOT_REACHED("Unexpected state");
    return false;
}

AbstractFramePtr
StackIter::abstractFramePtr() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case ION:
        break;
      case SCRIPTED:
        JS_ASSERT(interpFrame());
        return AbstractFramePtr(interpFrame());
      case NATIVE:
        break;
    }
    JS_NOT_REACHED("Unexpected state");
    return NullFramePtr();
}

void
StackIter::updatePcQuadratic()
{
    switch (data_.state_) {
      case DONE:
        break;
      case SCRIPTED:
        data_.pc_ = interpFrame()->pcQuadratic(data_.cx_);
        return;
      case ION:
        break;
      case NATIVE:
        break;
    }
    JS_NOT_REACHED("Unexpected state");
}

JSFunction *
StackIter::callee() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case SCRIPTED:
        JS_ASSERT(isFunctionFrame());
        return &interpFrame()->callee();
      case ION:
#ifdef JS_ION
        if (data_.ionFrames_.isScripted())
            return ionInlineFrames_.callee();
        JS_ASSERT(data_.ionFrames_.isNative());
        return data_.ionFrames_.callee();
#else
        break;
#endif
      case NATIVE:
        return nativeArgs().callee().toFunction();
    }
    JS_NOT_REACHED("Unexpected state");
    return NULL;
}

Value
StackIter::calleev() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case SCRIPTED:
        JS_ASSERT(isFunctionFrame());
        return interpFrame()->calleev();
      case ION:
#ifdef JS_ION
        return ObjectValue(*callee());
#else
        break;
#endif
      case NATIVE:
        return nativeArgs().calleev();
    }
    JS_NOT_REACHED("Unexpected state");
    return Value();
}

unsigned
StackIter::numActualArgs() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case SCRIPTED:
        JS_ASSERT(isFunctionFrame());
        return interpFrame()->numActualArgs();
      case ION:
#ifdef JS_ION
        return ionInlineFrames_.numActualArgs();
#else
        break;
#endif
      case NATIVE:
        return nativeArgs().length();
    }
    JS_NOT_REACHED("Unexpected state");
    return 0;
}

Value
StackIter::unaliasedActual(unsigned i, MaybeCheckAliasing checkAliasing) const
{
    switch (data_.state_) {
      case DONE:
        break;
      case SCRIPTED:
        return interpFrame()->unaliasedActual(i, checkAliasing);
      case ION:
        break;
      case NATIVE:
        break;
    }
    JS_NOT_REACHED("Unexpected state");
    return NullValue();
}

JSObject *
StackIter::scopeChain() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case ION:
#ifdef JS_ION
        return ionInlineFrames_.scopeChain();
#else
        break;
#endif
      case SCRIPTED:
        return interpFrame()->scopeChain();
      case NATIVE:
        break;
    }
    JS_NOT_REACHED("Unexpected state");
    return NULL;
}

CallObject &
StackIter::callObj() const
{
    JS_ASSERT(callee()->isHeavyweight());

    JSObject *pobj = scopeChain();
    while (!pobj->isCall())
        pobj = pobj->enclosingScope();
    return pobj->asCall();
}

bool
StackIter::hasArgsObj() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case ION:
        break;
      case SCRIPTED:
        return interpFrame()->hasArgsObj();
      case NATIVE:
        break;
    }
    JS_NOT_REACHED("Unexpected state");
    return false;
}

ArgumentsObject &
StackIter::argsObj() const
{
    JS_ASSERT(hasArgsObj());

    switch (data_.state_) {
      case DONE:
        break;
      case ION:
        break;
      case SCRIPTED:
        return interpFrame()->argsObj();
      case NATIVE:
        break;
    }
    JS_NOT_REACHED("Unexpected state");
    return interpFrame()->argsObj();
}

bool
StackIter::computeThis() const
{
    if (isScript() && !isIon()) {
        JS_ASSERT(data_.cx_);
        return ComputeThis(data_.cx_, interpFrame());
    }
    return true;
}

Value
StackIter::thisv() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case ION:
#ifdef JS_ION
        return ObjectValue(*ionInlineFrames_.thisObject());
#else
        break;
#endif
      case SCRIPTED:
      case NATIVE:
        return interpFrame()->thisValue();
    }
    JS_NOT_REACHED("Unexpected state");
    return NullValue();
}

Value
StackIter::returnValue() const
{
    switch (data_.state_) {
      case DONE:
        break;
      case ION:
        break;
      case SCRIPTED:
        return interpFrame()->returnValue();
      case NATIVE:
        break;
    }
    JS_NOT_REACHED("Unexpected state");
    return NullValue();
}

void
StackIter::setReturnValue(const Value &v)
{
    switch (data_.state_) {
      case DONE:
        break;
      case ION:
        break;
      case SCRIPTED:
        interpFrame()->setReturnValue(v);
        return;
      case NATIVE:
        break;
    }
    JS_NOT_REACHED("Unexpected state");
}

size_t
StackIter::numFrameSlots() const
{
    switch (data_.state_) {
      case DONE:
      case NATIVE:
        break;
      case ION:
#ifdef JS_ION
        return ionInlineFrames_.snapshotIterator().slots() - ionInlineFrames_.script()->nfixed;
#else
        break;
#endif
      case SCRIPTED:
        JS_ASSERT(data_.cx_);
        JS_ASSERT(data_.cx_->regs().spForStackDepth(0) == interpFrame()->base());
        return data_.cx_->regs().sp - interpFrame()->base();
    }
    JS_NOT_REACHED("Unexpected state");
    return 0;
}

Value
StackIter::frameSlotValue(size_t index) const
{
    switch (data_.state_) {
      case DONE:
      case NATIVE:
        break;
      case ION:
#ifdef JS_ION
      {
        ion::SnapshotIterator si(ionInlineFrames_.snapshotIterator());
        index += ionInlineFrames_.script()->nfixed;
        return si.maybeReadSlotByIndex(index);
      }
#else
        break;
#endif
      case SCRIPTED:
          return interpFrame()->base()[index];
    }
    JS_NOT_REACHED("Unexpected state");
    return NullValue();
}

/*****************************************************************************/

AllFramesIter::AllFramesIter(JSRuntime *rt)
  : seg_(rt->stackSpace.seg_),
    fp_(seg_ ? seg_->maybefp() : NULL)
#ifdef JS_ION
    , ionActivations_(rt),
    ionFrames_((uint8_t *)NULL)
#endif
{
    settleOnNewState();
}

#ifdef JS_ION
void
AllFramesIter::popIonFrame()
{
    JS_ASSERT(state_ == ION);

    ++ionFrames_;
    while (!ionFrames_.done() && !ionFrames_.isScripted())
        ++ionFrames_;

    if (!ionFrames_.done())
        return;

    // The activation has no other frames. If entryfp is NULL, it was invoked
    // by a native written in C++, using FastInvoke, on top of another activation.
    ion::IonActivation *activation = ionActivations_.activation();
    if (!activation->entryfp()) {
        JS_ASSERT(activation->prevpc());
        JS_ASSERT(fp_->beginsIonActivation());
        ++ionActivations_;
        settleOnNewState();
        return;
    }

    if (fp_->runningInIon()) {
        ++ionActivations_;
        fp_ = fp_->prev();
        settleOnNewState();
    } else {
        JS_ASSERT(fp_->callingIntoIon());
        state_ = SCRIPTED;
        ++ionActivations_;
    }
}
#endif

AllFramesIter&
AllFramesIter::operator++()
{
    switch (state_) {
      case SCRIPTED:
        fp_ = fp_->prev();
        settleOnNewState();
        break;
#ifdef JS_ION
      case ION:
        popIonFrame();
        break;
#endif
      case DONE:
      default:
        JS_NOT_REACHED("Unexpeced state");
    }
    return *this;
}

void
AllFramesIter::settleOnNewState()
{
    while (seg_ && (!fp_ || !seg_->contains(fp_))) {
        seg_ = seg_->prevInMemory();
        fp_ = seg_ ? seg_->maybefp() : NULL;
    }

    JS_ASSERT(!!seg_ == !!fp_);
    JS_ASSERT_IF(fp_, seg_->contains(fp_));

#ifdef JS_ION
    if (fp_ && fp_->beginsIonActivation()) {
        // Start at the first scripted frame.
        ionFrames_ = ion::IonFrameIterator(ionActivations_);
        while (!ionFrames_.isScripted() && !ionFrames_.done())
            ++ionFrames_;

        state_ = ionFrames_.done() ? SCRIPTED : ION;
        return;
    }
#endif

    state_ = fp_ ? SCRIPTED : DONE;
}

AbstractFramePtr
AllFramesIter::abstractFramePtr() const
{
    switch (state_) {
      case SCRIPTED:
        return AbstractFramePtr(interpFrame());
      case ION:
        break;
      case DONE:
        break;
    }
    JS_NOT_REACHED("Unexpected state");
    return NullFramePtr();
}

JSObject *
AbstractFramePtr::evalPrevScopeChain(JSRuntime *rt) const
{
    /* Find the stack segment containing this frame. */
    AllFramesIter alliter(rt);
    while (alliter.isIon() || alliter.abstractFramePtr() != *this)
        ++alliter;

    /* Eval frames are not compiled by Ion, though their caller might be. */
    StackIter iter(rt, *alliter.seg());
    while (!iter.isScript() || iter.isIon() || iter.abstractFramePtr() != *this)
        ++iter;
    ++iter;
    return iter.scopeChain();
}
