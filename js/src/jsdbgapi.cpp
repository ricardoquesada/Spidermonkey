/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS debugging API.
 */
#include <string.h>
#include <stdarg.h>
#include "jsprvtd.h"
#include "jstypes.h"
#include "jsutil.h"
#include "jsclist.h"
#include "jsapi.h"
#include "jscntxt.h"
#include "jsversion.h"
#include "jsdbgapi.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinterp.h"
#include "jslock.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstr.h"
#include "jswatchpoint.h"
#include "jswrapper.h"

#include "gc/Marking.h"
#include "frontend/BytecodeEmitter.h"
#include "frontend/Parser.h"
#include "vm/Debugger.h"

#include "jsatominlines.h"
#include "jsinferinlines.h"
#include "jsobjinlines.h"
#include "jsinterpinlines.h"
#include "jsscopeinlines.h"
#include "jsscriptinlines.h"

#include "vm/Stack-inl.h"

#include "jsautooplen.h"
#include "mozilla/Util.h"

#ifdef __APPLE__
#include "sharkctl.h"
#endif

using namespace js;
using namespace js::gc;
using namespace mozilla;

JS_PUBLIC_API(JSBool)
JS_GetDebugMode(JSContext *cx)
{
    return cx->compartment->debugMode();
}

JS_PUBLIC_API(JSBool)
JS_SetDebugMode(JSContext *cx, JSBool debug)
{
    return JS_SetDebugModeForCompartment(cx, cx->compartment, debug);
}

JS_PUBLIC_API(void)
JS_SetRuntimeDebugMode(JSRuntime *rt, JSBool debug)
{
    rt->debugMode = !!debug;
}

namespace js {

JSTrapStatus
ScriptDebugPrologue(JSContext *cx, StackFrame *fp)
{
    JS_ASSERT(fp == cx->fp());

    if (fp->isFramePushedByExecute()) {
        if (JSInterpreterHook hook = cx->runtime->debugHooks.executeHook)
            fp->setHookData(hook(cx, Jsvalify(fp), true, 0, cx->runtime->debugHooks.executeHookData));
    } else {
        if (JSInterpreterHook hook = cx->runtime->debugHooks.callHook)
            fp->setHookData(hook(cx, Jsvalify(fp), true, 0, cx->runtime->debugHooks.callHookData));
    }

    Value rval;
    JSTrapStatus status = Debugger::onEnterFrame(cx, &rval);
    switch (status) {
      case JSTRAP_CONTINUE:
        break;
      case JSTRAP_THROW:
        cx->setPendingException(rval);
        break;
      case JSTRAP_ERROR:
        cx->clearPendingException();
        break;
      case JSTRAP_RETURN:
        fp->setReturnValue(rval);
        break;
      default:
        JS_NOT_REACHED("bad Debugger::onEnterFrame JSTrapStatus value");
    }
    return status;
}

bool
ScriptDebugEpilogue(JSContext *cx, StackFrame *fp, bool okArg)
{
    JS_ASSERT(fp == cx->fp());
    JSBool ok = okArg;

    if (void *hookData = fp->maybeHookData()) {
        if (fp->isFramePushedByExecute()) {
            if (JSInterpreterHook hook = cx->runtime->debugHooks.executeHook)
                hook(cx, Jsvalify(fp), false, &ok, hookData);
        } else {
            if (JSInterpreterHook hook = cx->runtime->debugHooks.callHook)
                hook(cx, Jsvalify(fp), false, &ok, hookData);
        }
    }

    return Debugger::onLeaveFrame(cx, ok);
}

} /* namespace js */

JS_FRIEND_API(JSBool)
JS_SetDebugModeForAllCompartments(JSContext *cx, JSBool debug)
{
    AutoDebugModeGC dmgc(cx->runtime);

    for (CompartmentsIter c(cx->runtime); !c.done(); c.next()) {
        // Ignore special compartments (atoms, JSD compartments)
        if (c->principals) {
            if (!c->setDebugModeFromC(cx, !!debug, dmgc))
                return false;
        }
    }
    return true;
}

JS_FRIEND_API(JSBool)
JS_SetDebugModeForCompartment(JSContext *cx, JSCompartment *comp, JSBool debug)
{
    AutoDebugModeGC dmgc(cx->runtime);
    return comp->setDebugModeFromC(cx, !!debug, dmgc);
}

static JSBool
CheckDebugMode(JSContext *cx)
{
    JSBool debugMode = JS_GetDebugMode(cx);
    /*
     * :TODO:
     * This probably should be an assertion, since it's indicative of a severe
     * API misuse.
     */
    if (!debugMode) {
        JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR, js_GetErrorMessage,
                                     NULL, JSMSG_NEED_DEBUG_MODE);
    }
    return debugMode;
}

JS_PUBLIC_API(JSBool)
JS_SetSingleStepMode(JSContext *cx, JSScript *script, JSBool singleStep)
{
    assertSameCompartment(cx, script);
    if (!CheckDebugMode(cx))
        return JS_FALSE;

    return script->setStepModeFlag(cx, singleStep);
}

JS_PUBLIC_API(JSBool)
JS_SetTrap(JSContext *cx, JSScript *script, jsbytecode *pc, JSTrapHandler handler, jsval closure)
{
    assertSameCompartment(cx, script, closure);

    if (!CheckDebugMode(cx))
        return false;

    BreakpointSite *site = script->getOrCreateBreakpointSite(cx, pc);
    if (!site)
        return false;
    site->setTrap(cx->runtime->defaultFreeOp(), handler, closure);
    return true;
}

JS_PUBLIC_API(void)
JS_ClearTrap(JSContext *cx, JSScript *script, jsbytecode *pc,
             JSTrapHandler *handlerp, jsval *closurep)
{
    if (BreakpointSite *site = script->getBreakpointSite(pc)) {
        site->clearTrap(cx->runtime->defaultFreeOp(), handlerp, closurep);
    } else {
        if (handlerp)
            *handlerp = NULL;
        if (closurep)
            *closurep = JSVAL_VOID;
    }
}

JS_PUBLIC_API(void)
JS_ClearScriptTraps(JSContext *cx, JSScript *script)
{
    script->clearTraps(cx->runtime->defaultFreeOp());
}

JS_PUBLIC_API(void)
JS_ClearAllTrapsForCompartment(JSContext *cx)
{
    cx->compartment->clearTraps(cx->runtime->defaultFreeOp());
}

JS_PUBLIC_API(JSBool)
JS_SetInterrupt(JSRuntime *rt, JSInterruptHook hook, void *closure)
{
    rt->debugHooks.interruptHook = hook;
    rt->debugHooks.interruptHookData = closure;
    for (InterpreterFrames *f = rt->interpreterFrames; f; f = f->older)
        f->enableInterruptsUnconditionally();
    return true;
}

JS_PUBLIC_API(JSBool)
JS_ClearInterrupt(JSRuntime *rt, JSInterruptHook *hoop, void **closurep)
{
    if (hoop)
        *hoop = rt->debugHooks.interruptHook;
    if (closurep)
        *closurep = rt->debugHooks.interruptHookData;
    rt->debugHooks.interruptHook = 0;
    rt->debugHooks.interruptHookData = 0;
    return JS_TRUE;
}

/************************************************************************/

JS_PUBLIC_API(JSBool)
JS_SetWatchPoint(JSContext *cx, JSObject *obj_, jsid id,
                 JSWatchPointHandler handler, JSObject *closure_)
{
    assertSameCompartment(cx, obj_);

    RootedObject obj(cx, obj_), closure(cx, closure_);

    JSObject *origobj = obj;
    obj = GetInnerObject(cx, obj);
    if (!obj)
        return false;

    Value v;
    unsigned attrs;

    RootedId propid(cx);

    if (JSID_IS_INT(id)) {
        propid = id;
    } else if (JSID_IS_OBJECT(id)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_WATCH_PROP);
        return false;
    } else {
        if (!ValueToId(cx, IdToValue(id), propid.address()))
            return false;
    }

    /*
     * If, by unwrapping and innerizing, we changed the object, check
     * again to make sure that we're allowed to set a watch point.
     */
    if (origobj != obj && !CheckAccess(cx, obj, propid, JSACC_WATCH, &v, &attrs))
        return false;

    if (!obj->isNative()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_WATCH,
                             obj->getClass()->name);
        return false;
    }

    types::MarkTypePropertyConfigured(cx, obj, propid);

    WatchpointMap *wpmap = cx->compartment->watchpointMap;
    if (!wpmap) {
        wpmap = cx->runtime->new_<WatchpointMap>();
        if (!wpmap || !wpmap->init()) {
            js_ReportOutOfMemory(cx);
            return false;
        }
        cx->compartment->watchpointMap = wpmap;
    }
    return wpmap->watch(cx, obj, propid, handler, closure);
}

JS_PUBLIC_API(JSBool)
JS_ClearWatchPoint(JSContext *cx, JSObject *obj, jsid id,
                   JSWatchPointHandler *handlerp, JSObject **closurep)
{
    assertSameCompartment(cx, obj, id);

    if (WatchpointMap *wpmap = cx->compartment->watchpointMap)
        wpmap->unwatch(obj, id, handlerp, closurep);
    return true;
}

JS_PUBLIC_API(JSBool)
JS_ClearWatchPointsForObject(JSContext *cx, JSObject *obj)
{
    assertSameCompartment(cx, obj);

    if (WatchpointMap *wpmap = cx->compartment->watchpointMap)
        wpmap->unwatchObject(obj);
    return true;
}

JS_PUBLIC_API(JSBool)
JS_ClearAllWatchPoints(JSContext *cx)
{
    if (JSCompartment *comp = cx->compartment) {
        if (WatchpointMap *wpmap = comp->watchpointMap)
            wpmap->clear();
    }
    return true;
}

/************************************************************************/

JS_PUBLIC_API(unsigned)
JS_PCToLineNumber(JSContext *cx, JSScript *script, jsbytecode *pc)
{
    return js::PCToLineNumber(script, pc);
}

JS_PUBLIC_API(jsbytecode *)
JS_LineNumberToPC(JSContext *cx, JSScript *script, unsigned lineno)
{
    return js_LineNumberToPC(script, lineno);
}

JS_PUBLIC_API(jsbytecode *)
JS_EndPC(JSContext *cx, JSScript *script)
{
    return script->code + script->length;
}

JS_PUBLIC_API(JSBool)
JS_GetLinePCs(JSContext *cx, JSScript *script,
              unsigned startLine, unsigned maxLines,
              unsigned* count, unsigned** retLines, jsbytecode*** retPCs)
{
    unsigned* lines;
    jsbytecode** pcs;
    size_t len = (script->length > maxLines ? maxLines : script->length);
    lines = (unsigned*) cx->malloc_(len * sizeof(unsigned));
    if (!lines)
        return JS_FALSE;

    pcs = (jsbytecode**) cx->malloc_(len * sizeof(jsbytecode*));
    if (!pcs) {
        cx->free_(lines);
        return JS_FALSE;
    }

    unsigned lineno = script->lineno;
    unsigned offset = 0;
    unsigned i = 0;
    for (jssrcnote *sn = script->notes(); !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn)) {
        offset += SN_DELTA(sn);
        SrcNoteType type = (SrcNoteType) SN_TYPE(sn);
        if (type == SRC_SETLINE || type == SRC_NEWLINE) {
            if (type == SRC_SETLINE)
                lineno = (unsigned) js_GetSrcNoteOffset(sn, 0);
            else
                lineno++;

            if (lineno >= startLine) {
                lines[i] = lineno;
                pcs[i] = script->code + offset;
                if (++i >= maxLines)
                    break;
            }
        }
    }

    *count = i;
    if (retLines)
        *retLines = lines;
    else
        cx->free_(lines);

    if (retPCs)
        *retPCs = pcs;
    else
        cx->free_(pcs);

    return JS_TRUE;
}

JS_PUBLIC_API(unsigned)
JS_GetFunctionArgumentCount(JSContext *cx, JSFunction *fun)
{
    return fun->nargs;
}

JS_PUBLIC_API(JSBool)
JS_FunctionHasLocalNames(JSContext *cx, JSFunction *fun)
{
    return fun->script()->bindings.count() > 0;
}

extern JS_PUBLIC_API(uintptr_t *)
JS_GetFunctionLocalNameArray(JSContext *cx, JSFunction *fun, void **markp)
{
    BindingVector bindings(cx);
    if (!FillBindingVector(fun->script()->bindings, &bindings))
        return NULL;

    /* Munge data into the API this method implements.  Avert your eyes! */
    *markp = cx->tempLifoAlloc().mark();

    uintptr_t *names = cx->tempLifoAlloc().newArray<uintptr_t>(bindings.length());
    if (!names) {
        js_ReportOutOfMemory(cx);
        return NULL;
    }

    for (size_t i = 0; i < bindings.length(); i++)
        names[i] = reinterpret_cast<uintptr_t>(bindings[i].name());

    return names;
}

extern JS_PUBLIC_API(JSAtom *)
JS_LocalNameToAtom(uintptr_t w)
{
    return reinterpret_cast<JSAtom *>(w);
}

extern JS_PUBLIC_API(JSString *)
JS_AtomKey(JSAtom *atom)
{
    return atom;
}

extern JS_PUBLIC_API(void)
JS_ReleaseFunctionLocalNameArray(JSContext *cx, void *mark)
{
    cx->tempLifoAlloc().release(mark);
}

JS_PUBLIC_API(JSScript *)
JS_GetFunctionScript(JSContext *cx, JSFunction *fun)
{
    return fun->maybeScript();
}

JS_PUBLIC_API(JSNative)
JS_GetFunctionNative(JSContext *cx, JSFunction *fun)
{
    return fun->maybeNative();
}

JS_PUBLIC_API(JSPrincipals *)
JS_GetScriptPrincipals(JSScript *script)
{
    return script->principals;
}

JS_PUBLIC_API(JSPrincipals *)
JS_GetScriptOriginPrincipals(JSScript *script)
{
    return script->originPrincipals;
}

/************************************************************************/

/*
 *  Stack Frame Iterator
 */
JS_PUBLIC_API(JSStackFrame *)
JS_FrameIterator(JSContext *cx, JSStackFrame **iteratorp)
{
    StackFrame *fp = Valueify(*iteratorp);
    *iteratorp = Jsvalify((fp == NULL) ? js_GetTopStackFrame(cx, FRAME_EXPAND_ALL) : fp->prev());
    return *iteratorp;
}

JS_PUBLIC_API(JSScript *)
JS_GetFrameScript(JSContext *cx, JSStackFrame *fpArg)
{
    return Valueify(fpArg)->script();
}

JS_PUBLIC_API(jsbytecode *)
JS_GetFramePC(JSContext *cx, JSStackFrame *fpArg)
{
    /*
     * This API is used to compute the line number for jsd and XPConnect
     * exception handling backtraces. Once the stack gets really deep, the
     * overall cost can become quadratic. This can hang the browser (eventually
     * terminated by a slow-script dialog) when content causes infinite
     * recursion and a backtrace.
     */
    return Valueify(fpArg)->pcQuadratic(cx->stack, 100);
}

JS_PUBLIC_API(void *)
JS_GetFrameAnnotation(JSContext *cx, JSStackFrame *fpArg)
{
    StackFrame *fp = Valueify(fpArg);
    if (fp->annotation() && fp->scopeChain()->compartment()->principals) {
        /*
         * Give out an annotation only if privileges have not been revoked
         * or disabled globally.
         */
        return fp->annotation();
    }

    return NULL;
}

JS_PUBLIC_API(void)
JS_SetFrameAnnotation(JSContext *cx, JSStackFrame *fp, void *annotation)
{
    Valueify(fp)->setAnnotation(annotation);
}

JS_PUBLIC_API(JSObject *)
JS_GetFrameScopeChain(JSContext *cx, JSStackFrame *fpArg)
{
    StackFrame *fp = Valueify(fpArg);
    JS_ASSERT(cx->stack.space().containsSlow(fp));
    AutoCompartment ac(cx, fp->scopeChain());
    return GetDebugScopeForFrame(cx, fp);
}

JS_PUBLIC_API(JSObject *)
JS_GetFrameCallObject(JSContext *cx, JSStackFrame *fpArg)
{
    StackFrame *fp = Valueify(fpArg);
    JS_ASSERT(cx->stack.space().containsSlow(fp));

    if (!fp->isFunctionFrame())
        return NULL;

    JSObject *o = GetDebugScopeForFrame(cx, fp);

    /*
     * Given that fp is a function frame and GetDebugScopeForFrame always fills
     * in missing scopes, we can expect to find fp's CallObject on 'o'. Note:
     *  - GetDebugScopeForFrame wraps every ScopeObject (missing or not) with
     *    a DebugScopeObject proxy.
     *  - If fp is an eval-in-function, then fp has no callobj of its own and
     *    JS_GetFrameCallObject will return the innermost function's callobj.
     */
    while (o) {
        ScopeObject &scope = o->asDebugScope().scope();
        if (scope.isCall())
            return o;
        o = o->enclosingScope();
    }
    return NULL;
}

JS_PUBLIC_API(JSBool)
JS_GetFrameThis(JSContext *cx, JSStackFrame *fpArg, jsval *thisv)
{
    StackFrame *fp = Valueify(fpArg);

    js::AutoCompartment ac(cx, fp->scopeChain());
    if (!ComputeThis(cx, fp))
        return false;

    *thisv = fp->thisValue();
    return true;
}

JS_PUBLIC_API(JSFunction *)
JS_GetFrameFunction(JSContext *cx, JSStackFrame *fp)
{
    return Valueify(fp)->maybeScriptFunction();
}

JS_PUBLIC_API(JSObject *)
JS_GetFrameFunctionObject(JSContext *cx, JSStackFrame *fpArg)
{
    StackFrame *fp = Valueify(fpArg);
    if (!fp->isFunctionFrame())
        return NULL;

    JS_ASSERT(fp->callee().isFunction());
    return &fp->callee();
}

JS_PUBLIC_API(JSFunction *)
JS_GetScriptFunction(JSContext *cx, JSScript *script)
{
    return script->function();
}

JS_PUBLIC_API(JSObject *)
JS_GetParentOrScopeChain(JSContext *cx, JSObject *obj)
{
    return obj->enclosingScope();
}

JS_PUBLIC_API(JSBool)
JS_IsConstructorFrame(JSContext *cx, JSStackFrame *fp)
{
    return Valueify(fp)->isConstructing();
}

JS_PUBLIC_API(JSObject *)
JS_GetFrameCalleeObject(JSContext *cx, JSStackFrame *fp)
{
    return Valueify(fp)->maybeCalleev().toObjectOrNull();
}

JS_PUBLIC_API(const char *)
JS_GetDebugClassName(JSObject *obj)
{
    if (obj->isDebugScope())
        return obj->asDebugScope().scope().getClass()->name;
    return obj->getClass()->name;
}

JS_PUBLIC_API(JSBool)
JS_IsDebuggerFrame(JSContext *cx, JSStackFrame *fp)
{
    return Valueify(fp)->isDebuggerFrame();
}

JS_PUBLIC_API(JSBool)
JS_IsGlobalFrame(JSContext *cx, JSStackFrame *fp)
{
    return Valueify(fp)->isGlobalFrame();
}

JS_PUBLIC_API(jsval)
JS_GetFrameReturnValue(JSContext *cx, JSStackFrame *fp)
{
    return Valueify(fp)->returnValue();
}

JS_PUBLIC_API(void)
JS_SetFrameReturnValue(JSContext *cx, JSStackFrame *fpArg, jsval rval)
{
    StackFrame *fp = Valueify(fpArg);
#ifdef JS_METHODJIT
    JS_ASSERT(fp->script()->debugMode);
#endif
    assertSameCompartment(cx, fp, rval);
    fp->setReturnValue(rval);
}

/************************************************************************/

JS_PUBLIC_API(const char *)
JS_GetScriptFilename(JSContext *cx, JSScript *script)
{
    return script->filename;
}

JS_PUBLIC_API(const jschar *)
JS_GetScriptSourceMap(JSContext *cx, JSScript *script)
{
    ScriptSource *source = script->scriptSource();
    JS_ASSERT(source);
    return source->hasSourceMap() ? source->sourceMap() : NULL;
}

JS_PUBLIC_API(unsigned)
JS_GetScriptBaseLineNumber(JSContext *cx, JSScript *script)
{
    return script->lineno;
}

JS_PUBLIC_API(unsigned)
JS_GetScriptLineExtent(JSContext *cx, JSScript *script)
{
    return js_GetScriptLineExtent(script);
}

JS_PUBLIC_API(JSVersion)
JS_GetScriptVersion(JSContext *cx, JSScript *script)
{
    return VersionNumber(script->getVersion());
}

/***************************************************************************/

JS_PUBLIC_API(void)
JS_SetNewScriptHook(JSRuntime *rt, JSNewScriptHook hook, void *callerdata)
{
    rt->debugHooks.newScriptHook = hook;
    rt->debugHooks.newScriptHookData = callerdata;
}

JS_PUBLIC_API(void)
JS_SetDestroyScriptHook(JSRuntime *rt, JSDestroyScriptHook hook,
                        void *callerdata)
{
    rt->debugHooks.destroyScriptHook = hook;
    rt->debugHooks.destroyScriptHookData = callerdata;
}

/***************************************************************************/

JS_PUBLIC_API(JSBool)
JS_EvaluateUCInStackFrame(JSContext *cx, JSStackFrame *fpArg,
                          const jschar *chars, unsigned length,
                          const char *filename, unsigned lineno,
                          jsval *rval)
{
    if (!CheckDebugMode(cx))
        return false;

    SkipRoot skip(cx, &chars);

    Rooted<Env*> env(cx, JS_GetFrameScopeChain(cx, fpArg));
    if (!env)
        return false;

    StackFrame *fp = Valueify(fpArg);

    js::AutoCompartment ac(cx, env);
    return EvaluateInEnv(cx, env, fp, chars, length, filename, lineno, rval);
}

JS_PUBLIC_API(JSBool)
JS_EvaluateInStackFrame(JSContext *cx, JSStackFrame *fp,
                        const char *bytes, unsigned length,
                        const char *filename, unsigned lineno,
                        jsval *rval)
{
    jschar *chars;
    JSBool ok;
    size_t len = length;

    if (!CheckDebugMode(cx))
        return JS_FALSE;

    chars = InflateString(cx, bytes, &len);
    if (!chars)
        return JS_FALSE;
    length = (unsigned) len;
    ok = JS_EvaluateUCInStackFrame(cx, fp, chars, length, filename, lineno,
                                   rval);
    cx->free_(chars);

    return ok;
}

/************************************************************************/

/* This all should be reworked to avoid requiring JSScopeProperty types. */

static JSBool
GetPropertyDesc(JSContext *cx, JSObject *obj_, Shape *shape, JSPropertyDesc *pd)
{
    assertSameCompartment(cx, obj_);
    pd->id = IdToJsval(shape->propid());

    RootedObject obj(cx, obj_);

    JSBool wasThrowing = cx->isExceptionPending();
    Value lastException = UndefinedValue();
    if (wasThrowing)
        lastException = cx->getPendingException();
    cx->clearPendingException();

    Rooted<jsid> id(cx, shape->propid());
    RootedValue value(cx);
    if (!baseops::GetProperty(cx, obj, id, &value)) {
        if (!cx->isExceptionPending()) {
            pd->flags = JSPD_ERROR;
            pd->value = JSVAL_VOID;
        } else {
            pd->flags = JSPD_EXCEPTION;
            pd->value = cx->getPendingException();
        }
    } else {
        pd->flags = 0;
        pd->value = value;
    }

    if (wasThrowing)
        cx->setPendingException(lastException);

    pd->flags |= (shape->enumerable() ? JSPD_ENUMERATE : 0)
              |  (!shape->writable()  ? JSPD_READONLY  : 0)
              |  (!shape->configurable() ? JSPD_PERMANENT : 0);
    pd->spare = 0;
    pd->alias = JSVAL_VOID;

    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_GetPropertyDescArray(JSContext *cx, JSObject *obj_, JSPropertyDescArray *pda)
{
    RootedObject obj(cx, obj_);

    assertSameCompartment(cx, obj);
    uint32_t i = 0;
    JSPropertyDesc *pd = NULL;

    if (obj->isDebugScope()) {
        AutoIdVector props(cx);
        if (!Proxy::enumerate(cx, obj, props))
            return false;

        pd = (JSPropertyDesc *)cx->calloc_(props.length() * sizeof(JSPropertyDesc));
        if (!pd)
            return false;

        for (i = 0; i < props.length(); ++i) {
            pd[i].id = JSVAL_NULL;
            pd[i].value = JSVAL_NULL;
            if (!js_AddRoot(cx, &pd[i].id, NULL))
                goto bad;
            pd[i].id = IdToValue(props[i]);
            if (!js_AddRoot(cx, &pd[i].value, NULL))
                goto bad;
            if (!Proxy::get(cx, obj, obj, props.handleAt(i), MutableHandleValue::fromMarkedLocation(&pd[i].value)))
                goto bad;
        }

        pda->length = props.length();
        pda->array = pd;
        return true;
    }

    Class *clasp;
    clasp = obj->getClass();
    if (!obj->isNative() || (clasp->flags & JSCLASS_NEW_ENUMERATE)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_CANT_DESCRIBE_PROPS, clasp->name);
        return false;
    }
    if (!clasp->enumerate(cx, obj))
        return false;

    /* Return an empty pda early if obj has no own properties. */
    if (obj->nativeEmpty()) {
        pda->length = 0;
        pda->array = NULL;
        return true;
    }

    pd = (JSPropertyDesc *)cx->malloc_(obj->propertyCount() * sizeof(JSPropertyDesc));
    if (!pd)
        return false;
    for (Shape::Range r = obj->lastProperty()->all(); !r.empty(); r.popFront()) {
        pd[i].id = JSVAL_NULL;
        pd[i].value = JSVAL_NULL;
        pd[i].alias = JSVAL_NULL;
        if (!js_AddRoot(cx, &pd[i].id, NULL))
            goto bad;
        if (!js_AddRoot(cx, &pd[i].value, NULL))
            goto bad;
        Shape *shape = const_cast<Shape *>(&r.front());
        if (!GetPropertyDesc(cx, obj, shape, &pd[i]))
            goto bad;
        if ((pd[i].flags & JSPD_ALIAS) && !js_AddRoot(cx, &pd[i].alias, NULL))
            goto bad;
        if (++i == obj->propertyCount())
            break;
    }
    pda->length = i;
    pda->array = pd;
    return true;

bad:
    pda->length = i + 1;
    pda->array = pd;
    JS_PutPropertyDescArray(cx, pda);
    return false;
}

JS_PUBLIC_API(void)
JS_PutPropertyDescArray(JSContext *cx, JSPropertyDescArray *pda)
{
    JSPropertyDesc *pd;
    uint32_t i;

    pd = pda->array;
    for (i = 0; i < pda->length; i++) {
        js_RemoveRoot(cx->runtime, &pd[i].id);
        js_RemoveRoot(cx->runtime, &pd[i].value);
        if (pd[i].flags & JSPD_ALIAS)
            js_RemoveRoot(cx->runtime, &pd[i].alias);
    }
    cx->free_(pd);
    pda->array = NULL;
    pda->length = 0;
}

/************************************************************************/

JS_PUBLIC_API(JSBool)
JS_SetDebuggerHandler(JSRuntime *rt, JSDebuggerHandler handler, void *closure)
{
    rt->debugHooks.debuggerHandler = handler;
    rt->debugHooks.debuggerHandlerData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetSourceHandler(JSRuntime *rt, JSSourceHandler handler, void *closure)
{
    rt->debugHooks.sourceHandler = handler;
    rt->debugHooks.sourceHandlerData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetExecuteHook(JSRuntime *rt, JSInterpreterHook hook, void *closure)
{
    rt->debugHooks.executeHook = hook;
    rt->debugHooks.executeHookData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetCallHook(JSRuntime *rt, JSInterpreterHook hook, void *closure)
{
    rt->debugHooks.callHook = hook;
    rt->debugHooks.callHookData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetThrowHook(JSRuntime *rt, JSThrowHook hook, void *closure)
{
    rt->debugHooks.throwHook = hook;
    rt->debugHooks.throwHookData = closure;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_SetDebugErrorHook(JSRuntime *rt, JSDebugErrorHook hook, void *closure)
{
    rt->debugHooks.debugErrorHook = hook;
    rt->debugHooks.debugErrorHookData = closure;
    return JS_TRUE;
}

/************************************************************************/

JS_PUBLIC_API(size_t)
JS_GetObjectTotalSize(JSContext *cx, JSObject *obj)
{
    return obj->computedSizeOfThisSlotsElements();
}

static size_t
GetAtomTotalSize(JSContext *cx, JSAtom *atom)
{
    return sizeof(AtomStateEntry) + sizeof(HashNumber) +
           sizeof(JSString) +
           (atom->length() + 1) * sizeof(jschar);
}

JS_PUBLIC_API(size_t)
JS_GetFunctionTotalSize(JSContext *cx, JSFunction *fun)
{
    size_t nbytes;

    nbytes = sizeof *fun;
    nbytes += JS_GetObjectTotalSize(cx, fun);
    if (fun->isInterpreted())
        nbytes += JS_GetScriptTotalSize(cx, fun->script());
    if (fun->displayAtom())
        nbytes += GetAtomTotalSize(cx, fun->displayAtom());
    return nbytes;
}

JS_PUBLIC_API(size_t)
JS_GetScriptTotalSize(JSContext *cx, JSScript *script)
{
    size_t nbytes, pbytes;
    jssrcnote *sn, *notes;
    ObjectArray *objarray;
    JSPrincipals *principals;

    nbytes = sizeof *script;
    nbytes += script->length * sizeof script->code[0];
    nbytes += script->natoms * sizeof script->atoms[0];
    for (size_t i = 0; i < script->natoms; i++)
        nbytes += GetAtomTotalSize(cx, script->atoms[i]);

    if (script->filename)
        nbytes += strlen(script->filename) + 1;

    notes = script->notes();
    for (sn = notes; !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn))
        continue;
    nbytes += (sn - notes + 1) * sizeof *sn;

    if (script->hasObjects()) {
        objarray = script->objects();
        size_t i = objarray->length;
        nbytes += sizeof *objarray + i * sizeof objarray->vector[0];
        do {
            nbytes += JS_GetObjectTotalSize(cx, objarray->vector[--i]);
        } while (i != 0);
    }

    if (script->hasRegexps()) {
        objarray = script->regexps();
        size_t i = objarray->length;
        nbytes += sizeof *objarray + i * sizeof objarray->vector[0];
        do {
            nbytes += JS_GetObjectTotalSize(cx, objarray->vector[--i]);
        } while (i != 0);
    }

    if (script->hasTrynotes())
        nbytes += sizeof(TryNoteArray) + script->trynotes()->length * sizeof(JSTryNote);

    principals = script->principals;
    if (principals) {
        JS_ASSERT(principals->refcount);
        pbytes = sizeof *principals;
        if (principals->refcount > 1)
            pbytes = JS_HOWMANY(pbytes, principals->refcount);
        nbytes += pbytes;
    }

    return nbytes;
}

/************************************************************************/

JS_FRIEND_API(void)
js_RevertVersion(JSContext *cx)
{
    cx->clearVersionOverride();
}

JS_PUBLIC_API(const JSDebugHooks *)
JS_GetGlobalDebugHooks(JSRuntime *rt)
{
    return &rt->debugHooks;
}

/************************************************************************/

/* Profiling-related API */

/* Thread-unsafe error management */

static char gLastError[2000];

static void
#ifdef __GNUC__
__attribute__((unused,format(printf,1,2)))
#endif
UnsafeError(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    (void) vsnprintf(gLastError, sizeof(gLastError), format, args);
    va_end(args);

    gLastError[sizeof(gLastError) - 1] = '\0';
}

JS_PUBLIC_API(const char *)
JS_UnsafeGetLastProfilingError()
{
    return gLastError;
}

JS_PUBLIC_API(JSBool)
JS_StartProfiling(const char *profileName)
{
    JSBool ok = JS_TRUE;
#if defined(MOZ_SHARK) && defined(__APPLE__)
    if (!Shark::Start()) {
        UnsafeError("Failed to start Shark for %s", profileName);
        ok = JS_FALSE;
    }
#endif
#ifdef MOZ_VTUNE
    if (!js_StartVtune(profileName))
        ok = JS_FALSE;
#endif
#ifdef __linux__
    if (!js_StartPerf())
        ok = JS_FALSE;
#endif
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_StopProfiling(const char *profileName)
{
    JSBool ok = JS_TRUE;
#if defined(MOZ_SHARK) && defined(__APPLE__)
    Shark::Stop();
#endif
#ifdef MOZ_VTUNE
    if (!js_StopVtune())
        ok = JS_FALSE;
#endif
#ifdef __linux__
    if (!js_StopPerf())
        ok = JS_FALSE;
#endif
    return ok;
}

/*
 * Start or stop whatever platform- and configuration-specific profiling
 * backends are available.
 */
static JSBool
ControlProfilers(bool toState)
{
    JSBool ok = JS_TRUE;

    if (! Probes::ProfilingActive && toState) {
#if defined(MOZ_SHARK) && defined(__APPLE__)
        if (!Shark::Start()) {
            UnsafeError("Failed to start Shark");
            ok = JS_FALSE;
        }
#endif
#ifdef MOZ_CALLGRIND
        if (! js_StartCallgrind()) {
            UnsafeError("Failed to start Callgrind");
            ok = JS_FALSE;
        }
#endif
#ifdef MOZ_VTUNE
        if (! js_ResumeVtune())
            ok = JS_FALSE;
#endif
    } else if (Probes::ProfilingActive && ! toState) {
#if defined(MOZ_SHARK) && defined(__APPLE__)
        Shark::Stop();
#endif
#ifdef MOZ_CALLGRIND
        if (! js_StopCallgrind()) {
            UnsafeError("failed to stop Callgrind");
            ok = JS_FALSE;
        }
#endif
#ifdef MOZ_VTUNE
        if (! js_PauseVtune())
            ok = JS_FALSE;
#endif
    }

    Probes::ProfilingActive = toState;

    return ok;
}

/*
 * Pause/resume whatever profiling mechanism is currently compiled
 * in, if applicable. This will not affect things like dtrace.
 *
 * Do not mix calls to these APIs with calls to the individual
 * profilers' pause/resume functions, because only overall state is
 * tracked, not the state of each profiler.
 */
JS_PUBLIC_API(JSBool)
JS_PauseProfilers(const char *profileName)
{
    return ControlProfilers(false);
}

JS_PUBLIC_API(JSBool)
JS_ResumeProfilers(const char *profileName)
{
    return ControlProfilers(true);
}

JS_PUBLIC_API(JSBool)
JS_DumpProfile(const char *outfile, const char *profileName)
{
    JSBool ok = JS_TRUE;
#ifdef MOZ_CALLGRIND
    js_DumpCallgrind(outfile);
#endif
    return ok;
}

#ifdef MOZ_PROFILING

struct RequiredStringArg {
    JSContext *mCx;
    char *mBytes;
    RequiredStringArg(JSContext *cx, unsigned argc, jsval *vp, size_t argi, const char *caller)
        : mCx(cx), mBytes(NULL)
    {
        if (argc <= argi) {
            JS_ReportError(cx, "%s: not enough arguments", caller);
        } else if (!JSVAL_IS_STRING(JS_ARGV(cx, vp)[argi])) {
            JS_ReportError(cx, "%s: invalid arguments (string expected)", caller);
        } else {
            mBytes = JS_EncodeString(cx, JSVAL_TO_STRING(JS_ARGV(cx, vp)[argi]));
        }
    }
    operator void*() {
        return (void*) mBytes;
    }
    ~RequiredStringArg() {
        if (mBytes)
            mCx->free_(mBytes);
    }
};

static JSBool
StartProfiling(JSContext *cx, unsigned argc, jsval *vp)
{
    if (argc == 0) {
        JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(JS_StartProfiling(NULL)));
        return JS_TRUE;
    }

    RequiredStringArg profileName(cx, argc, vp, 0, "startProfiling");
    if (!profileName)
        return JS_FALSE;
    JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(JS_StartProfiling(profileName.mBytes)));
    return JS_TRUE;
}

static JSBool
StopProfiling(JSContext *cx, unsigned argc, jsval *vp)
{
    if (argc == 0) {
        JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(JS_StopProfiling(NULL)));
        return JS_TRUE;
    }

    RequiredStringArg profileName(cx, argc, vp, 0, "stopProfiling");
    if (!profileName)
        return JS_FALSE;
    JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(JS_StopProfiling(profileName.mBytes)));
    return JS_TRUE;
}

static JSBool
PauseProfilers(JSContext *cx, unsigned argc, jsval *vp)
{
    if (argc == 0) {
        JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(JS_PauseProfilers(NULL)));
        return JS_TRUE;
    }

    RequiredStringArg profileName(cx, argc, vp, 0, "pauseProfiling");
    if (!profileName)
        return JS_FALSE;
    JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(JS_PauseProfilers(profileName.mBytes)));
    return JS_TRUE;
}

static JSBool
ResumeProfilers(JSContext *cx, unsigned argc, jsval *vp)
{
    if (argc == 0) {
        JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(JS_ResumeProfilers(NULL)));
        return JS_TRUE;
    }

    RequiredStringArg profileName(cx, argc, vp, 0, "resumeProfiling");
    if (!profileName)
        return JS_FALSE;
    JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(JS_ResumeProfilers(profileName.mBytes)));
    return JS_TRUE;
}

/* Usage: DumpProfile([filename[, profileName]]) */
static JSBool
DumpProfile(JSContext *cx, unsigned argc, jsval *vp)
{
    bool ret;
    if (argc == 0) {
        ret = JS_DumpProfile(NULL, NULL);
    } else {
        RequiredStringArg filename(cx, argc, vp, 0, "dumpProfile");
        if (!filename)
            return JS_FALSE;

        if (argc == 1) {
            ret = JS_DumpProfile(filename.mBytes, NULL);
        } else {
            RequiredStringArg profileName(cx, argc, vp, 1, "dumpProfile");
            if (!profileName)
                return JS_FALSE;

            ret = JS_DumpProfile(filename.mBytes, profileName.mBytes);
        }
    }

    JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(ret));
    return true;
}

#ifdef MOZ_SHARK

static JSBool
IgnoreAndReturnTrue(JSContext *cx, unsigned argc, jsval *vp)
{
    JS_SET_RVAL(cx, vp, JSVAL_TRUE);
    return true;
}

#endif

#ifdef MOZ_CALLGRIND
static JSBool
StartCallgrind(JSContext *cx, unsigned argc, jsval *vp)
{
    JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(js_StartCallgrind()));
    return JS_TRUE;
}

static JSBool
StopCallgrind(JSContext *cx, unsigned argc, jsval *vp)
{
    JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(js_StopCallgrind()));
    return JS_TRUE;
}

static JSBool
DumpCallgrind(JSContext *cx, unsigned argc, jsval *vp)
{
    if (argc == 0) {
        JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(js_DumpCallgrind(NULL)));
        return JS_TRUE;
    }

    RequiredStringArg outFile(cx, argc, vp, 0, "dumpCallgrind");
    if (!outFile)
        return JS_FALSE;

    JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(js_DumpCallgrind(outFile.mBytes)));
    return JS_TRUE;
}
#endif

#ifdef MOZ_VTUNE
static JSBool
StartVtune(JSContext *cx, unsigned argc, jsval *vp)
{
    RequiredStringArg profileName(cx, argc, vp, 0, "startVtune");
    if (!profileName)
        return JS_FALSE;
    JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(js_StartVtune(profileName.mBytes)));
    return JS_TRUE;
}

static JSBool
StopVtune(JSContext *cx, unsigned argc, jsval *vp)
{
    JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(js_StopVtune()));
    return JS_TRUE;
}

static JSBool
PauseVtune(JSContext *cx, unsigned argc, jsval *vp)
{
    JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(js_PauseVtune()));
    return JS_TRUE;
}

static JSBool
ResumeVtune(JSContext *cx, unsigned argc, jsval *vp)
{
    JS_SET_RVAL(cx, vp, BOOLEAN_TO_JSVAL(js_ResumeVtune()));
    return JS_TRUE;
}
#endif

static JSFunctionSpec profiling_functions[] = {
    JS_FN("startProfiling",  StartProfiling,      1,0),
    JS_FN("stopProfiling",   StopProfiling,       1,0),
    JS_FN("pauseProfilers",  PauseProfilers,      1,0),
    JS_FN("resumeProfilers", ResumeProfilers,     1,0),
    JS_FN("dumpProfile",     DumpProfile,         2,0),
#ifdef MOZ_SHARK
    /* Keep users of the old shark API happy. */
    JS_FN("connectShark",    IgnoreAndReturnTrue, 0,0),
    JS_FN("disconnectShark", IgnoreAndReturnTrue, 0,0),
    JS_FN("startShark",      StartProfiling,      0,0),
    JS_FN("stopShark",       StopProfiling,       0,0),
#endif
#ifdef MOZ_CALLGRIND
    JS_FN("startCallgrind", StartCallgrind,       0,0),
    JS_FN("stopCallgrind",  StopCallgrind,        0,0),
    JS_FN("dumpCallgrind",  DumpCallgrind,        1,0),
#endif
#ifdef MOZ_VTUNE
    JS_FN("startVtune",     js_StartVtune,        1,0),
    JS_FN("stopVtune",      js_StopVtune,         0,0),
    JS_FN("pauseVtune",     js_PauseVtune,        0,0),
    JS_FN("resumeVtune",    js_ResumeVtune,       0,0),
#endif
    JS_FS_END
};

#endif

JS_PUBLIC_API(JSBool)
JS_DefineProfilingFunctions(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);

    assertSameCompartment(cx, obj);
#ifdef MOZ_PROFILING
    return JS_DefineFunctions(cx, obj, profiling_functions);
#else
    return true;
#endif
}

#ifdef MOZ_CALLGRIND

#include <valgrind/callgrind.h>

JS_FRIEND_API(JSBool)
js_StartCallgrind()
{
    JS_SILENCE_UNUSED_VALUE_IN_EXPR(CALLGRIND_START_INSTRUMENTATION);
    JS_SILENCE_UNUSED_VALUE_IN_EXPR(CALLGRIND_ZERO_STATS);
    return true;
}

JS_FRIEND_API(JSBool)
js_StopCallgrind()
{
    JS_SILENCE_UNUSED_VALUE_IN_EXPR(CALLGRIND_STOP_INSTRUMENTATION);
    return true;
}

JS_FRIEND_API(JSBool)
js_DumpCallgrind(const char *outfile)
{
    if (outfile) {
        JS_SILENCE_UNUSED_VALUE_IN_EXPR(CALLGRIND_DUMP_STATS_AT(outfile));
    } else {
        JS_SILENCE_UNUSED_VALUE_IN_EXPR(CALLGRIND_DUMP_STATS);
    }

    return true;
}

#endif /* MOZ_CALLGRIND */

#ifdef MOZ_VTUNE
#include <VTuneApi.h>

static const char *vtuneErrorMessages[] = {
  "unknown, error #0",
  "invalid 'max samples' field",
  "invalid 'samples per buffer' field",
  "invalid 'sample interval' field",
  "invalid path",
  "sample file in use",
  "invalid 'number of events' field",
  "unknown, error #7",
  "internal error",
  "bad event name",
  "VTStopSampling called without calling VTStartSampling",
  "no events selected for event-based sampling",
  "events selected cannot be run together",
  "no sampling parameters",
  "sample database already exists",
  "sampling already started",
  "time-based sampling not supported",
  "invalid 'sampling parameters size' field",
  "invalid 'event size' field",
  "sampling file already bound",
  "invalid event path",
  "invalid license",
  "invalid 'global options' field",

};

bool
js_StartVtune(const char *profileName)
{
    VTUNE_EVENT events[] = {
        { 1000000, 0, 0, 0, "CPU_CLK_UNHALTED.CORE" },
        { 1000000, 0, 0, 0, "INST_RETIRED.ANY" },
    };

    U32 n_events = sizeof(events) / sizeof(VTUNE_EVENT);
    char *default_filename = "mozilla-vtune.tb5";
    JSString *str;
    U32 status;

    VTUNE_SAMPLING_PARAMS params = {
        sizeof(VTUNE_SAMPLING_PARAMS),
        sizeof(VTUNE_EVENT),
        0, 0, /* Reserved fields */
        1,    /* Initialize in "paused" state */
        0,    /* Max samples, or 0 for "continuous" */
        4096, /* Samples per buffer */
        0.1,  /* Sampling interval in ms */
        1,    /* 1 for event-based sampling, 0 for time-based */

        n_events,
        events,
        default_filename,
    };

    if (profileName) {
        char filename[strlen(profileName) + strlen("-vtune.tb5") + 1];
        snprintf(filename, sizeof(filename), "%s-vtune.tb5", profileName);
        params.tb5Filename = filename;
    }

    status = VTStartSampling(&params);

    if (params.tb5Filename != default_filename)
        Foreground::free_(params.tb5Filename);

    if (status != 0) {
        if (status == VTAPI_MULTIPLE_RUNS)
            VTStopSampling(0);
        if (status < sizeof(vtuneErrorMessages))
            UnsafeError("Vtune setup error: %s", vtuneErrorMessages[status]);
        else
            UnsafeError("Vtune setup error: %d", status);
        return false;
    }
    return true;
}

bool
js_StopVtune()
{
    U32 status = VTStopSampling(1);
    if (status) {
        if (status < sizeof(vtuneErrorMessages))
            UnsafeError("Vtune shutdown error: %s", vtuneErrorMessages[status]);
        else
            UnsafeError("Vtune shutdown error: %d", status);
        return false;
    }
    return true;
}

bool
js_PauseVtune()
{
    VTPause();
    return true;
}

bool
js_ResumeVtune()
{
    VTResume();
    return true;
}

#endif /* MOZ_VTUNE */

#ifdef __linux__

/*
 * Code for starting and stopping |perf|, the Linux profiler.
 *
 * Output from profiling is written to mozperf.data in your cwd.
 *
 * To enable, set MOZ_PROFILE_WITH_PERF=1 in your environment.
 *
 * To pass additional parameters to |perf record|, provide them in the
 * MOZ_PROFILE_PERF_FLAGS environment variable.  If this variable does not
 * exist, we default it to "--call-graph".  (If you don't want --call-graph but
 * don't want to pass any other args, define MOZ_PROFILE_PERF_FLAGS to the empty
 * string.)
 *
 * If you include --pid or --output in MOZ_PROFILE_PERF_FLAGS, you're just
 * asking for trouble.
 *
 * Our split-on-spaces logic is lame, so don't expect MOZ_PROFILE_PERF_FLAGS to
 * work if you pass an argument which includes a space (e.g.
 * MOZ_PROFILE_PERF_FLAGS="-e 'foo bar'").
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

static bool perfInitialized = false;
static pid_t perfPid = 0;

JSBool js_StartPerf()
{
    const char *outfile = "mozperf.data";

    if (perfPid != 0) {
        UnsafeError("js_StartPerf: called while perf was already running!\n");
        return false;
    }

    // Bail if MOZ_PROFILE_WITH_PERF is empty or undefined.
    if (!getenv("MOZ_PROFILE_WITH_PERF") ||
        !strlen(getenv("MOZ_PROFILE_WITH_PERF"))) {
        return true;
    }

    /*
     * Delete mozperf.data the first time through -- we're going to append to it
     * later on, so we want it to be clean when we start out.
     */
    if (!perfInitialized) {
        perfInitialized = true;
        unlink(outfile);
        char cwd[4096];
        printf("Writing perf profiling data to %s/%s\n",
               getcwd(cwd, sizeof(cwd)), outfile);
    }

    pid_t mainPid = getpid();

    pid_t childPid = fork();
    if (childPid == 0) {
        /* perf record --append --pid $mainPID --output=$outfile $MOZ_PROFILE_PERF_FLAGS */

        char mainPidStr[16];
        snprintf(mainPidStr, sizeof(mainPidStr), "%d", mainPid);
        const char *defaultArgs[] = {"perf", "record", "--append",
                                     "--pid", mainPidStr, "--output", outfile};

        Vector<const char*, 0, SystemAllocPolicy> args;
        args.append(defaultArgs, ArrayLength(defaultArgs));

        const char *flags = getenv("MOZ_PROFILE_PERF_FLAGS");
        if (!flags) {
            flags = "--call-graph";
        }

        // Split |flags| on spaces.  (Don't bother to free it -- we're going to
        // exec anyway.)
        char *toksave;
        char *tok = strtok_r(strdup(flags), " ", &toksave);
        while (tok) {
            args.append(tok);
            tok = strtok_r(NULL, " ", &toksave);
        }

        args.append((char*) NULL);

        execvp("perf", const_cast<char**>(args.begin()));

        /* Reached only if execlp fails. */
        fprintf(stderr, "Unable to start perf.\n");
        exit(1);
    }
    else if (childPid > 0) {
        perfPid = childPid;

        /* Give perf a chance to warm up. */
        usleep(500 * 1000);
        return true;
    }
    else {
        UnsafeError("js_StartPerf: fork() failed\n");
        return false;
    }
}

JSBool js_StopPerf()
{
    if (perfPid == 0) {
        UnsafeError("js_StopPerf: perf is not running.\n");
        return true;
    }

    if (kill(perfPid, SIGINT)) {
        UnsafeError("js_StopPerf: kill failed\n");

        // Try to reap the process anyway.
        waitpid(perfPid, NULL, WNOHANG);
    }
    else {
        waitpid(perfPid, NULL, 0);
    }

    perfPid = 0;
    return true;
}

#endif /* __linux__ */

JS_PUBLIC_API(void)
JS_DumpBytecode(JSContext *cx, JSScript *scriptArg)
{
#if defined(DEBUG)
    Rooted<JSScript*> script(cx, scriptArg);

    Sprinter sprinter(cx);
    if (!sprinter.init())
        return;

    fprintf(stdout, "--- SCRIPT %s:%d ---\n", script->filename, script->lineno);
    js_Disassemble(cx, script, true, &sprinter);
    fputs(sprinter.string(), stdout);
    fprintf(stdout, "--- END SCRIPT %s:%d ---\n", script->filename, script->lineno);
#endif
}

extern JS_PUBLIC_API(void)
JS_DumpPCCounts(JSContext *cx, JSScript *scriptArg)
{
#if defined(DEBUG)
    Rooted<JSScript*> script(cx, scriptArg);
    JS_ASSERT(script->hasScriptCounts);

    Sprinter sprinter(cx);
    if (!sprinter.init())
        return;

    fprintf(stdout, "--- SCRIPT %s:%d ---\n", script->filename, script->lineno);
    js_DumpPCCounts(cx, script, &sprinter);
    fputs(sprinter.string(), stdout);
    fprintf(stdout, "--- END SCRIPT %s:%d ---\n", script->filename, script->lineno);
#endif
}

namespace {

typedef Vector<JSScript *, 0, SystemAllocPolicy> ScriptsToDump;

static void
DumpBytecodeScriptCallback(JSRuntime *rt, void *data, void *thing,
                           JSGCTraceKind traceKind, size_t thingSize)
{
    JS_ASSERT(traceKind == JSTRACE_SCRIPT);
    JSScript *script = static_cast<JSScript *>(thing);
    static_cast<ScriptsToDump *>(data)->append(script);
}

} /* anonymous namespace */

JS_PUBLIC_API(void)
JS_DumpCompartmentBytecode(JSContext *cx)
{
    ScriptsToDump scripts;
    IterateCells(cx->runtime, cx->compartment, gc::FINALIZE_SCRIPT, &scripts, DumpBytecodeScriptCallback);

    for (size_t i = 0; i < scripts.length(); i++) {
        if (scripts[i]->enclosingScriptsCompiledSuccessfully())
            JS_DumpBytecode(cx, scripts[i]);
    }
}

JS_PUBLIC_API(void)
JS_DumpCompartmentPCCounts(JSContext *cx)
{
    for (CellIter i(cx->compartment, gc::FINALIZE_SCRIPT); !i.done(); i.next()) {
        JSScript *script = i.get<JSScript>();
        if (script->hasScriptCounts && script->enclosingScriptsCompiledSuccessfully())
            JS_DumpPCCounts(cx, script);
    }
}

JS_PUBLIC_API(JSObject *)
JS_UnwrapObject(JSObject *obj)
{
    return UnwrapObject(obj);
}

JS_PUBLIC_API(JSObject *)
JS_UnwrapObjectAndInnerize(JSObject *obj)
{
    return UnwrapObject(obj, /* stopAtOuter = */ false);
}

JS_FRIEND_API(JSBool)
js_CallContextDebugHandler(JSContext *cx)
{
    ScriptFrameIter iter(cx);
    JS_ASSERT(!iter.done());

    jsval rval;
    switch (js::CallContextDebugHandler(cx, iter.script(), iter.pc(), &rval)) {
      case JSTRAP_ERROR:
        JS_ClearPendingException(cx);
        return JS_FALSE;
      case JSTRAP_THROW:
        JS_SetPendingException(cx, rval);
        return JS_FALSE;
      case JSTRAP_RETURN:
      case JSTRAP_CONTINUE:
      default:
        return JS_TRUE;
    }
}

