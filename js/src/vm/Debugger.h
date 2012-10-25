/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef Debugger_h__
#define Debugger_h__

#include "mozilla/Attributes.h"

#include "jsapi.h"
#include "jsclist.h"
#include "jscntxt.h"
#include "jscompartment.h"
#include "jsgc.h"
#include "jsweakmap.h"
#include "jswrapper.h"

#include "gc/Barrier.h"
#include "js/HashTable.h"
#include "vm/GlobalObject.h"

namespace js {

class Debugger {
    friend class Breakpoint;
    friend JSBool (::JS_DefineDebuggerObject)(JSContext *cx, JSObject *obj);

  public:
    enum Hook {
        OnDebuggerStatement,
        OnExceptionUnwind,
        OnNewScript,
        OnEnterFrame,
        HookCount
    };

    enum {
        JSSLOT_DEBUG_PROTO_START,
        JSSLOT_DEBUG_FRAME_PROTO = JSSLOT_DEBUG_PROTO_START,
        JSSLOT_DEBUG_ENV_PROTO,
        JSSLOT_DEBUG_OBJECT_PROTO,
        JSSLOT_DEBUG_SCRIPT_PROTO,
        JSSLOT_DEBUG_PROTO_STOP,
        JSSLOT_DEBUG_HOOK_START = JSSLOT_DEBUG_PROTO_STOP,
        JSSLOT_DEBUG_HOOK_STOP = JSSLOT_DEBUG_HOOK_START + HookCount,
        JSSLOT_DEBUG_COUNT = JSSLOT_DEBUG_HOOK_STOP
    };

  private:
    JSCList link;                       /* See JSRuntime::debuggerList. */
    HeapPtrObject object;               /* The Debugger object. Strong reference. */
    GlobalObjectSet debuggees;          /* Debuggee globals. Cross-compartment weak references. */
    js::HeapPtrObject uncaughtExceptionHook; /* Strong reference. */
    bool enabled;
    JSCList breakpoints;                /* cyclic list of all js::Breakpoints in this debugger */

    /*
     * Map from stack frames that are currently on the stack to Debugger.Frame
     * instances.
     *
     * The keys are always live stack frames. We drop them from this map as
     * soon as they leave the stack (see slowPathOnLeaveFrame) and in
     * removeDebuggee.
     *
     * We don't trace the keys of this map (the frames are on the stack and
     * thus necessarily live), but we do trace the values. It's like a WeakMap
     * that way, but since stack frames are not gc-things, the implementation
     * has to be different.
     */
    typedef HashMap<StackFrame *,
                    RelocatablePtrObject,
                    DefaultHasher<StackFrame *>,
                    RuntimeAllocPolicy> FrameMap;
    FrameMap frames;

    /* An ephemeral map from JSScript* to Debugger.Script instances. */
    typedef WeakMap<EncapsulatedPtrScript, RelocatablePtrObject> ScriptWeakMap;
    ScriptWeakMap scripts;

    /* The map from debuggee objects to their Debugger.Object instances. */
    typedef WeakMap<EncapsulatedPtrObject, RelocatablePtrObject> ObjectWeakMap;
    ObjectWeakMap objects;

    /* The map from debuggee Envs to Debugger.Environment instances. */
    ObjectWeakMap environments;

    class FrameRange;
    class ScriptQuery;

    bool addDebuggeeGlobal(JSContext *cx, Handle<GlobalObject*> obj);
    void removeDebuggeeGlobal(FreeOp *fop, GlobalObject *global,
                              GlobalObjectSet::Enum *compartmentEnum,
                              GlobalObjectSet::Enum *debugEnum);

    /*
     * Cope with an error or exception in a debugger hook.
     *
     * If callHook is true, then call the uncaughtExceptionHook, if any. If, in
     * addition, vp is non-null, then parse the value returned by
     * uncaughtExceptionHook as a resumption value.
     *
     * If there is no uncaughtExceptionHook, or if it fails, report and clear
     * the pending exception on ac.context and return JSTRAP_ERROR.
     *
     * This always calls ac.leave(); ac is a parameter because this method must
     * do some things in the debugger compartment and some things in the
     * debuggee compartment.
     */
    JSTrapStatus handleUncaughtException(Maybe<AutoCompartment> &ac, Value *vp, bool callHook);

    /*
     * Handle the result of a hook that is expected to return a resumption
     * value <https://wiki.mozilla.org/Debugger#Resumption_Values>. This is called
     * when we return from a debugging hook to debuggee code. The interpreter wants
     * a (JSTrapStatus, Value) pair telling it how to proceed.
     *
     * Precondition: ac is entered. We are in the debugger compartment.
     *
     * Postcondition: This called ac.leave(). See handleUncaughtException.
     *
     * If ok is false, the hook failed. If an exception is pending in
     * ac.context(), return handleUncaughtException(ac, vp, callhook).
     * Otherwise just return JSTRAP_ERROR.
     *
     * If ok is true, there must be no exception pending in ac.context(). rv may be:
     *     undefined - Return JSTRAP_CONTINUE to continue execution normally.
     *     {return: value} or {throw: value} - Call unwrapDebuggeeValue to
     *         unwrap value. Store the result in *vp and return JSTRAP_RETURN
     *         or JSTRAP_THROW. The interpreter will force the current frame to
     *         return or throw an exception.
     *     null - Return JSTRAP_ERROR to terminate the debuggee with an
     *         uncatchable error.
     *     anything else - Make a new TypeError the pending exception and
     *         return handleUncaughtException(ac, vp, callHook).
     */
    JSTrapStatus parseResumptionValue(Maybe<AutoCompartment> &ac, bool ok, const Value &rv,
                                      Value *vp, bool callHook = true);

    JSObject *unwrapDebuggeeArgument(JSContext *cx, const Value &v);

    static void traceObject(JSTracer *trc, JSObject *obj);
    void trace(JSTracer *trc);
    static void finalize(FreeOp *fop, JSObject *obj);
    void markKeysInCompartment(JSTracer *tracer);

    static Class jsclass;

    static Debugger *fromThisValue(JSContext *cx, const CallArgs &ca, const char *fnname);
    static JSBool getEnabled(JSContext *cx, unsigned argc, Value *vp);
    static JSBool setEnabled(JSContext *cx, unsigned argc, Value *vp);
    static JSBool getHookImpl(JSContext *cx, unsigned argc, Value *vp, Hook which);
    static JSBool setHookImpl(JSContext *cx, unsigned argc, Value *vp, Hook which);
    static JSBool getOnDebuggerStatement(JSContext *cx, unsigned argc, Value *vp);
    static JSBool setOnDebuggerStatement(JSContext *cx, unsigned argc, Value *vp);
    static JSBool getOnExceptionUnwind(JSContext *cx, unsigned argc, Value *vp);
    static JSBool setOnExceptionUnwind(JSContext *cx, unsigned argc, Value *vp);
    static JSBool getOnNewScript(JSContext *cx, unsigned argc, Value *vp);
    static JSBool setOnNewScript(JSContext *cx, unsigned argc, Value *vp);
    static JSBool getOnEnterFrame(JSContext *cx, unsigned argc, Value *vp);
    static JSBool setOnEnterFrame(JSContext *cx, unsigned argc, Value *vp);
    static JSBool getUncaughtExceptionHook(JSContext *cx, unsigned argc, Value *vp);
    static JSBool setUncaughtExceptionHook(JSContext *cx, unsigned argc, Value *vp);
    static JSBool addDebuggee(JSContext *cx, unsigned argc, Value *vp);
    static JSBool removeDebuggee(JSContext *cx, unsigned argc, Value *vp);
    static JSBool hasDebuggee(JSContext *cx, unsigned argc, Value *vp);
    static JSBool getDebuggees(JSContext *cx, unsigned argc, Value *vp);
    static JSBool getNewestFrame(JSContext *cx, unsigned argc, Value *vp);
    static JSBool clearAllBreakpoints(JSContext *cx, unsigned argc, Value *vp);
    static JSBool findScripts(JSContext *cx, unsigned argc, Value *vp);
    static JSBool wrap(JSContext *cx, unsigned argc, Value *vp);
    static JSBool construct(JSContext *cx, unsigned argc, Value *vp);
    static JSPropertySpec properties[];
    static JSFunctionSpec methods[];

    JSObject *getHook(Hook hook) const;
    bool hasAnyLiveHooks() const;

    static JSTrapStatus slowPathOnEnterFrame(JSContext *cx, Value *vp);
    static bool slowPathOnLeaveFrame(JSContext *cx, bool ok);
    static void slowPathOnNewScript(JSContext *cx, JSScript *script,
                                    GlobalObject *compileAndGoGlobal);
    static JSTrapStatus dispatchHook(JSContext *cx, Value *vp, Hook which);

    JSTrapStatus fireDebuggerStatement(JSContext *cx, Value *vp);
    JSTrapStatus fireExceptionUnwind(JSContext *cx, Value *vp);
    JSTrapStatus fireEnterFrame(JSContext *cx, Value *vp);

    /*
     * Allocate and initialize a Debugger.Script instance whose referent is
     * |script|.
     */
    JSObject *newDebuggerScript(JSContext *cx, HandleScript script);

    /*
     * Receive a "new script" event from the engine. A new script was compiled
     * or deserialized.
     */
    void fireNewScript(JSContext *cx, HandleScript script);

    static inline Debugger *fromLinks(JSCList *links);
    inline Breakpoint *firstBreakpoint() const;

  public:
    Debugger(JSContext *cx, JSObject *dbg);
    ~Debugger();

    bool init(JSContext *cx);
    inline const js::HeapPtrObject &toJSObject() const;
    inline js::HeapPtrObject &toJSObjectRef();
    static inline Debugger *fromJSObject(JSObject *obj);
    static Debugger *fromChildJSObject(JSObject *obj);

    /*********************************** Methods for interaction with the GC. */

    /*
     * A Debugger object is live if:
     *   * the Debugger JSObject is live (Debugger::trace handles this case); OR
     *   * it is in the middle of dispatching an event (the event dispatching
     *     code roots it in this case); OR
     *   * it is enabled, and it is debugging at least one live compartment,
     *     and at least one of the following is true:
     *       - it has a debugger hook installed
     *       - it has a breakpoint set on a live script
     *       - it has a watchpoint set on a live object.
     *
     * Debugger::markAllIteratively handles the last case. If it finds any
     * Debugger objects that are definitely live but not yet marked, it marks
     * them and returns true. If not, it returns false.
     */
    static void markCrossCompartmentDebuggerObjectReferents(JSTracer *tracer);
    static bool markAllIteratively(GCMarker *trc);
    static void sweepAll(FreeOp *fop);
    static void detachAllDebuggersFromGlobal(FreeOp *fop, GlobalObject *global,
                                             GlobalObjectSet::Enum *compartmentEnum);

    static inline JSTrapStatus onEnterFrame(JSContext *cx, Value *vp);
    static inline bool onLeaveFrame(JSContext *cx, bool ok);
    static inline JSTrapStatus onDebuggerStatement(JSContext *cx, Value *vp);
    static inline JSTrapStatus onExceptionUnwind(JSContext *cx, Value *vp);
    static inline void onNewScript(JSContext *cx, JSScript *script,
                                   GlobalObject *compileAndGoGlobal);
    static JSTrapStatus onTrap(JSContext *cx, Value *vp);
    static JSTrapStatus onSingleStep(JSContext *cx, Value *vp);

    /************************************* Functions for use by Debugger.cpp. */

    inline bool observesEnterFrame() const;
    inline bool observesNewScript() const;
    inline bool observesGlobal(GlobalObject *global) const;
    inline bool observesFrame(StackFrame *fp) const;
    bool observesScript(JSScript *script) const;

    /*
     * If env is NULL, call vp->setNull() and return true. Otherwise, find or
     * create a Debugger.Environment object for the given Env. On success,
     * store the Environment object in *vp and return true.
     */
    bool wrapEnvironment(JSContext *cx, Handle<Env*> env, Value *vp);

    /*
     * Like cx->compartment->wrap(cx, vp), but for the debugger compartment.
     *
     * Preconditions: *vp is a value from a debuggee compartment; cx is in the
     * debugger's compartment.
     *
     * If *vp is an object, this produces a (new or existing) Debugger.Object
     * wrapper for it. Otherwise this is the same as JSCompartment::wrap.
     */
    bool wrapDebuggeeValue(JSContext *cx, Value *vp);

    /*
     * Unwrap a Debug.Object, without rewrapping it for any particular debuggee
     * compartment.
     *
     * Preconditions: cx is in the debugger compartment. *vp is a value in that
     * compartment. (*vp should be a "debuggee value", meaning it is the
     * debugger's reflection of a value in the debuggee.)
     *
     * If *vp is a Debugger.Object, store the referent in *vp. Otherwise, if *vp
     * is an object, throw a TypeError, because it is not a debuggee
     * value. Otherwise *vp is a primitive, so leave it alone.
     *
     * When passing values from the debuggee to the debugger:
     *     enter debugger compartment;
     *     call wrapDebuggeeValue;  // compartment- and debugger-wrapping
     *
     * When passing values from the debugger to the debuggee:
     *     call unwrapDebuggeeValue;  // debugger-unwrapping
     *     enter debuggee compartment;
     *     call cx->compartment->wrap;  // compartment-rewrapping
     *
     * (Extreme nerd sidebar: Unwrapping happens in two steps because there are
     * two different kinds of symmetry at work: regardless of which direction
     * we're going, we want any exceptions to be created and thrown in the
     * debugger compartment--mirror symmetry. But compartment wrapping always
     * happens in the target compartment--rotational symmetry.)
     */
    bool unwrapDebuggeeValue(JSContext *cx, Value *vp);

    /* Store the Debugger.Frame object for the frame fp in *vp. */
    bool getScriptFrame(JSContext *cx, StackFrame *fp, Value *vp);

    /*
     * Set |*status| and |*value| to a (JSTrapStatus, Value) pair reflecting a
     * standard SpiderMonkey call state: a boolean success value |ok|, a return
     * value |rv|, and a context |cx| that may or may not have an exception set.
     * If an exception was pending on |cx|, it is cleared (and |ok| is asserted
     * to be false).
     */
    static void resultToCompletion(JSContext *cx, bool ok, const Value &rv,
                                   JSTrapStatus *status, Value *value);

    /*
     * Set |*result| to a JavaScript completion value corresponding to |status|
     * and |value|. |value| should be the return value or exception value, not
     * wrapped as a debuggee value. |cx| must be in the debugger compartment.
     */
    bool newCompletionValue(JSContext *cx, JSTrapStatus status, Value value, Value *result);

    /*
     * Precondition: we are in the debuggee compartment (ac is entered) and ok
     * is true if the operation in the debuggee compartment succeeded, false on
     * error or exception.
     *
     * Postcondition: we are in the debugger compartment, having called
     * ac.leave() even if an error occurred.
     *
     * On success, a completion value is in vp and ac.context does not have a
     * pending exception. (This ordinarily returns true even if the ok argument
     * is false.)
     */
    bool receiveCompletionValue(Maybe<AutoCompartment> &ac, bool ok, Value val, Value *vp);

    /*
     * Return the Debugger.Script object for |script|, or create a new one if
     * needed. The context |cx| must be in the debugger compartment; |script|
     * must be a script in a debuggee compartment.
     */
    JSObject *wrapScript(JSContext *cx, HandleScript script);

  private:
    Debugger(const Debugger &) MOZ_DELETE;
    Debugger & operator=(const Debugger &) MOZ_DELETE;
};

class BreakpointSite {
    friend class Breakpoint;
    friend struct ::JSCompartment;
    friend struct ::JSScript;
    friend class Debugger;

  public:
    JSScript *script;
    jsbytecode * const pc;

  private:
    JSCList breakpoints;  /* cyclic list of all js::Breakpoints at this instruction */
    size_t enabledCount;  /* number of breakpoints in the list that are enabled */
    JSTrapHandler trapHandler;  /* jsdbgapi trap state */
    HeapValue trapClosure;

    void recompile(FreeOp *fop);

  public:
    BreakpointSite(JSScript *script, jsbytecode *pc);
    Breakpoint *firstBreakpoint() const;
    bool hasBreakpoint(Breakpoint *bp);
    bool hasTrap() const { return !!trapHandler; }

    void inc(FreeOp *fop);
    void dec(FreeOp *fop);
    void setTrap(FreeOp *fop, JSTrapHandler handler, const Value &closure);
    void clearTrap(FreeOp *fop, JSTrapHandler *handlerp = NULL, Value *closurep = NULL);
    void destroyIfEmpty(FreeOp *fop);
};

/*
 * Each Breakpoint is a member of two linked lists: its debugger's list and its
 * site's list.
 *
 * GC rules:
 *   - script is live, breakpoint exists, and debugger is enabled
 *      ==> debugger is live
 *   - script is live, breakpoint exists, and debugger is live
 *      ==> retain the breakpoint and the handler object is live
 *
 * Debugger::markAllIteratively implements these two rules. It uses
 * Debugger::hasAnyLiveHooks to check for rule 1.
 *
 * Nothing else causes a breakpoint to be retained, so if its script or
 * debugger is collected, the breakpoint is destroyed during GC sweep phase,
 * even if the debugger compartment isn't being GC'd. This is implemented in
 * JSCompartment::sweepBreakpoints.
 */
class Breakpoint {
    friend struct ::JSCompartment;
    friend class Debugger;

  public:
    Debugger * const debugger;
    BreakpointSite * const site;
  private:
    js::HeapPtrObject handler;
    JSCList debuggerLinks;
    JSCList siteLinks;

  public:
    static Breakpoint *fromDebuggerLinks(JSCList *links);
    static Breakpoint *fromSiteLinks(JSCList *links);
    Breakpoint(Debugger *debugger, BreakpointSite *site, JSObject *handler);
    void destroy(FreeOp *fop);
    Breakpoint *nextInDebugger();
    Breakpoint *nextInSite();
    const HeapPtrObject &getHandler() const { return handler; }
    HeapPtrObject &getHandlerRef() { return handler; }
};

Debugger *
Debugger::fromLinks(JSCList *links)
{
    unsigned char *p = reinterpret_cast<unsigned char *>(links);
    return reinterpret_cast<Debugger *>(p - offsetof(Debugger, link));
}

Breakpoint *
Debugger::firstBreakpoint() const
{
    if (JS_CLIST_IS_EMPTY(&breakpoints))
        return NULL;
    return Breakpoint::fromDebuggerLinks(JS_NEXT_LINK(&breakpoints));
}

const js::HeapPtrObject &
Debugger::toJSObject() const
{
    JS_ASSERT(object);
    return object;
}

js::HeapPtrObject &
Debugger::toJSObjectRef()
{
    JS_ASSERT(object);
    return object;
}

Debugger *
Debugger::fromJSObject(JSObject *obj)
{
    JS_ASSERT(js::GetObjectClass(obj) == &jsclass);
    return (Debugger *) obj->getPrivate();
}

bool
Debugger::observesEnterFrame() const
{
    return enabled && getHook(OnEnterFrame);
}

bool
Debugger::observesNewScript() const
{
    return enabled && getHook(OnNewScript);
}

bool
Debugger::observesGlobal(GlobalObject *global) const
{
    return debuggees.has(global);
}

bool
Debugger::observesFrame(StackFrame *fp) const
{
    return observesGlobal(&fp->global());
}

JSTrapStatus
Debugger::onEnterFrame(JSContext *cx, Value *vp)
{
    if (cx->compartment->getDebuggees().empty())
        return JSTRAP_CONTINUE;
    return slowPathOnEnterFrame(cx, vp);
}

bool
Debugger::onLeaveFrame(JSContext *cx, bool ok)
{
    /* Traps must be cleared from eval frames, see slowPathOnLeaveFrame. */
    bool evalTraps = cx->fp()->isEvalFrame() &&
                     cx->fp()->script()->hasAnyBreakpointsOrStepMode();
    if (!cx->compartment->getDebuggees().empty() || evalTraps)
        ok = slowPathOnLeaveFrame(cx, ok);
    return ok;
}

JSTrapStatus
Debugger::onDebuggerStatement(JSContext *cx, Value *vp)
{
    return cx->compartment->getDebuggees().empty()
           ? JSTRAP_CONTINUE
           : dispatchHook(cx, vp, OnDebuggerStatement);
}

JSTrapStatus
Debugger::onExceptionUnwind(JSContext *cx, Value *vp)
{
    return cx->compartment->getDebuggees().empty()
           ? JSTRAP_CONTINUE
           : dispatchHook(cx, vp, OnExceptionUnwind);
}

void
Debugger::onNewScript(JSContext *cx, JSScript *script, GlobalObject *compileAndGoGlobal)
{
    JS_ASSERT_IF(script->compileAndGo, compileAndGoGlobal);
    JS_ASSERT_IF(!script->compileAndGo, !compileAndGoGlobal);
    if (!script->compartment()->getDebuggees().empty())
        slowPathOnNewScript(cx, script, compileAndGoGlobal);
}

extern JSBool
EvaluateInEnv(JSContext *cx, Handle<Env*> env, StackFrame *fp, const jschar *chars,
              unsigned length, const char *filename, unsigned lineno, Value *rval);

}

#endif /* Debugger_h__ */
