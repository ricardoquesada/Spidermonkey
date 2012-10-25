/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsinterp_h___
#define jsinterp_h___
/*
 * JS interpreter interface.
 */
#include "jsprvtd.h"
#include "jspubtd.h"
#include "jsopcode.h"

#include "vm/Stack.h"

namespace js {

/* Implemented in jsdbgapi: */

/*
 * Announce to the debugger that the thread has entered a new JavaScript frame,
 * |fp|. Call whatever hooks have been registered to observe new frames, and
 * return a JSTrapStatus code indication how execution should proceed:
 *
 * - JSTRAP_CONTINUE: Continue execution normally.
 *
 * - JSTRAP_THROW: Throw an exception. ScriptDebugPrologue has set |cx|'s
 *   pending exception to the value to be thrown.
 *
 * - JSTRAP_ERROR: Terminate execution (as is done when a script is terminated
 *   for running too long). ScriptDebugPrologue has cleared |cx|'s pending
 *   exception.
 *
 * - JSTRAP_RETURN: Return from the new frame immediately. ScriptDebugPrologue
 *   has set |cx->fp()|'s return value appropriately.
 */
extern JSTrapStatus
ScriptDebugPrologue(JSContext *cx, StackFrame *fp);

/*
 * Announce to the debugger that the thread has exited a JavaScript frame, |fp|.
 * If |ok| is true, the frame is returning normally; if |ok| is false, the frame
 * is throwing an exception or terminating.
 *
 * Call whatever hooks have been registered to observe frame exits. Change cx's
 * current exception and |fp|'s return value to reflect the changes in behavior
 * the hooks request, if any. Return the new error/success value.
 *
 * This function may be called twice for the same outgoing frame; only the
 * first call has any effect. (Permitting double calls simplifies some
 * cases where an onPop handler's resumption value changes a return to a
 * throw, or vice versa: we can redirect to a complete copy of the
 * alternative path, containing its own call to ScriptDebugEpilogue.)
 */
extern bool
ScriptDebugEpilogue(JSContext *cx, StackFrame *fp, bool ok);

/*
 * For a given |call|, convert null/undefined |this| into the global object for
 * the callee and replace other primitives with boxed versions. This assumes
 * that call.callee() is not strict mode code. This is the special/slow case of
 * ComputeThis.
 */
extern bool
BoxNonStrictThis(JSContext *cx, const CallReceiver &call);

/*
 * Ensure that fp->thisValue() is the correct value of |this| for the scripted
 * call represented by |fp|. ComputeThis is necessary because fp->thisValue()
 * may be set to 'undefined' when 'this' should really be the global object (as
 * an optimization to avoid global-this computation).
 */
inline bool
ComputeThis(JSContext *cx, StackFrame *fp);

enum MaybeConstruct {
    NO_CONSTRUCT = INITIAL_NONE,
    CONSTRUCT = INITIAL_CONSTRUCT
};

extern bool
ReportIsNotFunction(JSContext *cx, const Value &v, MaybeConstruct construct = NO_CONSTRUCT);

extern bool
ReportIsNotFunction(JSContext *cx, const Value *vp, MaybeConstruct construct = NO_CONSTRUCT);

extern JSObject *
ValueToCallable(JSContext *cx, const Value *vp, MaybeConstruct construct = NO_CONSTRUCT);

inline JSFunction *
ReportIfNotFunction(JSContext *cx, const Value &v, MaybeConstruct construct = NO_CONSTRUCT)
{
    if (v.isObject() && v.toObject().isFunction())
        return v.toObject().toFunction();

    ReportIsNotFunction(cx, v, construct);
    return NULL;
}

/*
 * InvokeKernel assumes that the given args have been pushed on the top of the
 * VM stack. Additionally, if 'args' is contained in a CallArgsList, that they
 * have already been marked 'active'.
 */
extern bool
InvokeKernel(JSContext *cx, CallArgs args, MaybeConstruct construct = NO_CONSTRUCT);

/*
 * Invoke assumes that 'args' has been pushed (via ContextStack::pushInvokeArgs)
 * and is currently at the top of the VM stack.
 */
inline bool
Invoke(JSContext *cx, InvokeArgsGuard &args, MaybeConstruct construct = NO_CONSTRUCT)
{
    args.setActive();
    bool ok = InvokeKernel(cx, args, construct);
    args.setInactive();
    return ok;
}

/*
 * This Invoke overload places the least requirements on the caller: it may be
 * called at any time and it takes care of copying the given callee, this, and
 * arguments onto the stack.
 */
extern bool
Invoke(JSContext *cx, const Value &thisv, const Value &fval, unsigned argc, Value *argv,
       Value *rval);

/*
 * This helper takes care of the infinite-recursion check necessary for
 * getter/setter calls.
 */
extern bool
InvokeGetterOrSetter(JSContext *cx, JSObject *obj, const Value &fval, unsigned argc, Value *argv,
                     Value *rval);

/*
 * InvokeConstructor* implement a function call from a constructor context
 * (e.g. 'new') handling the the creation of the new 'this' object.
 */
extern bool
InvokeConstructorKernel(JSContext *cx, CallArgs args);

/* See the InvokeArgsGuard overload of Invoke. */
inline bool
InvokeConstructor(JSContext *cx, InvokeArgsGuard &args)
{
    args.setActive();
    bool ok = InvokeConstructorKernel(cx, ImplicitCast<CallArgs>(args));
    args.setInactive();
    return ok;
}

/* See the fval overload of Invoke. */
extern bool
InvokeConstructor(JSContext *cx, const Value &fval, unsigned argc, Value *argv, Value *rval);

/*
 * Executes a script with the given scopeChain/this. The 'type' indicates
 * whether this is eval code or global code. To support debugging, the
 * evalFrame parameter can point to an arbitrary frame in the context's call
 * stack to simulate executing an eval in that frame.
 */
extern bool
ExecuteKernel(JSContext *cx, HandleScript script, JSObject &scopeChain, const Value &thisv,
              ExecuteType type, StackFrame *evalInFrame, Value *result);

/* Execute a script with the given scopeChain as global code. */
extern bool
Execute(JSContext *cx, HandleScript script, JSObject &scopeChain, Value *rval);

/* Flags to toggle js::Interpret() execution. */
enum InterpMode
{
    JSINTERP_NORMAL    = 0, /* interpreter is running normally */
    JSINTERP_REJOIN    = 1, /* as normal, but the frame has already started */
    JSINTERP_SKIP_TRAP = 2  /* as REJOIN, but skip trap at first opcode */
};

/*
 * Execute the caller-initialized frame for a user-defined script or function
 * pointed to by cx->fp until completion or error.
 */
extern JS_NEVER_INLINE bool
Interpret(JSContext *cx, StackFrame *stopFp, InterpMode mode = JSINTERP_NORMAL);

extern bool
RunScript(JSContext *cx, JSScript *script, StackFrame *fp);

extern bool
StrictlyEqual(JSContext *cx, const Value &lval, const Value &rval, bool *equal);

extern bool
LooselyEqual(JSContext *cx, const Value &lval, const Value &rval, bool *equal);

/* === except that NaN is the same as NaN and -0 is not the same as +0. */
extern bool
SameValue(JSContext *cx, const Value &v1, const Value &v2, bool *same);

extern JSType
TypeOfValue(JSContext *cx, const Value &v);

extern JSBool
HasInstance(JSContext *cx, HandleObject obj, const js::Value *v, JSBool *bp);

/*
 * A linked list of the |FrameRegs regs;| variables belonging to all
 * js::Interpret C++ frames on this thread's stack.
 *
 * Note that this is *not* a list of all JS frames running under the
 * interpreter; that would include inlined frames, whose FrameRegs are
 * saved in various pieces in various places. Rather, this lists each
 * js::Interpret call's live 'regs'; when control returns to that call, it
 * will resume execution with this 'regs' instance.
 *
 * When Debugger puts a script in single-step mode, all js::Interpret
 * invocations that might be presently running that script must have
 * interrupts enabled. It's not practical to simply check
 * script->stepModeEnabled() at each point some callee could have changed
 * it, because there are so many places js::Interpret could possibly cause
 * JavaScript to run: each place an object might be coerced to a primitive
 * or a number, for example. So instead, we simply expose a list of the
 * 'regs' those frames are using, and let Debugger tweak the affected
 * js::Interpret frames when an onStep handler is established.
 *
 * Elements of this list are allocated within the js::Interpret stack
 * frames themselves; the list is headed by this thread's js::ThreadData.
 */
class InterpreterFrames {
  public:
    class InterruptEnablerBase {
      public:
        virtual void enable() const = 0;
    };

    InterpreterFrames(JSContext *cx, FrameRegs *regs, const InterruptEnablerBase &enabler);
    ~InterpreterFrames();

    /* If this js::Interpret frame is running |script|, enable interrupts. */
    inline void enableInterruptsIfRunning(JSScript *script);
    inline void enableInterruptsUnconditionally() { enabler.enable(); }

    InterpreterFrames *older;

  private:
    JSContext *context;
    FrameRegs *regs;
    const InterruptEnablerBase &enabler;
};

/*
 * Unwind block and scope chains to match the given depth. The function sets
 * fp->sp on return to stackDepth.
 */
extern void
UnwindScope(JSContext *cx, uint32_t stackDepth);

/*
 * Unwind for an uncatchable exception. This means not running finalizers, etc;
 * just preserving the basic engine stack invariants.
 */
extern void
UnwindForUncatchableException(JSContext *cx, const FrameRegs &regs);

extern bool
OnUnknownMethod(JSContext *cx, HandleObject obj, Value idval, MutableHandleValue vp);

class TryNoteIter
{
    const FrameRegs &regs;
    JSScript *script;
    uint32_t pcOffset;
    JSTryNote *tn, *tnEnd;
    void settle();
  public:
    TryNoteIter(const FrameRegs &regs);
    bool done() const;
    void operator++();
    JSTryNote *operator*() const { return tn; }
};

/************************************************************************/

/*
 * To really poison a set of values, using 'magic' or 'undefined' isn't good
 * enough since often these will just be ignored by buggy code (see bug 629974)
 * in debug builds and crash in release builds. Instead, we use a safe-for-crash
 * pointer.
 */
static JS_ALWAYS_INLINE void
Debug_SetValueRangeToCrashOnTouch(Value *beg, Value *end)
{
#ifdef DEBUG
    for (Value *v = beg; v != end; ++v)
        v->setObject(*reinterpret_cast<JSObject *>(0x42));
#endif
}

static JS_ALWAYS_INLINE void
Debug_SetValueRangeToCrashOnTouch(Value *vec, size_t len)
{
#ifdef DEBUG
    Debug_SetValueRangeToCrashOnTouch(vec, vec + len);
#endif
}

static JS_ALWAYS_INLINE void
Debug_SetValueRangeToCrashOnTouch(HeapValue *vec, size_t len)
{
#ifdef DEBUG
    Debug_SetValueRangeToCrashOnTouch((Value *) vec, len);
#endif
}

}  /* namespace js */

#endif /* jsinterp_h___ */
