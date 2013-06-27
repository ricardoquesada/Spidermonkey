/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <limits.h>

#include "vm/Debugger.h"
#include "jsapi.h"
#include "jscntxt.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jswrapper.h"
#include "jsgcinlines.h"
#include "jsinterpinlines.h"
#include "jsobjinlines.h"
#include "jsopcodeinlines.h"
#include "jscompartment.h"

#include "frontend/BytecodeCompiler.h"
#include "frontend/BytecodeEmitter.h"
#include "gc/Marking.h"
#include "methodjit/Retcon.h"
#include "js/Vector.h"

#include "gc/FindSCCs-inl.h"
#include "vm/Stack-inl.h"

using namespace js;

using js::frontend::IsIdentifier;
using mozilla::ArrayLength;
using mozilla::Maybe;


/*** Forward declarations ************************************************************************/

extern Class DebuggerFrame_class;

enum {
    JSSLOT_DEBUGFRAME_OWNER,
    JSSLOT_DEBUGFRAME_ARGUMENTS,
    JSSLOT_DEBUGFRAME_ONSTEP_HANDLER,
    JSSLOT_DEBUGFRAME_ONPOP_HANDLER,
    JSSLOT_DEBUGFRAME_COUNT
};

extern Class DebuggerArguments_class;

enum {
    JSSLOT_DEBUGARGUMENTS_FRAME,
    JSSLOT_DEBUGARGUMENTS_COUNT
};

extern Class DebuggerEnv_class;

enum {
    JSSLOT_DEBUGENV_OWNER,
    JSSLOT_DEBUGENV_COUNT
};

extern Class DebuggerObject_class;

enum {
    JSSLOT_DEBUGOBJECT_OWNER,
    JSSLOT_DEBUGOBJECT_COUNT
};

extern Class DebuggerScript_class;

enum {
    JSSLOT_DEBUGSCRIPT_OWNER,
    JSSLOT_DEBUGSCRIPT_COUNT
};


/*** Utils ***************************************************************************************/

bool
ReportMoreArgsNeeded(JSContext *cx, const char *name, unsigned required)
{
    JS_ASSERT(required > 0);
    JS_ASSERT(required <= 10);
    char s[2];
    s[0] = '0' + (required - 1);
    s[1] = '\0';
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_MORE_ARGS_NEEDED,
                         name, s, required == 2 ? "" : "s");
    return false;
}

#define REQUIRE_ARGC(name, n)                                                 \
    JS_BEGIN_MACRO                                                            \
        if (argc < (n))                                                       \
            return ReportMoreArgsNeeded(cx, name, n);                         \
    JS_END_MACRO

bool
ReportObjectRequired(JSContext *cx)
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NOT_NONNULL_OBJECT);
    return false;
}

bool
ValueToIdentifier(JSContext *cx, const Value &v, MutableHandleId id)
{
    if (!ValueToId<CanGC>(cx, v, id))
        return false;
    if (!JSID_IS_ATOM(id) || !IsIdentifier(JSID_TO_ATOM(id))) {
        RootedValue val(cx, v);
        js_ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                                 JSDVG_SEARCH_STACK, val, NullPtr(), "not an identifier", NULL);
        return false;
    }
    return true;
}

/*
 * A range of all the Debugger.Frame objects for a particular StackFrame.
 *
 * FIXME This checks only current debuggers, so it relies on a hack in
 * Debugger::removeDebuggeeGlobal to make sure only current debuggers have Frame
 * objects with .live === true.
 */
class Debugger::FrameRange
{
    AbstractFramePtr frame;

    /* The debuggers in |fp|'s compartment, or NULL if there are none. */
    GlobalObject::DebuggerVector *debuggers;

    /*
     * The index of the front Debugger.Frame's debugger in debuggers.
     * nextDebugger < debuggerCount if and only if the range is not empty.
     */
    size_t debuggerCount, nextDebugger;

    /*
     * If the range is not empty, this is front Debugger.Frame's entry in its
     * debugger's frame table.
     */
    FrameMap::Ptr entry;

  public:
    /*
     * Return a range containing all Debugger.Frame instances referring to |fp|.
     * |global| is |fp|'s global object; if NULL or omitted, we compute it
     * ourselves from |fp|.
     *
     * We keep an index into the compartment's debugger list, and a
     * FrameMap::Ptr into the current debugger's frame map. Thus, if the set of
     * debuggers in |fp|'s compartment changes, this range becomes invalid.
     * Similarly, if stack frames are added to or removed from frontDebugger(),
     * then the range's front is invalid until popFront is called.
     */
    FrameRange(AbstractFramePtr frame, GlobalObject *global = NULL)
      : frame(frame)
    {
        nextDebugger = 0;

        /* Find our global, if we were not given one. */
        if (!global)
            global = &frame.script()->global();

        /* The frame and global must match. */
        JS_ASSERT(&frame.script()->global() == global);

        /* Find the list of debuggers we'll iterate over. There may be none. */
        debuggers = global->getDebuggers();
        if (debuggers) {
            debuggerCount = debuggers->length();
            findNext();
        } else {
            debuggerCount = 0;
        }
    }

    bool empty() const {
        return nextDebugger >= debuggerCount;
    }

    JSObject *frontFrame() const {
        JS_ASSERT(!empty());
        return entry->value;
    }

    Debugger *frontDebugger() const {
        JS_ASSERT(!empty());
        return (*debuggers)[nextDebugger];
    }

    /*
     * Delete the front frame from its Debugger's frame map. After this call,
     * the range's front is invalid until popFront is called.
     */
    void removeFrontFrame() const {
        JS_ASSERT(!empty());
        frontDebugger()->frames.remove(entry);
    }

    void popFront() {
        JS_ASSERT(!empty());
        nextDebugger++;
        findNext();
    }

  private:
    /*
     * Either make this range refer to the first appropriate Debugger.Frame at
     * or after nextDebugger, or make it empty.
     */
    void findNext() {
        while (!empty()) {
            Debugger *dbg = (*debuggers)[nextDebugger];
            entry = dbg->frames.lookup(frame);
            if (entry)
                break;
            nextDebugger++;
        }
    }
};


/*** Breakpoints *********************************************************************************/

BreakpointSite::BreakpointSite(JSScript *script, jsbytecode *pc)
  : script(script), pc(pc), enabledCount(0),
    trapHandler(NULL), trapClosure(UndefinedValue())
{
    JS_ASSERT(!script->hasBreakpointsAt(pc));
    JS_INIT_CLIST(&breakpoints);
}

void
BreakpointSite::recompile(FreeOp *fop)
{
#ifdef JS_METHODJIT
    if (script->hasMJITInfo()) {
        mjit::Recompiler::clearStackReferences(fop, script);
        mjit::ReleaseScriptCode(fop, script);
    }
#endif
}

void
BreakpointSite::inc(FreeOp *fop)
{
    enabledCount++;
    if (enabledCount == 1 && !trapHandler)
        recompile(fop);
}

void
BreakpointSite::dec(FreeOp *fop)
{
    JS_ASSERT(enabledCount > 0);
    enabledCount--;
    if (enabledCount == 0 && !trapHandler)
        recompile(fop);
}

void
BreakpointSite::setTrap(FreeOp *fop, JSTrapHandler handler, const Value &closure)
{
    trapHandler = handler;
    trapClosure = closure;

    if (enabledCount == 0)
        recompile(fop);
}

void
BreakpointSite::clearTrap(FreeOp *fop, JSTrapHandler *handlerp, Value *closurep)
{
    if (handlerp)
        *handlerp = trapHandler;
    if (closurep)
        *closurep = trapClosure;

    trapHandler = NULL;
    trapClosure = UndefinedValue();
    if (enabledCount == 0) {
        if (!fop->runtime()->isHeapBusy()) {
            /* If the GC is running then the script is being destroyed. */
            recompile(fop);
        }
        destroyIfEmpty(fop);
    }
}

void
BreakpointSite::destroyIfEmpty(FreeOp *fop)
{
    if (JS_CLIST_IS_EMPTY(&breakpoints) && !trapHandler)
        script->destroyBreakpointSite(fop, pc);
}

Breakpoint *
BreakpointSite::firstBreakpoint() const
{
    if (JS_CLIST_IS_EMPTY(&breakpoints))
        return NULL;
    return Breakpoint::fromSiteLinks(JS_NEXT_LINK(&breakpoints));
}

bool
BreakpointSite::hasBreakpoint(Breakpoint *bp)
{
    for (Breakpoint *p = firstBreakpoint(); p; p = p->nextInSite())
        if (p == bp)
            return true;
    return false;
}

Breakpoint::Breakpoint(Debugger *debugger, BreakpointSite *site, JSObject *handler)
    : debugger(debugger), site(site), handler(handler)
{
    JS_APPEND_LINK(&debuggerLinks, &debugger->breakpoints);
    JS_APPEND_LINK(&siteLinks, &site->breakpoints);
}

Breakpoint *
Breakpoint::fromDebuggerLinks(JSCList *links)
{
    return (Breakpoint *) ((unsigned char *) links - offsetof(Breakpoint, debuggerLinks));
}

Breakpoint *
Breakpoint::fromSiteLinks(JSCList *links)
{
    return (Breakpoint *) ((unsigned char *) links - offsetof(Breakpoint, siteLinks));
}

void
Breakpoint::destroy(FreeOp *fop)
{
    if (debugger->enabled)
        site->dec(fop);
    JS_REMOVE_LINK(&debuggerLinks);
    JS_REMOVE_LINK(&siteLinks);
    site->destroyIfEmpty(fop);
    fop->delete_(this);
}

Breakpoint *
Breakpoint::nextInDebugger()
{
    JSCList *link = JS_NEXT_LINK(&debuggerLinks);
    return (link == &debugger->breakpoints) ? NULL : fromDebuggerLinks(link);
}

Breakpoint *
Breakpoint::nextInSite()
{
    JSCList *link = JS_NEXT_LINK(&siteLinks);
    return (link == &site->breakpoints) ? NULL : fromSiteLinks(link);
}

/*** Debugger hook dispatch **********************************************************************/

Debugger::Debugger(JSContext *cx, JSObject *dbg)
  : object(dbg), uncaughtExceptionHook(NULL), enabled(true),
    frames(cx), scripts(cx), objects(cx), environments(cx)
{
    assertSameCompartment(cx, dbg);

    cx->runtime->debuggerList.insertBack(this);
    JS_INIT_CLIST(&breakpoints);
    JS_INIT_CLIST(&onNewGlobalObjectWatchersLink);
}

Debugger::~Debugger()
{
    JS_ASSERT(debuggees.empty());

    /* This always happens in the GC thread, so no locking is required. */
    JS_ASSERT(object->compartment()->rt->isHeapBusy());

    /*
     * These maps may contain finalized entries, so drop them before destructing to avoid calling
     * ~EncapsulatedPtr.
     */
    scripts.clearWithoutCallingDestructors();
    objects.clearWithoutCallingDestructors();
    environments.clearWithoutCallingDestructors();

    /*
     * Since the inactive state for this link is a singleton cycle, it's always
     * safe to apply JS_REMOVE_LINK to it, regardless of whether we're in the list or not.
     */
    JS_REMOVE_LINK(&onNewGlobalObjectWatchersLink);
}

bool
Debugger::init(JSContext *cx)
{
    bool ok = debuggees.init() &&
              frames.init() &&
              scripts.init() &&
              objects.init() &&
              environments.init();
    if (!ok)
        js_ReportOutOfMemory(cx);
    return ok;
}

JS_STATIC_ASSERT(unsigned(JSSLOT_DEBUGFRAME_OWNER) == unsigned(JSSLOT_DEBUGSCRIPT_OWNER));
JS_STATIC_ASSERT(unsigned(JSSLOT_DEBUGFRAME_OWNER) == unsigned(JSSLOT_DEBUGOBJECT_OWNER));
JS_STATIC_ASSERT(unsigned(JSSLOT_DEBUGFRAME_OWNER) == unsigned(JSSLOT_DEBUGENV_OWNER));

Debugger *
Debugger::fromChildJSObject(JSObject *obj)
{
    JS_ASSERT(obj->getClass() == &DebuggerFrame_class ||
              obj->getClass() == &DebuggerScript_class ||
              obj->getClass() == &DebuggerObject_class ||
              obj->getClass() == &DebuggerEnv_class);
    JSObject *dbgobj = &obj->getReservedSlot(JSSLOT_DEBUGOBJECT_OWNER).toObject();
    return fromJSObject(dbgobj);
}

bool
Debugger::getScriptFrame(JSContext *cx, const ScriptFrameIter &iter, MutableHandleValue vp)
{
    FrameMap::AddPtr p = frames.lookupForAdd(iter.abstractFramePtr());
    if (!p) {
        /* Create and populate the Debugger.Frame object. */
        JSObject *proto = &object->getReservedSlot(JSSLOT_DEBUG_FRAME_PROTO).toObject();
        JSObject *frameobj =
            NewObjectWithGivenProto(cx, &DebuggerFrame_class, proto, NULL);
        if (!frameobj)
            return false;
        StackIter::Data *data = iter.copyData();
        if (!data)
            return false;
        frameobj->setPrivate(data);
        frameobj->setReservedSlot(JSSLOT_DEBUGFRAME_OWNER, ObjectValue(*object));

        if (!frames.add(p, iter.abstractFramePtr(), frameobj)) {
            js_ReportOutOfMemory(cx);
            return false;
        }
    }
    vp.setObject(*p->value);
    return true;
}

JSObject *
Debugger::getHook(Hook hook) const
{
    JS_ASSERT(hook >= 0 && hook < HookCount);
    const Value &v = object->getReservedSlot(JSSLOT_DEBUG_HOOK_START + hook);
    return v.isUndefined() ? NULL : &v.toObject();
}

bool
Debugger::hasAnyLiveHooks() const
{
    if (!enabled)
        return false;

    if (getHook(OnDebuggerStatement) ||
        getHook(OnExceptionUnwind) ||
        getHook(OnNewScript) ||
        getHook(OnEnterFrame))
    {
        return true;
    }

    /* If any breakpoints are in live scripts, return true. */
    for (Breakpoint *bp = firstBreakpoint(); bp; bp = bp->nextInDebugger()) {
        if (IsScriptMarked(&bp->site->script))
            return true;
    }

    for (FrameMap::Range r = frames.all(); !r.empty(); r.popFront()) {
        JSObject *frameObj = r.front().value;
        if (!frameObj->getReservedSlot(JSSLOT_DEBUGFRAME_ONSTEP_HANDLER).isUndefined() ||
            !frameObj->getReservedSlot(JSSLOT_DEBUGFRAME_ONPOP_HANDLER).isUndefined())
            return true;
    }

    return false;
}

JSTrapStatus
Debugger::slowPathOnEnterFrame(JSContext *cx, AbstractFramePtr frame, MutableHandleValue vp)
{
    /* Build the list of recipients. */
    AutoValueVector triggered(cx);
    Handle<GlobalObject*> global = cx->global();

    if (GlobalObject::DebuggerVector *debuggers = global->getDebuggers()) {
        for (Debugger **p = debuggers->begin(); p != debuggers->end(); p++) {
            Debugger *dbg = *p;
            JS_ASSERT(dbg->observesFrame(frame));
            if (dbg->observesEnterFrame() && !triggered.append(ObjectValue(*dbg->toJSObject())))
                return JSTRAP_ERROR;
        }
    }

    /* Deliver the event, checking again as in dispatchHook. */
    for (Value *p = triggered.begin(); p != triggered.end(); p++) {
        Debugger *dbg = Debugger::fromJSObject(&p->toObject());
        if (dbg->debuggees.has(global) && dbg->observesEnterFrame()) {
            JSTrapStatus status = dbg->fireEnterFrame(cx, vp);
            if (status != JSTRAP_CONTINUE)
                return status;
        }
    }

    return JSTRAP_CONTINUE;
}

static void
DebuggerFrame_freeStackIterData(FreeOp *fop, RawObject obj);

/*
 * Handle leaving a frame with debuggers watching. |frameOk| indicates whether
 * the frame is exiting normally or abruptly. Set |cx|'s exception and/or
 * |cx->fp()|'s return value, and return a new success value.
 */
bool
Debugger::slowPathOnLeaveFrame(JSContext *cx, AbstractFramePtr frame, bool frameOk)
{
    Handle<GlobalObject*> global = cx->global();

    /* Save the frame's completion value. */
    JSTrapStatus status;
    RootedValue value(cx);
    Debugger::resultToCompletion(cx, frameOk, frame.returnValue(), &status, &value);

    /* Build a list of the recipients. */
    AutoObjectVector frames(cx);
    for (FrameRange r(frame, global); !r.empty(); r.popFront()) {
        if (!frames.append(r.frontFrame())) {
            cx->clearPendingException();
            return false;
        }
    }

    /* For each Debugger.Frame, fire its onPop handler, if any. */
    for (JSObject **p = frames.begin(); p != frames.end(); p++) {
        RootedObject frameobj(cx, *p);
        Debugger *dbg = Debugger::fromChildJSObject(frameobj);

        if (dbg->enabled &&
            !frameobj->getReservedSlot(JSSLOT_DEBUGFRAME_ONPOP_HANDLER).isUndefined()) {
            RootedValue handler(cx, frameobj->getReservedSlot(JSSLOT_DEBUGFRAME_ONPOP_HANDLER));

            Maybe<AutoCompartment> ac;
            ac.construct(cx, dbg->object);

            RootedValue completion(cx);
            if (!dbg->newCompletionValue(cx, status, value, &completion)) {
                status = dbg->handleUncaughtException(ac, false);
                break;
            }

            /* Call the onPop handler. */
            RootedValue rval(cx);
            bool hookOk = Invoke(cx, ObjectValue(*frameobj), handler, 1, completion.address(),
                                 rval.address());
            RootedValue nextValue(cx);
            JSTrapStatus nextStatus = dbg->parseResumptionValue(ac, hookOk, rval, &nextValue);

            /*
             * At this point, we are back in the debuggee compartment, and any error has
             * been wrapped up as a completion value.
             */
            JS_ASSERT(cx->compartment == global->compartment());
            JS_ASSERT(!cx->isExceptionPending());

            /* JSTRAP_CONTINUE means "make no change". */
            if (nextStatus != JSTRAP_CONTINUE) {
                status = nextStatus;
                value = nextValue;
            }
        }
    }

    /*
     * Clean up all Debugger.Frame instances. Use a fresh FrameRange, as one
     * debugger's onPop handler could have caused another debugger to create its
     * own Debugger.Frame instance.
     */
    for (FrameRange r(frame, global); !r.empty(); r.popFront()) {
        RootedObject frameobj(cx, r.frontFrame());
        Debugger *dbg = r.frontDebugger();
        JS_ASSERT(dbg == Debugger::fromChildJSObject(frameobj));

        DebuggerFrame_freeStackIterData(cx->runtime->defaultFreeOp(), frameobj);

        /* If this frame had an onStep handler, adjust the script's count. */
        if (!frameobj->getReservedSlot(JSSLOT_DEBUGFRAME_ONSTEP_HANDLER).isUndefined() &&
            !frame.script()->changeStepModeCount(cx, -1))
        {
            status = JSTRAP_ERROR;
            /* Don't exit the loop; we must mark all frames as dead. */
        }

        dbg->frames.remove(frame);
    }

    /*
     * If this is an eval frame, then from the debugger's perspective the
     * script is about to be destroyed. Remove any breakpoints in it.
     */
    if (frame.isEvalFrame()) {
        RootedScript script(cx, frame.script());
        script->clearBreakpointsIn(cx->runtime->defaultFreeOp(), NULL, NULL);
    }

    /* Establish (status, value) as our resumption value. */
    switch (status) {
      case JSTRAP_RETURN:
        frame.setReturnValue(value);
        return true;

      case JSTRAP_THROW:
        cx->setPendingException(value);
        return false;

      case JSTRAP_ERROR:
        JS_ASSERT(!cx->isExceptionPending());
        return false;

      default:
        JS_NOT_REACHED("bad final trap status");
    }
}

bool
Debugger::wrapEnvironment(JSContext *cx, Handle<Env*> env, MutableHandleValue rval)
{
    if (!env) {
        rval.setNull();
        return true;
    }

    /*
     * DebuggerEnv should only wrap a debug scope chain obtained (transitively)
     * from GetDebugScopeFor(Frame|Function).
     */
    JS_ASSERT(!env->isScope());

    JSObject *envobj;
    ObjectWeakMap::AddPtr p = environments.lookupForAdd(env);
    if (p) {
        envobj = p->value;
    } else {
        /* Create a new Debugger.Environment for env. */
        JSObject *proto = &object->getReservedSlot(JSSLOT_DEBUG_ENV_PROTO).toObject();
        envobj = NewObjectWithGivenProto(cx, &DebuggerEnv_class, proto, NULL);
        if (!envobj)
            return false;
        envobj->setPrivateGCThing(env);
        envobj->setReservedSlot(JSSLOT_DEBUGENV_OWNER, ObjectValue(*object));
        if (!environments.relookupOrAdd(p, env, envobj)) {
            js_ReportOutOfMemory(cx);
            return false;
        }

        CrossCompartmentKey key(CrossCompartmentKey::DebuggerEnvironment, object, env);
        if (!object->compartment()->putWrapper(key, ObjectValue(*envobj))) {
            environments.remove(env);
            js_ReportOutOfMemory(cx);
            return false;
        }
    }
    rval.setObject(*envobj);
    return true;
}

bool
Debugger::wrapDebuggeeValue(JSContext *cx, MutableHandleValue vp)
{
    assertSameCompartment(cx, object.get());

    if (vp.isObject()) {
        // Do we need this RootedObject?
        RootedObject obj(cx, &vp.toObject());

        ObjectWeakMap::AddPtr p = objects.lookupForAdd(obj);
        if (p) {
            vp.setObject(*p->value);
        } else {
            /* Create a new Debugger.Object for obj. */
            JSObject *proto = &object->getReservedSlot(JSSLOT_DEBUG_OBJECT_PROTO).toObject();
            JSObject *dobj =
                NewObjectWithGivenProto(cx, &DebuggerObject_class, proto, NULL);
            if (!dobj)
                return false;
            dobj->setPrivateGCThing(obj);
            dobj->setReservedSlot(JSSLOT_DEBUGOBJECT_OWNER, ObjectValue(*object));
            if (!objects.relookupOrAdd(p, obj, dobj)) {
                js_ReportOutOfMemory(cx);
                return false;
            }

            if (obj->compartment() != object->compartment()) {
                CrossCompartmentKey key(CrossCompartmentKey::DebuggerObject, object, obj);
                if (!object->compartment()->putWrapper(key, ObjectValue(*dobj))) {
                    objects.remove(obj);
                    js_ReportOutOfMemory(cx);
                    return false;
                }
            }

            vp.setObject(*dobj);
        }
    } else if (!cx->compartment->wrap(cx, vp)) {
        vp.setUndefined();
        return false;
    }

    return true;
}

bool
Debugger::unwrapDebuggeeValue(JSContext *cx, MutableHandleValue vp)
{
    assertSameCompartment(cx, object.get(), vp);
    if (vp.isObject()) {
        JSObject *dobj = &vp.toObject();
        if (dobj->getClass() != &DebuggerObject_class) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NOT_EXPECTED_TYPE,
                                 "Debugger", "Debugger.Object", dobj->getClass()->name);
            return false;
        }

        Value owner = dobj->getReservedSlot(JSSLOT_DEBUGOBJECT_OWNER);
        if (owner.isUndefined() || &owner.toObject() != object) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 owner.isUndefined()
                                 ? JSMSG_DEBUG_OBJECT_PROTO
                                 : JSMSG_DEBUG_OBJECT_WRONG_OWNER);
            return false;
        }

        vp.setObject(*static_cast<JSObject*>(dobj->getPrivate()));
    }
    return true;
}

JSTrapStatus
Debugger::handleUncaughtExceptionHelper(Maybe<AutoCompartment> &ac,
                                        MutableHandleValue *vp, bool callHook)
{
    JSContext *cx = ac.ref().context();
    if (cx->isExceptionPending()) {
        if (callHook && uncaughtExceptionHook) {
            Value fval = ObjectValue(*uncaughtExceptionHook);
            Value exc = cx->getPendingException();
            RootedValue rv(cx);
            cx->clearPendingException();
            if (Invoke(cx, ObjectValue(*object), fval, 1, &exc, rv.address()))
                return vp ? parseResumptionValue(ac, true, rv, *vp, false) : JSTRAP_CONTINUE;
        }

        if (cx->isExceptionPending()) {
            JS_ReportPendingException(cx);
            cx->clearPendingException();
        }
    }
    ac.destroy();
    return JSTRAP_ERROR;
}

JSTrapStatus
Debugger::handleUncaughtException(Maybe<AutoCompartment> &ac, MutableHandleValue vp, bool callHook)
{
    return handleUncaughtExceptionHelper(ac, &vp, callHook);
}

JSTrapStatus
Debugger::handleUncaughtException(Maybe<AutoCompartment> &ac, bool callHook)
{
    return handleUncaughtExceptionHelper(ac, NULL, callHook);
}

void
Debugger::resultToCompletion(JSContext *cx, bool ok, const Value &rv,
                             JSTrapStatus *status, MutableHandleValue value)
{
    JS_ASSERT_IF(ok, !cx->isExceptionPending());

    if (ok) {
        *status = JSTRAP_RETURN;
        value.set(rv);
    } else if (cx->isExceptionPending()) {
        *status = JSTRAP_THROW;
        value.set(cx->getPendingException());
        cx->clearPendingException();
    } else {
        *status = JSTRAP_ERROR;
        value.setUndefined();
    }
}

bool
Debugger::newCompletionValue(JSContext *cx, JSTrapStatus status, Value value_,
                             MutableHandleValue result)
{
    /*
     * We must be in the debugger's compartment, since that's where we want
     * to construct the completion value.
     */
    assertSameCompartment(cx, object.get());

    RootedId key(cx);
    RootedValue value(cx, value_);

    switch (status) {
      case JSTRAP_RETURN:
        key = NameToId(cx->names().return_);
        break;

      case JSTRAP_THROW:
        key = NameToId(cx->names().throw_);
        break;

      case JSTRAP_ERROR:
        result.setNull();
        return true;

      default:
        JS_NOT_REACHED("bad status passed to Debugger::newCompletionValue");
    }

    /* Common tail for JSTRAP_RETURN and JSTRAP_THROW. */
    RootedObject obj(cx, NewBuiltinClassInstance(cx, &ObjectClass));
    if (!obj ||
        !wrapDebuggeeValue(cx, &value) ||
        !DefineNativeProperty(cx, obj, key, value, JS_PropertyStub, JS_StrictPropertyStub,
                              JSPROP_ENUMERATE, 0, 0))
    {
        return false;
    }

    result.setObject(*obj);
    return true;
}

bool
Debugger::receiveCompletionValue(Maybe<AutoCompartment> &ac, bool ok, Value val,
                                 MutableHandleValue vp)
{
    JSContext *cx = ac.ref().context();

    JSTrapStatus status;
    RootedValue value(cx);
    resultToCompletion(cx, ok, val, &status, &value);
    ac.destroy();
    return newCompletionValue(cx, status, value, vp);
}

JSTrapStatus
Debugger::parseResumptionValue(Maybe<AutoCompartment> &ac, bool ok, const Value &rv, MutableHandleValue vp,
                               bool callHook)
{
    vp.setUndefined();
    if (!ok)
        return handleUncaughtException(ac, vp, callHook);
    if (rv.isUndefined()) {
        ac.destroy();
        return JSTRAP_CONTINUE;
    }
    if (rv.isNull()) {
        ac.destroy();
        return JSTRAP_ERROR;
    }

    /* Check that rv is {return: val} or {throw: val}. */
    JSContext *cx = ac.ref().context();
    Rooted<JSObject*> obj(cx);
    RootedShape shape(cx);
    RootedId returnId(cx, NameToId(cx->names().return_));
    RootedId throwId(cx, NameToId(cx->names().throw_));
    bool okResumption = rv.isObject();
    if (okResumption) {
        obj = &rv.toObject();
        okResumption = obj->isObject();
    }
    if (okResumption) {
        shape = obj->lastProperty();
        okResumption = shape->previous() &&
             !shape->previous()->previous() &&
             (shape->propid() == returnId || shape->propid() == throwId) &&
             shape->isDataDescriptor();
    }
    if (!okResumption) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_DEBUG_BAD_RESUMPTION);
        return handleUncaughtException(ac, vp, callHook);
    }

    RootedValue v(cx, vp.get());
    if (!js_NativeGet(cx, obj, obj, shape, 0, &v) || !unwrapDebuggeeValue(cx, &v))
        return handleUncaughtException(ac, &v, callHook);

    ac.destroy();
    if (!cx->compartment->wrap(cx, &v)) {
        vp.setUndefined();
        return JSTRAP_ERROR;
    }
    vp.set(v);

    return shape->propid() == returnId ? JSTRAP_RETURN : JSTRAP_THROW;
}

bool
CallMethodIfPresent(JSContext *cx, HandleObject obj, const char *name, int argc, Value *argv,
                    Value *rval)
{
    rval->setUndefined();
    JSAtom *atom = Atomize(cx, name, strlen(name));
    if (!atom)
        return false;

    Rooted<jsid> id(cx, AtomToId(atom));
    RootedValue fval(cx);
    return GetMethod(cx, obj, id, 0, &fval) &&
           (!js_IsCallable(fval) || Invoke(cx, ObjectValue(*obj), fval, argc, argv, rval));
}

JSTrapStatus
Debugger::fireDebuggerStatement(JSContext *cx, MutableHandleValue vp)
{
    RootedObject hook(cx, getHook(OnDebuggerStatement));
    JS_ASSERT(hook);
    JS_ASSERT(hook->isCallable());

    Maybe<AutoCompartment> ac;
    ac.construct(cx, object);

    ScriptFrameIter iter(cx);

    RootedValue argv(cx);
    if (!getScriptFrame(cx, iter, &argv))
        return handleUncaughtException(ac, vp, false);

    RootedValue rv(cx);
    bool ok = Invoke(cx, ObjectValue(*object), ObjectValue(*hook), 1, argv.address(), rv.address());
    return parseResumptionValue(ac, ok, rv, vp);
}

JSTrapStatus
Debugger::fireExceptionUnwind(JSContext *cx, MutableHandleValue vp)
{
    RootedObject hook(cx, getHook(OnExceptionUnwind));
    JS_ASSERT(hook);
    JS_ASSERT(hook->isCallable());

    RootedValue exc(cx, cx->getPendingException());
    cx->clearPendingException();

    Maybe<AutoCompartment> ac;
    ac.construct(cx, object);

    Value argv[2];
    AutoValueArray avr(cx, argv, 2);

    ScriptFrameIter iter(cx);

    argv[1] = exc;
    if (!getScriptFrame(cx, iter, avr.handleAt(0)) || !wrapDebuggeeValue(cx, avr.handleAt(1)))
        return handleUncaughtException(ac, vp, false);

    RootedValue rv(cx);
    bool ok = Invoke(cx, ObjectValue(*object), ObjectValue(*hook), 2, argv, rv.address());
    JSTrapStatus st = parseResumptionValue(ac, ok, rv, vp);
    if (st == JSTRAP_CONTINUE)
        cx->setPendingException(exc);
    return st;
}

JSTrapStatus
Debugger::fireEnterFrame(JSContext *cx, MutableHandleValue vp)
{
    RootedObject hook(cx, getHook(OnEnterFrame));
    JS_ASSERT(hook);
    JS_ASSERT(hook->isCallable());

    ScriptFrameIter iter(cx);
    Maybe<AutoCompartment> ac;
    ac.construct(cx, object);

    RootedValue argv(cx);
    if (!getScriptFrame(cx, iter, &argv))
        return handleUncaughtException(ac, vp, false);

    RootedValue rv(cx);
    bool ok = Invoke(cx, ObjectValue(*object), ObjectValue(*hook), 1, argv.address(), rv.address());
    return parseResumptionValue(ac, ok, rv, vp);
}

void
Debugger::fireNewScript(JSContext *cx, HandleScript script)
{
    RootedObject hook(cx, getHook(OnNewScript));
    JS_ASSERT(hook);
    JS_ASSERT(hook->isCallable());

    Maybe<AutoCompartment> ac;
    ac.construct(cx, object);

    JSObject *dsobj = wrapScript(cx, script);
    if (!dsobj) {
        handleUncaughtException(ac, false);
        return;
    }

    Value argv[1];
    argv[0].setObject(*dsobj);
    Value rv;
    if (!Invoke(cx, ObjectValue(*object), ObjectValue(*hook), 1, argv, &rv))
        handleUncaughtException(ac, true);
}

JSTrapStatus
Debugger::dispatchHook(JSContext *cx, MutableHandleValue vp, Hook which)
{
    JS_ASSERT(which == OnDebuggerStatement || which == OnExceptionUnwind);

    /*
     * Determine which debuggers will receive this event, and in what order.
     * Make a copy of the list, since the original is mutable and we will be
     * calling into arbitrary JS.
     *
     * Note: In the general case, 'triggered' contains references to objects in
     * different compartments--every compartment *except* this one.
     */
    AutoValueVector triggered(cx);
    Handle<GlobalObject*> global = cx->global();
    if (GlobalObject::DebuggerVector *debuggers = global->getDebuggers()) {
        for (Debugger **p = debuggers->begin(); p != debuggers->end(); p++) {
            Debugger *dbg = *p;
            if (dbg->enabled && dbg->getHook(which)) {
                if (!triggered.append(ObjectValue(*dbg->toJSObject())))
                    return JSTRAP_ERROR;
            }
        }
    }

    /*
     * Deliver the event to each debugger, checking again to make sure it
     * should still be delivered.
     */
    for (Value *p = triggered.begin(); p != triggered.end(); p++) {
        Debugger *dbg = Debugger::fromJSObject(&p->toObject());
        if (dbg->debuggees.has(global) && dbg->enabled && dbg->getHook(which)) {
            JSTrapStatus st = (which == OnDebuggerStatement)
                              ? dbg->fireDebuggerStatement(cx, vp)
                              : dbg->fireExceptionUnwind(cx, vp);
            if (st != JSTRAP_CONTINUE)
                return st;
        }
    }
    return JSTRAP_CONTINUE;
}

static bool
AddNewScriptRecipients(GlobalObject::DebuggerVector *src, AutoValueVector *dest)
{
    bool wasEmpty = dest->length() == 0;
    for (Debugger **p = src->begin(); p != src->end(); p++) {
        Debugger *dbg = *p;
        Value v = ObjectValue(*dbg->toJSObject());
        if (dbg->observesNewScript() &&
            (wasEmpty || Find(dest->begin(), dest->end(), v) == dest->end()) &&
            !dest->append(v))
        {
            return false;
        }
    }
    return true;
}

void
Debugger::slowPathOnNewScript(JSContext *cx, HandleScript script, GlobalObject *compileAndGoGlobal_)
{
    if (script->selfHosted)
        return;

    Rooted<GlobalObject*> compileAndGoGlobal(cx, compileAndGoGlobal_);

    JS_ASSERT(script->compileAndGo == !!compileAndGoGlobal);

    /*
     * Build the list of recipients. For compile-and-go scripts, this is the
     * same as the generic Debugger::dispatchHook code, but non-compile-and-go
     * scripts are not tied to particular globals. We deliver them to every
     * debugger observing any global in the script's compartment.
     */
    AutoValueVector triggered(cx);
    if (script->compileAndGo) {
        if (GlobalObject::DebuggerVector *debuggers = compileAndGoGlobal->getDebuggers()) {
            if (!AddNewScriptRecipients(debuggers, &triggered))
                return;
        }
    } else {
        GlobalObjectSet &debuggees = script->compartment()->getDebuggees();
        for (GlobalObjectSet::Range r = debuggees.all(); !r.empty(); r.popFront()) {
            if (!AddNewScriptRecipients(r.front()->getDebuggers(), &triggered))
                return;
        }
    }

    /*
     * Deliver the event to each debugger, checking again as in
     * Debugger::dispatchHook.
     */
    for (Value *p = triggered.begin(); p != triggered.end(); p++) {
        Debugger *dbg = Debugger::fromJSObject(&p->toObject());
        if ((!compileAndGoGlobal || dbg->debuggees.has(compileAndGoGlobal)) &&
            dbg->enabled && dbg->getHook(OnNewScript)) {
            dbg->fireNewScript(cx, script);
        }
    }
}

JSTrapStatus
Debugger::onTrap(JSContext *cx, MutableHandleValue vp)
{
    ScriptFrameIter iter(cx);
    RootedScript script(cx, iter.script());
    Rooted<GlobalObject*> scriptGlobal(cx, &script->global());
    jsbytecode *pc = iter.pc();
    BreakpointSite *site = script->getBreakpointSite(pc);
    JSOp op = JSOp(*pc);

    /* Build list of breakpoint handlers. */
    Vector<Breakpoint *> triggered(cx);
    for (Breakpoint *bp = site->firstBreakpoint(); bp; bp = bp->nextInSite()) {
        if (!triggered.append(bp))
            return JSTRAP_ERROR;
    }

    for (Breakpoint **p = triggered.begin(); p != triggered.end(); p++) {
        Breakpoint *bp = *p;

        /* Handlers can clear breakpoints. Check that bp still exists. */
        if (!site || !site->hasBreakpoint(bp))
            continue;


        /*
         * There are two reasons we have to check whether dbg is enabled and
         * debugging scriptGlobal.
         *
         * One is just that one breakpoint handler can disable other Debuggers
         * or remove debuggees.
         *
         * The other has to do with non-compile-and-go scripts, which have no
         * specific global--until they are executed. Only now do we know which
         * global the script is running against.
         */
        Debugger *dbg = bp->debugger;
        if (dbg->enabled && dbg->debuggees.lookup(scriptGlobal)) {
            Maybe<AutoCompartment> ac;
            ac.construct(cx, dbg->object);

            Value argv[1];
            AutoValueArray ava(cx, argv, 1);
            if (!dbg->getScriptFrame(cx, iter, ava.handleAt(0)))
                return dbg->handleUncaughtException(ac, vp, false);
            RootedValue rv(cx);
            Rooted<JSObject*> handler(cx, bp->handler);
            bool ok = CallMethodIfPresent(cx, handler, "hit", 1, argv, rv.address());
            JSTrapStatus st = dbg->parseResumptionValue(ac, ok, rv, vp, true);
            if (st != JSTRAP_CONTINUE)
                return st;

            /* Calling JS code invalidates site. Reload it. */
            site = script->getBreakpointSite(pc);
        }
    }

    if (site && site->trapHandler) {
        JSTrapStatus st = site->trapHandler(cx, script, pc, vp.address(), site->trapClosure);
        if (st != JSTRAP_CONTINUE)
            return st;
    }

    /* By convention, return the true op to the interpreter in vp. */
    vp.setInt32(op);
    return JSTRAP_CONTINUE;
}

JSTrapStatus
Debugger::onSingleStep(JSContext *cx, MutableHandleValue vp)
{
    ScriptFrameIter iter(cx);

    /*
     * We may be stepping over a JSOP_EXCEPTION, that pushes the context's
     * pending exception for a 'catch' clause to handle. Don't let the
     * onStep handlers mess with that (other than by returning a resumption
     * value).
     */
    RootedValue exception(cx, UndefinedValue());
    bool exceptionPending = cx->isExceptionPending();
    if (exceptionPending) {
        exception = cx->getPendingException();
        cx->clearPendingException();
    }

    /*
     * Build list of Debugger.Frame instances referring to this frame with
     * onStep handlers.
     */
    AutoObjectVector frames(cx);
    for (FrameRange r(iter.abstractFramePtr()); !r.empty(); r.popFront()) {
        JSObject *frame = r.frontFrame();
        if (!frame->getReservedSlot(JSSLOT_DEBUGFRAME_ONSTEP_HANDLER).isUndefined() &&
            !frames.append(frame))
        {
            return JSTRAP_ERROR;
        }
    }

#ifdef DEBUG
    /*
     * Validate the single-step count on this frame's script, to ensure that
     * we're not receiving traps we didn't ask for. Even when frames is
     * non-empty (and thus we know this trap was requested), do the check
     * anyway, to make sure the count has the correct non-zero value.
     *
     * The converse --- ensuring that we do receive traps when we should --- can
     * be done with unit tests.
     */
    {
        uint32_t stepperCount = 0;
        RawScript trappingScript = iter.script();
        GlobalObject *global = cx->global();
        if (GlobalObject::DebuggerVector *debuggers = global->getDebuggers()) {
            for (Debugger **p = debuggers->begin(); p != debuggers->end(); p++) {
                Debugger *dbg = *p;
                for (FrameMap::Range r = dbg->frames.all(); !r.empty(); r.popFront()) {
                    AbstractFramePtr frame = r.front().key;
                    JSObject *frameobj = r.front().value;
                    if (frame.script() == trappingScript &&
                        !frameobj->getReservedSlot(JSSLOT_DEBUGFRAME_ONSTEP_HANDLER).isUndefined())
                    {
                        stepperCount++;
                    }
                }
            }
        }
        if (trappingScript->compileAndGo)
            JS_ASSERT(stepperCount == trappingScript->stepModeCount());
        else
            JS_ASSERT(stepperCount <= trappingScript->stepModeCount());
    }
#endif

    /* Preserve the debuggee's iterValue while handlers run. */
    class PreserveIterValue {
        JSContext *cx;
        RootedValue savedIterValue;

      public:
        PreserveIterValue(JSContext *cx) : cx(cx), savedIterValue(cx, cx->iterValue) {
            cx->iterValue.setMagic(JS_NO_ITER_VALUE);
        }
        ~PreserveIterValue() {
            cx->iterValue = savedIterValue;
        }
    };
    PreserveIterValue piv(cx);

    /* Call all the onStep handlers we found. */
    for (JSObject **p = frames.begin(); p != frames.end(); p++) {
        RootedObject frame(cx, *p);
        Debugger *dbg = Debugger::fromChildJSObject(frame);

        Maybe<AutoCompartment> ac;
        ac.construct(cx, dbg->object);

        const Value &handler = frame->getReservedSlot(JSSLOT_DEBUGFRAME_ONSTEP_HANDLER);
        RootedValue rval(cx);
        bool ok = Invoke(cx, ObjectValue(*frame), handler, 0, NULL, rval.address());
        JSTrapStatus st = dbg->parseResumptionValue(ac, ok, rval, vp);
        if (st != JSTRAP_CONTINUE)
            return st;
    }

    vp.setUndefined();
    if (exceptionPending)
        cx->setPendingException(exception);
    return JSTRAP_CONTINUE;
}

JSTrapStatus
Debugger::fireNewGlobalObject(JSContext *cx, Handle<GlobalObject *> global, MutableHandleValue vp)
{
    RootedObject hook(cx, getHook(OnNewGlobalObject));
    JS_ASSERT(hook);
    JS_ASSERT(hook->isCallable());

    Maybe<AutoCompartment> ac;
    ac.construct(cx, object);

    Value argv[1];
    AutoArrayRooter argvRooter(cx, ArrayLength(argv), argv);
    argv[0].setObject(*global);
    if (!wrapDebuggeeValue(cx, argvRooter.handleAt(0)))
        return handleUncaughtException(ac, false);

    RootedValue rv(cx);
    bool ok = Invoke(cx, ObjectValue(*object), ObjectValue(*hook), 1, argv, rv.address());
    return parseResumptionValue(ac, ok, rv, vp);
}

bool
Debugger::slowPathOnNewGlobalObject(JSContext *cx, Handle<GlobalObject *> global)
{
    JS_ASSERT(!JS_CLIST_IS_EMPTY(&cx->runtime->onNewGlobalObjectWatchers));

    /*
     * Make a copy of the runtime's onNewGlobalObjectWatchers before running the
     * handlers. Since one Debugger's handler can disable another's, the list
     * can be mutated while we're walking it.
     */
    AutoObjectVector watchers(cx);
    for (JSCList *link = JS_LIST_HEAD(&cx->runtime->onNewGlobalObjectWatchers);
         link != &cx->runtime->onNewGlobalObjectWatchers;
         link = JS_NEXT_LINK(link)) {
        Debugger *dbg = fromOnNewGlobalObjectWatchersLink(link);
        JS_ASSERT(dbg->observesNewGlobalObject());
        if (!watchers.append(dbg->object))
            return false;
    }

    JSTrapStatus status = JSTRAP_CONTINUE;
    RootedValue value(cx);

    for (size_t i = 0; i < watchers.length(); i++) {
        Debugger *dbg = fromJSObject(watchers[i]);

        // One Debugger's onNewGlobalObject handler can disable another's, so we
        // must test this in the loop.
        if (dbg->observesNewGlobalObject()) {
            status = dbg->fireNewGlobalObject(cx, global, &value);
            if (status != JSTRAP_CONTINUE && status != JSTRAP_RETURN)
                break;
        }
    }

    switch (status) {
      case JSTRAP_CONTINUE:
      case JSTRAP_RETURN: // Treat return like continue, ignoring the value.
        return true;

      case JSTRAP_ERROR:
        JS_ASSERT(!cx->isExceptionPending());
        return false;

      case JSTRAP_THROW:
        cx->setPendingException(value);
        return false;

      default:
        JS_NOT_REACHED("bad status from Debugger::fireNewGlobalObject");
    }
}


/*** Debugger JSObjects **************************************************************************/

/* static */ bool
Debugger::isDebugWrapper(RawObject o)
{
    Class *c = o->getClass();
    return c == &DebuggerObject_class ||
           c == &DebuggerEnv_class ||
           c == &DebuggerScript_class;
}

void
Debugger::markKeysInCompartment(JSTracer *tracer)
{
    /*
     * WeakMap::Range is deliberately private, to discourage C++ code from
     * enumerating WeakMap keys. However in this case we need access, so we
     * make a base-class reference. Range is public in HashMap.
     */
    objects.markKeys(tracer);
    environments.markKeys(tracer);
    scripts.markKeys(tracer);
}

/*
 * Ordinarily, WeakMap keys and values are marked because at some point it was
 * discovered that the WeakMap was live; that is, some object containing the
 * WeakMap was marked during mark phase.
 *
 * However, during compartment GC, we have to do something about
 * cross-compartment WeakMaps in non-GC'd compartments. If their keys and values
 * might need to be marked, we have to do it manually.
 *
 * Each Debugger object keeps three cross-compartment WeakMaps: objects, script,
 * and environments. They have the nice property that all their values are in
 * the same compartment as the Debugger object, so we only need to mark the
 * keys. We must simply mark all keys that are in a compartment being GC'd.
 *
 * We must scan all Debugger objects regardless of whether they *currently*
 * have any debuggees in a compartment being GC'd, because the WeakMap
 * entries persist even when debuggees are removed.
 *
 * This happens during the initial mark phase, not iterative marking, because
 * all the edges being reported here are strong references.
 */
void
Debugger::markCrossCompartmentDebuggerObjectReferents(JSTracer *tracer)
{
    JSRuntime *rt = tracer->runtime;

    /*
     * Mark all objects in comp that are referents of Debugger.Objects in other
     * compartments.
     */
    for (Debugger *dbg = rt->debuggerList.getFirst(); dbg; dbg = dbg->getNext()) {
        if (!dbg->object->zone()->isCollecting())
            dbg->markKeysInCompartment(tracer);
    }
}

/*
 * This method has two tasks:
 *   1. Mark Debugger objects that are unreachable except for debugger hooks that
 *      may yet be called.
 *   2. Mark breakpoint handlers.
 *
 * This happens during the iterative part of the GC mark phase. This method
 * returns true if it has to mark anything; GC calls it repeatedly until it
 * returns false.
 */
bool
Debugger::markAllIteratively(GCMarker *trc)
{
    bool markedAny = false;

    /*
     * Find all Debugger objects in danger of GC. This code is a little
     * convoluted since the easiest way to find them is via their debuggees.
     */
    JSRuntime *rt = trc->runtime;
    for (CompartmentsIter c(rt); !c.done(); c.next()) {
        GlobalObjectSet &debuggees = c->getDebuggees();
        for (GlobalObjectSet::Enum e(debuggees); !e.empty(); e.popFront()) {
            GlobalObject *global = e.front();
            if (!IsObjectMarked(&global))
                continue;
            else if (global != e.front())
                e.rekeyFront(global);

            /*
             * Every debuggee has at least one debugger, so in this case
             * getDebuggers can't return NULL.
             */
            const GlobalObject::DebuggerVector *debuggers = global->getDebuggers();
            JS_ASSERT(debuggers);
            for (Debugger * const *p = debuggers->begin(); p != debuggers->end(); p++) {
                Debugger *dbg = *p;

                /*
                 * dbg is a Debugger with at least one debuggee. Check three things:
                 *   - dbg is actually in a compartment that is being marked
                 *   - it isn't already marked
                 *   - it actually has hooks that might be called
                 */
                HeapPtrObject &dbgobj = dbg->toJSObjectRef();
                if (!dbgobj->zone()->isGCMarking())
                    continue;

                bool dbgMarked = IsObjectMarked(&dbgobj);
                if (!dbgMarked && dbg->hasAnyLiveHooks()) {
                    /*
                     * obj could be reachable only via its live, enabled
                     * debugger hooks, which may yet be called.
                     */
                    MarkObject(trc, &dbgobj, "enabled Debugger");
                    markedAny = true;
                    dbgMarked = true;
                }

                if (dbgMarked) {
                    /* Search for breakpoints to mark. */
                    for (Breakpoint *bp = dbg->firstBreakpoint(); bp; bp = bp->nextInDebugger()) {
                        if (IsScriptMarked(&bp->site->script)) {
                            /*
                             * The debugger and the script are both live.
                             * Therefore the breakpoint handler is live.
                             */
                            if (!IsObjectMarked(&bp->getHandlerRef())) {
                                MarkObject(trc, &bp->getHandlerRef(), "breakpoint handler");
                                markedAny = true;
                            }
                        }
                    }
                }
            }
        }
    }
    return markedAny;
}

void
Debugger::traceObject(JSTracer *trc, RawObject obj)
{
    if (Debugger *dbg = Debugger::fromJSObject(obj))
        dbg->trace(trc);
}

void
Debugger::trace(JSTracer *trc)
{
    if (uncaughtExceptionHook)
        MarkObject(trc, &uncaughtExceptionHook, "hooks");

    /*
     * Mark Debugger.Frame objects. These are all reachable from JS, because the
     * corresponding StackFrames are still on the stack.
     *
     * (Once we support generator frames properly, we will need
     * weakly-referenced Debugger.Frame objects as well, for suspended generator
     * frames.)
     */
    for (FrameMap::Range r = frames.all(); !r.empty(); r.popFront()) {
        RelocatablePtrObject &frameobj = r.front().value;
        JS_ASSERT(frameobj->getPrivate());
        MarkObject(trc, &frameobj, "live Debugger.Frame");
    }

    /* Trace the weak map from JSScript instances to Debugger.Script objects. */
    scripts.trace(trc);

    /* Trace the referent -> Debugger.Object weak map. */
    objects.trace(trc);

    /* Trace the referent -> Debugger.Environment weak map. */
    environments.trace(trc);
}

void
Debugger::sweepAll(FreeOp *fop)
{
    JSRuntime *rt = fop->runtime();

    for (Debugger *dbg = rt->debuggerList.getFirst(); dbg; dbg = dbg->getNext()) {
        if (IsObjectAboutToBeFinalized(&dbg->object)) {
            /*
             * dbg is being GC'd. Detach it from its debuggees. The debuggee
             * might be GC'd too. Since detaching requires access to both
             * objects, this must be done before finalize time.
             */
            for (GlobalObjectSet::Enum e(dbg->debuggees); !e.empty(); e.popFront())
                dbg->removeDebuggeeGlobal(fop, e.front(), NULL, &e);
        }
    }

    for (CompartmentsIter comp(rt); !comp.done(); comp.next()) {
        /* For each debuggee being GC'd, detach it from all its debuggers. */
        GlobalObjectSet &debuggees = comp->getDebuggees();
        for (GlobalObjectSet::Enum e(debuggees); !e.empty(); e.popFront()) {
            GlobalObject *global = e.front();
            if (IsObjectAboutToBeFinalized(&global))
                detachAllDebuggersFromGlobal(fop, global, &e);
            else if (global != e.front())
                e.rekeyFront(global);
        }
    }
}

void
Debugger::detachAllDebuggersFromGlobal(FreeOp *fop, GlobalObject *global,
                                       GlobalObjectSet::Enum *compartmentEnum)
{
    const GlobalObject::DebuggerVector *debuggers = global->getDebuggers();
    JS_ASSERT(!debuggers->empty());
    while (!debuggers->empty())
        debuggers->back()->removeDebuggeeGlobal(fop, global, compartmentEnum, NULL);
}

/* static */ void
Debugger::findCompartmentEdges(Zone *zone, js::gc::ComponentFinder<Zone> &finder)
{
    /*
     * For debugger cross compartment wrappers, add edges in the opposite
     * direction to those already added by JSCompartment::findOutgoingEdges.
     * This ensure that debuggers and their debuggees are finalized in the same
     * group.
     */
    for (Debugger *dbg = zone->rt->debuggerList.getFirst(); dbg; dbg = dbg->getNext()) {
        Zone *w = dbg->object->zone();
        if (w == zone || !w->isGCMarking())
            continue;
        if (dbg->scripts.hasKeyInZone(zone) ||
            dbg->objects.hasKeyInZone(zone) ||
            dbg->environments.hasKeyInZone(zone))
        {
            finder.addEdgeTo(w);
        }
    }
}

void
Debugger::finalize(FreeOp *fop, RawObject obj)
{
    Debugger *dbg = fromJSObject(obj);
    if (!dbg)
        return;
    JS_ASSERT(dbg->debuggees.empty());
    fop->delete_(dbg);
}

Class Debugger::jsclass = {
    "Debugger",
    JSCLASS_HAS_PRIVATE | JSCLASS_IMPLEMENTS_BARRIERS |
    JSCLASS_HAS_RESERVED_SLOTS(JSSLOT_DEBUG_COUNT),
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, Debugger::finalize,
    NULL,                 /* checkAccess */
    NULL,                 /* call        */
    NULL,                 /* construct   */
    NULL,                 /* hasInstance */
    Debugger::traceObject
};

Debugger *
Debugger::fromThisValue(JSContext *cx, const CallArgs &args, const char *fnname)
{
    if (!args.thisv().isObject()) {
        ReportObjectRequired(cx);
        return NULL;
    }
    JSObject *thisobj = &args.thisv().toObject();
    if (thisobj->getClass() != &Debugger::jsclass) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_INCOMPATIBLE_PROTO,
                             "Debugger", fnname, thisobj->getClass()->name);
        return NULL;
    }

    /*
     * Forbid Debugger.prototype, which is of the Debugger JSClass but isn't
     * really a Debugger object. The prototype object is distinguished by
     * having a NULL private value.
     */
    Debugger *dbg = fromJSObject(thisobj);
    if (!dbg) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_INCOMPATIBLE_PROTO,
                             "Debugger", fnname, "prototype object");
    }
    return dbg;
}

#define THIS_DEBUGGER(cx, argc, vp, fnname, args, dbg)                       \
    CallArgs args = CallArgsFromVp(argc, vp);                                \
    Debugger *dbg = Debugger::fromThisValue(cx, args, fnname);               \
    if (!dbg)                                                                \
        return false

JSBool
Debugger::getEnabled(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGGER(cx, argc, vp, "get enabled", args, dbg);
    args.rval().setBoolean(dbg->enabled);
    return true;
}

JSBool
Debugger::setEnabled(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.set enabled", 1);
    THIS_DEBUGGER(cx, argc, vp, "set enabled", args, dbg);
    bool enabled = ToBoolean(args[0]);

    if (enabled != dbg->enabled) {
        for (Breakpoint *bp = dbg->firstBreakpoint(); bp; bp = bp->nextInDebugger()) {
            if (enabled)
                bp->site->inc(cx->runtime->defaultFreeOp());
            else
                bp->site->dec(cx->runtime->defaultFreeOp());
        }

        /*
         * Add or remove ourselves from the runtime's list of Debuggers
         * that care about new globals.
         */
        if (dbg->getHook(OnNewGlobalObject)) {
            if (enabled) {
                /* If we were not enabled, the link should be a singleton list. */
                JS_ASSERT(JS_CLIST_IS_EMPTY(&dbg->onNewGlobalObjectWatchersLink));
                JS_APPEND_LINK(&dbg->onNewGlobalObjectWatchersLink,
                               &cx->runtime->onNewGlobalObjectWatchers);
            } else {
                /* If we were enabled, the link should be inserted in the list. */
                JS_ASSERT(!JS_CLIST_IS_EMPTY(&dbg->onNewGlobalObjectWatchersLink));
                JS_REMOVE_AND_INIT_LINK(&dbg->onNewGlobalObjectWatchersLink);
            }
        }
    }

    dbg->enabled = enabled;
    args.rval().setUndefined();
    return true;
}

JSBool
Debugger::getHookImpl(JSContext *cx, unsigned argc, Value *vp, Hook which)
{
    JS_ASSERT(which >= 0 && which < HookCount);
    THIS_DEBUGGER(cx, argc, vp, "getHook", args, dbg);
    args.rval().set(dbg->object->getReservedSlot(JSSLOT_DEBUG_HOOK_START + which));
    return true;
}

JSBool
Debugger::setHookImpl(JSContext *cx, unsigned argc, Value *vp, Hook which)
{
    JS_ASSERT(which >= 0 && which < HookCount);
    REQUIRE_ARGC("Debugger.setHook", 1);
    THIS_DEBUGGER(cx, argc, vp, "setHook", args, dbg);
    if (args[0].isObject()) {
        if (!args[0].toObject().isCallable())
            return ReportIsNotFunction(cx, args[0], args.length() - 1);
    } else if (!args[0].isUndefined()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NOT_CALLABLE_OR_UNDEFINED);
        return false;
    }
    dbg->object->setReservedSlot(JSSLOT_DEBUG_HOOK_START + which, args[0]);
    args.rval().setUndefined();
    return true;
}

JSBool
Debugger::getOnDebuggerStatement(JSContext *cx, unsigned argc, Value *vp)
{
    return getHookImpl(cx, argc, vp, OnDebuggerStatement);
}

JSBool
Debugger::setOnDebuggerStatement(JSContext *cx, unsigned argc, Value *vp)
{
    return setHookImpl(cx, argc, vp, OnDebuggerStatement);
}

JSBool
Debugger::getOnExceptionUnwind(JSContext *cx, unsigned argc, Value *vp)
{
    return getHookImpl(cx, argc, vp, OnExceptionUnwind);
}

JSBool
Debugger::setOnExceptionUnwind(JSContext *cx, unsigned argc, Value *vp)
{
    return setHookImpl(cx, argc, vp, OnExceptionUnwind);
}

JSBool
Debugger::getOnNewScript(JSContext *cx, unsigned argc, Value *vp)
{
    return getHookImpl(cx, argc, vp, OnNewScript);
}

JSBool
Debugger::setOnNewScript(JSContext *cx, unsigned argc, Value *vp)
{
    return setHookImpl(cx, argc, vp, OnNewScript);
}

JSBool
Debugger::getOnEnterFrame(JSContext *cx, unsigned argc, Value *vp)
{
    return getHookImpl(cx, argc, vp, OnEnterFrame);
}

JSBool
Debugger::setOnEnterFrame(JSContext *cx, unsigned argc, Value *vp)
{
    return setHookImpl(cx, argc, vp, OnEnterFrame);
}

JSBool
Debugger::getOnNewGlobalObject(JSContext *cx, unsigned argc, Value *vp)
{
    return getHookImpl(cx, argc, vp, OnNewGlobalObject);
}

JSBool
Debugger::setOnNewGlobalObject(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGGER(cx, argc, vp, "setOnNewGlobalObject", args, dbg);
    RootedObject oldHook(cx, dbg->getHook(OnNewGlobalObject));

    if (!setHookImpl(cx, argc, vp, OnNewGlobalObject))
        return false;

    /*
     * Add or remove ourselves from the runtime's list of Debuggers that
     * care about new globals.
     */
    if (dbg->enabled) {
        JSObject *newHook = dbg->getHook(OnNewGlobalObject);
        if (!oldHook && newHook) {
            /* If we didn't have a hook, the link should be a singleton list. */
            JS_ASSERT(JS_CLIST_IS_EMPTY(&dbg->onNewGlobalObjectWatchersLink));
            JS_APPEND_LINK(&dbg->onNewGlobalObjectWatchersLink,
                           &cx->runtime->onNewGlobalObjectWatchers);
        } else if (oldHook && !newHook) {
            /* If we did have a hook, the link should be inserted in the list. */
            JS_ASSERT(!JS_CLIST_IS_EMPTY(&dbg->onNewGlobalObjectWatchersLink));
            JS_REMOVE_AND_INIT_LINK(&dbg->onNewGlobalObjectWatchersLink);
        }
    }

    return true;
}

JSBool
Debugger::getUncaughtExceptionHook(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGGER(cx, argc, vp, "get uncaughtExceptionHook", args, dbg);
    args.rval().setObjectOrNull(dbg->uncaughtExceptionHook);
    return true;
}

JSBool
Debugger::setUncaughtExceptionHook(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.set uncaughtExceptionHook", 1);
    THIS_DEBUGGER(cx, argc, vp, "set uncaughtExceptionHook", args, dbg);
    if (!args[0].isNull() && (!args[0].isObject() || !args[0].toObject().isCallable())) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_ASSIGN_FUNCTION_OR_NULL,
                             "uncaughtExceptionHook");
        return false;
    }

    dbg->uncaughtExceptionHook = args[0].toObjectOrNull();
    args.rval().setUndefined();
    return true;
}

GlobalObject *
Debugger::unwrapDebuggeeArgument(JSContext *cx, const Value &v)
{
    if (!v.isObject()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_UNEXPECTED_TYPE,
                             "argument", "not a global object");
        return NULL;
    }

    RootedObject obj(cx, &v.toObject());

    /* If it's a Debugger.Object belonging to this debugger, dereference that. */
    if (obj->getClass() == &DebuggerObject_class) {
        RootedValue rv(cx, v);
        if (!unwrapDebuggeeValue(cx, &rv))
            return NULL;
        obj = &rv.toObject();
    }

    /* If we have a cross-compartment wrapper, dereference as far as is secure. */
    obj = UnwrapObjectChecked(obj);
    if (!obj) {
        JS_ReportError(cx, "Permission denied to access object");
        return NULL;
    }

    /* If that produced an outer window, innerize it. */
    obj = GetInnerObject(cx, obj);
    if (!obj)
        return NULL;

    /* If that didn't produce a global object, it's an error. */
    if (!obj->isGlobal()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_UNEXPECTED_TYPE,
                             "argument", "not a global object");
        return NULL;
    }

    return &obj->asGlobal();
}

JSBool
Debugger::addDebuggee(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.addDebuggee", 1);
    THIS_DEBUGGER(cx, argc, vp, "addDebuggee", args, dbg);
    Rooted<GlobalObject*> global(cx, dbg->unwrapDebuggeeArgument(cx, args[0]));
    if (!global)
        return false;

    if (!dbg->addDebuggeeGlobal(cx, global))
        return false;

    RootedValue v(cx, ObjectValue(*global));
    if (!dbg->wrapDebuggeeValue(cx, &v))
        return false;
    args.rval().set(v);
    return true;
}

JSBool
Debugger::addAllGlobalsAsDebuggees(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGGER(cx, argc, vp, "addAllGlobalsAsDebuggees", args, dbg);
    AutoDebugModeGC dmgc(cx->runtime);
    for (CompartmentsIter c(cx->runtime); !c.done(); c.next()) {
        if (c == dbg->object->compartment())
            continue;
        c->zone()->scheduledForDestruction = false;
        GlobalObject *global = c->maybeGlobal();
        if (global) {
            Rooted<GlobalObject*> rg(cx, global);
            dbg->addDebuggeeGlobal(cx, rg, dmgc);
        }
    }

    args.rval().setUndefined();
    return true;
}

JSBool
Debugger::removeDebuggee(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.removeDebuggee", 1);
    THIS_DEBUGGER(cx, argc, vp, "removeDebuggee", args, dbg);
    GlobalObject *global = dbg->unwrapDebuggeeArgument(cx, args[0]);
    if (!global)
        return false;
    if (dbg->debuggees.has(global))
        dbg->removeDebuggeeGlobal(cx->runtime->defaultFreeOp(), global, NULL, NULL);
    args.rval().setUndefined();
    return true;
}

JSBool
Debugger::removeAllDebuggees(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGGER(cx, argc, vp, "removeAllDebuggees", args, dbg);
    AutoDebugModeGC dmgc(cx->runtime);
    for (GlobalObjectSet::Enum e(dbg->debuggees); !e.empty(); e.popFront())
        dbg->removeDebuggeeGlobal(cx->runtime->defaultFreeOp(), e.front(), dmgc, NULL, &e);
    args.rval().setUndefined();
    return true;
}

JSBool
Debugger::hasDebuggee(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.hasDebuggee", 1);
    THIS_DEBUGGER(cx, argc, vp, "hasDebuggee", args, dbg);
    GlobalObject *global = dbg->unwrapDebuggeeArgument(cx, args[0]);
    if (!global)
        return false;
    args.rval().setBoolean(!!dbg->debuggees.lookup(global));
    return true;
}

JSBool
Debugger::getDebuggees(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGGER(cx, argc, vp, "getDebuggees", args, dbg);
    RootedObject arrobj(cx, NewDenseAllocatedArray(cx, dbg->debuggees.count()));
    if (!arrobj)
        return false;
    arrobj->ensureDenseInitializedLength(cx, 0, dbg->debuggees.count());
    unsigned i = 0;
    for (GlobalObjectSet::Enum e(dbg->debuggees); !e.empty(); e.popFront()) {
        RootedValue v(cx, ObjectValue(*e.front()));
        if (!dbg->wrapDebuggeeValue(cx, &v))
            return false;
        arrobj->setDenseElement(i++, v);
    }
    args.rval().setObject(*arrobj);
    return true;
}

JSBool
Debugger::getNewestFrame(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGGER(cx, argc, vp, "getNewestFrame", args, dbg);

    /*
     * cx->fp() would return the topmost frame in the current context.
     * Since there may be multiple contexts, use AllFramesIter instead.
     */
    for (AllFramesIter i(cx->runtime); !i.done(); ++i) {
        /*
         * Debug-mode currently disables Ion compilation in the compartment of
         * the debuggee.
         */
        if (i.isIon())
            continue;
        if (dbg->observesFrame(i.abstractFramePtr())) {
            ScriptFrameIter iter(i.seg()->cx(), StackIter::GO_THROUGH_SAVED);
            while (iter.isIon() || iter.abstractFramePtr() != i.abstractFramePtr())
                ++iter;
            return dbg->getScriptFrame(cx, iter, args.rval());
        }
    }
    args.rval().setNull();
    return true;
}

JSBool
Debugger::clearAllBreakpoints(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGGER(cx, argc, vp, "clearAllBreakpoints", args, dbg);
    for (GlobalObjectSet::Range r = dbg->debuggees.all(); !r.empty(); r.popFront())
        r.front()->compartment()->clearBreakpointsIn(cx->runtime->defaultFreeOp(), dbg, NULL);
    return true;
}

JSBool
Debugger::construct(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /* Check that the arguments, if any, are cross-compartment wrappers. */
    for (unsigned i = 0; i < argc; i++) {
        const Value &arg = args[i];
        if (!arg.isObject())
            return ReportObjectRequired(cx);
        JSObject *argobj = &arg.toObject();
        if (!IsCrossCompartmentWrapper(argobj)) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CCW_REQUIRED, "Debugger");
            return false;
        }
    }

    /* Get Debugger.prototype. */
    RootedValue v(cx);
    RootedObject callee(cx, &args.callee());
    if (!JSObject::getProperty(cx, callee, callee, cx->names().classPrototype, &v))
        return false;
    RootedObject proto(cx, &v.toObject());
    JS_ASSERT(proto->getClass() == &Debugger::jsclass);

    /*
     * Make the new Debugger object. Each one has a reference to
     * Debugger.{Frame,Object,Script}.prototype in reserved slots. The rest of
     * the reserved slots are for hooks; they default to undefined.
     */
    RootedObject obj(cx, NewObjectWithGivenProto(cx, &Debugger::jsclass, proto, NULL));
    if (!obj)
        return false;
    for (unsigned slot = JSSLOT_DEBUG_PROTO_START; slot < JSSLOT_DEBUG_PROTO_STOP; slot++)
        obj->setReservedSlot(slot, proto->getReservedSlot(slot));

    Debugger *dbg = cx->new_<Debugger>(cx, obj.get());
    if (!dbg)
        return false;
    obj->setPrivate(dbg);
    if (!dbg->init(cx)) {
        js_delete(dbg);
        return false;
    }

    /* Add the initial debuggees, if any. */
    for (unsigned i = 0; i < argc; i++) {
        Rooted<GlobalObject*> debuggee(cx, &GetProxyPrivate(&args[i].toObject()).toObject().global());
        if (!dbg->addDebuggeeGlobal(cx, debuggee))
            return false;
    }

    args.rval().setObject(*obj);
    return true;
}

bool
Debugger::addDebuggeeGlobal(JSContext *cx, Handle<GlobalObject*> global)
{
    AutoDebugModeGC dmgc(cx->runtime);
    return addDebuggeeGlobal(cx, global, dmgc);
}

bool
Debugger::addDebuggeeGlobal(JSContext *cx,
                            Handle<GlobalObject*> global,
                            AutoDebugModeGC &dmgc)
{
    if (debuggees.has(global))
        return true;

    JSCompartment *debuggeeCompartment = global->compartment();

    /*
     * Check for cycles. If global's compartment is reachable from this
     * Debugger object's compartment by following debuggee-to-debugger links,
     * then adding global would create a cycle. (Typically nobody is debugging
     * the debugger, in which case we zip through this code without looping.)
     */
    Vector<JSCompartment *> visited(cx);
    if (!visited.append(object->compartment()))
        return false;
    for (size_t i = 0; i < visited.length(); i++) {
        JSCompartment *c = visited[i];
        if (c == debuggeeCompartment) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_DEBUG_LOOP);
            return false;
        }

        /*
         * Find all compartments containing debuggers debugging global objects
         * in c. Add those compartments to visited.
         */
        for (GlobalObjectSet::Range r = c->getDebuggees().all(); !r.empty(); r.popFront()) {
            GlobalObject::DebuggerVector *v = r.front()->getDebuggers();
            for (Debugger **p = v->begin(); p != v->end(); p++) {
                JSCompartment *next = (*p)->object->compartment();
                if (Find(visited, next) == visited.end() && !visited.append(next))
                    return false;
            }
        }
    }

    /* Refuse to enable debug mode for a compartment that has running scripts. */
    if (!debuggeeCompartment->debugMode() && debuggeeCompartment->hasScriptsOnStack()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_DEBUG_NOT_IDLE);
        return false;
    }

    /*
     * Each debugger-debuggee relation must be stored in up to three places.
     * JSCompartment::addDebuggee enables debug mode if needed.
     */
    AutoCompartment ac(cx, global);
    GlobalObject::DebuggerVector *v = GlobalObject::getOrCreateDebuggers(cx, global);
    if (!v || !v->append(this)) {
        js_ReportOutOfMemory(cx);
    } else {
        if (!debuggees.put(global)) {
            js_ReportOutOfMemory(cx);
        } else {
            if (global->getDebuggers()->length() > 1)
                return true;
            if (debuggeeCompartment->addDebuggee(cx, global, dmgc))
                return true;

            /* Maintain consistency on error. */
            debuggees.remove(global);
        }
        JS_ASSERT(v->back() == this);
        v->popBack();
    }
    return false;
}

void
Debugger::removeDebuggeeGlobal(FreeOp *fop, GlobalObject *global,
                               GlobalObjectSet::Enum *compartmentEnum,
                               GlobalObjectSet::Enum *debugEnum)
{
    AutoDebugModeGC dmgc(global->compartment()->rt);
    return removeDebuggeeGlobal(fop, global, dmgc, compartmentEnum, debugEnum);
}

void
Debugger::removeDebuggeeGlobal(FreeOp *fop, GlobalObject *global,
                               AutoDebugModeGC &dmgc,
                               GlobalObjectSet::Enum *compartmentEnum,
                               GlobalObjectSet::Enum *debugEnum)
{
    /*
     * Each debuggee is in two HashSets: one for its compartment and one for
     * its debugger (this). The caller might be enumerating either set; if so,
     * use HashSet::Enum::removeFront rather than HashSet::remove below, to
     * avoid invalidating the live enumerator.
     */
    JS_ASSERT(global->compartment()->getDebuggees().has(global));
    JS_ASSERT_IF(compartmentEnum, compartmentEnum->front() == global);
    JS_ASSERT(debuggees.has(global));
    JS_ASSERT_IF(debugEnum, debugEnum->front() == global);

    /*
     * FIXME Debugger::slowPathOnLeaveFrame needs to kill all Debugger.Frame
     * objects referring to a particular js::StackFrame. This is hard if
     * Debugger objects that are no longer debugging the relevant global might
     * have live Frame objects. So we take the easy way out and kill them here.
     * This is a bug, since it's observable and contrary to the spec. One
     * possible fix would be to put such objects into a compartment-wide bag
     * which slowPathOnLeaveFrame would have to examine.
     */
    for (FrameMap::Enum e(frames); !e.empty(); e.popFront()) {
        AbstractFramePtr frame = e.front().key;
        if (&frame.script()->global() == global) {
            DebuggerFrame_freeStackIterData(fop, e.front().value);
            e.removeFront();
        }
    }

    GlobalObject::DebuggerVector *v = global->getDebuggers();
    Debugger **p;
    for (p = v->begin(); p != v->end(); p++) {
        if (*p == this)
            break;
    }
    JS_ASSERT(p != v->end());

    /*
     * The relation must be removed from up to three places: *v and debuggees
     * for sure, and possibly the compartment's debuggee set.
     */
    v->erase(p);
    if (debugEnum)
        debugEnum->removeFront();
    else
        debuggees.remove(global);

    /*
     * The debuggee needs to be removed from the compartment last, as this can
     * trigger GCs if the compartment's debug mode is being changed, and the
     * global cannot be rooted on the stack without a cx.
     */
    if (v->empty())
        global->compartment()->removeDebuggee(fop, global, dmgc, compartmentEnum);
}

/*
 * A class for parsing 'findScripts' query arguments and searching for
 * scripts that match the criteria they represent.
 */
class Debugger::ScriptQuery {
  public:
    /* Construct a ScriptQuery to use matching scripts for |dbg|. */
    ScriptQuery(JSContext *cx, Debugger *dbg):
        cx(cx), debugger(dbg), compartments(cx), url(cx), innermostForCompartment(cx) {}

    /*
     * Initialize this ScriptQuery. Raise an error and return false if we
     * haven't enough memory.
     */
    bool init() {
        if (!compartments.init() ||
            !innermostForCompartment.init())
        {
            js_ReportOutOfMemory(cx);
            return false;
        }

        return true;
    }

    /*
     * Parse the query object |query|, and prepare to match only the scripts
     * it specifies.
     */
    bool parseQuery(HandleObject query) {
        /*
         * Check for a 'global' property, which limits the results to those
         * scripts scoped to a particular global object.
         */
        RootedValue global(cx);
        if (!JSObject::getProperty(cx, query, query, cx->names().global, &global))
            return false;
        if (global.isUndefined()) {
            matchAllDebuggeeGlobals();
        } else {
            GlobalObject *globalObject = debugger->unwrapDebuggeeArgument(cx, global);
            if (!globalObject)
                return false;

            /*
             * If the given global isn't a debuggee, just leave the set of
             * acceptable globals empty; we'll return no scripts.
             */
            if (debugger->debuggees.has(globalObject)) {
                if (!matchSingleGlobal(globalObject))
                    return false;
            }
        }

        /* Check for a 'url' property. */
        if (!JSObject::getProperty(cx, query, query, cx->names().url, &url))
            return false;
        if (!url.isUndefined() && !url.isString()) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_UNEXPECTED_TYPE,
                                 "query object's 'url' property", "neither undefined nor a string");
            return false;
        }

        /* Check for a 'line' property. */
        RootedValue lineProperty(cx);
        if (!JSObject::getProperty(cx, query, query, cx->names().line, &lineProperty))
            return false;
        if (lineProperty.isUndefined()) {
            hasLine = false;
        } else if (lineProperty.isNumber()) {
            if (url.isUndefined()) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_QUERY_LINE_WITHOUT_URL);
                return false;
            }
            double doubleLine = lineProperty.toNumber();
            if (doubleLine <= 0 || (unsigned int) doubleLine != doubleLine) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_DEBUG_BAD_LINE);
                return false;
            }
            hasLine = true;
            line = doubleLine;
        } else {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_UNEXPECTED_TYPE,
                                 "query object's 'line' property",
                                 "neither undefined nor an integer");
            return false;
        }

        /* Check for an 'innermost' property. */
        PropertyName *innermostName = cx->names().innermost;
        RootedValue innermostProperty(cx);
        if (!JSObject::getProperty(cx, query, query, innermostName, &innermostProperty))
            return false;
        innermost = ToBoolean(innermostProperty);
        if (innermost) {
            /* Technically, we need only check hasLine, but this is clearer. */
            if (url.isUndefined() || !hasLine) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_QUERY_INNERMOST_WITHOUT_LINE_URL);
                return false;
            }
        }

        return true;
    }

    /* Set up this ScriptQuery appropriately for a missing query argument. */
    bool omittedQuery() {
        url.setUndefined();
        hasLine = false;
        innermost = false;
        return matchAllDebuggeeGlobals();
    }

    /*
     * Search all relevant compartments and the stack for scripts matching
     * this query, and append the matching scripts to |vector|.
     */
    bool findScripts(AutoScriptVector *v) {
        if (!prepareQuery())
            return false;

        /* Search each compartment for debuggee scripts. */
        vector = v;
        oom = false;
        IterateScripts(cx->runtime, NULL, this, considerScript);
        if (oom) {
            js_ReportOutOfMemory(cx);
            return false;
        }

        /*
         * For most queries, we just accumulate results in 'vector' as we find
         * them. But if this is an 'innermost' query, then we've accumulated the
         * results in the 'innermostForCompartment' map. In that case, we now need to
         * walk that map and populate 'vector'.
         */
        if (innermost) {
            for (CompartmentToScriptMap::Range r = innermostForCompartment.all();
                 !r.empty();
                 r.popFront()) {
                if (!v->append(r.front().value)) {
                    js_ReportOutOfMemory(cx);
                    return false;
                }
            }
        }

        return true;
    }

  private:
    /* The context in which we should do our work. */
    JSContext *cx;

    /* The debugger for which we conduct queries. */
    Debugger *debugger;

    typedef HashSet<JSCompartment *, DefaultHasher<JSCompartment *>, RuntimeAllocPolicy>
        CompartmentSet;

    /* A script must be in one of these compartments to match the query. */
    CompartmentSet compartments;

    /* If this is a string, matching scripts have urls equal to it. */
    RootedValue url;

    /* url as a C string. */
    JSAutoByteString urlCString;

    /* True if the query contained a 'line' property. */
    bool hasLine;

    /* The line matching scripts must cover. */
    unsigned int line;

    /* True if the query has an 'innermost' property whose value is true. */
    bool innermost;

    typedef HashMap<JSCompartment *, JSScript *, DefaultHasher<JSCompartment *>, RuntimeAllocPolicy>
        CompartmentToScriptMap;

    /*
     * For 'innermost' queries, a map from compartments to the innermost script
     * we've seen so far in that compartment. (Template instantiation code size
     * explosion ho!)
     */
    CompartmentToScriptMap innermostForCompartment;

    /* The vector to which to append the scripts found. */
    AutoScriptVector *vector;

    /* Indicates whether OOM has occurred while matching. */
    bool oom;

    /* Arrange for this ScriptQuery to match only scripts that run in |global|. */
    bool matchSingleGlobal(GlobalObject *global) {
        JS_ASSERT(compartments.count() == 0);
        if (!compartments.put(global->compartment())) {
            js_ReportOutOfMemory(cx);
            return false;
        }
        return true;
    }

    /*
     * Arrange for this ScriptQuery to match all scripts running in debuggee
     * globals.
     */
    bool matchAllDebuggeeGlobals() {
        JS_ASSERT(compartments.count() == 0);
        /* Build our compartment set from the debugger's set of debuggee globals. */
        for (GlobalObjectSet::Range r = debugger->debuggees.all(); !r.empty(); r.popFront()) {
            if (!compartments.put(r.front()->compartment())) {
                js_ReportOutOfMemory(cx);
                return false;
            }
        }
        return true;
    }

    /*
     * Given that parseQuery or omittedQuery has been called, prepare to
     * match scripts. Set urlCString as appropriate.
     */
    bool prepareQuery() {
        /* Compute urlCString, if a url was given. */
        if (url.isString()) {
            if (!urlCString.encodeLatin1(cx, url.toString()))
                return false;
        }

        return true;
    }

    static void considerScript(JSRuntime *rt, void *data, JSScript *script) {
        ScriptQuery *self = static_cast<ScriptQuery *>(data);
        self->consider(script);
    }

    /*
     * If |script| matches this query, append it to |vector| or place it in
     * |innermostForCompartment|, as appropriate. Set |oom| if an out of memory
     * condition occurred.
     */
    void consider(JSScript *script) {
        if (oom || script->selfHosted)
            return;
        JSCompartment *compartment = script->compartment();
        if (!compartments.has(compartment))
            return;
        if (urlCString.ptr()) {
            if (!script->filename() || strcmp(script->filename(), urlCString.ptr()) != 0)
                return;
        }
        if (hasLine) {
            if (line < script->lineno || script->lineno + js_GetScriptLineExtent(script) < line)
                return;
        }
        if (innermost) {
            /*
             * For 'innermost' queries, we don't place scripts in |vector| right
             * away; we may later find another script that is nested inside this
             * one. Instead, we record the innermost script we've found so far
             * for each compartment in innermostForCompartment, and only
             * populate |vector| at the bottom of findScripts, when we've
             * traversed all the scripts.
             *
             * So: check this script against the innermost one we've found so
             * far (if any), as recorded in innermostForCompartment, and replace
             * that if it's better.
             */
            CompartmentToScriptMap::AddPtr p = innermostForCompartment.lookupForAdd(compartment);
            if (p) {
                /* Is our newly found script deeper than the last one we found? */
                JSScript *incumbent = p->value;
                if (script->staticLevel > incumbent->staticLevel)
                    p->value = script;
            } else {
                /*
                 * This is the first matching script we've encountered for this
                 * compartment, so it is thus the innermost such script.
                 */
                if (!innermostForCompartment.add(p, compartment, script)) {
                    oom = true;
                    return;
                }
            }
        } else {
            /* Record this matching script in the results vector. */
            if (!vector->append(script)) {
                oom = true;
                return;
            }
        }

        return;
    }
};

JSBool
Debugger::findScripts(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGGER(cx, argc, vp, "findScripts", args, dbg);

    ScriptQuery query(cx, dbg);
    if (!query.init())
        return false;

    if (argc >= 1) {
        RootedObject queryObject(cx, NonNullObject(cx, args[0]));
        if (!queryObject || !query.parseQuery(queryObject))
            return false;
    } else {
        if (!query.omittedQuery())
            return false;
    }

    /*
     * Accumulate the scripts in an AutoScriptVector, instead of creating
     * the JS array as we go, because we mustn't allocate JS objects or GC
     * while we use the CellIter.
     */
    AutoScriptVector scripts(cx);

    if (!query.findScripts(&scripts))
        return false;

    RootedObject result(cx, NewDenseAllocatedArray(cx, scripts.length()));
    if (!result)
        return false;

    result->ensureDenseInitializedLength(cx, 0, scripts.length());

    for (size_t i = 0; i < scripts.length(); i++) {
        JSObject *scriptObject = dbg->wrapScript(cx, scripts.handleAt(i));
        if (!scriptObject)
            return false;
        result->setDenseElement(i, ObjectValue(*scriptObject));
    }

    args.rval().setObject(*result);
    return true;
}

JSBool
Debugger::findAllGlobals(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGGER(cx, argc, vp, "findAllGlobals", args, dbg);

    RootedObject result(cx, NewDenseEmptyArray(cx));
    if (!result)
        return false;

    for (CompartmentsIter c(cx->runtime); !c.done(); c.next()) {
        c->zone()->scheduledForDestruction = false;

        GlobalObject *global = c->maybeGlobal();
        if (global) {
            /*
             * We pulled |global| out of nowhere, so it's possible that it was
             * marked gray by XPConnect. Since we're now exposing it to JS code,
             * we need to mark it black.
             */
            JS::ExposeGCThingToActiveJS(global, JSTRACE_OBJECT);

            RootedValue globalValue(cx, ObjectValue(*global));
            if (!dbg->wrapDebuggeeValue(cx, &globalValue))
                return false;
            if (!js_NewbornArrayPush(cx, result, globalValue))
                return false;
        }
    }

    args.rval().setObject(*result);
    return true;
}

JSPropertySpec Debugger::properties[] = {
    JS_PSGS("enabled", Debugger::getEnabled, Debugger::setEnabled, 0),
    JS_PSGS("onDebuggerStatement", Debugger::getOnDebuggerStatement,
            Debugger::setOnDebuggerStatement, 0),
    JS_PSGS("onExceptionUnwind", Debugger::getOnExceptionUnwind,
            Debugger::setOnExceptionUnwind, 0),
    JS_PSGS("onNewScript", Debugger::getOnNewScript, Debugger::setOnNewScript, 0),
    JS_PSGS("onEnterFrame", Debugger::getOnEnterFrame, Debugger::setOnEnterFrame, 0),
    JS_PSGS("onNewGlobalObject", Debugger::getOnNewGlobalObject, Debugger::setOnNewGlobalObject, 0),
    JS_PSGS("uncaughtExceptionHook", Debugger::getUncaughtExceptionHook,
            Debugger::setUncaughtExceptionHook, 0),
    JS_PS_END
};

JSFunctionSpec Debugger::methods[] = {
    JS_FN("addDebuggee", Debugger::addDebuggee, 1, 0),
    JS_FN("addAllGlobalsAsDebuggees", Debugger::addAllGlobalsAsDebuggees, 0, 0),
    JS_FN("removeDebuggee", Debugger::removeDebuggee, 1, 0),
    JS_FN("removeAllDebuggees", Debugger::removeAllDebuggees, 0, 0),
    JS_FN("hasDebuggee", Debugger::hasDebuggee, 1, 0),
    JS_FN("getDebuggees", Debugger::getDebuggees, 0, 0),
    JS_FN("getNewestFrame", Debugger::getNewestFrame, 0, 0),
    JS_FN("clearAllBreakpoints", Debugger::clearAllBreakpoints, 1, 0),
    JS_FN("findScripts", Debugger::findScripts, 1, 0),
    JS_FN("findAllGlobals", Debugger::findAllGlobals, 0, 0),
    JS_FS_END
};


/*** Debugger.Script *****************************************************************************/

static inline JSScript *
GetScriptReferent(JSObject *obj)
{
    JS_ASSERT(obj->getClass() == &DebuggerScript_class);
    return static_cast<JSScript *>(obj->getPrivate());
}

static inline void
SetScriptReferent(JSObject *obj, JSScript *script)
{
    JS_ASSERT(obj->getClass() == &DebuggerScript_class);
    obj->setPrivateGCThing(script);
}

static void
DebuggerScript_trace(JSTracer *trc, RawObject obj)
{
    /* This comes from a private pointer, so no barrier needed. */
    if (JSScript *script = GetScriptReferent(obj)) {
        MarkCrossCompartmentScriptUnbarriered(trc, obj, &script, "Debugger.Script referent");
        obj->setPrivateUnbarriered(script);
    }
}

Class DebuggerScript_class = {
    "Script",
    JSCLASS_HAS_PRIVATE | JSCLASS_IMPLEMENTS_BARRIERS |
    JSCLASS_HAS_RESERVED_SLOTS(JSSLOT_DEBUGSCRIPT_COUNT),
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, NULL,
    NULL,                 /* checkAccess */
    NULL,                 /* call        */
    NULL,                 /* construct   */
    NULL,                 /* hasInstance */
    DebuggerScript_trace
};

JSObject *
Debugger::newDebuggerScript(JSContext *cx, HandleScript script)
{
    assertSameCompartment(cx, object.get());

    JSObject *proto = &object->getReservedSlot(JSSLOT_DEBUG_SCRIPT_PROTO).toObject();
    JS_ASSERT(proto);
    JSObject *scriptobj = NewObjectWithGivenProto(cx, &DebuggerScript_class, proto, NULL);
    if (!scriptobj)
        return NULL;
    scriptobj->setReservedSlot(JSSLOT_DEBUGSCRIPT_OWNER, ObjectValue(*object));
    scriptobj->setPrivateGCThing(script);

    return scriptobj;
}

JSObject *
Debugger::wrapScript(JSContext *cx, HandleScript script)
{
    assertSameCompartment(cx, object.get());
    JS_ASSERT(cx->compartment != script->compartment());
    ScriptWeakMap::AddPtr p = scripts.lookupForAdd(script);
    if (!p) {
        JSObject *scriptobj = newDebuggerScript(cx, script);
        if (!scriptobj)
            return NULL;

        /* The allocation may have caused a GC, which can remove table entries. */
        if (!scripts.relookupOrAdd(p, script, scriptobj)) {
            js_ReportOutOfMemory(cx);
            return NULL;
        }

        CrossCompartmentKey key(CrossCompartmentKey::DebuggerScript, object, script);
        if (!object->compartment()->putWrapper(key, ObjectValue(*scriptobj))) {
            scripts.remove(script);
            js_ReportOutOfMemory(cx);
            return NULL;
        }
    }

    JS_ASSERT(GetScriptReferent(p->value) == script);
    return p->value;
}

static JSObject *
DebuggerScript_check(JSContext *cx, const Value &v, const char *clsname, const char *fnname)
{
    if (!v.isObject()) {
        ReportObjectRequired(cx);
        return NULL;
    }
    JSObject *thisobj = &v.toObject();
    if (thisobj->getClass() != &DebuggerScript_class) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_INCOMPATIBLE_PROTO,
                             clsname, fnname, thisobj->getClass()->name);
        return NULL;
    }

    /*
     * Check for Debugger.Script.prototype, which is of class DebuggerScript_class
     * but whose script is null.
     */
    if (!GetScriptReferent(thisobj)) {
        JS_ASSERT(!GetScriptReferent(thisobj));
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_INCOMPATIBLE_PROTO,
                             clsname, fnname, "prototype object");
        return NULL;
    }

    return thisobj;
}

static JSObject *
DebuggerScript_checkThis(JSContext *cx, const CallArgs &args, const char *fnname)
{
    return DebuggerScript_check(cx, args.thisv(), "Debugger.Script", fnname);
}

#define THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, fnname, args, obj, script)            \
    CallArgs args = CallArgsFromVp(argc, vp);                                       \
    RootedObject obj(cx, DebuggerScript_checkThis(cx, args, fnname));               \
    if (!obj)                                                                       \
        return false;                                                               \
    Rooted<JSScript*> script(cx, GetScriptReferent(obj))

static JSBool
DebuggerScript_getUrl(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "(get url)", args, obj, script);

    if (script->filename()) {
        JSString *str = js_NewStringCopyZ<CanGC>(cx, script->filename());
        if (!str)
            return false;
        args.rval().setString(str);
    } else {
        args.rval().setNull();
    }
    return true;
}

static JSBool
DebuggerScript_getStartLine(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "(get startLine)", args, obj, script);
    args.rval().setNumber(script->lineno);
    return true;
}

static JSBool
DebuggerScript_getLineCount(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "(get lineCount)", args, obj, script);

    unsigned maxLine = js_GetScriptLineExtent(script);
    args.rval().setNumber(double(maxLine));
    return true;
}

static JSBool
DebuggerScript_getStaticLevel(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "(get staticLevel)", args, obj, script);
    args.rval().setNumber(uint32_t(script->staticLevel));
    return true;
}

static JSBool
DebuggerScript_getSourceMapUrl(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "(get sourceMapURL)", args, obj, script);

    ScriptSource *source = script->scriptSource();
    JS_ASSERT(source);

    if (source->hasSourceMap()) {
        JSString *str = JS_NewUCStringCopyZ(cx, source->sourceMap());
        if (!str)
            return false;
        args.rval().setString(str);
    } else {
        args.rval().setNull();
    }

    return true;
}

static JSBool
DebuggerScript_getChildScripts(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "getChildScripts", args, obj, script);
    Debugger *dbg = Debugger::fromChildJSObject(obj);

    RootedObject result(cx, NewDenseEmptyArray(cx));
    if (!result)
        return false;
    if (script->hasObjects()) {
        /*
         * script->savedCallerFun indicates that this is a direct eval script
         * and the calling function is stored as script->objects()->vector[0].
         * It is not really a child script of this script, so skip it.
         */
        ObjectArray *objects = script->objects();
        RootedFunction fun(cx);
        RootedScript funScript(cx);
        RootedObject obj(cx), s(cx);
        for (uint32_t i = script->savedCallerFun ? 1 : 0; i < objects->length; i++) {
            obj = objects->vector[i];
            if (obj->isFunction()) {
                fun = static_cast<JSFunction *>(obj.get());
                funScript = fun->nonLazyScript();
                s = dbg->wrapScript(cx, funScript);
                if (!s || !js_NewbornArrayPush(cx, result, ObjectValue(*s)))
                    return false;
            }
        }
    }
    args.rval().setObject(*result);
    return true;
}

static bool
ScriptOffset(JSContext *cx, JSScript *script, const Value &v, size_t *offsetp)
{
    double d;
    size_t off;

    bool ok = v.isNumber();
    if (ok) {
        d = v.toNumber();
        off = size_t(d);
    }
    if (!ok || off != d || !IsValidBytecodeOffset(cx, script, off)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_DEBUG_BAD_OFFSET);
        return false;
    }
    *offsetp = off;
    return true;
}

static JSBool
DebuggerScript_getOffsetLine(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.Script.getOffsetLine", 1);
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "getOffsetLine", args, obj, script);
    size_t offset;
    if (!ScriptOffset(cx, script, args[0], &offset))
        return false;
    unsigned lineno = JS_PCToLineNumber(cx, script, script->code + offset);
    args.rval().setNumber(lineno);
    return true;
}

class BytecodeRangeWithPosition : private BytecodeRange
{
  public:
    using BytecodeRange::empty;
    using BytecodeRange::frontPC;
    using BytecodeRange::frontOpcode;
    using BytecodeRange::frontOffset;

    BytecodeRangeWithPosition(JSContext *cx, JSScript *script)
      : BytecodeRange(cx, script), lineno(script->lineno), column(0),
        sn(script->notes()), snpc(script->code)
    {
        if (!SN_IS_TERMINATOR(sn))
            snpc += SN_DELTA(sn);
        updatePosition();
        while (frontPC() != script->main())
            popFront();
    }

    void popFront() {
        BytecodeRange::popFront();
        if (!empty())
            updatePosition();
    }

    size_t frontLineNumber() const { return lineno; }
    size_t frontColumnNumber() const { return column; }

  private:
    void updatePosition() {
        /*
         * Determine the current line number by reading all source notes up to
         * and including the current offset.
         */
        while (!SN_IS_TERMINATOR(sn) && snpc <= frontPC()) {
            SrcNoteType type = (SrcNoteType) SN_TYPE(sn);
            if (type == SRC_COLSPAN) {
                ptrdiff_t colspan = js_GetSrcNoteOffset(sn, 0);

                if (colspan >= SN_COLSPAN_DOMAIN / 2)
                    colspan -= SN_COLSPAN_DOMAIN;
                JS_ASSERT(ptrdiff_t(column) + colspan >= 0);
                column += colspan;
            } if (type == SRC_SETLINE) {
                lineno = size_t(js_GetSrcNoteOffset(sn, 0));
                column = 0;
            } else if (type == SRC_NEWLINE) {
                lineno++;
                column = 0;
            }

            sn = SN_NEXT(sn);
            snpc += SN_DELTA(sn);
        }
    }

    size_t lineno;
    size_t column;
    jssrcnote *sn;
    jsbytecode *snpc;
};

/*
 * FlowGraphSummary::populate(cx, script) computes a summary of script's
 * control flow graph used by DebuggerScript_{getAllOffsets,getLineOffsets}.
 *
 * An instruction on a given line is an entry point for that line if it can be
 * reached from (an instruction on) a different line. We distinguish between the
 * following cases:
 *   - hasNoEdges:
 *       The instruction cannot be reached, so the instruction is not an entry
 *       point for the line it is on.
 *   - hasSingleEdge:
 *   - hasMultipleEdgesFromSingleLine:
 *       The instruction can be reached from a single line. If this line is
 *       different from the line the instruction is on, the instruction is an
 *       entry point for that line.
 *   - hasMultipleEdgesFromMultipleLines:
 *       The instruction can be reached from multiple lines. At least one of
 *       these lines is guaranteed to be different from the line the instruction
 *       is on, so the instruction is an entry point for that line.
 *
 * Similarly, an instruction on a given position (line/column pair) is an
 * entry point for that position if it can be reached from (an instruction on) a
 * different position. Again, we distinguish between the following cases:
 *   - hasNoEdges:
 *       The instruction cannot be reached, so the instruction is not an entry
 *       point for the position it is on.
 *   - hasSingleEdge:
 *       The instruction can be reached from a single position. If this line is
 *       different from the position the instruction is on, the instruction is
 *       an entry point for that position.
 *   - hasMultipleEdgesFromSingleLine:
 *   - hasMultipleEdgesFromMultipleLines:
 *       The instruction can be reached from multiple positions. At least one
 *       of these positions is guaranteed to be different from the position the
 *       instruction is on, so the instruction is an entry point for that
 *       position.
 */
class FlowGraphSummary {
  public:
    class Entry {
      public:
        static Entry createWithNoEdges() {
            return Entry(SIZE_MAX, 0);
        }

        static Entry createWithSingleEdge(size_t lineno, size_t column) {
            return Entry(lineno, column);
        }

        static Entry createWithMultipleEdgesFromSingleLine(size_t lineno) {
            return Entry(lineno, SIZE_MAX);
        }

        static Entry createWithMultipleEdgesFromMultipleLines() {
            return Entry(SIZE_MAX, SIZE_MAX);
        }

        Entry() {}

        bool hasNoEdges() const {
            return lineno_ == SIZE_MAX && column_ != SIZE_MAX;
        }

        bool hasSingleEdge() const {
            return lineno_ != SIZE_MAX && column_ != SIZE_MAX;
        }

        bool hasMultipleEdgesFromSingleLine() const {
            return lineno_ != SIZE_MAX && column_ == SIZE_MAX;
        }

        bool hasMultipleEdgesFromMultipleLines() const {
            return lineno_ == SIZE_MAX && column_ == SIZE_MAX;
        }

        bool operator==(const Entry &other) const {
            return lineno_ == other.lineno_ && column_ == other.column_;
        }

        bool operator!=(const Entry &other) const {
            return lineno_ != other.lineno_ || column_ != other.column_;
        }

        size_t lineno() const {
            return lineno_;
        }

        size_t column() const {
            return column_;
        }

      private:
        Entry(size_t lineno, size_t column) : lineno_(lineno), column_(column) {}

        size_t lineno_;
        size_t column_;
    };

    FlowGraphSummary(JSContext *cx) : entries_(cx) {}

    Entry &operator[](size_t index) {
        return entries_[index];
    }

    bool populate(JSContext *cx, JSScript *script) {
        if (!entries_.growBy(script->length))
            return false;
        unsigned mainOffset = script->main() - script->code;
        entries_[mainOffset] = Entry::createWithMultipleEdgesFromMultipleLines();
        for (size_t i = mainOffset + 1; i < script->length; i++)
            entries_[i] = Entry::createWithNoEdges();

        size_t prevLineno = script->lineno;
        size_t prevColumn = 0;
        JSOp prevOp = JSOP_NOP;
        for (BytecodeRangeWithPosition r(cx, script); !r.empty(); r.popFront()) {
            size_t lineno = r.frontLineNumber();
            size_t column = r.frontColumnNumber();
            JSOp op = r.frontOpcode();

            if (FlowsIntoNext(prevOp))
                addEdge(prevLineno, prevColumn, r.frontOffset());

            if (js_CodeSpec[op].type() == JOF_JUMP) {
                addEdge(lineno, column, r.frontOffset() + GET_JUMP_OFFSET(r.frontPC()));
            } else if (op == JSOP_TABLESWITCH) {
                jsbytecode *pc = r.frontPC();
                size_t offset = r.frontOffset();
                ptrdiff_t step = JUMP_OFFSET_LEN;
                size_t defaultOffset = offset + GET_JUMP_OFFSET(pc);
                pc += step;
                addEdge(lineno, column, defaultOffset);

                int32_t low = GET_JUMP_OFFSET(pc);
                pc += JUMP_OFFSET_LEN;
                int ncases = GET_JUMP_OFFSET(pc) - low + 1;
                pc += JUMP_OFFSET_LEN;

                for (int i = 0; i < ncases; i++) {
                    size_t target = offset + GET_JUMP_OFFSET(pc);
                    addEdge(lineno, column, target);
                    pc += step;
                }
            }

            prevLineno = lineno;
            prevColumn = column;
            prevOp = op;
        }

        return true;
    }

  private:
    void addEdge(size_t sourceLineno, size_t sourceColumn, size_t targetOffset) {
        if (entries_[targetOffset].hasNoEdges())
            entries_[targetOffset] = Entry::createWithSingleEdge(sourceLineno, sourceColumn);
        else if (entries_[targetOffset].lineno() != sourceLineno)
            entries_[targetOffset] = Entry::createWithMultipleEdgesFromMultipleLines();
        else if (entries_[targetOffset].column() != sourceColumn)
            entries_[targetOffset] = Entry::createWithMultipleEdgesFromSingleLine(sourceLineno);
    }

    Vector<Entry> entries_;
};

static JSBool
DebuggerScript_getAllOffsets(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "getAllOffsets", args, obj, script);

    /*
     * First pass: determine which offsets in this script are jump targets and
     * which line numbers jump to them.
     */
    FlowGraphSummary flowData(cx);
    if (!flowData.populate(cx, script))
        return false;

    /* Second pass: build the result array. */
    RootedObject result(cx, NewDenseEmptyArray(cx));
    if (!result)
        return false;
    for (BytecodeRangeWithPosition r(cx, script); !r.empty(); r.popFront()) {
        size_t offset = r.frontOffset();
        size_t lineno = r.frontLineNumber();

        /* Make a note, if the current instruction is an entry point for the current line. */
        if (!flowData[offset].hasNoEdges() && flowData[offset].lineno() != lineno) {
            /* Get the offsets array for this line. */
            RootedObject offsets(cx);
            RootedValue offsetsv(cx);

            RootedId id(cx, INT_TO_JSID(lineno));

            bool found;
            if (!JSObject::hasProperty(cx, result, id, &found))
                return false;
            if (found && !JSObject::getGeneric(cx, result, result, id, &offsetsv))
                return false;

            if (offsetsv.isObject()) {
                offsets = &offsetsv.toObject();
            } else {
                JS_ASSERT(offsetsv.isUndefined());

                /*
                 * Create an empty offsets array for this line.
                 * Store it in the result array.
                 */
                RootedId id(cx);
                offsets = NewDenseEmptyArray(cx);
                if (!offsets ||
                    !ValueToId<CanGC>(cx, NumberValue(lineno), &id))
                {
                    return false;
                }

                RootedValue value(cx, ObjectValue(*offsets));
                if (!JSObject::defineGeneric(cx, result, id, value))
                    return false;
            }

            /* Append the current offset to the offsets array. */
            if (!js_NewbornArrayPush(cx, offsets, NumberValue(offset)))
                return false;
        }
    }

    args.rval().setObject(*result);
    return true;
}

static JSBool
DebuggerScript_getAllColumnOffsets(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "getAllColumnOffsets", args, obj, script);

    /*
     * First pass: determine which offsets in this script are jump targets and
     * which positions jump to them.
     */
    FlowGraphSummary flowData(cx);
    if (!flowData.populate(cx, script))
        return false;

    /* Second pass: build the result array. */
    RootedObject result(cx, NewDenseEmptyArray(cx));
    if (!result)
        return false;
    for (BytecodeRangeWithPosition r(cx, script); !r.empty(); r.popFront()) {
        size_t lineno = r.frontLineNumber();
        size_t column = r.frontColumnNumber();
        size_t offset = r.frontOffset();

        /* Make a note, if the current instruction is an entry point for the current position. */
        if (!flowData[offset].hasNoEdges() &&
            (flowData[offset].lineno() != lineno ||
             flowData[offset].column() != column)) {
            RootedObject entry(cx, NewBuiltinClassInstance(cx, &ObjectClass));
            if (!entry)
                return false;

            RootedId id(cx, NameToId(cx->names().lineNumber));
            RootedValue value(cx, NumberValue(lineno));
            if (!JSObject::defineGeneric(cx, entry, id, value))
                return false;

            value = NumberValue(column);
            if (!JSObject::defineProperty(cx, entry, cx->names().columnNumber, value))
                return false;

            id = NameToId(cx->names().offset);
            value = NumberValue(offset);
            if (!JSObject::defineGeneric(cx, entry, id, value))
                return false;

            if (!js_NewbornArrayPush(cx, result, ObjectValue(*entry)))
                return false;
        }
    }

    args.rval().setObject(*result);
    return true;
}

static JSBool
DebuggerScript_getLineOffsets(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "getLineOffsets", args, obj, script);
    REQUIRE_ARGC("Debugger.Script.getLineOffsets", 1);

    /* Parse lineno argument. */
    size_t lineno;
    bool ok = false;
    if (args[0].isNumber()) {
        double d = args[0].toNumber();
        lineno = size_t(d);
        ok = (lineno == d);
    }
    if (!ok) {
        JS_ReportErrorNumber(cx,  js_GetErrorMessage, NULL, JSMSG_DEBUG_BAD_LINE);
        return false;
    }

    /*
     * First pass: determine which offsets in this script are jump targets and
     * which line numbers jump to them.
     */
    FlowGraphSummary flowData(cx);
    if (!flowData.populate(cx, script))
        return false;

    /* Second pass: build the result array. */
    RootedObject result(cx, NewDenseEmptyArray(cx));
    if (!result)
        return false;
    for (BytecodeRangeWithPosition r(cx, script); !r.empty(); r.popFront()) {
        size_t offset = r.frontOffset();

        /* If the op at offset is an entry point, append offset to result. */
        if (r.frontLineNumber() == lineno &&
            !flowData[offset].hasNoEdges() &&
            flowData[offset].lineno() != lineno)
        {
            if (!js_NewbornArrayPush(cx, result, NumberValue(offset)))
                return false;
        }
    }

    args.rval().setObject(*result);
    return true;
}

bool
Debugger::observesScript(JSScript *script) const
{
    if (!enabled)
        return false;
    return observesGlobal(&script->global()) && !script->selfHosted;
}

static JSBool
DebuggerScript_setBreakpoint(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.Script.setBreakpoint", 2);
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "setBreakpoint", args, obj, script);
    Debugger *dbg = Debugger::fromChildJSObject(obj);

    if (!dbg->observesScript(script)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_DEBUG_NOT_DEBUGGING);
        return false;
    }

    size_t offset;
    if (!ScriptOffset(cx, script, args[0], &offset))
        return false;

    JSObject *handler = NonNullObject(cx, args[1]);
    if (!handler)
        return false;

    jsbytecode *pc = script->code + offset;
    BreakpointSite *site = script->getOrCreateBreakpointSite(cx, pc);
    if (!site)
        return false;
    site->inc(cx->runtime->defaultFreeOp());
    if (cx->runtime->new_<Breakpoint>(dbg, site, handler)) {
        args.rval().setUndefined();
        return true;
    }
    site->dec(cx->runtime->defaultFreeOp());
    site->destroyIfEmpty(cx->runtime->defaultFreeOp());
    return false;
}

static JSBool
DebuggerScript_getBreakpoints(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "getBreakpoints", args, obj, script);
    Debugger *dbg = Debugger::fromChildJSObject(obj);

    jsbytecode *pc;
    if (argc > 0) {
        size_t offset;
        if (!ScriptOffset(cx, script, args[0], &offset))
            return false;
        pc = script->code + offset;
    } else {
        pc = NULL;
    }

    RootedObject arr(cx, NewDenseEmptyArray(cx));
    if (!arr)
        return false;

    for (unsigned i = 0; i < script->length; i++) {
        BreakpointSite *site = script->getBreakpointSite(script->code + i);
        if (site && (!pc || site->pc == pc)) {
            for (Breakpoint *bp = site->firstBreakpoint(); bp; bp = bp->nextInSite()) {
                if (bp->debugger == dbg &&
                    !js_NewbornArrayPush(cx, arr, ObjectValue(*bp->getHandler())))
                {
                    return false;
                }
            }
        }
    }
    args.rval().setObject(*arr);
    return true;
}

static JSBool
DebuggerScript_clearBreakpoint(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.Script.clearBreakpoint", 1);
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "clearBreakpoint", args, obj, script);
    Debugger *dbg = Debugger::fromChildJSObject(obj);

    JSObject *handler = NonNullObject(cx, args[0]);
    if (!handler)
        return false;

    script->clearBreakpointsIn(cx->runtime->defaultFreeOp(), dbg, handler);
    args.rval().setUndefined();
    return true;
}

static JSBool
DebuggerScript_clearAllBreakpoints(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGSCRIPT_SCRIPT(cx, argc, vp, "clearAllBreakpoints", args, obj, script);
    Debugger *dbg = Debugger::fromChildJSObject(obj);
    script->clearBreakpointsIn(cx->runtime->defaultFreeOp(), dbg, NULL);
    args.rval().setUndefined();
    return true;
}

static JSBool
DebuggerScript_construct(JSContext *cx, unsigned argc, Value *vp)
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NO_CONSTRUCTOR, "Debugger.Script");
    return false;
}

static JSPropertySpec DebuggerScript_properties[] = {
    JS_PSG("url", DebuggerScript_getUrl, 0),
    JS_PSG("startLine", DebuggerScript_getStartLine, 0),
    JS_PSG("lineCount", DebuggerScript_getLineCount, 0),
    JS_PSG("staticLevel", DebuggerScript_getStaticLevel, 0),
    JS_PSG("sourceMapURL", DebuggerScript_getSourceMapUrl, 0),
    JS_PS_END
};

static JSFunctionSpec DebuggerScript_methods[] = {
    JS_FN("getChildScripts", DebuggerScript_getChildScripts, 0, 0),
    JS_FN("getAllOffsets", DebuggerScript_getAllOffsets, 0, 0),
    JS_FN("getAllColumnOffsets", DebuggerScript_getAllColumnOffsets, 0, 0),
    JS_FN("getLineOffsets", DebuggerScript_getLineOffsets, 1, 0),
    JS_FN("getOffsetLine", DebuggerScript_getOffsetLine, 0, 0),
    JS_FN("setBreakpoint", DebuggerScript_setBreakpoint, 2, 0),
    JS_FN("getBreakpoints", DebuggerScript_getBreakpoints, 1, 0),
    JS_FN("clearBreakpoint", DebuggerScript_clearBreakpoint, 1, 0),
    JS_FN("clearAllBreakpoints", DebuggerScript_clearAllBreakpoints, 0, 0),
    JS_FS_END
};


/*** Debugger.Frame ******************************************************************************/

static void
DebuggerFrame_freeStackIterData(FreeOp *fop, RawObject obj)
{
    fop->delete_((StackIter::Data *)obj->getPrivate());
    obj->setPrivate(NULL);
}

static void
DebuggerFrame_finalize(FreeOp *fop, RawObject obj)
{
    DebuggerFrame_freeStackIterData(fop, obj);
}

Class DebuggerFrame_class = {
    "Frame", JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(JSSLOT_DEBUGFRAME_COUNT),
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, DebuggerFrame_finalize
};

static JSObject *
CheckThisFrame(JSContext *cx, const CallArgs &args, const char *fnname, bool checkLive)
{
    if (!args.thisv().isObject()) {
        ReportObjectRequired(cx);
        return NULL;
    }
    JSObject *thisobj = &args.thisv().toObject();
    if (thisobj->getClass() != &DebuggerFrame_class) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_INCOMPATIBLE_PROTO,
                             "Debugger.Frame", fnname, thisobj->getClass()->name);
        return NULL;
    }

    /*
     * Forbid Debugger.Frame.prototype, which is of class DebuggerFrame_class
     * but isn't really a working Debugger.Frame object. The prototype object
     * is distinguished by having a NULL private value. Also, forbid popped
     * frames.
     */
    if (!thisobj->getPrivate()) {
        if (thisobj->getReservedSlot(JSSLOT_DEBUGFRAME_OWNER).isUndefined()) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_INCOMPATIBLE_PROTO,
                                 "Debugger.Frame", fnname, "prototype object");
            return NULL;
        }
        if (checkLive) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_DEBUG_NOT_LIVE,
                                 "Debugger.Frame");
            return NULL;
        }
    }
    return thisobj;
}

#define THIS_FRAME(cx, argc, vp, fnname, args, thisobj, iter)                \
    CallArgs args = CallArgsFromVp(argc, vp);                                \
    RootedObject thisobj(cx, CheckThisFrame(cx, args, fnname, true));        \
    if (!thisobj)                                                            \
        return false;                                                        \
    ScriptFrameIter iter(*(StackIter::Data *)thisobj->getPrivate());

#define THIS_FRAME_OWNER(cx, argc, vp, fnname, args, thisobj, iter, dbg)       \
    THIS_FRAME(cx, argc, vp, fnname, args, thisobj, iter);                     \
    Debugger *dbg = Debugger::fromChildJSObject(thisobj)

static JSBool
DebuggerFrame_getType(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_FRAME(cx, argc, vp, "get type", args, thisobj, iter);

    /*
     * Indirect eval frames are both isGlobalFrame() and isEvalFrame(), so the
     * order of checks here is significant.
     */
    args.rval().setString(iter.isEvalFrame()
                          ? cx->names().eval
                          : iter.isGlobalFrame()
                          ? cx->names().global
                          : cx->names().call);
    return true;
}

static JSBool
DebuggerFrame_getEnvironment(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_FRAME_OWNER(cx, argc, vp, "get environment", args, thisobj, iter, dbg);

    Rooted<Env*> env(cx);
    {
        AutoCompartment ac(cx, iter.scopeChain());
        env = GetDebugScopeForFrame(cx, iter.abstractFramePtr());
        if (!env)
            return false;
    }

    return dbg->wrapEnvironment(cx, env, args.rval());
}

static JSBool
DebuggerFrame_getCallee(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_FRAME(cx, argc, vp, "get callee", args, thisobj, iter);
    RootedValue calleev(cx, (iter.isFunctionFrame() && !iter.isEvalFrame()) ? iter.calleev() : NullValue());
    if (!Debugger::fromChildJSObject(thisobj)->wrapDebuggeeValue(cx, &calleev))
        return false;
    args.rval().set(calleev);
    return true;
}

static JSBool
DebuggerFrame_getGenerator(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_FRAME(cx, argc, vp, "get generator", args, thisobj, iter);
    args.rval().setBoolean(iter.isGeneratorFrame());
    return true;
}

static JSBool
DebuggerFrame_getConstructing(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_FRAME(cx, argc, vp, "get constructing", args, thisobj, iter);
    args.rval().setBoolean(iter.isFunctionFrame() && iter.isConstructing());
    return true;
}

static JSBool
DebuggerFrame_getThis(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_FRAME(cx, argc, vp, "get this", args, thisobj, iter);
    RootedValue thisv(cx);
    {
        AutoCompartment ac(cx, iter.scopeChain());
        if (!iter.computeThis())
            return false;
        thisv = iter.thisv();
    }
    if (!Debugger::fromChildJSObject(thisobj)->wrapDebuggeeValue(cx, &thisv))
        return false;
    args.rval().set(thisv);
    return true;
}

static JSBool
DebuggerFrame_getOlder(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_FRAME(cx, argc, vp, "get this", args, thisobj, iter);
    Debugger *dbg = Debugger::fromChildJSObject(thisobj);

    for (++iter; !iter.done(); ++iter) {
        if (iter.isIon())
            continue;
        if (dbg->observesFrame(iter.abstractFramePtr()))
            return dbg->getScriptFrame(cx, iter, args.rval());
    }
    args.rval().setNull();
    return true;
}

Class DebuggerArguments_class = {
    "Arguments", JSCLASS_HAS_RESERVED_SLOTS(JSSLOT_DEBUGARGUMENTS_COUNT),
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub
};

/* The getter used for each element of frame.arguments. See DebuggerFrame_getArguments. */
static JSBool
DebuggerArguments_getArg(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    int32_t i = args.callee().toFunction()->getExtendedSlot(0).toInt32();

    /* Check that the this value is an Arguments object. */
    if (!args.thisv().isObject()) {
        ReportObjectRequired(cx);
        return false;
    }
    RootedObject argsobj(cx, &args.thisv().toObject());
    if (argsobj->getClass() != &DebuggerArguments_class) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_INCOMPATIBLE_PROTO,
                             "Arguments", "getArgument", argsobj->getClass()->name);
        return false;
    }

    /*
     * Put the Debugger.Frame into the this-value slot, then use THIS_FRAME
     * to check that it is still live and get the fp.
     */
    args.setThis(argsobj->getReservedSlot(JSSLOT_DEBUGARGUMENTS_FRAME));
    THIS_FRAME(cx, argc, vp, "get argument", ca2, thisobj, iter);

    /*
     * Since getters can be extracted and applied to other objects,
     * there is no guarantee this object has an ith argument.
     */
    JS_ASSERT(i >= 0);
    RootedValue arg(cx);
    RootedScript script(cx);
    if (unsigned(i) < iter.numActualArgs()) {
        script = iter.script();
        if (unsigned(i) < iter.numFormalArgs() && script->formalIsAliased(i)) {
            for (AliasedFormalIter fi(script); ; fi++) {
                if (fi.frameIndex() == unsigned(i)) {
                    arg = iter.callObj().aliasedVar(fi);
                    break;
                }
            }
        } else if (script->argsObjAliasesFormals() && iter.hasArgsObj()) {
            arg = iter.argsObj().arg(i);
        } else {
            arg = iter.unaliasedActual(i, DONT_CHECK_ALIASING);
        }
    } else {
        arg.setUndefined();
    }

    if (!Debugger::fromChildJSObject(thisobj)->wrapDebuggeeValue(cx, &arg))
        return false;
    args.rval().set(arg);
    return true;
}

static JSBool
DebuggerFrame_getArguments(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_FRAME(cx, argc, vp, "get arguments", args, thisobj, iter);
    Value argumentsv = thisobj->getReservedSlot(JSSLOT_DEBUGFRAME_ARGUMENTS);
    if (!argumentsv.isUndefined()) {
        JS_ASSERT(argumentsv.isObjectOrNull());
        args.rval().set(argumentsv);
        return true;
    }

    RootedObject argsobj(cx);
    if (iter.hasArgs()) {
        /* Create an arguments object. */
        Rooted<GlobalObject*> global(cx);
        global = &args.callee().global();
        JSObject *proto = global->getOrCreateArrayPrototype(cx);
        if (!proto)
            return false;
        argsobj = NewObjectWithGivenProto(cx, &DebuggerArguments_class, proto, global);
        if (!argsobj)
            return false;
        SetReservedSlot(argsobj, JSSLOT_DEBUGARGUMENTS_FRAME, ObjectValue(*thisobj));

        JS_ASSERT(iter.numActualArgs() <= 0x7fffffff);
        unsigned fargc = iter.numActualArgs();
        RootedValue fargcVal(cx, Int32Value(fargc));
        if (!DefineNativeProperty(cx, argsobj, cx->names().length,
                                  fargcVal, NULL, NULL,
                                  JSPROP_PERMANENT | JSPROP_READONLY, 0, 0))
        {
            return false;
        }

        Rooted<jsid> id(cx);
        RootedValue undefinedValue(cx, UndefinedValue());
        for (unsigned i = 0; i < fargc; i++) {
            RootedFunction getobj(cx);
            getobj = NewFunction(cx, NullPtr(), DebuggerArguments_getArg, 0,
                                 JSFunction::NATIVE_FUN, global, NullPtr(),
                                 JSFunction::ExtendedFinalizeKind);
            if (!getobj)
                return false;
            id = INT_TO_JSID(i);
            if (!getobj ||
                !DefineNativeProperty(cx, argsobj, id, undefinedValue,
                                      JS_DATA_TO_FUNC_PTR(PropertyOp, getobj.get()), NULL,
                                      JSPROP_ENUMERATE | JSPROP_SHARED | JSPROP_GETTER, 0, 0))
            {
                return false;
            }
            getobj->setExtendedSlot(0, Int32Value(i));
        }
    } else {
        argsobj = NULL;
    }
    args.rval().setObjectOrNull(argsobj);
    thisobj->setReservedSlot(JSSLOT_DEBUGFRAME_ARGUMENTS, args.rval());
    return true;
}

static JSBool
DebuggerFrame_getScript(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_FRAME(cx, argc, vp, "get script", args, thisobj, iter);
    Debugger *debug = Debugger::fromChildJSObject(thisobj);

    RootedObject scriptObject(cx);
    if (iter.isFunctionFrame() && !iter.isEvalFrame()) {
        RootedFunction callee(cx, iter.callee());
        if (callee->isInterpreted()) {
            RootedScript script(cx, callee->nonLazyScript());
            scriptObject = debug->wrapScript(cx, script);
            if (!scriptObject)
                return false;
        }
    } else {
        /*
         * We got eval, JS_Evaluate*, or JS_ExecuteScript non-function script
         * frames.
         */
        RootedScript script(cx, iter.script());
        scriptObject = debug->wrapScript(cx, script);
        if (!scriptObject)
            return false;
    }
    args.rval().setObjectOrNull(scriptObject);
    return true;
}

static JSBool
DebuggerFrame_getOffset(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_FRAME(cx, argc, vp, "get offset", args, thisobj, iter);
    RawScript script = iter.script();
    iter.updatePcQuadratic();
    jsbytecode *pc = iter.pc();
    JS_ASSERT(script->code <= pc);
    JS_ASSERT(pc < script->code + script->length);
    size_t offset = pc - script->code;
    args.rval().setNumber(double(offset));
    return true;
}

static JSBool
DebuggerFrame_getLive(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    JSObject *thisobj = CheckThisFrame(cx, args, "get live", false);
    if (!thisobj)
        return false;
    bool hasFrame = !!thisobj->getPrivate();
    args.rval().setBoolean(hasFrame);
    return true;
}

static bool
IsValidHook(const Value &v)
{
    return v.isUndefined() || (v.isObject() && v.toObject().isCallable());
}

static JSBool
DebuggerFrame_getOnStep(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_FRAME(cx, argc, vp, "get onStep", args, thisobj, iter);
    (void) iter;  // Silence GCC warning
    Value handler = thisobj->getReservedSlot(JSSLOT_DEBUGFRAME_ONSTEP_HANDLER);
    JS_ASSERT(IsValidHook(handler));
    args.rval().set(handler);
    return true;
}

static JSBool
DebuggerFrame_setOnStep(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.Frame.set onStep", 1);
    THIS_FRAME(cx, argc, vp, "set onStep", args, thisobj, iter);
    if (!IsValidHook(args[0])) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NOT_CALLABLE_OR_UNDEFINED);
        return false;
    }

    Value prior = thisobj->getReservedSlot(JSSLOT_DEBUGFRAME_ONSTEP_HANDLER);
    int delta = !args[0].isUndefined() - !prior.isUndefined();
    if (delta != 0) {
        /* Try to adjust this frame's script single-step mode count. */
        AutoCompartment ac(cx, iter.scopeChain());
        if (!iter.script()->changeStepModeCount(cx, delta))
            return false;
    }

    /* Now that the step mode switch has succeeded, we can install the handler. */
    thisobj->setReservedSlot(JSSLOT_DEBUGFRAME_ONSTEP_HANDLER, args[0]);
    args.rval().setUndefined();
    return true;
}

static JSBool
DebuggerFrame_getOnPop(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_FRAME(cx, argc, vp, "get onPop", args, thisobj, iter);
    (void) iter;  // Silence GCC warning
    Value handler = thisobj->getReservedSlot(JSSLOT_DEBUGFRAME_ONPOP_HANDLER);
    JS_ASSERT(IsValidHook(handler));
    args.rval().set(handler);
    return true;
}

static JSBool
DebuggerFrame_setOnPop(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.Frame.set onPop", 1);
    THIS_FRAME(cx, argc, vp, "set onPop", args, thisobj, iter);
    (void) iter;  // Silence GCC warning
    if (!IsValidHook(args[0])) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NOT_CALLABLE_OR_UNDEFINED);
        return false;
    }

    thisobj->setReservedSlot(JSSLOT_DEBUGFRAME_ONPOP_HANDLER, args[0]);
    args.rval().setUndefined();
    return true;
}

/*
 * Evaluate |chars[0..length-1]| in the environment |env|, treating that
 * source as appearing starting at |lineno| in |filename|. Store the return
 * value in |*rval|. Use |thisv| as the 'this' value.
 *
 * If |frame| is non-NULL, evaluate as for a direct eval in that frame; |env|
 * must be either |frame|'s DebugScopeObject, or some extension of that
 * environment; either way, |frame|'s scope is where newly declared variables
 * go. In this case, |frame| must have a computed 'this' value, equal to |thisv|.
 */
JSBool
js::EvaluateInEnv(JSContext *cx, Handle<Env*> env, HandleValue thisv, AbstractFramePtr frame,
                  StableCharPtr chars, unsigned length, const char *filename, unsigned lineno,
                  MutableHandleValue rval)
{
    assertSameCompartment(cx, env, frame);
    JS_ASSERT_IF(frame, thisv.get() == frame.thisValue());

    JS_ASSERT(!IsPoisonedPtr(chars.get()));

    /*
     * NB: This function breaks the assumption that the compiler can see all
     * calls and properly compute a static level. In practice, any non-zero
     * static level will suffice.
     */
    CompileOptions options(cx);
    options.setPrincipals(env->compartment()->principals)
           .setCompileAndGo(true)
           .setNoScriptRval(false)
           .setFileAndLine(filename, lineno);
    RootedScript callerScript(cx, frame ? frame.script() : NULL);
    RootedScript script(cx, frontend::CompileScript(cx, env, callerScript,
                                                    options, chars.get(), length,
                                                    /* source = */ NULL,
                                                    /* staticLevel = */ frame ? 1 : 0));
    if (!script)
        return false;

    script->isActiveEval = true;
    ExecuteType type = !frame && env->isGlobal() ? EXECUTE_DEBUG_GLOBAL : EXECUTE_DEBUG;
    return ExecuteKernel(cx, script, *env, thisv, type, frame, rval.address());
}

static JSBool
DebuggerGenericEval(JSContext *cx, const char *fullMethodName,
                    const Value &code, Value *bindings, MutableHandleValue vp,
                    Debugger *dbg, HandleObject scope, ScriptFrameIter *iter)
{
    /* Either we're specifying the frame, or a global. */
    JS_ASSERT_IF(iter, !scope);
    JS_ASSERT_IF(!iter, scope && scope->isGlobal());

    /* Check the first argument, the eval code string. */
    if (!code.isString()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NOT_EXPECTED_TYPE,
                             fullMethodName, "string", InformalValueTypeName(code));
        return false;
    }
    Rooted<JSStableString *> stable(cx, code.toString()->ensureStable(cx));
    if (!stable)
        return false;

    /*
     * Gather keys and values of bindings, if any. This must be done in the
     * debugger compartment, since that is where any exceptions must be
     * thrown.
     */
    AutoIdVector keys(cx);
    AutoValueVector values(cx);
    if (bindings) {
        RootedObject bindingsobj(cx, NonNullObject(cx, *bindings));
        if (!bindingsobj ||
            !GetPropertyNames(cx, bindingsobj, JSITER_OWNONLY, &keys) ||
            !values.growBy(keys.length()))
        {
            return false;
        }
        for (size_t i = 0; i < keys.length(); i++) {
            MutableHandleValue valp = values.handleAt(i);
            if (!JSObject::getGeneric(cx, bindingsobj, bindingsobj, keys.handleAt(i), valp) ||
                !dbg->unwrapDebuggeeValue(cx, valp))
            {
                return false;
            }
        }
    }

    Maybe<AutoCompartment> ac;
    if (iter)
        ac.construct(cx, iter->scopeChain());
    else
        ac.construct(cx, scope);

    RootedValue thisv(cx);
    Rooted<Env *> env(cx);
    if (iter) {
        /* ExecuteInEnv requires 'fp' to have a computed 'this" value. */
        if (!iter->computeThis())
            return false;
        thisv = iter->thisv();
        env = GetDebugScopeForFrame(cx, iter->abstractFramePtr());
        if (!env)
            return false;
    } else {
        thisv = ObjectValue(*scope);
        env = scope;
    }

    /* If evalWithBindings, create the inner environment. */
    if (bindings) {
        /* TODO - This should probably be a Call object, like ES5 strict eval. */
        env = NewObjectWithGivenProto(cx, &ObjectClass, NULL, env);
        if (!env)
            return false;
        RootedId id(cx);
        for (size_t i = 0; i < keys.length(); i++) {
            id = keys[i];
            MutableHandleValue val = values.handleAt(i);
            if (!cx->compartment->wrap(cx, val) ||
                !DefineNativeProperty(cx, env, id, val, NULL, NULL, 0, 0, 0))
            {
                return false;
            }
        }
    }

    /* Run the code and produce the completion value. */
    RootedValue rval(cx);
    JS::Anchor<JSString *> anchor(stable);
    AbstractFramePtr frame = iter ? iter->abstractFramePtr() : NullFramePtr();
    bool ok = EvaluateInEnv(cx, env, thisv, frame, stable->chars(), stable->length(),
                            "debugger eval code", 1, &rval);
    return dbg->receiveCompletionValue(ac, ok, rval, vp);
}

static JSBool
DebuggerFrame_eval(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_FRAME(cx, argc, vp, "eval", args, thisobj, iter);
    REQUIRE_ARGC("Debugger.Frame.prototype.eval", 1);
    Debugger *dbg = Debugger::fromChildJSObject(thisobj);
    return DebuggerGenericEval(cx, "Debugger.Frame.prototype.eval",
                               args[0], NULL, args.rval(), dbg, NullPtr(), &iter);
}

static JSBool
DebuggerFrame_evalWithBindings(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_FRAME(cx, argc, vp, "evalWithBindings", args, thisobj, iter);
    REQUIRE_ARGC("Debugger.Frame.prototype.evalWithBindings", 2);
    Debugger *dbg = Debugger::fromChildJSObject(thisobj);
    return DebuggerGenericEval(cx, "Debugger.Frame.prototype.evalWithBindings",
                               args[0], &args[1], args.rval(), dbg, NullPtr(), &iter);
}

static JSBool
DebuggerFrame_construct(JSContext *cx, unsigned argc, Value *vp)
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NO_CONSTRUCTOR, "Debugger.Frame");
    return false;
}

static JSPropertySpec DebuggerFrame_properties[] = {
    JS_PSG("arguments", DebuggerFrame_getArguments, 0),
    JS_PSG("callee", DebuggerFrame_getCallee, 0),
    JS_PSG("constructing", DebuggerFrame_getConstructing, 0),
    JS_PSG("environment", DebuggerFrame_getEnvironment, 0),
    JS_PSG("generator", DebuggerFrame_getGenerator, 0),
    JS_PSG("live", DebuggerFrame_getLive, 0),
    JS_PSG("offset", DebuggerFrame_getOffset, 0),
    JS_PSG("older", DebuggerFrame_getOlder, 0),
    JS_PSG("script", DebuggerFrame_getScript, 0),
    JS_PSG("this", DebuggerFrame_getThis, 0),
    JS_PSG("type", DebuggerFrame_getType, 0),
    JS_PSGS("onStep", DebuggerFrame_getOnStep, DebuggerFrame_setOnStep, 0),
    JS_PSGS("onPop", DebuggerFrame_getOnPop, DebuggerFrame_setOnPop, 0),
    JS_PS_END
};

static JSFunctionSpec DebuggerFrame_methods[] = {
    JS_FN("eval", DebuggerFrame_eval, 1, 0),
    JS_FN("evalWithBindings", DebuggerFrame_evalWithBindings, 1, 0),
    JS_FS_END
};


/*** Debugger.Object *****************************************************************************/

static void
DebuggerObject_trace(JSTracer *trc, RawObject obj)
{
    /*
     * There is a barrier on private pointers, so the Unbarriered marking
     * is okay.
     */
    if (JSObject *referent = (JSObject *) obj->getPrivate()) {
        MarkCrossCompartmentObjectUnbarriered(trc, obj, &referent, "Debugger.Object referent");
        obj->setPrivateUnbarriered(referent);
    }
}

Class DebuggerObject_class = {
    "Object",
    JSCLASS_HAS_PRIVATE | JSCLASS_IMPLEMENTS_BARRIERS |
    JSCLASS_HAS_RESERVED_SLOTS(JSSLOT_DEBUGOBJECT_COUNT),
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, NULL,
    NULL,                 /* checkAccess */
    NULL,                 /* call        */
    NULL,                 /* construct   */
    NULL,                 /* hasInstance */
    DebuggerObject_trace
};

static JSObject *
DebuggerObject_checkThis(JSContext *cx, const CallArgs &args, const char *fnname)
{
    if (!args.thisv().isObject()) {
        ReportObjectRequired(cx);
        return NULL;
    }
    JSObject *thisobj = &args.thisv().toObject();
    if (thisobj->getClass() != &DebuggerObject_class) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_INCOMPATIBLE_PROTO,
                             "Debugger.Object", fnname, thisobj->getClass()->name);
        return NULL;
    }

    /*
     * Forbid Debugger.Object.prototype, which is of class DebuggerObject_class
     * but isn't a real working Debugger.Object. The prototype object is
     * distinguished by having no referent.
     */
    if (!thisobj->getPrivate()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_INCOMPATIBLE_PROTO,
                             "Debugger.Object", fnname, "prototype object");
        return NULL;
    }
    return thisobj;
}

#define THIS_DEBUGOBJECT_REFERENT(cx, argc, vp, fnname, args, obj)            \
    CallArgs args = CallArgsFromVp(argc, vp);                                 \
    RootedObject obj(cx, DebuggerObject_checkThis(cx, args, fnname));         \
    if (!obj)                                                                 \
        return false;                                                         \
    obj = (JSObject *) obj->getPrivate();                                     \
    JS_ASSERT(obj)

#define THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, fnname, args, dbg, obj) \
    CallArgs args = CallArgsFromVp(argc, vp);                                 \
    RootedObject obj(cx, DebuggerObject_checkThis(cx, args, fnname));         \
    if (!obj)                                                                 \
        return false;                                                         \
    Debugger *dbg = Debugger::fromChildJSObject(obj);                         \
    obj = (JSObject *) obj->getPrivate();                                     \
    JS_ASSERT(obj)

static JSBool
DebuggerObject_construct(JSContext *cx, unsigned argc, Value *vp)
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NO_CONSTRUCTOR, "Debugger.Object");
    return false;
}

static JSBool
DebuggerObject_getProto(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "get proto", args, dbg, refobj);
    RootedObject proto(cx);
    {
        AutoCompartment ac(cx, refobj);
        if (!JSObject::getProto(cx, refobj, &proto))
            return false;
    }
    RootedValue protov(cx, ObjectOrNullValue(proto));
    if (!dbg->wrapDebuggeeValue(cx, &protov))
        return false;
    args.rval().set(protov);
    return true;
}

static JSBool
DebuggerObject_getClass(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_REFERENT(cx, argc, vp, "get class", args, refobj);
    const char *s = refobj->getClass()->name;
    JSAtom *str = Atomize(cx, s, strlen(s));
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

static JSBool
DebuggerObject_getCallable(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_REFERENT(cx, argc, vp, "get callable", args, refobj);
    args.rval().setBoolean(refobj->isCallable());
    return true;
}

static JSBool
DebuggerObject_getName(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "get name", args, dbg, obj);
    if (!obj->isFunction()) {
        args.rval().setUndefined();
        return true;
    }

    JSString *name = obj->toFunction()->atom();
    if (!name) {
        args.rval().setUndefined();
        return true;
    }

    RootedValue namev(cx, StringValue(name));
    if (!dbg->wrapDebuggeeValue(cx, &namev))
        return false;
    args.rval().set(namev);
    return true;
}

static JSBool
DebuggerObject_getDisplayName(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "get display name", args, dbg, obj);
    if (!obj->isFunction()) {
        args.rval().setUndefined();
        return true;
    }

    JSString *name = obj->toFunction()->displayAtom();
    if (!name) {
        args.rval().setUndefined();
        return true;
    }

    RootedValue namev(cx, StringValue(name));
    if (!dbg->wrapDebuggeeValue(cx, &namev))
        return false;
    args.rval().set(namev);
    return true;
}

static JSBool
DebuggerObject_getParameterNames(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_REFERENT(cx, argc, vp, "get parameterNames", args, obj);
    if (!obj->isFunction()) {
        args.rval().setUndefined();
        return true;
    }

    RootedFunction fun(cx, obj->toFunction());
    JSObject *result = NewDenseAllocatedArray(cx, fun->nargs);
    if (!result)
        return false;
    result->ensureDenseInitializedLength(cx, 0, fun->nargs);

    if (fun->isInterpreted()) {
        JS_ASSERT(fun->nargs == fun->nonLazyScript()->bindings.numArgs());

        if (fun->nargs > 0) {
            BindingVector bindings(cx);
            RootedScript script(cx, fun->nonLazyScript());
            if (!FillBindingVector(script, &bindings))
                return false;
            for (size_t i = 0; i < fun->nargs; i++) {
                Value v;
                if (bindings[i].name()->length() == 0)
                    v = UndefinedValue();
                else
                    v = StringValue(bindings[i].name());
                result->setDenseElement(i, v);
            }
        }
    } else {
        for (size_t i = 0; i < fun->nargs; i++)
            result->setDenseElement(i, UndefinedValue());
    }

    args.rval().setObject(*result);
    return true;
}

static JSBool
DebuggerObject_getScript(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "get script", args, dbg, obj);

    if (!obj->isFunction()) {
        args.rval().setUndefined();
        return true;
    }

    RootedFunction fun(cx, obj->toFunction());
    if (fun->isBuiltin()) {
        args.rval().setUndefined();
        return true;
    }

    RootedScript script(cx, fun->nonLazyScript());
    RootedObject scriptObject(cx, dbg->wrapScript(cx, script));
    if (!scriptObject)
        return false;

    args.rval().setObject(*scriptObject);
    return true;
}

static JSBool
DebuggerObject_getEnvironment(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "get environment", args, dbg, obj);

    /* Don't bother switching compartments just to check obj's type and get its env. */
    if (!obj->isFunction() || !obj->toFunction()->isInterpreted()) {
        args.rval().setUndefined();
        return true;
    }

    Rooted<Env*> env(cx);
    {
        AutoCompartment ac(cx, obj);
        RootedFunction fun(cx, obj->toFunction());
        env = GetDebugScopeForFunction(cx, fun);
        if (!env)
            return false;
    }

    return dbg->wrapEnvironment(cx, env, args.rval());
}

static JSBool
DebuggerObject_getGlobal(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "get global", args, dbg, obj);

    RootedValue v(cx, ObjectValue(obj->global()));
    if (!dbg->wrapDebuggeeValue(cx, &v))
        return false;
    args.rval().set(v);
    return true;
}

static JSBool
DebuggerObject_getOwnPropertyDescriptor(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "getOwnPropertyDescriptor", args, dbg, obj);

    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, argc >= 1 ? args[0] : UndefinedValue(), &id))
        return false;

    /* Bug: This can cause the debuggee to run! */
    AutoPropertyDescriptorRooter desc(cx);
    {
        Maybe<AutoCompartment> ac;
        ac.construct(cx, obj);
        if (!cx->compartment->wrapId(cx, id.address()))
            return false;

        ErrorCopier ec(ac, dbg->toJSObject());
        if (!GetOwnPropertyDescriptor(cx, obj, id, &desc))
            return false;
    }

    if (desc.obj) {
        /* Rewrap the debuggee values in desc for the debugger. */
        RootedValue value(cx, desc.value);
        if (!dbg->wrapDebuggeeValue(cx, &value))
            return false;
        desc.value = value;

        if (desc.attrs & JSPROP_GETTER) {
            RootedValue get(cx, ObjectOrNullValue(CastAsObject(desc.getter)));
            if (!dbg->wrapDebuggeeValue(cx, &get))
                return false;
            desc.getter = CastAsPropertyOp(get.toObjectOrNull());
        }
        if (desc.attrs & JSPROP_SETTER) {
            RootedValue set(cx, ObjectOrNullValue(CastAsObject(desc.setter)));
            if (!dbg->wrapDebuggeeValue(cx, &set))
                return false;
            desc.setter = CastAsStrictPropertyOp(set.toObjectOrNull());
        }
    }

    return NewPropertyDescriptorObject(cx, &desc, args.rval());
}

static JSBool
DebuggerObject_getOwnPropertyNames(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "getOwnPropertyNames", args, dbg, obj);

    AutoIdVector keys(cx);
    {
        Maybe<AutoCompartment> ac;
        ac.construct(cx, obj);
        ErrorCopier ec(ac, dbg->toJSObject());
        if (!GetPropertyNames(cx, obj, JSITER_OWNONLY | JSITER_HIDDEN, &keys))
            return false;
    }

    AutoValueVector vals(cx);
    if (!vals.resize(keys.length()))
        return false;

    for (size_t i = 0, len = keys.length(); i < len; i++) {
         jsid id = keys[i];
         if (JSID_IS_INT(id)) {
             JSString *str = Int32ToString<CanGC>(cx, JSID_TO_INT(id));
             if (!str)
                 return false;
             vals[i].setString(str);
         } else if (JSID_IS_ATOM(id)) {
             vals[i].setString(JSID_TO_STRING(id));
             if (!cx->compartment->wrap(cx, vals.handleAt(i)))
                 return false;
         } else {
             vals[i].setObject(*JSID_TO_OBJECT(id));
             if (!dbg->wrapDebuggeeValue(cx, vals.handleAt(i)))
                 return false;
         }
    }

    JSObject *aobj = NewDenseCopiedArray(cx, vals.length(), vals.begin());
    if (!aobj)
        return false;
    args.rval().setObject(*aobj);
    return true;
}

static JSBool
DebuggerObject_defineProperty(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "defineProperty", args, dbg, obj);
    REQUIRE_ARGC("Debugger.Object.defineProperty", 2);

    RootedId id(cx);
    if (!ValueToId<CanGC>(cx, args[0], &id))
        return false;

    const Value &descval = args[1];
    AutoPropDescArrayRooter descs(cx);
    if (!descs.reserve(3)) // desc, unwrappedDesc, rewrappedDesc
        return false;
    PropDesc *desc = descs.append();
    if (!desc || !desc->initialize(cx, descval, false))
        return false;
    desc->clearPd();

    PropDesc *unwrappedDesc = descs.append();
    if (!unwrappedDesc || !desc->unwrapDebuggerObjectsInto(cx, dbg, obj, unwrappedDesc))
        return false;
    if (!unwrappedDesc->checkGetter(cx) || !unwrappedDesc->checkSetter(cx))
        return false;

    {
        PropDesc *rewrappedDesc = descs.append();
        if (!rewrappedDesc)
            return false;
        RootedId wrappedId(cx);

        Maybe<AutoCompartment> ac;
        ac.construct(cx, obj);
        if (!unwrappedDesc->wrapInto(cx, obj, id, wrappedId.address(), rewrappedDesc))
            return false;

        ErrorCopier ec(ac, dbg->toJSObject());
        bool dummy;
        if (!DefineProperty(cx, obj, wrappedId, *rewrappedDesc, true, &dummy))
            return false;
    }

    args.rval().setUndefined();
    return true;
}

static JSBool
DebuggerObject_defineProperties(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "defineProperties", args, dbg, obj);
    REQUIRE_ARGC("Debugger.Object.defineProperties", 1);

    RootedValue arg(cx, args[0]);
    RootedObject props(cx, ToObject(cx, arg));
    if (!props)
        return false;

    AutoIdVector ids(cx);
    AutoPropDescArrayRooter descs(cx);
    if (!ReadPropertyDescriptors(cx, props, false, &ids, &descs))
        return false;
    size_t n = ids.length();

    AutoPropDescArrayRooter unwrappedDescs(cx);
    for (size_t i = 0; i < n; i++) {
        if (!unwrappedDescs.append())
            return false;
        if (!descs[i].unwrapDebuggerObjectsInto(cx, dbg, obj, &unwrappedDescs[i]))
            return false;
        if (!unwrappedDescs[i].checkGetter(cx) || !unwrappedDescs[i].checkSetter(cx))
            return false;
    }

    {
        AutoIdVector rewrappedIds(cx);
        AutoPropDescArrayRooter rewrappedDescs(cx);

        Maybe<AutoCompartment> ac;
        ac.construct(cx, obj);
        RootedId id(cx);
        for (size_t i = 0; i < n; i++) {
            if (!rewrappedIds.append(jsid()) || !rewrappedDescs.append())
                return false;
            id = ids[i];
            if (!unwrappedDescs[i].wrapInto(cx, obj, id, &rewrappedIds[i], &rewrappedDescs[i]))
                return false;
        }

        ErrorCopier ec(ac, dbg->toJSObject());
        for (size_t i = 0; i < n; i++) {
            bool dummy;
            if (!DefineProperty(cx, obj, rewrappedIds.handleAt(i),
                                rewrappedDescs[i], true, &dummy))
            {
                return false;
            }
        }
    }

    args.rval().setUndefined();
    return true;
}

/*
 * This does a non-strict delete, as a matter of API design. The case where the
 * property is non-configurable isn't necessarily exceptional here.
 */
static JSBool
DebuggerObject_deleteProperty(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "deleteProperty", args, dbg, obj);
    RootedValue nameArg(cx, args.get(0));

    Maybe<AutoCompartment> ac;
    ac.construct(cx, obj);
    if (!cx->compartment->wrap(cx, &nameArg))
        return false;

    ErrorCopier ec(ac, dbg->toJSObject());
    return JSObject::deleteByValue(cx, obj, nameArg, args.rval(), false);
}

enum SealHelperOp { Seal, Freeze, PreventExtensions };

static JSBool
DebuggerObject_sealHelper(JSContext *cx, unsigned argc, Value *vp, SealHelperOp op, const char *name)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, name, args, dbg, obj);

    Maybe<AutoCompartment> ac;
    ac.construct(cx, obj);
    ErrorCopier ec(ac, dbg->toJSObject());
    bool ok;
    if (op == Seal) {
        ok = JSObject::seal(cx, obj);
    } else if (op == Freeze) {
        ok = JSObject::freeze(cx, obj);
    } else {
        JS_ASSERT(op == PreventExtensions);
        if (!obj->isExtensible()) {
            args.rval().setUndefined();
            return true;
        }
        ok = JSObject::preventExtensions(cx, obj);
    }
    if (!ok)
        return false;
    args.rval().setUndefined();
    return true;
}

static JSBool
DebuggerObject_seal(JSContext *cx, unsigned argc, Value *vp)
{
    return DebuggerObject_sealHelper(cx, argc, vp, Seal, "seal");
}

static JSBool
DebuggerObject_freeze(JSContext *cx, unsigned argc, Value *vp)
{
    return DebuggerObject_sealHelper(cx, argc, vp, Freeze, "freeze");
}

static JSBool
DebuggerObject_preventExtensions(JSContext *cx, unsigned argc, Value *vp)
{
    return DebuggerObject_sealHelper(cx, argc, vp, PreventExtensions, "preventExtensions");
}

static JSBool
DebuggerObject_isSealedHelper(JSContext *cx, unsigned argc, Value *vp, SealHelperOp op,
                              const char *name)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, name, args, dbg, obj);

    Maybe<AutoCompartment> ac;
    ac.construct(cx, obj);
    ErrorCopier ec(ac, dbg->toJSObject());
    bool r;
    if (op == Seal) {
        if (!JSObject::isSealed(cx, obj, &r))
            return false;
    } else if (op == Freeze) {
        if (!JSObject::isFrozen(cx, obj, &r))
            return false;
    } else {
        r = obj->isExtensible();
    }
    args.rval().setBoolean(r);
    return true;
}

static JSBool
DebuggerObject_isSealed(JSContext *cx, unsigned argc, Value *vp)
{
    return DebuggerObject_isSealedHelper(cx, argc, vp, Seal, "isSealed");
}

static JSBool
DebuggerObject_isFrozen(JSContext *cx, unsigned argc, Value *vp)
{
    return DebuggerObject_isSealedHelper(cx, argc, vp, Freeze, "isFrozen");
}

static JSBool
DebuggerObject_isExtensible(JSContext *cx, unsigned argc, Value *vp)
{
    return DebuggerObject_isSealedHelper(cx, argc, vp, PreventExtensions, "isExtensible");
}

enum ApplyOrCallMode { ApplyMode, CallMode };

static JSBool
ApplyOrCall(JSContext *cx, unsigned argc, Value *vp, ApplyOrCallMode mode)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "apply", args, dbg, obj);

    /*
     * Any JS exceptions thrown must be in the debugger compartment, so do
     * sanity checks and fallible conversions before entering the debuggee.
     */
    RootedValue calleev(cx, ObjectValue(*obj));
    if (!obj->isCallable()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_INCOMPATIBLE_PROTO,
                             "Debugger.Object", "apply", obj->getClass()->name);
        return false;
    }

    /*
     * Unwrap Debugger.Objects. This happens in the debugger's compartment since
     * that is where any exceptions must be reported.
     */
    RootedValue thisv(cx, argc > 0 ? args[0] : UndefinedValue());
    if (!dbg->unwrapDebuggeeValue(cx, &thisv))
        return false;
    unsigned callArgc = 0;
    Value *callArgv = NULL;
    AutoValueVector argv(cx);
    if (mode == ApplyMode) {
        if (argc >= 2 && !args[1].isNullOrUndefined()) {
            if (!args[1].isObject()) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_APPLY_ARGS,
                                     js_apply_str);
                return false;
            }
            RootedObject argsobj(cx, &args[1].toObject());
            if (!GetLengthProperty(cx, argsobj, &callArgc))
                return false;
            callArgc = unsigned(Min(callArgc, StackSpace::ARGS_LENGTH_MAX));
            if (!argv.growBy(callArgc) || !GetElements(cx, argsobj, callArgc, argv.begin()))
                return false;
            callArgv = argv.begin();
        }
    } else {
        callArgc = argc > 0 ? unsigned(Min(argc - 1, StackSpace::ARGS_LENGTH_MAX)) : 0;
        callArgv = args.array() + 1;
    }

    AutoArrayRooter callArgvRooter(cx, callArgc, callArgv);
    for (unsigned i = 0; i < callArgc; i++) {
        if (!dbg->unwrapDebuggeeValue(cx, callArgvRooter.handleAt(i)))
            return false;
    }

    /*
     * Enter the debuggee compartment and rewrap all input value for that compartment.
     * (Rewrapping always takes place in the destination compartment.)
     */
    Maybe<AutoCompartment> ac;
    ac.construct(cx, obj);
    if (!cx->compartment->wrap(cx, &calleev) || !cx->compartment->wrap(cx, &thisv))
        return false;

    RootedValue arg(cx);
    for (unsigned i = 0; i < callArgc; i++) {
        arg = callArgv[i];
        if (!cx->compartment->wrap(cx, &arg))
             return false;
        callArgv[i] = arg;
    }

    /*
     * Call the function. Use receiveCompletionValue to return to the debugger
     * compartment and populate args.rval().
     */
    RootedValue rval(cx);
    bool ok = Invoke(cx, thisv, calleev, callArgc, callArgv, rval.address());
    return dbg->receiveCompletionValue(ac, ok, rval, args.rval());
}

static JSBool
DebuggerObject_apply(JSContext *cx, unsigned argc, Value *vp)
{
    return ApplyOrCall(cx, argc, vp, ApplyMode);
}

static JSBool
DebuggerObject_call(JSContext *cx, unsigned argc, Value *vp)
{
    return ApplyOrCall(cx, argc, vp, CallMode);
}

static JSBool
DebuggerObject_makeDebuggeeValue(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.Object.prototype.makeDebuggeeValue", 1);
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "makeDebuggeeValue", args, dbg, referent);

    RootedValue arg0(cx, args[0]);

    /* Non-objects are already debuggee values. */
    if (arg0.isObject()) {
        // Enter this Debugger.Object's referent's compartment, and wrap the
        // argument as appropriate for references from there.
        {
            AutoCompartment ac(cx, referent);
            if (!cx->compartment->wrap(cx, &arg0))
                return false;
        }

        // Back in the debugger's compartment, produce a new Debugger.Object
        // instance referring to the wrapped argument.
        if (!dbg->wrapDebuggeeValue(cx, &arg0))
            return false;
    }

    args.rval().set(arg0);
    return true;
}

static bool
RequireGlobalObject(JSContext *cx, HandleValue dbgobj, HandleObject obj)
{
    if (!obj->isGlobal()) {
        /* Help the poor programmer by pointing out wrappers around globals. */
        if (obj->isWrapper()) {
            JSObject *unwrapped = js::UnwrapObject(obj);
            if (unwrapped->isGlobal()) {
                js_ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_DEBUG_WRAPPER_IN_WAY,
                                         JSDVG_SEARCH_STACK, dbgobj, NullPtr(),
                                         "a global object", NULL);
                return false;
            }
        }

        js_ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_DEBUG_BAD_REFERENT,
                                 JSDVG_SEARCH_STACK, dbgobj, NullPtr(),
                                 "a global object", NULL);
        return false;
    }

    return true;
}

static JSBool
DebuggerObject_evalInGlobal(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.Object.prototype.evalInGlobal", 1);
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "evalInGlobal", args, dbg, referent);
    if (!RequireGlobalObject(cx, args.thisv(), referent))
        return false;

    return DebuggerGenericEval(cx, "Debugger.Object.prototype.evalInGlobal",
                               args[0], NULL, args.rval(), dbg, referent, NULL);
}

static JSBool
DebuggerObject_evalInGlobalWithBindings(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.Object.prototype.evalInGlobalWithBindings", 2);
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "evalInGlobalWithBindings", args, dbg, referent);
    if (!RequireGlobalObject(cx, args.thisv(), referent))
        return false;

    return DebuggerGenericEval(cx, "Debugger.Object.prototype.evalInGlobalWithBindings",
                               args[0], &args[1], args.rval(), dbg, referent, NULL);
}

static JSBool
DebuggerObject_unwrap(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGOBJECT_OWNER_REFERENT(cx, argc, vp, "unwrap", args, dbg, referent);
    JSObject *unwrapped = UnwrapOneChecked(referent);
    if (!unwrapped) {
        vp->setNull();
        return true;
    }

    args.rval().setObject(*unwrapped);
    if (!dbg->wrapDebuggeeValue(cx, args.rval()))
        return false;
    return true;
}

static JSPropertySpec DebuggerObject_properties[] = {
    JS_PSG("proto", DebuggerObject_getProto, 0),
    JS_PSG("class", DebuggerObject_getClass, 0),
    JS_PSG("callable", DebuggerObject_getCallable, 0),
    JS_PSG("name", DebuggerObject_getName, 0),
    JS_PSG("displayName", DebuggerObject_getDisplayName, 0),
    JS_PSG("parameterNames", DebuggerObject_getParameterNames, 0),
    JS_PSG("script", DebuggerObject_getScript, 0),
    JS_PSG("environment", DebuggerObject_getEnvironment, 0),
    JS_PSG("global", DebuggerObject_getGlobal, 0),
    JS_PS_END
};

static JSFunctionSpec DebuggerObject_methods[] = {
    JS_FN("getOwnPropertyDescriptor", DebuggerObject_getOwnPropertyDescriptor, 1, 0),
    JS_FN("getOwnPropertyNames", DebuggerObject_getOwnPropertyNames, 0, 0),
    JS_FN("defineProperty", DebuggerObject_defineProperty, 2, 0),
    JS_FN("defineProperties", DebuggerObject_defineProperties, 1, 0),
    JS_FN("deleteProperty", DebuggerObject_deleteProperty, 1, 0),
    JS_FN("seal", DebuggerObject_seal, 0, 0),
    JS_FN("freeze", DebuggerObject_freeze, 0, 0),
    JS_FN("preventExtensions", DebuggerObject_preventExtensions, 0, 0),
    JS_FN("isSealed", DebuggerObject_isSealed, 0, 0),
    JS_FN("isFrozen", DebuggerObject_isFrozen, 0, 0),
    JS_FN("isExtensible", DebuggerObject_isExtensible, 0, 0),
    JS_FN("apply", DebuggerObject_apply, 0, 0),
    JS_FN("call", DebuggerObject_call, 0, 0),
    JS_FN("makeDebuggeeValue", DebuggerObject_makeDebuggeeValue, 1, 0),
    JS_FN("evalInGlobal", DebuggerObject_evalInGlobal, 1, 0),
    JS_FN("evalInGlobalWithBindings", DebuggerObject_evalInGlobalWithBindings, 2, 0),
    JS_FN("unwrap", DebuggerObject_unwrap, 0, 0),
    JS_FS_END
};


/*** Debugger.Environment ************************************************************************/

static void
DebuggerEnv_trace(JSTracer *trc, RawObject obj)
{
    /*
     * There is a barrier on private pointers, so the Unbarriered marking
     * is okay.
     */
    if (Env *referent = (JSObject *) obj->getPrivate()) {
        MarkCrossCompartmentObjectUnbarriered(trc, obj, &referent, "Debugger.Environment referent");
        obj->setPrivateUnbarriered(referent);
    }
}

Class DebuggerEnv_class = {
    "Environment",
    JSCLASS_HAS_PRIVATE | JSCLASS_IMPLEMENTS_BARRIERS |
    JSCLASS_HAS_RESERVED_SLOTS(JSSLOT_DEBUGENV_COUNT),
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, NULL,
    NULL,                 /* checkAccess */
    NULL,                 /* call        */
    NULL,                 /* construct   */
    NULL,                 /* hasInstance */
    DebuggerEnv_trace
};

static JSObject *
DebuggerEnv_checkThis(JSContext *cx, const CallArgs &args, const char *fnname)
{
    if (!args.thisv().isObject()) {
        ReportObjectRequired(cx);
        return NULL;
    }
    JSObject *thisobj = &args.thisv().toObject();
    if (thisobj->getClass() != &DebuggerEnv_class) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_INCOMPATIBLE_PROTO,
                             "Debugger.Environment", fnname, thisobj->getClass()->name);
        return NULL;
    }

    /*
     * Forbid Debugger.Environment.prototype, which is of class DebuggerEnv_class
     * but isn't a real working Debugger.Environment. The prototype object is
     * distinguished by having no referent.
     */
    if (!thisobj->getPrivate()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_INCOMPATIBLE_PROTO,
                             "Debugger.Environment", fnname, "prototype object");
        return NULL;
    }
    return thisobj;
}

#define THIS_DEBUGENV(cx, argc, vp, fnname, args, envobj, env)                \
    CallArgs args = CallArgsFromVp(argc, vp);                                 \
    JSObject *envobj = DebuggerEnv_checkThis(cx, args, fnname);               \
    if (!envobj)                                                              \
        return false;                                                         \
    Rooted<Env*> env(cx, static_cast<Env *>(envobj->getPrivate()));           \
    JS_ASSERT(env);                                                           \
    JS_ASSERT(!env->isScope())

#define THIS_DEBUGENV_OWNER(cx, argc, vp, fnname, args, envobj, env, dbg)     \
    THIS_DEBUGENV(cx, argc, vp, fnname, args, envobj, env);                   \
    Debugger *dbg = Debugger::fromChildJSObject(envobj)

static JSBool
DebuggerEnv_construct(JSContext *cx, unsigned argc, Value *vp)
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NO_CONSTRUCTOR, "Debugger.Environment");
    return false;
}

static bool
IsDeclarative(Env *env)
{
    return env->isDebugScope() && env->asDebugScope().isForDeclarative();
}

static bool
IsWith(Env *env)
{
    return env->isDebugScope() && env->asDebugScope().scope().isWith();
}

static JSBool
DebuggerEnv_getType(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGENV(cx, argc, vp, "get type", args, envobj, env);

    /* Don't bother switching compartments just to check env's class. */
    const char *s;
    if (IsDeclarative(env))
        s = "declarative";
    else if (IsWith(env))
        s = "with";
    else
        s = "object";

    JSAtom *str = Atomize(cx, s, strlen(s), InternAtom);
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

static JSBool
DebuggerEnv_getParent(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGENV_OWNER(cx, argc, vp, "get parent", args, envobj, env, dbg);

    /* Don't bother switching compartments just to get env's parent. */
    Rooted<Env*> parent(cx, env->enclosingScope());
    return dbg->wrapEnvironment(cx, parent, args.rval());
}

static JSBool
DebuggerEnv_getObject(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGENV_OWNER(cx, argc, vp, "get type", args, envobj, env, dbg);

    /*
     * Don't bother switching compartments just to check env's class and
     * possibly get its proto.
     */
    if (IsDeclarative(env)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_DEBUG_NO_SCOPE_OBJECT);
        return false;
    }

    JSObject *obj;
    if (IsWith(env)) {
        obj = &env->asDebugScope().scope().asWith().object();
    } else {
        obj = env;
        JS_ASSERT(!obj->isDebugScope());
    }

    args.rval().setObject(*obj);
    if (!dbg->wrapDebuggeeValue(cx, args.rval()))
        return false;
    return true;
}

static JSBool
DebuggerEnv_getCallee(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGENV_OWNER(cx, argc, vp, "get callee", args, envobj, env, dbg);

    args.rval().setNull();

    if (!env->isDebugScope())
        return true;

    JSObject &scope = env->asDebugScope().scope();
    if (!scope.isCall())
        return true;

    CallObject &callobj = scope.asCall();
    if (callobj.isForEval())
        return true;

    args.rval().setObject(callobj.callee());
    if (!dbg->wrapDebuggeeValue(cx, args.rval()))
        return false;
    return true;
}

static JSBool
DebuggerEnv_names(JSContext *cx, unsigned argc, Value *vp)
{
    THIS_DEBUGENV_OWNER(cx, argc, vp, "names", args, envobj, env, dbg);

    AutoIdVector keys(cx);
    {
        Maybe<AutoCompartment> ac;
        ac.construct(cx, env);
        ErrorCopier ec(ac, dbg->toJSObject());
        if (!GetPropertyNames(cx, env, JSITER_HIDDEN, &keys))
            return false;
    }

    RootedObject arr(cx, NewDenseEmptyArray(cx));
    if (!arr)
        return false;
    RootedId id(cx);
    for (size_t i = 0, len = keys.length(); i < len; i++) {
        id = keys[i];
        if (JSID_IS_ATOM(id) && IsIdentifier(JSID_TO_ATOM(id))) {
            if (!cx->compartment->wrapId(cx, id.address()))
                return false;
            if (!js_NewbornArrayPush(cx, arr, StringValue(JSID_TO_STRING(id))))
                return false;
        }
    }
    args.rval().setObject(*arr);
    return true;
}

static JSBool
DebuggerEnv_find(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.Environment.find", 1);
    THIS_DEBUGENV_OWNER(cx, argc, vp, "find", args, envobj, env, dbg);

    RootedId id(cx);
    if (!ValueToIdentifier(cx, args[0], &id))
        return false;

    {
        Maybe<AutoCompartment> ac;
        ac.construct(cx, env);
        if (!cx->compartment->wrapId(cx, id.address()))
            return false;

        /* This can trigger resolve hooks. */
        ErrorCopier ec(ac, dbg->toJSObject());
        RootedShape prop(cx);
        RootedObject pobj(cx);
        for (; env && !prop; env = env->enclosingScope()) {
            if (!JSObject::lookupGeneric(cx, env, id, &pobj, &prop))
                return false;
            if (prop)
                break;
        }
    }

    return dbg->wrapEnvironment(cx, env, args.rval());
}

static JSBool
DebuggerEnv_getVariable(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.Environment.getVariable", 1);
    THIS_DEBUGENV_OWNER(cx, argc, vp, "getVariable", args, envobj, env, dbg);

    RootedId id(cx);
    if (!ValueToIdentifier(cx, args[0], &id))
        return false;

    RootedValue v(cx);
    {
        Maybe<AutoCompartment> ac;
        ac.construct(cx, env);
        if (!cx->compartment->wrapId(cx, id.address()))
            return false;

        /* This can trigger getters. */
        ErrorCopier ec(ac, dbg->toJSObject());
        if (!JSObject::getGeneric(cx, env, env, id, &v))
            return false;
    }

    if (!dbg->wrapDebuggeeValue(cx, &v))
        return false;
    args.rval().set(v);
    return true;
}

static JSBool
DebuggerEnv_setVariable(JSContext *cx, unsigned argc, Value *vp)
{
    REQUIRE_ARGC("Debugger.Environment.setVariable", 2);
    THIS_DEBUGENV_OWNER(cx, argc, vp, "setVariable", args, envobj, env, dbg);

    RootedId id(cx);
    if (!ValueToIdentifier(cx, args[0], &id))
        return false;

    RootedValue v(cx, args[1]);
    if (!dbg->unwrapDebuggeeValue(cx, &v))
        return false;

    {
        Maybe<AutoCompartment> ac;
        ac.construct(cx, env);
        if (!cx->compartment->wrapId(cx, id.address()) || !cx->compartment->wrap(cx, &v))
            return false;

        /* This can trigger setters. */
        ErrorCopier ec(ac, dbg->toJSObject());

        /* Make sure the environment actually has the specified binding. */
        bool has;
        if (!JSObject::hasProperty(cx, env, id, &has))
            return false;
        if (!has) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_DEBUG_VARIABLE_NOT_FOUND);
            return false;
        }

        /* Just set the property. */
        if (!JSObject::setGeneric(cx, env, env, id, &v, true))
            return false;
    }

    args.rval().setUndefined();
    return true;
}

static JSPropertySpec DebuggerEnv_properties[] = {
    JS_PSG("type", DebuggerEnv_getType, 0),
    JS_PSG("object", DebuggerEnv_getObject, 0),
    JS_PSG("parent", DebuggerEnv_getParent, 0),
    JS_PSG("callee", DebuggerEnv_getCallee, 0),
    JS_PS_END
};

static JSFunctionSpec DebuggerEnv_methods[] = {
    JS_FN("names", DebuggerEnv_names, 0, 0),
    JS_FN("find", DebuggerEnv_find, 1, 0),
    JS_FN("getVariable", DebuggerEnv_getVariable, 1, 0),
    JS_FN("setVariable", DebuggerEnv_setVariable, 2, 0),
    JS_FS_END
};



/*** Glue ****************************************************************************************/

extern JS_PUBLIC_API(JSBool)
JS_DefineDebuggerObject(JSContext *cx, JSObject *obj_)
{
    RootedObject obj(cx, obj_);

    RootedObject
        objProto(cx),
        debugCtor(cx),
        debugProto(cx),
        frameProto(cx),
        scriptProto(cx),
        objectProto(cx),
        envProto(cx);

    objProto = obj->asGlobal().getOrCreateObjectPrototype(cx);
    if (!objProto)
        return false;


    debugProto = js_InitClass(cx, obj,
                              objProto, &Debugger::jsclass, Debugger::construct,
                              1, Debugger::properties, Debugger::methods, NULL, NULL,
                              debugCtor.address());
    if (!debugProto)
        return false;

    frameProto = js_InitClass(cx, debugCtor, objProto, &DebuggerFrame_class,
                              DebuggerFrame_construct, 0,
                              DebuggerFrame_properties, DebuggerFrame_methods,
                              NULL, NULL);
    if (!frameProto)
        return false;

    scriptProto = js_InitClass(cx, debugCtor, objProto, &DebuggerScript_class,
                               DebuggerScript_construct, 0,
                               DebuggerScript_properties, DebuggerScript_methods,
                               NULL, NULL);
    if (!scriptProto)
        return false;

    objectProto = js_InitClass(cx, debugCtor, objProto, &DebuggerObject_class,
                               DebuggerObject_construct, 0,
                               DebuggerObject_properties, DebuggerObject_methods,
                               NULL, NULL);
    if (!objectProto)
        return false;

    envProto = js_InitClass(cx, debugCtor, objProto, &DebuggerEnv_class,
                                      DebuggerEnv_construct, 0,
                                      DebuggerEnv_properties, DebuggerEnv_methods,
                                      NULL, NULL);
    if (!envProto)
        return false;

    debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_FRAME_PROTO, ObjectValue(*frameProto));
    debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_OBJECT_PROTO, ObjectValue(*objectProto));
    debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_SCRIPT_PROTO, ObjectValue(*scriptProto));
    debugProto->setReservedSlot(Debugger::JSSLOT_DEBUG_ENV_PROTO, ObjectValue(*envProto));
    return true;
}
