/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JavaScript bytecode interpreter.
 */

#include "mozilla/FloatingPoint.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "jstypes.h"
#include "jsutil.h"
#include "jsprf.h"
#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jsbool.h"
#include "jscntxt.h"
#include "jsdate.h"
#include "jsversion.h"
#include "jsdbgapi.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinterp.h"
#include "jsiter.h"
#include "jslibmath.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jspropertycache.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstr.h"

#include "builtin/Eval.h"
#include "gc/Marking.h"
#include "vm/Debugger.h"

#ifdef JS_METHODJIT
#include "methodjit/MethodJIT.h"
#include "methodjit/Logging.h"
#endif

#include "jsatominlines.h"
#include "jsinferinlines.h"
#include "jsinterpinlines.h"
#include "jsobjinlines.h"
#include "jsopcodeinlines.h"
#include "jsprobes.h"
#include "jspropertycacheinlines.h"
#include "jsscopeinlines.h"
#include "jsscriptinlines.h"
#include "jstypedarrayinlines.h"

#include "builtin/Iterator-inl.h"
#include "vm/Stack-inl.h"
#include "vm/String-inl.h"

#if JS_HAS_XML_SUPPORT
#include "jsxml.h"
#endif

#include "jsautooplen.h"

#if defined(JS_METHODJIT) && defined(JS_MONOIC)
#include "methodjit/MonoIC.h"
#endif

using namespace js;
using namespace js::gc;
using namespace js::types;

/* Some objects (e.g., With) delegate 'this' to another object. */
static inline JSObject *
CallThisObjectHook(JSContext *cx, HandleObject obj, Value *argv)
{
    JSObject *thisp = JSObject::thisObject(cx, obj);
    if (!thisp)
        return NULL;
    argv[-1].setObject(*thisp);
    return thisp;
}

/*
 * ECMA requires "the global object", but in embeddings such as the browser,
 * which have multiple top-level objects (windows, frames, etc. in the DOM),
 * we prefer fun's parent.  An example that causes this code to run:
 *
 *   // in window w1
 *   function f() { return this }
 *   function g() { return f }
 *
 *   // in window w2
 *   var h = w1.g()
 *   alert(h() == w1)
 *
 * The alert should display "true".
 */
bool
js::BoxNonStrictThis(JSContext *cx, const CallReceiver &call)
{
    /*
     * Check for SynthesizeFrame poisoning and fast constructors which
     * didn't check their callee properly.
     */
    Value thisv = call.thisv();
    JS_ASSERT(!thisv.isMagic());

#ifdef DEBUG
    JSFunction *fun = call.callee().isFunction() ? call.callee().toFunction() : NULL;
    JS_ASSERT_IF(fun && fun->isInterpreted(), !fun->inStrictMode());
#endif

    if (thisv.isNullOrUndefined()) {
        Rooted<GlobalObject*> global(cx, &call.callee().global());
        JSObject *thisp = JSObject::thisObject(cx, global);
        if (!thisp)
            return false;
        call.setThis(ObjectValue(*thisp));
        return true;
    }

    if (!thisv.isObject()) {
        if (!js_PrimitiveToObject(cx, &thisv))
            return false;
        call.setThis(thisv);
    }

    return true;
}

#if JS_HAS_NO_SUCH_METHOD

const uint32_t JSSLOT_FOUND_FUNCTION  = 0;
const uint32_t JSSLOT_SAVED_ID        = 1;

Class js_NoSuchMethodClass = {
    "NoSuchMethod",
    JSCLASS_HAS_RESERVED_SLOTS(2) | JSCLASS_IS_ANONYMOUS,
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub,
};

/*
 * When JSOP_CALLPROP or JSOP_CALLELEM does not find the method property of
 * the base object, we search for the __noSuchMethod__ method in the base.
 * If it exists, we store the method and the property's id into an object of
 * NoSuchMethod class and store this object into the callee's stack slot.
 * Later, Invoke will recognise such an object and transfer control to
 * NoSuchMethod that invokes the method like:
 *
 *   this.__noSuchMethod__(id, args)
 *
 * where id is the name of the method that this invocation attempted to
 * call by name, and args is an Array containing this invocation's actual
 * parameters.
 */
bool
js::OnUnknownMethod(JSContext *cx, HandleObject obj, Value idval_, MutableHandleValue vp)
{
    RootedValue idval(cx, idval_);

    RootedId id(cx, NameToId(cx->runtime->atomState.noSuchMethodAtom));
    RootedValue value(cx);
    if (!GetMethod(cx, obj, id, 0, &value))
        return false;
    TypeScript::MonitorUnknown(cx, cx->fp()->script(), cx->regs().pc);

    if (value.get().isPrimitive()) {
        vp.set(value);
    } else {
#if JS_HAS_XML_SUPPORT
        /* Extract the function name from function::name qname. */
        if (idval.get().isObject()) {
            JSObject *obj = &idval.get().toObject();
            if (js_GetLocalNameFromFunctionQName(obj, id.address(), cx))
                idval = IdToValue(id);
        }
#endif

        JSObject *obj = NewObjectWithClassProto(cx, &js_NoSuchMethodClass, NULL, NULL);
        if (!obj)
            return false;

        obj->setSlot(JSSLOT_FOUND_FUNCTION, value);
        obj->setSlot(JSSLOT_SAVED_ID, idval);
        vp.setObject(*obj);
    }
    return true;
}

static JSBool
NoSuchMethod(JSContext *cx, unsigned argc, Value *vp)
{
    InvokeArgsGuard args;
    if (!cx->stack.pushInvokeArgs(cx, 2, &args))
        return JS_FALSE;

    JS_ASSERT(vp[0].isObject());
    JS_ASSERT(vp[1].isObject());
    JSObject *obj = &vp[0].toObject();
    JS_ASSERT(obj->getClass() == &js_NoSuchMethodClass);

    args.setCallee(obj->getSlot(JSSLOT_FOUND_FUNCTION));
    args.setThis(vp[1]);
    args[0] = obj->getSlot(JSSLOT_SAVED_ID);
    JSObject *argsobj = NewDenseCopiedArray(cx, argc, vp + 2);
    if (!argsobj)
        return JS_FALSE;
    args[1].setObject(*argsobj);
    JSBool ok = Invoke(cx, args);
    vp[0] = args.rval();
    return ok;
}

#endif /* JS_HAS_NO_SUCH_METHOD */

bool
js::ReportIsNotFunction(JSContext *cx, const Value &v, MaybeConstruct construct)
{
    unsigned error = construct ? JSMSG_NOT_CONSTRUCTOR : JSMSG_NOT_FUNCTION;

    RootedValue val(cx, v);
    js_ReportValueError3(cx, error, JSDVG_SEARCH_STACK, val, NullPtr(), NULL, NULL);
    return false;
}

bool
js::ReportIsNotFunction(JSContext *cx, const Value *vp, MaybeConstruct construct)
{
    ptrdiff_t spIndex = cx->stack.spIndexOf(vp);
    unsigned error = construct ? JSMSG_NOT_CONSTRUCTOR : JSMSG_NOT_FUNCTION;

    RootedValue val(cx, *vp);
    js_ReportValueError3(cx, error, spIndex, val, NullPtr(), NULL, NULL);
    return false;
}

JSObject *
js::ValueToCallable(JSContext *cx, const Value *vp, MaybeConstruct construct)
{
    if (vp->isObject()) {
        JSObject *callable = &vp->toObject();
        if (callable->isCallable())
            return callable;
    }

    ReportIsNotFunction(cx, vp, construct);
    return NULL;
}

bool
js::RunScript(JSContext *cx, JSScript *script, StackFrame *fp)
{
    JS_ASSERT(script);
    JS_ASSERT(fp == cx->fp());
    JS_ASSERT(fp->script() == script);
    JS_ASSERT_IF(!fp->isGeneratorFrame(), cx->regs().pc == script->code);
    JS_ASSERT_IF(fp->isEvalFrame(), script->isActiveEval);
#ifdef JS_METHODJIT_SPEW
    JMCheckLogging();
#endif

    JS_CHECK_RECURSION(cx, return false);

    /* FIXME: Once bug 470510 is fixed, make this an assert. */
    if (script->compileAndGo) {
        if (fp->global().isCleared()) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CLEARED_SCOPE);
            return false;
        }
    }

#ifdef DEBUG
    struct CheckStackBalance {
        JSContext *cx;
        StackFrame *fp;
        RootedObject enumerators;
        CheckStackBalance(JSContext *cx)
          : cx(cx), fp(cx->fp()), enumerators(cx, cx->enumerators)
        {}
        ~CheckStackBalance() {
            JS_ASSERT(fp == cx->fp());
            JS_ASSERT_IF(!fp->isGeneratorFrame(), enumerators == cx->enumerators);
        }
    } check(cx);
#endif

    SPSEntryMarker marker(cx->runtime);

#ifdef JS_METHODJIT
    mjit::CompileStatus status;
    status = mjit::CanMethodJIT(cx, script, script->code, fp->isConstructing(),
                                mjit::CompileRequest_Interpreter, fp);
    if (status == mjit::Compile_Error)
        return false;

    if (status == mjit::Compile_Okay)
        return mjit::JaegerStatusToSuccess(mjit::JaegerShot(cx, false));
#endif

    return Interpret(cx, fp);
}

/*
 * Find a function reference and its 'this' value implicit first parameter
 * under argc arguments on cx's stack, and call the function.  Push missing
 * required arguments, allocate declared local variables, and pop everything
 * when done.  Then push the return value.
 */
bool
js::InvokeKernel(JSContext *cx, CallArgs args, MaybeConstruct construct)
{
    JS_ASSERT(args.length() <= StackSpace::ARGS_LENGTH_MAX);
    JS_ASSERT(!cx->compartment->activeAnalysis);

    /* We should never enter a new script while cx->iterValue is live. */
    JS_ASSERT(cx->iterValue.isMagic(JS_NO_ITER_VALUE));

    /* MaybeConstruct is a subset of InitialFrameFlags */
    InitialFrameFlags initial = (InitialFrameFlags) construct;

    if (args.calleev().isPrimitive())
        return ReportIsNotFunction(cx, args.calleev().address(), construct);

    JSObject &callee = args.callee();
    Class *clasp = callee.getClass();

    /* Invoke non-functions. */
    if (JS_UNLIKELY(clasp != &FunctionClass)) {
#if JS_HAS_NO_SUCH_METHOD
        if (JS_UNLIKELY(clasp == &js_NoSuchMethodClass))
            return NoSuchMethod(cx, args.length(), args.base());
#endif
        JS_ASSERT_IF(construct, !clasp->construct);
        if (!clasp->call)
            return ReportIsNotFunction(cx, args.calleev().address(), construct);
        return CallJSNative(cx, clasp->call, args);
    }

    /* Invoke native functions. */
    RootedFunction fun(cx, callee.toFunction());
    JS_ASSERT_IF(construct, !fun->isNativeConstructor());
    if (fun->isNative())
        return CallJSNative(cx, fun->native(), args);

    if (!TypeMonitorCall(cx, args, construct))
        return false;

    /* Get pointer to new frame/slots, prepare arguments. */
    InvokeFrameGuard ifg;
    if (!cx->stack.pushInvokeFrame(cx, args, initial, &ifg))
        return false;

    /* Run function until JSOP_STOP, JSOP_RETURN or error. */
    JSBool ok = RunScript(cx, fun->script(), ifg.fp());

    /* Propagate the return value out. */
    args.rval().set(ifg.fp()->returnValue());
    JS_ASSERT_IF(ok && construct, !args.rval().isPrimitive());
    return ok;
}

bool
js::Invoke(JSContext *cx, const Value &thisv, const Value &fval, unsigned argc, Value *argv,
           Value *rval)
{
    InvokeArgsGuard args;
    if (!cx->stack.pushInvokeArgs(cx, argc, &args))
        return false;

    args.setCallee(fval);
    args.setThis(thisv);
    PodCopy(args.array(), argv, argc);

    if (args.thisv().isObject()) {
        /*
         * We must call the thisObject hook in case we are not called from the
         * interpreter, where a prior bytecode has computed an appropriate
         * |this| already.
         */
        RootedObject thisObj(cx, &args.thisv().toObject());
        JSObject *thisp = JSObject::thisObject(cx, thisObj);
        if (!thisp)
             return false;
        args.setThis(ObjectValue(*thisp));
    }

    if (!Invoke(cx, args))
        return false;

    *rval = args.rval();
    return true;
}

bool
js::InvokeConstructorKernel(JSContext *cx, CallArgs args)
{
    JS_ASSERT(!FunctionClass.construct);

    args.setThis(MagicValue(JS_IS_CONSTRUCTING));

    if (!args.calleev().isObject())
        return ReportIsNotFunction(cx, args.calleev().address(), CONSTRUCT);

    JSObject &callee = args.callee();
    if (callee.isFunction()) {
        JSFunction *fun = callee.toFunction();

        if (fun->isNativeConstructor()) {
            Probes::calloutBegin(cx, fun);
            bool ok = CallJSNativeConstructor(cx, fun->native(), args);
            Probes::calloutEnd(cx, fun);
            return ok;
        }

        if (!fun->isInterpretedConstructor())
            return ReportIsNotFunction(cx, args.calleev().address(), CONSTRUCT);

        if (!InvokeKernel(cx, args, CONSTRUCT))
            return false;

        JS_ASSERT(args.rval().isObject());
        return true;
    }

    Class *clasp = callee.getClass();
    if (!clasp->construct)
        return ReportIsNotFunction(cx, args.calleev().address(), CONSTRUCT);

    return CallJSNativeConstructor(cx, clasp->construct, args);
}

bool
js::InvokeConstructor(JSContext *cx, const Value &fval, unsigned argc, Value *argv, Value *rval)
{
    InvokeArgsGuard args;
    if (!cx->stack.pushInvokeArgs(cx, argc, &args))
        return false;

    args.setCallee(fval);
    args.setThis(MagicValue(JS_THIS_POISON));
    PodCopy(args.array(), argv, argc);

    if (!InvokeConstructor(cx, args))
        return false;

    *rval = args.rval();
    return true;
}

bool
js::InvokeGetterOrSetter(JSContext *cx, JSObject *obj, const Value &fval, unsigned argc, Value *argv,
                         Value *rval)
{
    /*
     * Invoke could result in another try to get or set the same id again, see
     * bug 355497.
     */
    JS_CHECK_RECURSION(cx, return false);

    return Invoke(cx, ObjectValue(*obj), fval, argc, argv, rval);
}

bool
js::ExecuteKernel(JSContext *cx, HandleScript script, JSObject &scopeChain, const Value &thisv,
                  ExecuteType type, StackFrame *evalInFrame, Value *result)
{
    JS_ASSERT_IF(evalInFrame, type == EXECUTE_DEBUG);
    JS_ASSERT_IF(type == EXECUTE_GLOBAL, !scopeChain.isScope());

    if (script->isEmpty()) {
        if (result)
            result->setUndefined();
        return true;
    }

    ExecuteFrameGuard efg;
    if (!cx->stack.pushExecuteFrame(cx, script, thisv, scopeChain, type, evalInFrame, &efg))
        return false;

    if (!script->ensureRanAnalysis(cx))
        return false;
    TypeScript::SetThis(cx, script, efg.fp()->thisValue());

    Probes::startExecution(cx, script);
    bool ok = RunScript(cx, script, efg.fp());
    Probes::stopExecution(cx, script);

    /* Propgate the return value out. */
    if (result)
        *result = efg.fp()->returnValue();
    return ok;
}

bool
js::Execute(JSContext *cx, HandleScript script, JSObject &scopeChainArg, Value *rval)
{
    /* The scope chain could be anything, so innerize just in case. */
    RootedObject scopeChain(cx, &scopeChainArg);
    scopeChain = GetInnerObject(cx, scopeChain);
    if (!scopeChain)
        return false;

    /* If we were handed a non-native object, complain bitterly. */
    if (!scopeChain->isNative()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NON_NATIVE_SCOPE);
        return false;
    }
    JS_ASSERT(!scopeChain->getOps()->defineProperty);

    /* The VAROBJFIX option makes varObj == globalObj in global code. */
    if (!cx->hasRunOption(JSOPTION_VAROBJFIX)) {
        if (!scopeChain->setVarObj(cx))
            return false;
    }

    /* Use the scope chain as 'this', modulo outerization. */
    JSObject *thisObj = JSObject::thisObject(cx, scopeChain);
    if (!thisObj)
        return false;
    Value thisv = ObjectValue(*thisObj);

    return ExecuteKernel(cx, script, *scopeChain, thisv, EXECUTE_GLOBAL,
                         NULL /* evalInFrame */, rval);
}

JSBool
js::HasInstance(JSContext *cx, HandleObject obj, const Value *v, JSBool *bp)
{
    Class *clasp = obj->getClass();
    if (clasp->hasInstance)
        return clasp->hasInstance(cx, obj, v, bp);

    RootedValue val(cx, ObjectValue(*obj));
    js_ReportValueError(cx, JSMSG_BAD_INSTANCEOF_RHS,
                        JSDVG_SEARCH_STACK, val, NullPtr());
    return JS_FALSE;
}

bool
js::LooselyEqual(JSContext *cx, const Value &lval, const Value &rval, bool *result)
{
#if JS_HAS_XML_SUPPORT
    if (JS_UNLIKELY(lval.isObject() && lval.toObject().isXML()) ||
                    (rval.isObject() && rval.toObject().isXML())) {
        JSBool res;
        if (!js_TestXMLEquality(cx, lval, rval, &res))
            return false;
        *result = !!res;
        return true;
    }
#endif

    if (SameType(lval, rval)) {
        if (lval.isString()) {
            JSString *l = lval.toString();
            JSString *r = rval.toString();
            return EqualStrings(cx, l, r, result);
        }

        if (lval.isDouble()) {
            double l = lval.toDouble(), r = rval.toDouble();
            *result = (l == r);
            return true;
        }

        if (lval.isObject()) {
            JSObject *l = &lval.toObject();
            JSObject *r = &rval.toObject();

            if (JSEqualityOp eq = l->getClass()->ext.equality) {
                JSBool res;
                Rooted<JSObject*> lobj(cx, l);
                if (!eq(cx, lobj, &rval, &res))
                    return false;
                *result = !!res;
                return true;
            }

            *result = l == r;
            return true;
        }

        *result = lval.payloadAsRawUint32() == rval.payloadAsRawUint32();
        return true;
    }

    if (lval.isNullOrUndefined()) {
        *result = rval.isNullOrUndefined();
        return true;
    }

    if (rval.isNullOrUndefined()) {
        *result = false;
        return true;
    }

    RootedValue lvalue(cx, lval);
    RootedValue rvalue(cx, rval);

    if (!ToPrimitive(cx, lvalue.address()))
        return false;
    if (!ToPrimitive(cx, rvalue.address()))
        return false;

    if (lvalue.get().isString() && rvalue.get().isString()) {
        JSString *l = lvalue.get().toString();
        JSString *r = rvalue.get().toString();
        return EqualStrings(cx, l, r, result);
    }

    double l, r;
    if (!ToNumber(cx, lvalue, &l) || !ToNumber(cx, rvalue, &r))
        return false;
    *result = (l == r);
    return true;
}

bool
js::StrictlyEqual(JSContext *cx, const Value &lref, const Value &rref, bool *equal)
{
    Value lval = lref, rval = rref;
    if (SameType(lval, rval)) {
        if (lval.isString())
            return EqualStrings(cx, lval.toString(), rval.toString(), equal);
        if (lval.isDouble()) {
            *equal = (lval.toDouble() == rval.toDouble());
            return true;
        }
        if (lval.isObject()) {
            *equal = lval.toObject() == rval.toObject();
            return true;
        }
        if (lval.isUndefined()) {
            *equal = true;
            return true;
        }
        *equal = lval.payloadAsRawUint32() == rval.payloadAsRawUint32();
        return true;
    }

    if (lval.isDouble() && rval.isInt32()) {
        double ld = lval.toDouble();
        double rd = rval.toInt32();
        *equal = (ld == rd);
        return true;
    }
    if (lval.isInt32() && rval.isDouble()) {
        double ld = lval.toInt32();
        double rd = rval.toDouble();
        *equal = (ld == rd);
        return true;
    }

    *equal = false;
    return true;
}

static inline bool
IsNegativeZero(const Value &v)
{
    return v.isDouble() && MOZ_DOUBLE_IS_NEGATIVE_ZERO(v.toDouble());
}

static inline bool
IsNaN(const Value &v)
{
    return v.isDouble() && MOZ_DOUBLE_IS_NaN(v.toDouble());
}

bool
js::SameValue(JSContext *cx, const Value &v1, const Value &v2, bool *same)
{
    if (IsNegativeZero(v1)) {
        *same = IsNegativeZero(v2);
        return true;
    }
    if (IsNegativeZero(v2)) {
        *same = false;
        return true;
    }
    if (IsNaN(v1) && IsNaN(v2)) {
        *same = true;
        return true;
    }
    return StrictlyEqual(cx, v1, v2, same);
}

JSType
js::TypeOfValue(JSContext *cx, const Value &vref)
{
    Value v = vref;
    if (v.isNumber())
        return JSTYPE_NUMBER;
    if (v.isString())
        return JSTYPE_STRING;
    if (v.isNull())
        return JSTYPE_OBJECT;
    if (v.isUndefined())
        return JSTYPE_VOID;
    if (v.isObject()) {
        RootedObject obj(cx, &v.toObject());
        return JSObject::typeOf(cx, obj);
    }
    JS_ASSERT(v.isBoolean());
    return JSTYPE_BOOLEAN;
}

/*
 * Enter the new with scope using an object at sp[-1] and associate the depth
 * of the with block with sp + stackIndex.
 */
static bool
EnterWith(JSContext *cx, int stackIndex)
{
    StackFrame *fp = cx->fp();
    Value *sp = cx->regs().sp;
    JS_ASSERT(stackIndex < 0);
    JS_ASSERT(int(cx->regs().stackDepth()) + stackIndex >= 0);

    RootedObject obj(cx);
    if (sp[-1].isObject()) {
        obj = &sp[-1].toObject();
    } else {
        obj = js_ValueToNonNullObject(cx, sp[-1]);
        if (!obj)
            return false;
        sp[-1].setObject(*obj);
    }

    WithObject *withobj = WithObject::create(cx, obj, fp->scopeChain(),
                                             cx->regs().stackDepth() + stackIndex);
    if (!withobj)
        return false;

    fp->pushOnScopeChain(*withobj);
    return true;
}

/* Unwind block and scope chains to match the given depth. */
void
js::UnwindScope(JSContext *cx, uint32_t stackDepth)
{
    StackFrame *fp = cx->fp();
    JS_ASSERT(stackDepth <= cx->regs().stackDepth());

    for (ScopeIter si(fp, cx); !si.done(); ++si) {
        switch (si.type()) {
          case ScopeIter::Block:
            if (si.staticBlock().stackDepth() < stackDepth)
                return;
            fp->popBlock(cx);
            break;
          case ScopeIter::With:
            if (si.scope().asWith().stackDepth() < stackDepth)
                return;
            fp->popWith(cx);
            break;
          case ScopeIter::Call:
          case ScopeIter::StrictEvalScope:
            break;
        }
    }
}

void
js::UnwindForUncatchableException(JSContext *cx, const FrameRegs &regs)
{

    /* c.f. the regular (catchable) TryNoteIter loop in Interpret. */
    for (TryNoteIter tni(regs); !tni.done(); ++tni) {
        JSTryNote *tn = *tni;
        if (tn->kind == JSTRY_ITER) {
            Value *sp = regs.spForStackDepth(tn->stackDepth);
            UnwindIteratorForUncatchableException(cx, &sp[-1].toObject());
        }
    }
}

TryNoteIter::TryNoteIter(const FrameRegs &regs)
  : regs(regs),
    script(regs.fp()->script()),
    pcOffset(regs.pc - script->main())
{
    if (script->hasTrynotes()) {
        tn = script->trynotes()->vector;
        tnEnd = tn + script->trynotes()->length;
    } else {
        tn = tnEnd = NULL;
    }
    settle();
}

void
TryNoteIter::operator++()
{
    ++tn;
    settle();
}

bool
TryNoteIter::done() const
{
    return tn == tnEnd;
}

void
TryNoteIter::settle()
{
    for (; tn != tnEnd; ++tn) {
        /* If pc is out of range, try the next one. */
        if (pcOffset - tn->start >= tn->length)
            continue;

        /*
         * We have a note that covers the exception pc but we must check
         * whether the interpreter has already executed the corresponding
         * handler. This is possible when the executed bytecode implements
         * break or return from inside a for-in loop.
         *
         * In this case the emitter generates additional [enditer] and [gosub]
         * opcodes to close all outstanding iterators and execute the finally
         * blocks. If such an [enditer] throws an exception, its pc can still
         * be inside several nested for-in loops and try-finally statements
         * even if we have already closed the corresponding iterators and
         * invoked the finally blocks.
         *
         * To address this, we make [enditer] always decrease the stack even
         * when its implementation throws an exception. Thus already executed
         * [enditer] and [gosub] opcodes will have try notes with the stack
         * depth exceeding the current one and this condition is what we use to
         * filter them out.
         */
        if (tn->stackDepth <= regs.stackDepth())
            break;
    }
}

/*
 * Increment/decrement the value 'v'. The resulting value is stored in *slot.
 * The result of the expression (taking into account prefix/postfix) is stored
 * in *expr.
 */
static bool
DoIncDec(JSContext *cx, HandleScript script, jsbytecode *pc, const Value &v, Value *slot, Value *expr)
{
    const JSCodeSpec &cs = js_CodeSpec[*pc];

    if (v.isInt32()) {
        int32_t i = v.toInt32();
        if (i > JSVAL_INT_MIN && i < JSVAL_INT_MAX) {
            int32_t sum = i + (cs.format & JOF_INC ? 1 : -1);
            *slot = Int32Value(sum);
            *expr = (cs.format & JOF_POST) ? Int32Value(i) : *slot;
            return true;
        }
    }

    double d;
    if (!ToNumber(cx, v, &d))
        return false;

    double sum = d + (cs.format & JOF_INC ? 1 : -1);
    *slot = NumberValue(sum);
    *expr = (cs.format & JOF_POST) ? NumberValue(d) : *slot;

    TypeScript::MonitorOverflow(cx, script, pc);
    return true;
}

#define PUSH_COPY(v)             do { *regs.sp++ = v; assertSameCompartment(cx, regs.sp[-1]); } while (0)
#define PUSH_COPY_SKIP_CHECK(v)  *regs.sp++ = v
#define PUSH_NULL()              regs.sp++->setNull()
#define PUSH_UNDEFINED()         regs.sp++->setUndefined()
#define PUSH_BOOLEAN(b)          regs.sp++->setBoolean(b)
#define PUSH_DOUBLE(d)           regs.sp++->setDouble(d)
#define PUSH_INT32(i)            regs.sp++->setInt32(i)
#define PUSH_STRING(s)           do { regs.sp++->setString(s); assertSameCompartment(cx, regs.sp[-1]); } while (0)
#define PUSH_OBJECT(obj)         do { regs.sp++->setObject(obj); assertSameCompartment(cx, regs.sp[-1]); } while (0)
#define PUSH_OBJECT_OR_NULL(obj) do { regs.sp++->setObjectOrNull(obj); assertSameCompartment(cx, regs.sp[-1]); } while (0)
#define PUSH_HOLE()              regs.sp++->setMagic(JS_ARRAY_HOLE)
#define POP_COPY_TO(v)           v = *--regs.sp
#define POP_RETURN_VALUE()       regs.fp()->setReturnValue(*--regs.sp)

#define FETCH_OBJECT(cx, n, obj)                                              \
    JS_BEGIN_MACRO                                                            \
        HandleValue val = HandleValue::fromMarkedLocation(&regs.sp[n]);       \
        obj = ToObject(cx, (val));                                            \
        if (!obj)                                                             \
            goto error;                                                       \
    JS_END_MACRO

/*
 * Threaded interpretation via computed goto appears to be well-supported by
 * GCC 3 and higher.  IBM's C compiler when run with the right options (e.g.,
 * -qlanglvl=extended) also supports threading.  Ditto the SunPro C compiler.
 * Currently it's broken for JS_VERSION < 160, though this isn't worth fixing.
 * Add your compiler support macros here.
 */
#ifndef JS_THREADED_INTERP
# if JS_VERSION >= 160 && (                                                   \
    __GNUC__ >= 3 ||                                                          \
    (__IBMC__ >= 700 && defined __IBM_COMPUTED_GOTO) ||                       \
    __SUNPRO_C >= 0x570)
#  define JS_THREADED_INTERP 1
# else
#  define JS_THREADED_INTERP 0
# endif
#endif

template<typename T>
class GenericInterruptEnabler : public InterpreterFrames::InterruptEnablerBase {
  public:
    GenericInterruptEnabler(T *variable, T value) : variable(variable), value(value) { }
    void enable() const { *variable = value; }

  private:
    T *variable;
    T value;
};

inline InterpreterFrames::InterpreterFrames(JSContext *cx, FrameRegs *regs,
                                            const InterruptEnablerBase &enabler)
  : context(cx), regs(regs), enabler(enabler)
{
    older = cx->runtime->interpreterFrames;
    cx->runtime->interpreterFrames = this;
}

inline InterpreterFrames::~InterpreterFrames()
{
    context->runtime->interpreterFrames = older;
}

#if defined(DEBUG) && !defined(JS_THREADSAFE) && !defined(JSGC_ROOT_ANALYSIS)
void
js::AssertValidPropertyCacheHit(JSContext *cx, JSObject *start_,
                                JSObject *found, PropertyCacheEntry *entry)
{
    jsbytecode *pc;
    JSScript *script = cx->stack.currentScript(&pc);

    uint64_t sample = cx->runtime->gcNumber;
    PropertyCacheEntry savedEntry = *entry;

    RootedPropertyName name(cx, GetNameFromBytecode(cx, script, pc, JSOp(*pc)));
    RootedObject start(cx, start_);
    RootedObject pobj(cx);
    RootedShape prop(cx);
    bool ok = baseops::LookupProperty(cx, start, name, &pobj, &prop);
    JS_ASSERT(ok);

    if (cx->runtime->gcNumber != sample)
        JS_PROPERTY_CACHE(cx).restore(&savedEntry);
    JS_ASSERT(prop);
    JS_ASSERT(pobj == found);
    JS_ASSERT(entry->prop == prop);
}
#endif /* DEBUG && !JS_THREADSAFE */

/*
 * Ensure that the intrepreter switch can close call-bytecode cases in the
 * same way as non-call bytecodes.
 */
JS_STATIC_ASSERT(JSOP_NAME_LENGTH == JSOP_CALLNAME_LENGTH);
JS_STATIC_ASSERT(JSOP_GETARG_LENGTH == JSOP_CALLARG_LENGTH);
JS_STATIC_ASSERT(JSOP_GETLOCAL_LENGTH == JSOP_CALLLOCAL_LENGTH);
JS_STATIC_ASSERT(JSOP_XMLNAME_LENGTH == JSOP_CALLXMLNAME_LENGTH);

/*
 * Same for JSOP_SETNAME and JSOP_SETPROP, which differ only slightly but
 * remain distinct for the decompiler.
 */
JS_STATIC_ASSERT(JSOP_SETNAME_LENGTH == JSOP_SETPROP_LENGTH);

/* See TRY_BRANCH_AFTER_COND. */
JS_STATIC_ASSERT(JSOP_IFNE_LENGTH == JSOP_IFEQ_LENGTH);
JS_STATIC_ASSERT(JSOP_IFNE == JSOP_IFEQ + 1);

/* For the fastest case inder JSOP_INCNAME, etc. */
JS_STATIC_ASSERT(JSOP_INCNAME_LENGTH == JSOP_DECNAME_LENGTH);
JS_STATIC_ASSERT(JSOP_INCNAME_LENGTH == JSOP_NAMEINC_LENGTH);
JS_STATIC_ASSERT(JSOP_INCNAME_LENGTH == JSOP_NAMEDEC_LENGTH);

/*
 * Inline fast paths for iteration. js_IteratorMore and js_IteratorNext handle
 * all cases, but we inline the most frequently taken paths here.
 */
static inline bool
IteratorMore(JSContext *cx, JSObject *iterobj, bool *cond, MutableHandleValue rval)
{
    if (iterobj->isPropertyIterator()) {
        NativeIterator *ni = iterobj->asPropertyIterator().getNativeIterator();
        if (ni->isKeyIter()) {
            *cond = (ni->props_cursor < ni->props_end);
            return true;
        }
    }
    Rooted<JSObject*> iobj(cx, iterobj);
    if (!js_IteratorMore(cx, iobj, rval))
        return false;
    *cond = rval.isTrue();
    return true;
}

static inline bool
IteratorNext(JSContext *cx, JSObject *iterobj, MutableHandleValue rval)
{
    if (iterobj->isPropertyIterator()) {
        NativeIterator *ni = iterobj->asPropertyIterator().getNativeIterator();
        if (ni->isKeyIter()) {
            JS_ASSERT(ni->props_cursor < ni->props_end);
            rval.setString(*ni->current());
            ni->incCursor();
            return true;
        }
    }
    return js_IteratorNext(cx, iterobj, rval);
}

/*
 * For bytecodes which push values and then fall through, make sure the
 * types of the pushed values are consistent with type inference information.
 */
static inline void
TypeCheckNextBytecode(JSContext *cx, JSScript *script, unsigned n, const FrameRegs &regs)
{
#ifdef DEBUG
    if (cx->typeInferenceEnabled() &&
        n == GetBytecodeLength(regs.pc)) {
        TypeScript::CheckBytecode(cx, script, regs.pc, regs.sp);
    }
#endif
}

JS_NEVER_INLINE bool
js::Interpret(JSContext *cx, StackFrame *entryFrame, InterpMode interpMode)
{
    JSAutoResolveFlags rf(cx, RESOLVE_INFER);

    if (interpMode == JSINTERP_NORMAL)
        gc::MaybeVerifyBarriers(cx, true);

    JS_ASSERT(!cx->compartment->activeAnalysis);

#if JS_THREADED_INTERP
#define CHECK_PCCOUNT_INTERRUPTS() JS_ASSERT_IF(script->hasScriptCounts, jumpTable == interruptJumpTable)
#else
#define CHECK_PCCOUNT_INTERRUPTS() JS_ASSERT_IF(script->hasScriptCounts, switchMask == -1)
#endif

    /*
     * Macros for threaded interpreter loop
     */
#if JS_THREADED_INTERP
    static void *const normalJumpTable[] = {
# define OPDEF(op,val,name,token,length,nuses,ndefs,prec,format) \
        JS_EXTENSION &&L_##op,
# include "jsopcode.tbl"
# undef OPDEF
    };

    static void *const interruptJumpTable[] = {
# define OPDEF(op,val,name,token,length,nuses,ndefs,prec,format)              \
        JS_EXTENSION &&interrupt,
# include "jsopcode.tbl"
# undef OPDEF
    };

    register void * const *jumpTable = normalJumpTable;

    typedef GenericInterruptEnabler<void * const *> InterruptEnabler;
    InterruptEnabler interrupts(&jumpTable, interruptJumpTable);

# define DO_OP()            JS_BEGIN_MACRO                                    \
                                CHECK_PCCOUNT_INTERRUPTS();                   \
                                JS_EXTENSION_(goto *jumpTable[op]);           \
                            JS_END_MACRO
# define DO_NEXT_OP(n)      JS_BEGIN_MACRO                                    \
                                TypeCheckNextBytecode(cx, script, n, regs);   \
                                js::gc::MaybeVerifyBarriers(cx);              \
                                op = (JSOp) *(regs.pc += (n));                \
                                DO_OP();                                      \
                            JS_END_MACRO

# define BEGIN_CASE(OP)     L_##OP:
# define END_CASE(OP)       DO_NEXT_OP(OP##_LENGTH);
# define END_VARLEN_CASE    DO_NEXT_OP(len);
# define ADD_EMPTY_CASE(OP) BEGIN_CASE(OP)                                    \
                                JS_ASSERT(js_CodeSpec[OP].length == 1);       \
                                op = (JSOp) *++regs.pc;                       \
                                DO_OP();

# define END_EMPTY_CASES

#else /* !JS_THREADED_INTERP */

    register int switchMask = 0;
    int switchOp;
    typedef GenericInterruptEnabler<int> InterruptEnabler;
    InterruptEnabler interrupts(&switchMask, -1);

# define DO_OP()            goto do_op
# define DO_NEXT_OP(n)      JS_BEGIN_MACRO                                    \
                                JS_ASSERT((n) == len);                        \
                                goto advance_pc;                              \
                            JS_END_MACRO

# define BEGIN_CASE(OP)     case OP:
# define END_CASE(OP)       END_CASE_LEN(OP##_LENGTH)
# define END_CASE_LEN(n)    END_CASE_LENX(n)
# define END_CASE_LENX(n)   END_CASE_LEN##n

/*
 * To share the code for all len == 1 cases we use the specialized label with
 * code that falls through to advance_pc: .
 */
# define END_CASE_LEN1      goto advance_pc_by_one;
# define END_CASE_LEN2      len = 2; goto advance_pc;
# define END_CASE_LEN3      len = 3; goto advance_pc;
# define END_CASE_LEN4      len = 4; goto advance_pc;
# define END_CASE_LEN5      len = 5; goto advance_pc;
# define END_CASE_LEN6      len = 6; goto advance_pc;
# define END_CASE_LEN7      len = 7; goto advance_pc;
# define END_CASE_LEN8      len = 8; goto advance_pc;
# define END_CASE_LEN9      len = 9; goto advance_pc;
# define END_CASE_LEN10     len = 10; goto advance_pc;
# define END_CASE_LEN11     len = 11; goto advance_pc;
# define END_CASE_LEN12     len = 12; goto advance_pc;
# define END_VARLEN_CASE    goto advance_pc;
# define ADD_EMPTY_CASE(OP) BEGIN_CASE(OP)
# define END_EMPTY_CASES    goto advance_pc_by_one;

#endif /* !JS_THREADED_INTERP */

#define LOAD_DOUBLE(PCOFF, dbl)                                               \
    (dbl = script->getConst(GET_UINT32_INDEX(regs.pc + (PCOFF))).toDouble())

#if defined(JS_METHODJIT)
    bool useMethodJIT = false;
#endif

#ifdef JS_METHODJIT

#define RESET_USE_METHODJIT()                                                 \
    JS_BEGIN_MACRO                                                            \
        useMethodJIT = cx->methodJitEnabled &&                                \
           (interpMode == JSINTERP_NORMAL ||                                  \
            interpMode == JSINTERP_REJOIN ||                                  \
            interpMode == JSINTERP_SKIP_TRAP);                                \
    JS_END_MACRO

#define CHECK_PARTIAL_METHODJIT(status)                                       \
    JS_BEGIN_MACRO                                                            \
        switch (status) {                                                     \
          case mjit::Jaeger_UnfinishedAtTrap:                                 \
            interpMode = JSINTERP_SKIP_TRAP;                                  \
            /* FALLTHROUGH */                                                 \
          case mjit::Jaeger_Unfinished:                                       \
            op = (JSOp) *regs.pc;                                             \
            SET_SCRIPT(regs.fp()->script());                                  \
            if (cx->isExceptionPending())                                     \
                goto error;                                                   \
            DO_OP();                                                          \
          default:;                                                           \
        }                                                                     \
    JS_END_MACRO

#else

#define RESET_USE_METHODJIT() ((void) 0)

#endif

    /*
     * Prepare to call a user-supplied branch handler, and abort the script
     * if it returns false.
     */
#define CHECK_BRANCH()                                                        \
    JS_BEGIN_MACRO                                                            \
        if (cx->runtime->interrupt && !js_HandleExecutionInterrupt(cx))       \
            goto error;                                                       \
    JS_END_MACRO

#define BRANCH(n)                                                             \
    JS_BEGIN_MACRO                                                            \
        regs.pc += (n);                                                       \
        op = (JSOp) *regs.pc;                                                 \
        if ((n) <= 0)                                                         \
            goto check_backedge;                                              \
        DO_OP();                                                              \
    JS_END_MACRO

#define SET_SCRIPT(s)                                                         \
    JS_BEGIN_MACRO                                                            \
        script = (s);                                                         \
        if (script->hasAnyBreakpointsOrStepMode() || script->hasScriptCounts) \
            interrupts.enable();                                              \
        JS_ASSERT_IF(interpMode == JSINTERP_SKIP_TRAP,                        \
                     script->hasAnyBreakpointsOrStepMode());                  \
    JS_END_MACRO

    /* Repoint cx->regs to a local variable for faster access. */
    FrameRegs regs = cx->regs();
    PreserveRegsGuard interpGuard(cx, regs);

    /*
     * Help Debugger find frames running scripts that it has put in
     * single-step mode.
     */
    InterpreterFrames interpreterFrame(cx, &regs, interrupts);

    /* Copy in hot values that change infrequently. */
    JSRuntime *const rt = cx->runtime;
    Rooted<JSScript*> script(cx);
    SET_SCRIPT(regs.fp()->script());

    /*
     * Pool of rooters for use in this interpreter frame. References to these
     * are used for local variables within interpreter cases. This avoids
     * creating new rooters each time an interpreter case is entered, and also
     * correctness pitfalls due to incorrect compilation of destructor calls
     * around computed gotos.
     */
    RootedValue rootValue0(cx), rootValue1(cx);
    RootedString rootString0(cx), rootString1(cx);
    RootedObject rootObject0(cx), rootObject1(cx), rootObject2(cx);
    RootedFunction rootFunction0(cx);
    RootedTypeObject rootType0(cx);
    RootedPropertyName rootName0(cx);
    RootedId rootId0(cx);
    RootedShape rootShape0(cx);

    if (!entryFrame)
        entryFrame = regs.fp();

#if JS_HAS_GENERATORS
    if (JS_UNLIKELY(regs.fp()->isGeneratorFrame())) {
        JS_ASSERT(size_t(regs.pc - script->code) <= script->length);
        JS_ASSERT(regs.stackDepth() <= script->nslots);

        /*
         * To support generator_throw and to catch ignored exceptions,
         * fail if cx->isExceptionPending() is true.
         */
        if (cx->isExceptionPending()) {
            Probes::enterScript(cx, script, script->function(), regs.fp());
            goto error;
        }
    }
#endif

    /* State communicated between non-local jumps: */
    bool interpReturnOK;

    /* Don't call the script prologue if executing between Method and Trace JIT. */
    if (interpMode == JSINTERP_NORMAL) {
        StackFrame *fp = regs.fp();
        if (!fp->isGeneratorFrame()) {
            if (!fp->prologue(cx, UseNewTypeAtEntry(cx, fp)))
                goto error;
        } else {
            Probes::enterScript(cx, script, script->function(), fp);
        }
        if (cx->compartment->debugMode()) {
            JSTrapStatus status = ScriptDebugPrologue(cx, fp);
            switch (status) {
              case JSTRAP_CONTINUE:
                break;
              case JSTRAP_RETURN:
                interpReturnOK = true;
                goto forced_return;
              case JSTRAP_THROW:
              case JSTRAP_ERROR:
                goto error;
              default:
                JS_NOT_REACHED("bad ScriptDebugPrologue status");
            }
        }
    }

    /* The REJOIN mode acts like the normal mode, except the prologue is skipped. */
    if (interpMode == JSINTERP_REJOIN)
        interpMode = JSINTERP_NORMAL;

    RESET_USE_METHODJIT();

    /*
     * It is important that "op" be initialized before calling DO_OP because
     * it is possible for "op" to be specially assigned during the normal
     * processing of an opcode while looping. We rely on DO_NEXT_OP to manage
     * "op" correctly in all other cases.
     */
    JSOp op;
    int32_t len;
    len = 0;

    if (rt->profilingScripts || cx->runtime->debugHooks.interruptHook)
        interrupts.enable();

    DO_NEXT_OP(len);

#if JS_THREADED_INTERP
    /*
     * This is a loop, but it does not look like a loop. The loop-closing
     * jump is distributed throughout goto *jumpTable[op] inside of DO_OP.
     * When interrupts are enabled, jumpTable is set to interruptJumpTable
     * where all jumps point to the interrupt label. The latter, after
     * calling the interrupt handler, dispatches through normalJumpTable to
     * continue the normal bytecode processing.
     */

#else /* !JS_THREADED_INTERP */
    for (;;) {
      advance_pc_by_one:
        JS_ASSERT(js_CodeSpec[op].length == 1);
        len = 1;
      advance_pc:
        js::gc::MaybeVerifyBarriers(cx);
        regs.pc += len;
        op = (JSOp) *regs.pc;

      do_op:
        CHECK_PCCOUNT_INTERRUPTS();
        switchOp = int(op) | switchMask;
      do_switch:
        switch (switchOp) {
#endif

#if JS_THREADED_INTERP
  interrupt:
#else /* !JS_THREADED_INTERP */
  case -1:
    JS_ASSERT(switchMask == -1);
#endif /* !JS_THREADED_INTERP */
    {
        bool moreInterrupts = false;

        if (cx->runtime->profilingScripts) {
            if (!script->hasScriptCounts)
                script->initScriptCounts(cx);
            moreInterrupts = true;
        }

        if (script->hasScriptCounts) {
            PCCounts counts = script->getPCCounts(regs.pc);
            counts.get(PCCounts::BASE_INTERP)++;
            moreInterrupts = true;
        }

        JSInterruptHook hook = cx->runtime->debugHooks.interruptHook;
        if (hook || script->stepModeEnabled()) {
            Value rval;
            JSTrapStatus status = JSTRAP_CONTINUE;
            if (hook)
                status = hook(cx, script, regs.pc, &rval, cx->runtime->debugHooks.interruptHookData);
            if (status == JSTRAP_CONTINUE && script->stepModeEnabled())
                status = Debugger::onSingleStep(cx, &rval);
            switch (status) {
              case JSTRAP_ERROR:
                goto error;
              case JSTRAP_CONTINUE:
                break;
              case JSTRAP_RETURN:
                regs.fp()->setReturnValue(rval);
                interpReturnOK = true;
                goto forced_return;
              case JSTRAP_THROW:
                cx->setPendingException(rval);
                goto error;
              default:;
            }
            moreInterrupts = true;
        }

        if (script->hasAnyBreakpointsOrStepMode())
            moreInterrupts = true;

        if (script->hasBreakpointsAt(regs.pc) && interpMode != JSINTERP_SKIP_TRAP) {
            Value rval;
            JSTrapStatus status = Debugger::onTrap(cx, &rval);
            switch (status) {
              case JSTRAP_ERROR:
                goto error;
              case JSTRAP_RETURN:
                regs.fp()->setReturnValue(rval);
                interpReturnOK = true;
                goto forced_return;
              case JSTRAP_THROW:
                cx->setPendingException(rval);
                goto error;
              default:
                break;
            }
            JS_ASSERT(status == JSTRAP_CONTINUE);
            JS_ASSERT(rval.isInt32() && rval.toInt32() == op);
        }

        interpMode = JSINTERP_NORMAL;

#if JS_THREADED_INTERP
        jumpTable = moreInterrupts ? interruptJumpTable : normalJumpTable;
        JS_EXTENSION_(goto *normalJumpTable[op]);
#else
        switchMask = moreInterrupts ? -1 : 0;
        switchOp = int(op);
        goto do_switch;
#endif
    }

/* No-ops for ease of decompilation. */
ADD_EMPTY_CASE(JSOP_NOP)
ADD_EMPTY_CASE(JSOP_UNUSED1)
ADD_EMPTY_CASE(JSOP_UNUSED2)
ADD_EMPTY_CASE(JSOP_UNUSED3)
ADD_EMPTY_CASE(JSOP_UNUSED10)
ADD_EMPTY_CASE(JSOP_UNUSED11)
ADD_EMPTY_CASE(JSOP_UNUSED12)
ADD_EMPTY_CASE(JSOP_UNUSED13)
ADD_EMPTY_CASE(JSOP_UNUSED15)
ADD_EMPTY_CASE(JSOP_UNUSED17)
ADD_EMPTY_CASE(JSOP_UNUSED18)
ADD_EMPTY_CASE(JSOP_UNUSED19)
ADD_EMPTY_CASE(JSOP_UNUSED20)
ADD_EMPTY_CASE(JSOP_UNUSED21)
ADD_EMPTY_CASE(JSOP_UNUSED22)
ADD_EMPTY_CASE(JSOP_UNUSED23)
ADD_EMPTY_CASE(JSOP_UNUSED24)
ADD_EMPTY_CASE(JSOP_UNUSED25)
ADD_EMPTY_CASE(JSOP_UNUSED29)
ADD_EMPTY_CASE(JSOP_UNUSED30)
ADD_EMPTY_CASE(JSOP_UNUSED31)
ADD_EMPTY_CASE(JSOP_CONDSWITCH)
ADD_EMPTY_CASE(JSOP_TRY)
#if JS_HAS_XML_SUPPORT
ADD_EMPTY_CASE(JSOP_STARTXML)
ADD_EMPTY_CASE(JSOP_STARTXMLEXPR)
#endif
ADD_EMPTY_CASE(JSOP_LOOPHEAD)
ADD_EMPTY_CASE(JSOP_LOOPENTRY)
END_EMPTY_CASES

BEGIN_CASE(JSOP_LABEL)
END_CASE(JSOP_LABEL)

check_backedge:
{
    CHECK_BRANCH();
    if (op != JSOP_LOOPHEAD)
        DO_OP();

#ifdef JS_METHODJIT
    if (!useMethodJIT)
        DO_OP();
    mjit::CompileStatus status =
        mjit::CanMethodJIT(cx, script, regs.pc, regs.fp()->isConstructing(),
                           mjit::CompileRequest_Interpreter, regs.fp());
    if (status == mjit::Compile_Error)
        goto error;
    if (status == mjit::Compile_Okay) {
        void *ncode =
            script->nativeCodeForPC(regs.fp()->isConstructing(), regs.pc);
        JS_ASSERT(ncode);
        mjit::JaegerStatus status = mjit::JaegerShotAtSafePoint(cx, ncode, true);
        if (status == mjit::Jaeger_ThrowBeforeEnter)
            goto error;
        CHECK_PARTIAL_METHODJIT(status);
        interpReturnOK = (status == mjit::Jaeger_Returned);
        if (entryFrame != regs.fp())
            goto jit_return;
        regs.fp()->setFinishedInInterpreter();
        goto leave_on_safe_point;
    }
    if (status == mjit::Compile_Abort)
        useMethodJIT = false;
#endif /* JS_METHODJIT */

    DO_OP();
}

/* ADD_EMPTY_CASE is not used here as JSOP_LINENO_LENGTH == 3. */
BEGIN_CASE(JSOP_LINENO)
END_CASE(JSOP_LINENO)

BEGIN_CASE(JSOP_UNDEFINED)
    PUSH_UNDEFINED();
END_CASE(JSOP_UNDEFINED)

BEGIN_CASE(JSOP_POP)
    regs.sp--;
END_CASE(JSOP_POP)

BEGIN_CASE(JSOP_POPN)
    JS_ASSERT(GET_UINT16(regs.pc) <= regs.stackDepth());
    regs.sp -= GET_UINT16(regs.pc);
#ifdef DEBUG
    if (StaticBlockObject *block = regs.fp()->maybeBlockChain())
        JS_ASSERT(regs.stackDepth() >= block->stackDepth() + block->slotCount());
#endif
END_CASE(JSOP_POPN)

BEGIN_CASE(JSOP_SETRVAL)
BEGIN_CASE(JSOP_POPV)
    POP_RETURN_VALUE();
END_CASE(JSOP_POPV)

BEGIN_CASE(JSOP_ENTERWITH)
    if (!EnterWith(cx, -1))
        goto error;

    /*
     * We must ensure that different "with" blocks have different stack depth
     * associated with them. This allows the try handler search to properly
     * recover the scope chain. Thus we must keep the stack at least at the
     * current level.
     *
     * We set sp[-1] to the current "with" object to help asserting the
     * enter/leave balance in [leavewith].
     */
    regs.sp[-1].setObject(*regs.fp()->scopeChain());
END_CASE(JSOP_ENTERWITH)

BEGIN_CASE(JSOP_LEAVEWITH)
    JS_ASSERT(regs.sp[-1].toObject() == *regs.fp()->scopeChain());
    regs.fp()->popWith(cx);
    regs.sp--;
END_CASE(JSOP_LEAVEWITH)

BEGIN_CASE(JSOP_RETURN)
    POP_RETURN_VALUE();
    /* FALL THROUGH */

BEGIN_CASE(JSOP_RETRVAL)    /* fp return value already set */
BEGIN_CASE(JSOP_STOP)
{
    /*
     * When the inlined frame exits with an exception or an error, ok will be
     * false after the inline_return label.
     */
    CHECK_BRANCH();

    interpReturnOK = true;
    if (entryFrame != regs.fp())
  inline_return:
    {
        if (cx->compartment->debugMode())
            interpReturnOK = ScriptDebugEpilogue(cx, regs.fp(), interpReturnOK);

        if (!regs.fp()->isYielding())
            regs.fp()->epilogue(cx);
        else
            Probes::exitScript(cx, script, script->function(), regs.fp());

        /* The JIT inlines the epilogue. */
#ifdef JS_METHODJIT
  jit_return:
#endif

        /* The results of lowered call/apply frames need to be shifted. */
        bool shiftResult = regs.fp()->loweredCallOrApply();

        cx->stack.popInlineFrame(regs);
        SET_SCRIPT(regs.fp()->script());

        JS_ASSERT(*regs.pc == JSOP_NEW || *regs.pc == JSOP_CALL ||
                  *regs.pc == JSOP_FUNCALL || *regs.pc == JSOP_FUNAPPLY);

        /* Resume execution in the calling frame. */
        RESET_USE_METHODJIT();
        if (JS_LIKELY(interpReturnOK)) {
            TypeScript::Monitor(cx, script, regs.pc, regs.sp[-1]);

            if (shiftResult) {
                regs.sp[-2] = regs.sp[-1];
                regs.sp--;
            }

            len = JSOP_CALL_LENGTH;
            DO_NEXT_OP(len);
        }

        /* Increment pc so that |sp - fp->slots == ReconstructStackDepth(pc)|. */
        regs.pc += JSOP_CALL_LENGTH;
        goto error;
    } else {
        JS_ASSERT(regs.stackDepth() == 0);
    }
    interpReturnOK = true;
    goto exit;
}

BEGIN_CASE(JSOP_DEFAULT)
    regs.sp--;
    /* FALL THROUGH */
BEGIN_CASE(JSOP_GOTO)
{
    len = GET_JUMP_OFFSET(regs.pc);
    BRANCH(len);
}
END_CASE(JSOP_GOTO)

BEGIN_CASE(JSOP_IFEQ)
{
    bool cond = ToBoolean(regs.sp[-1]);
    regs.sp--;
    if (cond == false) {
        len = GET_JUMP_OFFSET(regs.pc);
        BRANCH(len);
    }
}
END_CASE(JSOP_IFEQ)

BEGIN_CASE(JSOP_IFNE)
{
    bool cond = ToBoolean(regs.sp[-1]);
    regs.sp--;
    if (cond != false) {
        len = GET_JUMP_OFFSET(regs.pc);
        BRANCH(len);
    }
}
END_CASE(JSOP_IFNE)

BEGIN_CASE(JSOP_OR)
{
    bool cond = ToBoolean(regs.sp[-1]);
    if (cond == true) {
        len = GET_JUMP_OFFSET(regs.pc);
        DO_NEXT_OP(len);
    }
}
END_CASE(JSOP_OR)

BEGIN_CASE(JSOP_AND)
{
    bool cond = ToBoolean(regs.sp[-1]);
    if (cond == false) {
        len = GET_JUMP_OFFSET(regs.pc);
        DO_NEXT_OP(len);
    }
}
END_CASE(JSOP_AND)

/*
 * If the index value at sp[n] is not an int that fits in a jsval, it could
 * be an object (an XML QName, AttributeName, or AnyName), but only if we are
 * compiling with JS_HAS_XML_SUPPORT.  Otherwise convert the index value to a
 * string atom id.
 */
#define FETCH_ELEMENT_ID(obj, n, id)                                          \
    JS_BEGIN_MACRO                                                            \
        const Value &idval_ = regs.sp[n];                                     \
        if (!ValueToId(cx, obj, idval_, id.address()))                        \
            goto error;                                                       \
    JS_END_MACRO

#define TRY_BRANCH_AFTER_COND(cond,spdec)                                     \
    JS_BEGIN_MACRO                                                            \
        JS_ASSERT(js_CodeSpec[op].length == 1);                               \
        unsigned diff_ = (unsigned) GET_UINT8(regs.pc) - (unsigned) JSOP_IFEQ;         \
        if (diff_ <= 1) {                                                     \
            regs.sp -= spdec;                                                 \
            if (cond == (diff_ != 0)) {                                       \
                ++regs.pc;                                                    \
                len = GET_JUMP_OFFSET(regs.pc);                               \
                BRANCH(len);                                                  \
            }                                                                 \
            len = 1 + JSOP_IFEQ_LENGTH;                                       \
            DO_NEXT_OP(len);                                                  \
        }                                                                     \
    JS_END_MACRO

BEGIN_CASE(JSOP_IN)
{
    HandleValue rref = HandleValue::fromMarkedLocation(&regs.sp[-1]);
    if (!rref.isObject()) {
        js_ReportValueError(cx, JSMSG_IN_NOT_OBJECT, -1, rref, NullPtr());
        goto error;
    }
    RootedObject &obj = rootObject0;
    obj = &rref.toObject();
    RootedId &id = rootId0;
    FETCH_ELEMENT_ID(obj, -2, id);
    RootedObject &obj2 = rootObject1;
    RootedShape &prop = rootShape0;
    if (!JSObject::lookupGeneric(cx, obj, id, &obj2, &prop))
        goto error;
    bool cond = prop != NULL;
    TRY_BRANCH_AFTER_COND(cond, 2);
    regs.sp--;
    regs.sp[-1].setBoolean(cond);
}
END_CASE(JSOP_IN)

BEGIN_CASE(JSOP_ITER)
{
    JS_ASSERT(regs.stackDepth() >= 1);
    uint8_t flags = GET_UINT8(regs.pc);
    MutableHandleValue res = MutableHandleValue::fromMarkedLocation(&regs.sp[-1]);
    if (!ValueToIterator(cx, flags, res))
        goto error;
    JS_ASSERT(!res.isPrimitive());
}
END_CASE(JSOP_ITER)

BEGIN_CASE(JSOP_MOREITER)
{
    JS_ASSERT(regs.stackDepth() >= 1);
    JS_ASSERT(regs.sp[-1].isObject());
    PUSH_NULL();
    bool cond;
    MutableHandleValue res = MutableHandleValue::fromMarkedLocation(&regs.sp[-1]);
    if (!IteratorMore(cx, &regs.sp[-2].toObject(), &cond, res))
        goto error;
    regs.sp[-1].setBoolean(cond);
}
END_CASE(JSOP_MOREITER)

BEGIN_CASE(JSOP_ITERNEXT)
{
    JS_ASSERT(regs.sp[-1].isObject());
    PUSH_NULL();
    MutableHandleValue res = MutableHandleValue::fromMarkedLocation(&regs.sp[-1]);
    if (!IteratorNext(cx, &regs.sp[-2].toObject(), res))
        goto error;
}
END_CASE(JSOP_ITERNEXT)

BEGIN_CASE(JSOP_ENDITER)
{
    JS_ASSERT(regs.stackDepth() >= 1);
    bool ok = CloseIterator(cx, &regs.sp[-1].toObject());
    regs.sp--;
    if (!ok)
        goto error;
}
END_CASE(JSOP_ENDITER)

BEGIN_CASE(JSOP_DUP)
{
    JS_ASSERT(regs.stackDepth() >= 1);
    const Value &rref = regs.sp[-1];
    PUSH_COPY(rref);
}
END_CASE(JSOP_DUP)

BEGIN_CASE(JSOP_DUP2)
{
    JS_ASSERT(regs.stackDepth() >= 2);
    const Value &lref = regs.sp[-2];
    const Value &rref = regs.sp[-1];
    PUSH_COPY(lref);
    PUSH_COPY(rref);
}
END_CASE(JSOP_DUP2)

BEGIN_CASE(JSOP_SWAP)
{
    JS_ASSERT(regs.stackDepth() >= 2);
    Value &lref = regs.sp[-2];
    Value &rref = regs.sp[-1];
    lref.swap(rref);
}
END_CASE(JSOP_SWAP)

BEGIN_CASE(JSOP_PICK)
{
    unsigned i = GET_UINT8(regs.pc);
    JS_ASSERT(regs.stackDepth() >= i + 1);
    Value lval = regs.sp[-int(i + 1)];
    memmove(regs.sp - (i + 1), regs.sp - i, sizeof(Value) * i);
    regs.sp[-1] = lval;
}
END_CASE(JSOP_PICK)

BEGIN_CASE(JSOP_SETCONST)
{
    RootedPropertyName &name = rootName0;
    name = script->getName(regs.pc);

    RootedValue &rval = rootValue0;
    rval = regs.sp[-1];

    RootedObject &obj = rootObject0;
    obj = &regs.fp()->varObj();
    if (!JSObject::defineProperty(cx, obj, name, rval,
                                  JS_PropertyStub, JS_StrictPropertyStub,
                                  JSPROP_ENUMERATE | JSPROP_PERMANENT | JSPROP_READONLY)) {
        goto error;
    }
}
END_CASE(JSOP_SETCONST);

#if JS_HAS_DESTRUCTURING
BEGIN_CASE(JSOP_ENUMCONSTELEM)
{
    RootedValue &rval = rootValue0;
    rval = regs.sp[-3];

    RootedObject &obj = rootObject0;
    FETCH_OBJECT(cx, -2, obj);
    RootedId &id = rootId0;
    FETCH_ELEMENT_ID(obj, -1, id);
    if (!JSObject::defineGeneric(cx, obj, id, rval,
                                 JS_PropertyStub, JS_StrictPropertyStub,
                                 JSPROP_ENUMERATE | JSPROP_PERMANENT | JSPROP_READONLY)) {
        goto error;
    }
    regs.sp -= 3;
}
END_CASE(JSOP_ENUMCONSTELEM)
#endif

BEGIN_CASE(JSOP_BINDGNAME)
    PUSH_OBJECT(regs.fp()->global());
END_CASE(JSOP_BINDGNAME)

BEGIN_CASE(JSOP_BINDNAME)
{
    RootedObject &scopeChain = rootObject0;
    scopeChain = regs.fp()->scopeChain();

    RootedPropertyName &name = rootName0;
    name = script->getName(regs.pc);

    /* Assigning to an undeclared name adds a property to the global object. */
    RootedObject &scope = rootObject1;
    if (!LookupNameWithGlobalDefault(cx, name, scopeChain, &scope))
        goto error;

    PUSH_OBJECT(*scope);
}
END_CASE(JSOP_BINDNAME)

#define BITWISE_OP(OP)                                                        \
    JS_BEGIN_MACRO                                                            \
        int32_t i, j;                                                         \
        if (!ToInt32(cx, regs.sp[-2], &i))                                    \
            goto error;                                                       \
        if (!ToInt32(cx, regs.sp[-1], &j))                                    \
            goto error;                                                       \
        i = i OP j;                                                           \
        regs.sp--;                                                            \
        regs.sp[-1].setInt32(i);                                              \
    JS_END_MACRO

BEGIN_CASE(JSOP_BITOR)
    BITWISE_OP(|);
END_CASE(JSOP_BITOR)

BEGIN_CASE(JSOP_BITXOR)
    BITWISE_OP(^);
END_CASE(JSOP_BITXOR)

BEGIN_CASE(JSOP_BITAND)
    BITWISE_OP(&);
END_CASE(JSOP_BITAND)

#undef BITWISE_OP

#define EQUALITY_OP(OP)                                                       \
    JS_BEGIN_MACRO                                                            \
        Value rval = regs.sp[-1];                                             \
        Value lval = regs.sp[-2];                                             \
        bool cond;                                                            \
        if (!LooselyEqual(cx, lval, rval, &cond))                             \
            goto error;                                                       \
        cond = cond OP JS_TRUE;                                               \
        TRY_BRANCH_AFTER_COND(cond, 2);                                       \
        regs.sp--;                                                            \
        regs.sp[-1].setBoolean(cond);                                         \
    JS_END_MACRO

BEGIN_CASE(JSOP_EQ)
    EQUALITY_OP(==);
END_CASE(JSOP_EQ)

BEGIN_CASE(JSOP_NE)
    EQUALITY_OP(!=);
END_CASE(JSOP_NE)

#undef EQUALITY_OP

#define STRICT_EQUALITY_OP(OP, COND)                                          \
    JS_BEGIN_MACRO                                                            \
        const Value &rref = regs.sp[-1];                                      \
        const Value &lref = regs.sp[-2];                                      \
        bool equal;                                                           \
        if (!StrictlyEqual(cx, lref, rref, &equal))                           \
            goto error;                                                       \
        COND = equal OP JS_TRUE;                                              \
        regs.sp--;                                                            \
    JS_END_MACRO

BEGIN_CASE(JSOP_STRICTEQ)
{
    bool cond;
    STRICT_EQUALITY_OP(==, cond);
    regs.sp[-1].setBoolean(cond);
}
END_CASE(JSOP_STRICTEQ)

BEGIN_CASE(JSOP_STRICTNE)
{
    bool cond;
    STRICT_EQUALITY_OP(!=, cond);
    regs.sp[-1].setBoolean(cond);
}
END_CASE(JSOP_STRICTNE)

BEGIN_CASE(JSOP_CASE)
{
    bool cond;
    STRICT_EQUALITY_OP(==, cond);
    if (cond) {
        regs.sp--;
        len = GET_JUMP_OFFSET(regs.pc);
        BRANCH(len);
    }
}
END_CASE(JSOP_CASE)

#undef STRICT_EQUALITY_OP

BEGIN_CASE(JSOP_LT)
{
    bool cond;
    const Value &lref = regs.sp[-2];
    const Value &rref = regs.sp[-1];
    if (!LessThanOperation(cx, lref, rref, &cond))
        goto error;
    TRY_BRANCH_AFTER_COND(cond, 2);
    regs.sp[-2].setBoolean(cond);
    regs.sp--;
}
END_CASE(JSOP_LT)

BEGIN_CASE(JSOP_LE)
{
    bool cond;
    const Value &lref = regs.sp[-2];
    const Value &rref = regs.sp[-1];
    if (!LessThanOrEqualOperation(cx, lref, rref, &cond))
        goto error;
    TRY_BRANCH_AFTER_COND(cond, 2);
    regs.sp[-2].setBoolean(cond);
    regs.sp--;
}
END_CASE(JSOP_LE)

BEGIN_CASE(JSOP_GT)
{
    bool cond;
    const Value &lref = regs.sp[-2];
    const Value &rref = regs.sp[-1];
    if (!GreaterThanOperation(cx, lref, rref, &cond))
        goto error;
    TRY_BRANCH_AFTER_COND(cond, 2);
    regs.sp[-2].setBoolean(cond);
    regs.sp--;
}
END_CASE(JSOP_GT)

BEGIN_CASE(JSOP_GE)
{
    bool cond;
    const Value &lref = regs.sp[-2];
    const Value &rref = regs.sp[-1];
    if (!GreaterThanOrEqualOperation(cx, lref, rref, &cond))
        goto error;
    TRY_BRANCH_AFTER_COND(cond, 2);
    regs.sp[-2].setBoolean(cond);
    regs.sp--;
}
END_CASE(JSOP_GE)

#define SIGNED_SHIFT_OP(OP)                                                   \
    JS_BEGIN_MACRO                                                            \
        int32_t i, j;                                                         \
        if (!ToInt32(cx, regs.sp[-2], &i))                                    \
            goto error;                                                       \
        if (!ToInt32(cx, regs.sp[-1], &j))                                    \
            goto error;                                                       \
        i = i OP (j & 31);                                                    \
        regs.sp--;                                                            \
        regs.sp[-1].setInt32(i);                                              \
    JS_END_MACRO

BEGIN_CASE(JSOP_LSH)
    SIGNED_SHIFT_OP(<<);
END_CASE(JSOP_LSH)

BEGIN_CASE(JSOP_RSH)
    SIGNED_SHIFT_OP(>>);
END_CASE(JSOP_RSH)

#undef SIGNED_SHIFT_OP

BEGIN_CASE(JSOP_URSH)
{
    uint32_t u;
    if (!ToUint32(cx, regs.sp[-2], &u))
        goto error;
    int32_t j;
    if (!ToInt32(cx, regs.sp[-1], &j))
        goto error;

    u >>= (j & 31);

    regs.sp--;
    if (!regs.sp[-1].setNumber(uint32_t(u)))
        TypeScript::MonitorOverflow(cx, script, regs.pc);
}
END_CASE(JSOP_URSH)

BEGIN_CASE(JSOP_ADD)
{
    Value lval = regs.sp[-2];
    Value rval = regs.sp[-1];
    if (!AddOperation(cx, lval, rval, &regs.sp[-2]))
        goto error;
    regs.sp--;
}
END_CASE(JSOP_ADD)

BEGIN_CASE(JSOP_SUB)
{
    RootedValue &lval = rootValue0, &rval = rootValue1;
    lval = regs.sp[-2];
    rval = regs.sp[-1];
    if (!SubOperation(cx, lval, rval, &regs.sp[-2]))
        goto error;
    regs.sp--;
}
END_CASE(JSOP_SUB)

BEGIN_CASE(JSOP_MUL)
{
    RootedValue &lval = rootValue0, &rval = rootValue1;
    lval = regs.sp[-2];
    rval = regs.sp[-1];
    if (!MulOperation(cx, lval, rval, &regs.sp[-2]))
        goto error;
    regs.sp--;
}
END_CASE(JSOP_MUL)

BEGIN_CASE(JSOP_DIV)
{
    RootedValue &lval = rootValue0, &rval = rootValue1;
    lval = regs.sp[-2];
    rval = regs.sp[-1];
    if (!DivOperation(cx, lval, rval, &regs.sp[-2]))
        goto error;
    regs.sp--;
}
END_CASE(JSOP_DIV)

BEGIN_CASE(JSOP_MOD)
{
    RootedValue &lval = rootValue0, &rval = rootValue1;
    lval = regs.sp[-2];
    rval = regs.sp[-1];
    if (!ModOperation(cx, lval, rval, &regs.sp[-2]))
        goto error;
    regs.sp--;
}
END_CASE(JSOP_MOD)

BEGIN_CASE(JSOP_NOT)
{
    bool cond = ToBoolean(regs.sp[-1]);
    regs.sp--;
    PUSH_BOOLEAN(!cond);
}
END_CASE(JSOP_NOT)

BEGIN_CASE(JSOP_BITNOT)
{
    int32_t i;
    if (!ToInt32(cx, regs.sp[-1], &i))
        goto error;
    i = ~i;
    regs.sp[-1].setInt32(i);
}
END_CASE(JSOP_BITNOT)

BEGIN_CASE(JSOP_NEG)
{
    /*
     * When the operand is int jsval, INT32_FITS_IN_JSVAL(i) implies
     * INT32_FITS_IN_JSVAL(-i) unless i is 0 or INT32_MIN when the
     * results, -0.0 or INT32_MAX + 1, are double values.
     */
    Value ref = regs.sp[-1];
    int32_t i;
    if (ref.isInt32() && (i = ref.toInt32()) != 0 && i != INT32_MIN) {
        i = -i;
        regs.sp[-1].setInt32(i);
    } else {
        double d;
        if (!ToNumber(cx, regs.sp[-1], &d))
            goto error;
        d = -d;
        if (!regs.sp[-1].setNumber(d) && !ref.isDouble())
            TypeScript::MonitorOverflow(cx, script, regs.pc);
    }
}
END_CASE(JSOP_NEG)

BEGIN_CASE(JSOP_POS)
    if (!ToNumber(cx, &regs.sp[-1]))
        goto error;
    if (!regs.sp[-1].isInt32())
        TypeScript::MonitorOverflow(cx, script, regs.pc);
END_CASE(JSOP_POS)

BEGIN_CASE(JSOP_DELNAME)
{
    RootedPropertyName &name = rootName0;
    name = script->getName(regs.pc);

    RootedObject &scopeObj = rootObject0;
    scopeObj = cx->stack.currentScriptedScopeChain();

    RootedObject &scope = rootObject1;
    RootedObject &pobj = rootObject2;
    RootedShape &prop = rootShape0;
    if (!LookupName(cx, name, scopeObj, &scope, &pobj, &prop))
        goto error;

    /* Strict mode code should never contain JSOP_DELNAME opcodes. */
    JS_ASSERT(!script->strictModeCode);

    /* ECMA says to return true if name is undefined or inherited. */
    PUSH_BOOLEAN(true);
    if (prop) {
        MutableHandleValue res = MutableHandleValue::fromMarkedLocation(&regs.sp[-1]);
        if (!JSObject::deleteProperty(cx, scope, name, res, false))
            goto error;
    }
}
END_CASE(JSOP_DELNAME)

BEGIN_CASE(JSOP_DELPROP)
{
    RootedPropertyName &name = rootName0;
    name = script->getName(regs.pc);

    RootedObject &obj = rootObject0;
    FETCH_OBJECT(cx, -1, obj);

    MutableHandleValue res = MutableHandleValue::fromMarkedLocation(&regs.sp[-1]);
    if (!JSObject::deleteProperty(cx, obj, name, res, script->strictModeCode))
        goto error;
}
END_CASE(JSOP_DELPROP)

BEGIN_CASE(JSOP_DELELEM)
{
    /* Fetch the left part and resolve it to a non-null object. */
    RootedObject &obj = rootObject0;
    FETCH_OBJECT(cx, -2, obj);

    RootedValue &propval = rootValue0;
    propval = regs.sp[-1];

    MutableHandleValue res = MutableHandleValue::fromMarkedLocation(&regs.sp[-2]);
    if (!JSObject::deleteByValue(cx, obj, propval, res, script->strictModeCode))
        goto error;

    regs.sp--;
}
END_CASE(JSOP_DELELEM)

BEGIN_CASE(JSOP_TOID)
{
    /*
     * Increment or decrement requires use to lookup the same property twice, but we need to avoid
     * the oberservable stringification the second time.
     * There must be an object value below the id, which will not be popped
     * but is necessary in interning the id for XML.
     */
    RootedValue &objval = rootValue0, &idval = rootValue1;
    objval = regs.sp[-2];
    idval = regs.sp[-1];

    MutableHandleValue res = MutableHandleValue::fromMarkedLocation(&regs.sp[-1]);
    if (!ToIdOperation(cx, objval, idval, res))
        goto error;
}
END_CASE(JSOP_TOID)

BEGIN_CASE(JSOP_TYPEOFEXPR)
BEGIN_CASE(JSOP_TYPEOF)
{
    const Value &ref = regs.sp[-1];
    JSType type = JS_TypeOfValue(cx, ref);
    regs.sp[-1].setString(rt->atomState.typeAtoms[type]);
}
END_CASE(JSOP_TYPEOF)

BEGIN_CASE(JSOP_VOID)
    regs.sp[-1].setUndefined();
END_CASE(JSOP_VOID)

BEGIN_CASE(JSOP_INCELEM)
BEGIN_CASE(JSOP_DECELEM)
BEGIN_CASE(JSOP_ELEMINC)
BEGIN_CASE(JSOP_ELEMDEC)
    /* No-op */
END_CASE(JSOP_INCELEM)

BEGIN_CASE(JSOP_INCPROP)
BEGIN_CASE(JSOP_DECPROP)
BEGIN_CASE(JSOP_PROPINC)
BEGIN_CASE(JSOP_PROPDEC)
BEGIN_CASE(JSOP_INCNAME)
BEGIN_CASE(JSOP_DECNAME)
BEGIN_CASE(JSOP_NAMEINC)
BEGIN_CASE(JSOP_NAMEDEC)
BEGIN_CASE(JSOP_INCGNAME)
BEGIN_CASE(JSOP_DECGNAME)
BEGIN_CASE(JSOP_GNAMEINC)
BEGIN_CASE(JSOP_GNAMEDEC)
    /* No-op */
END_CASE(JSOP_INCPROP)

BEGIN_CASE(JSOP_DECALIASEDVAR)
BEGIN_CASE(JSOP_ALIASEDVARDEC)
BEGIN_CASE(JSOP_INCALIASEDVAR)
BEGIN_CASE(JSOP_ALIASEDVARINC)
    /* No-op */
END_CASE(JSOP_ALIASEDVARINC)

BEGIN_CASE(JSOP_DECARG)
BEGIN_CASE(JSOP_ARGDEC)
BEGIN_CASE(JSOP_INCARG)
BEGIN_CASE(JSOP_ARGINC)
{
    unsigned i = GET_ARGNO(regs.pc);
    if (script->argsObjAliasesFormals()) {
        const Value &arg = regs.fp()->argsObj().arg(i);
        Value v;
        if (!DoIncDec(cx, script, regs.pc, arg, &v, &regs.sp[0]))
            goto error;
        regs.fp()->argsObj().setArg(i, v);
    } else {
        Value &arg = regs.fp()->unaliasedFormal(i);
        if (!DoIncDec(cx, script, regs.pc, arg, &arg, &regs.sp[0]))
            goto error;
    }
    regs.sp++;
}
END_CASE(JSOP_ARGINC);

BEGIN_CASE(JSOP_DECLOCAL)
BEGIN_CASE(JSOP_LOCALDEC)
BEGIN_CASE(JSOP_INCLOCAL)
BEGIN_CASE(JSOP_LOCALINC)
{
    unsigned i = GET_SLOTNO(regs.pc);
    Value &local = regs.fp()->unaliasedLocal(i);
    if (!DoIncDec(cx, script, regs.pc, local, &local, &regs.sp[0]))
        goto error;
    regs.sp++;
}
END_CASE(JSOP_LOCALINC)

BEGIN_CASE(JSOP_THIS)
    if (!ComputeThis(cx, regs.fp()))
        goto error;
    PUSH_COPY(regs.fp()->thisValue());
END_CASE(JSOP_THIS)

BEGIN_CASE(JSOP_GETPROP)
BEGIN_CASE(JSOP_GETXPROP)
BEGIN_CASE(JSOP_LENGTH)
BEGIN_CASE(JSOP_CALLPROP)
{
    RootedValue &lval = rootValue0;
    lval = regs.sp[-1];

    RootedValue rval(cx);
    if (!GetPropertyOperation(cx, regs.pc, &lval, &rval))
        goto error;

    TypeScript::Monitor(cx, script, regs.pc, rval);

    regs.sp[-1] = rval;
    assertSameCompartment(cx, regs.sp[-1]);
}
END_CASE(JSOP_GETPROP)

BEGIN_CASE(JSOP_SETGNAME)
BEGIN_CASE(JSOP_SETNAME)
{
    RootedObject &scope = rootObject0;
    scope = &regs.sp[-2].toObject();

    HandleValue value = HandleValue::fromMarkedLocation(&regs.sp[-1]);

    if (!SetNameOperation(cx, script, regs.pc, scope, value))
        goto error;

    regs.sp[-2] = regs.sp[-1];
    regs.sp--;
}
END_CASE(JSOP_SETNAME)

BEGIN_CASE(JSOP_SETPROP)
{
    HandleValue lval = HandleValue::fromMarkedLocation(&regs.sp[-2]);
    HandleValue rval = HandleValue::fromMarkedLocation(&regs.sp[-1]);

    if (!SetPropertyOperation(cx, regs.pc, lval, rval))
        goto error;

    regs.sp[-2] = regs.sp[-1];
    regs.sp--;
}
END_CASE(JSOP_SETPROP)

BEGIN_CASE(JSOP_GETELEM)
BEGIN_CASE(JSOP_CALLELEM)
{
    MutableHandleValue lval = MutableHandleValue::fromMarkedLocation(&regs.sp[-2]);
    HandleValue rval = HandleValue::fromMarkedLocation(&regs.sp[-1]);

    MutableHandleValue res = MutableHandleValue::fromMarkedLocation(&regs.sp[-2]);
    if (!GetElementOperation(cx, op, lval, rval, res))
        goto error;
    TypeScript::Monitor(cx, script, regs.pc, res);
    regs.sp--;
}
END_CASE(JSOP_GETELEM)

BEGIN_CASE(JSOP_SETELEM)
{
    RootedObject &obj = rootObject0;
    FETCH_OBJECT(cx, -3, obj);
    RootedId &id = rootId0;
    FETCH_ELEMENT_ID(obj, -2, id);
    Value &value = regs.sp[-1];
    if (!SetObjectElementOperation(cx, obj, id, value, script->strictModeCode))
        goto error;
    regs.sp[-3] = value;
    regs.sp -= 2;
}
END_CASE(JSOP_SETELEM)

BEGIN_CASE(JSOP_ENUMELEM)
{
    RootedObject &obj = rootObject0;
    RootedValue &rval = rootValue0;

    /* Funky: the value to set is under the [obj, id] pair. */
    FETCH_OBJECT(cx, -2, obj);
    RootedId &id = rootId0;
    FETCH_ELEMENT_ID(obj, -1, id);
    rval = regs.sp[-3];
    if (!JSObject::setGeneric(cx, obj, obj, id, &rval, script->strictModeCode))
        goto error;
    regs.sp -= 3;
}
END_CASE(JSOP_ENUMELEM)

BEGIN_CASE(JSOP_EVAL)
{
    CallArgs args = CallArgsFromSp(GET_ARGC(regs.pc), regs.sp);
    if (IsBuiltinEvalForScope(regs.fp()->scopeChain(), args.calleev())) {
        if (!DirectEval(cx, args))
            goto error;
    } else {
        if (!InvokeKernel(cx, args))
            goto error;
    }
    regs.sp = args.spAfterCall();
    TypeScript::Monitor(cx, script, regs.pc, regs.sp[-1]);
}
END_CASE(JSOP_EVAL)

BEGIN_CASE(JSOP_FUNAPPLY)
    if (!GuardFunApplyArgumentsOptimization(cx))
        goto error;
    /* FALL THROUGH */

BEGIN_CASE(JSOP_NEW)
BEGIN_CASE(JSOP_CALL)
BEGIN_CASE(JSOP_FUNCALL)
{
    if (regs.fp()->hasPushedSPSFrame())
        cx->runtime->spsProfiler.updatePC(script, regs.pc);
    JS_ASSERT(regs.stackDepth() >= 2 + GET_ARGC(regs.pc));
    CallArgs args = CallArgsFromSp(GET_ARGC(regs.pc), regs.sp);

    bool construct = (*regs.pc == JSOP_NEW);

    RootedFunction &fun = rootFunction0;
    /* Don't bother trying to fast-path calls to scripted non-constructors. */
    if (!IsFunctionObject(args.calleev(), fun.address()) || !fun->isInterpretedConstructor()) {
        if (construct) {
            if (!InvokeConstructorKernel(cx, args))
                goto error;
        } else {
            if (!InvokeKernel(cx, args))
                goto error;
        }
        Value *newsp = args.spAfterCall();
        TypeScript::Monitor(cx, script, regs.pc, newsp[-1]);
        regs.sp = newsp;
        len = JSOP_CALL_LENGTH;
        DO_NEXT_OP(len);
    }

    if (!TypeMonitorCall(cx, args, construct))
        goto error;

    InitialFrameFlags initial = construct ? INITIAL_CONSTRUCT : INITIAL_NONE;

    bool newType = cx->typeInferenceEnabled() && UseNewType(cx, script, regs.pc);

    JSScript *newScript = fun->script();

    if (newScript->compileAndGo && newScript->hasClearedGlobal()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CLEARED_SCOPE);
        goto error;
    }

    if (!cx->stack.pushInlineFrame(cx, regs, args, *fun, newScript, initial))
        goto error;

    SET_SCRIPT(regs.fp()->script());
    RESET_USE_METHODJIT();

#ifdef JS_METHODJIT
    if (!newType) {
        /* Try to ensure methods are method JIT'd.  */
        mjit::CompileStatus status = mjit::CanMethodJIT(cx, script, script->code,
                                                        construct,
                                                        mjit::CompileRequest_Interpreter,
                                                        regs.fp());
        if (status == mjit::Compile_Error)
            goto error;
        if (status == mjit::Compile_Okay) {
            mjit::JaegerStatus status = mjit::JaegerShot(cx, true);
            CHECK_PARTIAL_METHODJIT(status);
            interpReturnOK = mjit::JaegerStatusToSuccess(status);
            goto jit_return;
        }
    }
#endif

    if (!regs.fp()->prologue(cx, newType))
        goto error;
    if (cx->compartment->debugMode()) {
        switch (ScriptDebugPrologue(cx, regs.fp())) {
          case JSTRAP_CONTINUE:
            break;
          case JSTRAP_RETURN:
            interpReturnOK = true;
            goto forced_return;
          case JSTRAP_THROW:
          case JSTRAP_ERROR:
            goto error;
          default:
            JS_NOT_REACHED("bad ScriptDebugPrologue status");
        }
    }

    /* Load first op and dispatch it (safe since JSOP_STOP). */
    op = (JSOp) *regs.pc;
    DO_OP();
}

BEGIN_CASE(JSOP_SETCALL)
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_LEFTSIDE_OF_ASS);
    goto error;
}
END_CASE(JSOP_SETCALL)

BEGIN_CASE(JSOP_IMPLICITTHIS)
{
    RootedPropertyName &name = rootName0;
    name = script->getName(regs.pc);

    RootedObject &scopeObj = rootObject0;
    scopeObj = cx->stack.currentScriptedScopeChain();

    RootedObject &scope = rootObject1;
    if (!LookupNameWithGlobalDefault(cx, name, scopeObj, &scope))
        goto error;

    Value v;
    if (!ComputeImplicitThis(cx, scope, &v))
        goto error;
    PUSH_COPY(v);
}
END_CASE(JSOP_IMPLICITTHIS)

BEGIN_CASE(JSOP_GETGNAME)
BEGIN_CASE(JSOP_CALLGNAME)
BEGIN_CASE(JSOP_NAME)
BEGIN_CASE(JSOP_CALLNAME)
{
    RootedValue &rval = rootValue0;

    if (!NameOperation(cx, script, regs.pc, rval.address()))
        goto error;

    PUSH_COPY(rval);
    TypeScript::Monitor(cx, script, regs.pc, rval);
}
END_CASE(JSOP_NAME)

BEGIN_CASE(JSOP_INTRINSICNAME)
BEGIN_CASE(JSOP_CALLINTRINSIC)
{
    RootedValue &rval = rootValue0;

    if (!IntrinsicNameOperation(cx, script, regs.pc, rval.address()))
        goto error;

    PUSH_COPY(rval);
    TypeScript::Monitor(cx, script, regs.pc, rval);
}
END_CASE(JSOP_INTRINSICNAME)

BEGIN_CASE(JSOP_UINT16)
    PUSH_INT32((int32_t) GET_UINT16(regs.pc));
END_CASE(JSOP_UINT16)

BEGIN_CASE(JSOP_UINT24)
    PUSH_INT32((int32_t) GET_UINT24(regs.pc));
END_CASE(JSOP_UINT24)

BEGIN_CASE(JSOP_INT8)
    PUSH_INT32(GET_INT8(regs.pc));
END_CASE(JSOP_INT8)

BEGIN_CASE(JSOP_INT32)
    PUSH_INT32(GET_INT32(regs.pc));
END_CASE(JSOP_INT32)

BEGIN_CASE(JSOP_DOUBLE)
{
    double dbl;
    LOAD_DOUBLE(0, dbl);
    PUSH_DOUBLE(dbl);
}
END_CASE(JSOP_DOUBLE)

BEGIN_CASE(JSOP_STRING)
    PUSH_STRING(script->getAtom(regs.pc));
END_CASE(JSOP_STRING)

BEGIN_CASE(JSOP_OBJECT)
    PUSH_OBJECT(*script->getObject(regs.pc));
END_CASE(JSOP_OBJECT)

BEGIN_CASE(JSOP_REGEXP)
{
    /*
     * Push a regexp object cloned from the regexp literal object mapped by the
     * bytecode at pc.
     */
    uint32_t index = GET_UINT32_INDEX(regs.pc);
    JSObject *proto = regs.fp()->global().getOrCreateRegExpPrototype(cx);
    if (!proto)
        goto error;
    JSObject *obj = CloneRegExpObject(cx, script->getRegExp(index), proto);
    if (!obj)
        goto error;
    PUSH_OBJECT(*obj);
}
END_CASE(JSOP_REGEXP)

BEGIN_CASE(JSOP_ZERO)
    PUSH_INT32(0);
END_CASE(JSOP_ZERO)

BEGIN_CASE(JSOP_ONE)
    PUSH_INT32(1);
END_CASE(JSOP_ONE)

BEGIN_CASE(JSOP_NULL)
    PUSH_NULL();
END_CASE(JSOP_NULL)

BEGIN_CASE(JSOP_FALSE)
    PUSH_BOOLEAN(false);
END_CASE(JSOP_FALSE)

BEGIN_CASE(JSOP_TRUE)
    PUSH_BOOLEAN(true);
END_CASE(JSOP_TRUE)

{
BEGIN_CASE(JSOP_TABLESWITCH)
{
    jsbytecode *pc2 = regs.pc;
    len = GET_JUMP_OFFSET(pc2);

    /*
     * ECMAv2+ forbids conversion of discriminant, so we will skip to the
     * default case if the discriminant isn't already an int jsval.  (This
     * opcode is emitted only for dense int-domain switches.)
     */
    const Value &rref = *--regs.sp;
    int32_t i;
    if (rref.isInt32()) {
        i = rref.toInt32();
    } else {
        double d;
        /* Don't use MOZ_DOUBLE_IS_INT32; treat -0 (double) as 0. */
        if (!rref.isDouble() || (d = rref.toDouble()) != (i = int32_t(rref.toDouble())))
            DO_NEXT_OP(len);
    }

    pc2 += JUMP_OFFSET_LEN;
    int32_t low = GET_JUMP_OFFSET(pc2);
    pc2 += JUMP_OFFSET_LEN;
    int32_t high = GET_JUMP_OFFSET(pc2);

    i -= low;
    if ((uint32_t)i < (uint32_t)(high - low + 1)) {
        pc2 += JUMP_OFFSET_LEN + JUMP_OFFSET_LEN * i;
        int32_t off = (int32_t) GET_JUMP_OFFSET(pc2);
        if (off)
            len = off;
    }
}
END_VARLEN_CASE
}

{
BEGIN_CASE(JSOP_LOOKUPSWITCH)
{
    int32_t off;
    off = JUMP_OFFSET_LEN;

    /*
     * JSOP_LOOKUPSWITCH are never used if any atom index in it would exceed
     * 64K limit.
     */
    jsbytecode *pc2 = regs.pc;

    Value lval = regs.sp[-1];
    regs.sp--;

    int npairs;
    if (!lval.isPrimitive())
        goto end_lookup_switch;

    pc2 += off;
    npairs = GET_UINT16(pc2);
    pc2 += UINT16_LEN;
    JS_ASSERT(npairs);  /* empty switch uses JSOP_TABLESWITCH */

    bool match;
#define SEARCH_PAIRS(MATCH_CODE)                                              \
    for (;;) {                                                                \
        Value rval = script->getConst(GET_UINT32_INDEX(pc2));                 \
        MATCH_CODE                                                            \
        pc2 += UINT32_INDEX_LEN;                                              \
        if (match)                                                            \
            break;                                                            \
        pc2 += off;                                                           \
        if (--npairs == 0) {                                                  \
            pc2 = regs.pc;                                                    \
            break;                                                            \
        }                                                                     \
    }

    if (lval.isString()) {
        JSLinearString *str = lval.toString()->ensureLinear(cx);
        if (!str)
            goto error;
        JSLinearString *str2;
        SEARCH_PAIRS(
            match = (rval.isString() &&
                     ((str2 = &rval.toString()->asLinear()) == str ||
                      EqualStrings(str2, str)));
        )
    } else if (lval.isNumber()) {
        double ldbl = lval.toNumber();
        SEARCH_PAIRS(
            match = rval.isNumber() && ldbl == rval.toNumber();
        )
    } else {
        SEARCH_PAIRS(
            match = (lval == rval);
        )
    }
#undef SEARCH_PAIRS

  end_lookup_switch:
    len = GET_JUMP_OFFSET(pc2);
}
END_VARLEN_CASE
}

BEGIN_CASE(JSOP_ACTUALSFILLED)
{
    PUSH_INT32(Max(regs.fp()->numActualArgs(), GET_UINT16(regs.pc)));
}
END_CASE(JSOP_ACTUALSFILLED)

BEGIN_CASE(JSOP_ARGUMENTS)
    JS_ASSERT(!regs.fp()->fun()->hasRest());
    if (script->needsArgsObj()) {
        ArgumentsObject *obj = ArgumentsObject::createExpected(cx, regs.fp());
        if (!obj)
            goto error;
        PUSH_COPY(ObjectValue(*obj));
    } else {
        PUSH_COPY(MagicValue(JS_OPTIMIZED_ARGUMENTS));
    }
END_CASE(JSOP_ARGUMENTS)

BEGIN_CASE(JSOP_REST)
{
    RootedObject &rest = rootObject0;
    rest = regs.fp()->createRestParameter(cx);
    if (!rest)
        goto error;
    PUSH_COPY(ObjectValue(*rest));
    if (!SetInitializerObjectType(cx, script, regs.pc, rest))
        goto error;
}
END_CASE(JSOP_REST)

BEGIN_CASE(JSOP_CALLALIASEDVAR)
BEGIN_CASE(JSOP_GETALIASEDVAR)
{
    ScopeCoordinate sc = ScopeCoordinate(regs.pc);
    PUSH_COPY(regs.fp()->aliasedVarScope(sc).aliasedVar(sc));
    TypeScript::Monitor(cx, script, regs.pc, regs.sp[-1]);
}
END_CASE(JSOP_GETALIASEDVAR)

BEGIN_CASE(JSOP_SETALIASEDVAR)
{
    ScopeCoordinate sc = ScopeCoordinate(regs.pc);
    regs.fp()->aliasedVarScope(sc).setAliasedVar(sc, regs.sp[-1]);
}
END_CASE(JSOP_SETALIASEDVAR)

BEGIN_CASE(JSOP_GETARG)
BEGIN_CASE(JSOP_CALLARG)
{
    unsigned i = GET_ARGNO(regs.pc);
    if (script->argsObjAliasesFormals())
        PUSH_COPY(regs.fp()->argsObj().arg(i));
    else
        PUSH_COPY(regs.fp()->unaliasedFormal(i));
}
END_CASE(JSOP_GETARG)

BEGIN_CASE(JSOP_SETARG)
{
    unsigned i = GET_ARGNO(regs.pc);
    if (script->argsObjAliasesFormals())
        regs.fp()->argsObj().setArg(i, regs.sp[-1]);
    else
        regs.fp()->unaliasedFormal(i) = regs.sp[-1];
}
END_CASE(JSOP_SETARG)

BEGIN_CASE(JSOP_GETLOCAL)
BEGIN_CASE(JSOP_CALLLOCAL)
{
    unsigned i = GET_SLOTNO(regs.pc);
    PUSH_COPY_SKIP_CHECK(regs.fp()->unaliasedLocal(i));

    /*
     * Skip the same-compartment assertion if the local will be immediately
     * popped. We do not guarantee sync for dead locals when coming in from the
     * method JIT, and a GETLOCAL followed by POP is not considered to be
     * a use of the variable.
     */
    if (regs.pc[JSOP_GETLOCAL_LENGTH] != JSOP_POP)
        assertSameCompartment(cx, regs.sp[-1]);
}
END_CASE(JSOP_GETLOCAL)

BEGIN_CASE(JSOP_SETLOCAL)
{
    unsigned i = GET_SLOTNO(regs.pc);
    regs.fp()->unaliasedLocal(i) = regs.sp[-1];
}
END_CASE(JSOP_SETLOCAL)

BEGIN_CASE(JSOP_DEFCONST)
BEGIN_CASE(JSOP_DEFVAR)
{
    /* ES5 10.5 step 8 (with subsequent errata). */
    unsigned attrs = JSPROP_ENUMERATE;
    if (!regs.fp()->isEvalFrame())
        attrs |= JSPROP_PERMANENT;
    if (op == JSOP_DEFCONST)
        attrs |= JSPROP_READONLY;

    /* Step 8b. */
    RootedObject &obj = rootObject0;
    obj = &regs.fp()->varObj();

    RootedPropertyName &name = rootName0;
    name = script->getName(regs.pc);

    if (!DefVarOrConstOperation(cx, obj, name, attrs))
        goto error;
}
END_CASE(JSOP_DEFVAR)

BEGIN_CASE(JSOP_DEFFUN)
{
    /*
     * A top-level function defined in Global or Eval code (see ECMA-262
     * Ed. 3), or else a SpiderMonkey extension: a named function statement in
     * a compound statement (not at the top statement level of global code, or
     * at the top level of a function body).
     */
    RootedFunction &fun = rootFunction0;
    fun = script->getFunction(GET_UINT32_INDEX(regs.pc));

    /*
     * If static link is not current scope, clone fun's object to link to the
     * current scope via parent. We do this to enable sharing of compiled
     * functions among multiple equivalent scopes, amortizing the cost of
     * compilation over a number of executions.  Examples include XUL scripts
     * and event handlers shared among Firefox or other Mozilla app chrome
     * windows, and user-defined JS functions precompiled and then shared among
     * requests in server-side JS.
     */
    HandleObject scopeChain = regs.fp()->scopeChain();
    if (fun->environment() != scopeChain) {
        fun = CloneFunctionObjectIfNotSingleton(cx, fun, scopeChain);
        if (!fun)
            goto error;
    } else {
        JS_ASSERT(script->compileAndGo);
        JS_ASSERT(regs.fp()->isGlobalFrame() || regs.fp()->isEvalInFunction());
    }

    /*
     * ECMA requires functions defined when entering Eval code to be
     * impermanent.
     */
    unsigned attrs = regs.fp()->isEvalFrame()
                  ? JSPROP_ENUMERATE
                  : JSPROP_ENUMERATE | JSPROP_PERMANENT;

    /*
     * We define the function as a property of the variable object and not the
     * current scope chain even for the case of function expression statements
     * and functions defined by eval inside let or with blocks.
     */
    RootedObject &parent = rootObject0;
    parent = &regs.fp()->varObj();

    /* ES5 10.5 (NB: with subsequent errata). */
    RootedPropertyName &name = rootName0;
    name = fun->atom()->asPropertyName();
    RootedShape &shape = rootShape0;
    RootedObject &pobj = rootObject1;
    if (!JSObject::lookupProperty(cx, parent, name, &pobj, &shape))
        goto error;

    RootedValue &rval = rootValue0;
    rval = ObjectValue(*fun);

    do {
        /* Steps 5d, 5f. */
        if (!shape || pobj != parent) {
            if (!JSObject::defineProperty(cx, parent, name, rval,
                                          JS_PropertyStub, JS_StrictPropertyStub, attrs))
            {
                goto error;
            }
            break;
        }

        /* Step 5e. */
        JS_ASSERT(parent->isNative());
        if (parent->isGlobal()) {
            if (shape->configurable()) {
                if (!JSObject::defineProperty(cx, parent, name, rval,
                                              JS_PropertyStub, JS_StrictPropertyStub, attrs))
                {
                    goto error;
                }
                break;
            }

            if (shape->isAccessorDescriptor() || !shape->writable() || !shape->enumerable()) {
                JSAutoByteString bytes;
                if (js_AtomToPrintableString(cx, name, &bytes)) {
                    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                         JSMSG_CANT_REDEFINE_PROP, bytes.ptr());
                }
                goto error;
            }
        }

        /*
         * Non-global properties, and global properties which we aren't simply
         * redefining, must be set.  First, this preserves their attributes.
         * Second, this will produce warnings and/or errors as necessary if the
         * specified Call object property is not writable (const).
         */

        /* Step 5f. */
        if (!JSObject::setProperty(cx, parent, parent, name, &rval, script->strictModeCode))
            goto error;
    } while (false);
}
END_CASE(JSOP_DEFFUN)

BEGIN_CASE(JSOP_LAMBDA)
{
    /* Load the specified function object literal. */
    RootedFunction &fun = rootFunction0;
    fun = script->getFunction(GET_UINT32_INDEX(regs.pc));

    JSFunction *obj = CloneFunctionObjectIfNotSingleton(cx, fun, regs.fp()->scopeChain());
    if (!obj)
        goto error;

    JS_ASSERT(obj->getProto());
    PUSH_OBJECT(*obj);
}
END_CASE(JSOP_LAMBDA)

BEGIN_CASE(JSOP_CALLEE)
    JS_ASSERT(regs.fp()->isNonEvalFunctionFrame());
    PUSH_COPY(regs.fp()->calleev());
END_CASE(JSOP_CALLEE)

BEGIN_CASE(JSOP_GETTER)
BEGIN_CASE(JSOP_SETTER)
{
    JSOp op2 = JSOp(*++regs.pc);
    RootedId &id = rootId0;
    RootedValue &rval = rootValue0;
    RootedValue &scratch = rootValue1;
    int i;

    RootedObject &obj = rootObject0;
    switch (op2) {
      case JSOP_SETNAME:
      case JSOP_SETPROP:
        id = NameToId(script->getName(regs.pc));
        rval = regs.sp[-1];
        i = -1;
        goto gs_pop_lval;
      case JSOP_SETELEM:
        rval = regs.sp[-1];
        id = JSID_VOID;
        i = -2;
      gs_pop_lval:
        FETCH_OBJECT(cx, i - 1, obj);
        break;

      case JSOP_INITPROP:
        JS_ASSERT(regs.stackDepth() >= 2);
        rval = regs.sp[-1];
        i = -1;
        id = NameToId(script->getName(regs.pc));
        goto gs_get_lval;
      default:
        JS_ASSERT(op2 == JSOP_INITELEM);
        JS_ASSERT(regs.stackDepth() >= 3);
        rval = regs.sp[-1];
        id = JSID_VOID;
        i = -2;
      gs_get_lval:
      {
        const Value &lref = regs.sp[i-1];
        JS_ASSERT(lref.isObject());
        obj = &lref.toObject();
        break;
      }
    }

    /* Ensure that id has a type suitable for use with obj. */
    if (JSID_IS_VOID(id))
        FETCH_ELEMENT_ID(obj, i, id);

    if (!js_IsCallable(rval)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_GETTER_OR_SETTER,
                             (op == JSOP_GETTER) ? js_getter_str : js_setter_str);
        goto error;
    }

    /*
     * Getters and setters are just like watchpoints from an access control
     * point of view.
     */
    Value rtmp;
    unsigned attrs;
    if (!CheckAccess(cx, obj, id, JSACC_WATCH, &rtmp, &attrs))
        goto error;

    PropertyOp getter;
    StrictPropertyOp setter;
    if (op == JSOP_GETTER) {
        getter = CastAsPropertyOp(&rval.toObject());
        setter = JS_StrictPropertyStub;
        attrs = JSPROP_GETTER;
    } else {
        getter = JS_PropertyStub;
        setter = CastAsStrictPropertyOp(&rval.toObject());
        attrs = JSPROP_SETTER;
    }
    attrs |= JSPROP_ENUMERATE | JSPROP_SHARED;

    scratch.setUndefined();
    if (!JSObject::defineGeneric(cx, obj, id, scratch, getter, setter, attrs))
        goto error;

    regs.sp += i;
    if (js_CodeSpec[op2].ndefs > js_CodeSpec[op2].nuses) {
        JS_ASSERT(js_CodeSpec[op2].ndefs == js_CodeSpec[op2].nuses + 1);
        regs.sp[-1] = rval;
        assertSameCompartment(cx, regs.sp[-1]);
    }
    len = js_CodeSpec[op2].length;
    DO_NEXT_OP(len);
}

BEGIN_CASE(JSOP_HOLE)
    PUSH_HOLE();
END_CASE(JSOP_HOLE)

BEGIN_CASE(JSOP_NEWINIT)
{
    uint8_t i = GET_UINT8(regs.pc);
    JS_ASSERT(i == JSProto_Array || i == JSProto_Object);

    RootedObject &obj = rootObject0;
    if (i == JSProto_Array) {
        obj = NewDenseEmptyArray(cx);
    } else {
        gc::AllocKind kind = GuessObjectGCKind(0);
        obj = NewBuiltinClassInstance(cx, &ObjectClass, kind);
    }
    if (!obj || !SetInitializerObjectType(cx, script, regs.pc, obj))
        goto error;

    PUSH_OBJECT(*obj);
    TypeScript::Monitor(cx, script, regs.pc, regs.sp[-1]);
}
END_CASE(JSOP_NEWINIT)

BEGIN_CASE(JSOP_NEWARRAY)
{
    unsigned count = GET_UINT24(regs.pc);
    RootedObject &obj = rootObject0;
    obj = NewDenseAllocatedArray(cx, count);
    if (!obj || !SetInitializerObjectType(cx, script, regs.pc, obj))
        goto error;

    PUSH_OBJECT(*obj);
    TypeScript::Monitor(cx, script, regs.pc, regs.sp[-1]);
}
END_CASE(JSOP_NEWARRAY)

BEGIN_CASE(JSOP_NEWOBJECT)
{
    RootedObject &baseobj = rootObject0;
    baseobj = script->getObject(regs.pc);

    RootedObject &obj = rootObject1;
    obj = CopyInitializerObject(cx, baseobj);
    if (!obj || !SetInitializerObjectType(cx, script, regs.pc, obj))
        goto error;

    PUSH_OBJECT(*obj);
    TypeScript::Monitor(cx, script, regs.pc, regs.sp[-1]);
}
END_CASE(JSOP_NEWOBJECT)

BEGIN_CASE(JSOP_ENDINIT)
{
    /* FIXME remove JSOP_ENDINIT bug 588522 */
    JS_ASSERT(regs.stackDepth() >= 1);
    JS_ASSERT(regs.sp[-1].isObject());
}
END_CASE(JSOP_ENDINIT)

BEGIN_CASE(JSOP_INITPROP)
{
    /* Load the property's initial value into rval. */
    JS_ASSERT(regs.stackDepth() >= 2);
    RootedValue &rval = rootValue0;
    rval = regs.sp[-1];

    /* Load the object being initialized into lval/obj. */
    RootedObject &obj = rootObject0;
    obj = &regs.sp[-2].toObject();
    JS_ASSERT(obj->isObject());

    PropertyName *name = script->getName(regs.pc);

    RootedId &id = rootId0;
    id = NameToId(name);

    if (JS_UNLIKELY(name == cx->runtime->atomState.protoAtom)
        ? !baseops::SetPropertyHelper(cx, obj, obj, id, 0, &rval, script->strictModeCode)
        : !DefineNativeProperty(cx, obj, id, rval, NULL, NULL,
                                JSPROP_ENUMERATE, 0, 0, 0)) {
        goto error;
    }

    regs.sp--;
}
END_CASE(JSOP_INITPROP);

BEGIN_CASE(JSOP_INITELEM_INC)
BEGIN_CASE(JSOP_INITELEM)
{
    /* Pop the element's value into rval. */
    JS_ASSERT(regs.stackDepth() >= 3);
    HandleValue rref = HandleValue::fromMarkedLocation(&regs.sp[-1]);

    RootedObject &obj = rootObject0;

    /* Find the object being initialized at top of stack. */
    const Value &lref = regs.sp[-3];
    JS_ASSERT(lref.isObject());
    obj = &lref.toObject();

    /* Fetch id now that we have obj. */
    RootedId &id = rootId0;
    FETCH_ELEMENT_ID(obj, -2, id);

    /*
     * If rref is a hole, do not call JSObject::defineProperty. In this case,
     * obj must be an array, so if the current op is the last element
     * initialiser, set the array length to one greater than id.
     */
    if (rref.isMagic(JS_ARRAY_HOLE)) {
        JS_ASSERT(obj->isArray());
        JS_ASSERT(JSID_IS_INT(id));
        JS_ASSERT(uint32_t(JSID_TO_INT(id)) < StackSpace::ARGS_LENGTH_MAX);
        if (JSOp(regs.pc[JSOP_INITELEM_LENGTH]) == JSOP_ENDINIT &&
            !SetLengthProperty(cx, obj, (uint32_t) (JSID_TO_INT(id) + 1)))
        {
            goto error;
        }
    } else {
        if (!JSObject::defineGeneric(cx, obj, id, rref, NULL, NULL, JSPROP_ENUMERATE))
            goto error;
    }
    if (op == JSOP_INITELEM_INC) {
        JS_ASSERT(obj->isArray());
        if (JSID_TO_INT(id) == INT32_MAX) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_SPREAD_TOO_LARGE);
            goto error;
        }
        regs.sp[-2].setInt32(JSID_TO_INT(id) + 1);
        regs.sp--;
    } else {
        regs.sp -= 2;
    }
}
END_CASE(JSOP_INITELEM)

BEGIN_CASE(JSOP_SPREAD)
{
    int32_t count = regs.sp[-2].toInt32();
    RootedObject arr(cx, &regs.sp[-3].toObject());
    const Value iterable = regs.sp[-1];
    ForOfIterator iter(cx, iterable);
    RootedValue &iterVal = rootValue0;
    while (iter.next()) {
        if (count == INT32_MAX) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_SPREAD_TOO_LARGE);
            goto error;
        }
        iterVal = iter.value();
        if (!JSObject::defineElement(cx, arr, count++, iterVal, NULL, NULL, JSPROP_ENUMERATE))
            goto error;
    }
    if (!iter.close())
        goto error;
    regs.sp[-2].setInt32(count);
    regs.sp--;
}
END_CASE(JSOP_SPREAD)

{
BEGIN_CASE(JSOP_GOSUB)
    PUSH_BOOLEAN(false);
    int32_t i = (regs.pc - script->code) + JSOP_GOSUB_LENGTH;
    len = GET_JUMP_OFFSET(regs.pc);
    PUSH_INT32(i);
END_VARLEN_CASE
}

{
BEGIN_CASE(JSOP_RETSUB)
    /* Pop [exception or hole, retsub pc-index]. */
    Value rval, lval;
    POP_COPY_TO(rval);
    POP_COPY_TO(lval);
    JS_ASSERT(lval.isBoolean());
    if (lval.toBoolean()) {
        /*
         * Exception was pending during finally, throw it *before* we adjust
         * pc, because pc indexes into script->trynotes.  This turns out not to
         * be necessary, but it seems clearer.  And it points out a FIXME:
         * 350509, due to Igor Bukanov.
         */
        cx->setPendingException(rval);
        goto error;
    }
    JS_ASSERT(rval.isInt32());

    /* Increment the PC by this much. */
    len = rval.toInt32() - int32_t(regs.pc - script->code);
END_VARLEN_CASE
}

BEGIN_CASE(JSOP_EXCEPTION)
    PUSH_COPY(cx->getPendingException());
    cx->clearPendingException();
    CHECK_BRANCH();
END_CASE(JSOP_EXCEPTION)

BEGIN_CASE(JSOP_FINALLY)
    CHECK_BRANCH();
END_CASE(JSOP_FINALLY)

BEGIN_CASE(JSOP_THROWING)
{
    JS_ASSERT(!cx->isExceptionPending());
    Value v;
    POP_COPY_TO(v);
    cx->setPendingException(v);
}
END_CASE(JSOP_THROWING)

BEGIN_CASE(JSOP_THROW)
{
    JS_ASSERT(!cx->isExceptionPending());
    CHECK_BRANCH();
    Value v;
    POP_COPY_TO(v);
    cx->setPendingException(v);
    /* let the code at error try to catch the exception. */
    goto error;
}

BEGIN_CASE(JSOP_INSTANCEOF)
{
    RootedValue &rref = rootValue0;
    rref = regs.sp[-1];
    if (rref.isPrimitive()) {
        js_ReportValueError(cx, JSMSG_BAD_INSTANCEOF_RHS, -1, rref, NullPtr());
        goto error;
    }
    RootedObject &obj = rootObject0;
    obj = &rref.toObject();
    const Value &lref = regs.sp[-2];
    JSBool cond = JS_FALSE;
    if (!HasInstance(cx, obj, &lref, &cond))
        goto error;
    regs.sp--;
    regs.sp[-1].setBoolean(cond);
}
END_CASE(JSOP_INSTANCEOF)

BEGIN_CASE(JSOP_DEBUGGER)
{
    JSTrapStatus st = JSTRAP_CONTINUE;
    Value rval;
    if (JSDebuggerHandler handler = cx->runtime->debugHooks.debuggerHandler)
        st = handler(cx, script, regs.pc, &rval, cx->runtime->debugHooks.debuggerHandlerData);
    if (st == JSTRAP_CONTINUE)
        st = Debugger::onDebuggerStatement(cx, &rval);
    switch (st) {
      case JSTRAP_ERROR:
        goto error;
      case JSTRAP_CONTINUE:
        break;
      case JSTRAP_RETURN:
        regs.fp()->setReturnValue(rval);
        interpReturnOK = true;
        goto forced_return;
      case JSTRAP_THROW:
        cx->setPendingException(rval);
        goto error;
      default:;
    }
}
END_CASE(JSOP_DEBUGGER)

#if JS_HAS_XML_SUPPORT
BEGIN_CASE(JSOP_DEFXMLNS)
{
    JS_ASSERT(!script->strictModeCode);

    if (!js_SetDefaultXMLNamespace(cx, regs.sp[-1]))
        goto error;
    regs.sp--;
}
END_CASE(JSOP_DEFXMLNS)

BEGIN_CASE(JSOP_ANYNAME)
{
    JS_ASSERT(!script->strictModeCode);

    cx->runtime->gcExactScanningEnabled = false;

    jsid id;
    if (!js_GetAnyName(cx, &id))
        goto error;
    PUSH_COPY(IdToValue(id));
}
END_CASE(JSOP_ANYNAME)
#endif

BEGIN_CASE(JSOP_QNAMEPART)
    /*
     * We do not JS_ASSERT(!script->strictModeCode) here because JSOP_QNAMEPART
     * is used for __proto__ and (in contexts where we favor JSOP_*ELEM instead
     * of JSOP_*PROP) obj.prop compiled as obj['prop'].
     */
    PUSH_STRING(script->getAtom(regs.pc));
END_CASE(JSOP_QNAMEPART)

#if JS_HAS_XML_SUPPORT
BEGIN_CASE(JSOP_QNAMECONST)
{
    JS_ASSERT(!script->strictModeCode);
    Value rval = StringValue(script->getAtom(regs.pc));
    Value lval = regs.sp[-1];
    JSObject *obj = js_ConstructXMLQNameObject(cx, lval, rval);
    if (!obj)
        goto error;
    regs.sp[-1].setObject(*obj);
}
END_CASE(JSOP_QNAMECONST)

BEGIN_CASE(JSOP_QNAME)
{
    JS_ASSERT(!script->strictModeCode);

    Value rval = regs.sp[-1];
    Value lval = regs.sp[-2];
    JSObject *obj = js_ConstructXMLQNameObject(cx, lval, rval);
    if (!obj)
        goto error;
    regs.sp--;
    regs.sp[-1].setObject(*obj);
}
END_CASE(JSOP_QNAME)

BEGIN_CASE(JSOP_TOATTRNAME)
{
    JS_ASSERT(!script->strictModeCode);

    Value rval;
    rval = regs.sp[-1];
    if (!js_ToAttributeName(cx, &rval))
        goto error;
    regs.sp[-1] = rval;
}
END_CASE(JSOP_TOATTRNAME)

BEGIN_CASE(JSOP_TOATTRVAL)
{
    JS_ASSERT(!script->strictModeCode);

    Value rval;
    rval = regs.sp[-1];
    JS_ASSERT(rval.isString());
    JSString *str = js_EscapeAttributeValue(cx, rval.toString(), JS_FALSE);
    if (!str)
        goto error;
    regs.sp[-1].setString(str);
}
END_CASE(JSOP_TOATTRVAL)

BEGIN_CASE(JSOP_ADDATTRNAME)
BEGIN_CASE(JSOP_ADDATTRVAL)
{
    JS_ASSERT(!script->strictModeCode);

    Value rval = regs.sp[-1];
    Value lval = regs.sp[-2];
    JSString *str = lval.toString();
    JSString *str2 = rval.toString();
    str = js_AddAttributePart(cx, op == JSOP_ADDATTRNAME, str, str2);
    if (!str)
        goto error;
    regs.sp--;
    regs.sp[-1].setString(str);
}
END_CASE(JSOP_ADDATTRNAME)

BEGIN_CASE(JSOP_BINDXMLNAME)
{
    JS_ASSERT(!script->strictModeCode);

    Value lval;
    lval = regs.sp[-1];
    RootedObject &obj = rootObject0;
    jsid id;
    if (!js_FindXMLProperty(cx, lval, &obj, &id))
        goto error;
    regs.sp[-1].setObjectOrNull(obj);
    PUSH_COPY(IdToValue(id));
}
END_CASE(JSOP_BINDXMLNAME)

BEGIN_CASE(JSOP_SETXMLNAME)
{
    JS_ASSERT(!script->strictModeCode);

    Rooted<JSObject*> obj(cx, &regs.sp[-3].toObject());
    RootedValue &rval = rootValue0;
    rval = regs.sp[-1];
    RootedId &id = rootId0;
    FETCH_ELEMENT_ID(obj, -2, id);
    if (!JSObject::setGeneric(cx, obj, obj, id, &rval, script->strictModeCode))
        goto error;
    rval = regs.sp[-1];
    regs.sp -= 2;
    regs.sp[-1] = rval;
}
END_CASE(JSOP_SETXMLNAME)

BEGIN_CASE(JSOP_CALLXMLNAME)
BEGIN_CASE(JSOP_XMLNAME)
{
    JS_ASSERT(!script->strictModeCode);

    Value lval = regs.sp[-1];
    RootedObject &obj = rootObject0;
    RootedId &id = rootId0;
    if (!js_FindXMLProperty(cx, lval, &obj, id.address()))
        goto error;
    RootedValue &rval = rootValue0;
    if (!JSObject::getGeneric(cx, obj, obj, id, &rval))
        goto error;
    regs.sp[-1] = rval;
    if (op == JSOP_CALLXMLNAME) {
        Value v;
        if (!ComputeImplicitThis(cx, obj, &v))
            goto error;
        PUSH_COPY(v);
    }
}
END_CASE(JSOP_XMLNAME)

BEGIN_CASE(JSOP_DESCENDANTS)
BEGIN_CASE(JSOP_DELDESC)
{
    JS_ASSERT(!script->strictModeCode);

    JSObject *obj;
    FETCH_OBJECT(cx, -2, obj);
    jsval rval = regs.sp[-1];
    if (!js_GetXMLDescendants(cx, obj, rval, &rval))
        goto error;

    if (op == JSOP_DELDESC) {
        regs.sp[-1] = rval;   /* set local root */
        if (!js_DeleteXMLListElements(cx, JSVAL_TO_OBJECT(rval)))
            goto error;
        rval = JSVAL_TRUE;                  /* always succeed */
    }

    regs.sp--;
    regs.sp[-1] = rval;
}
END_CASE(JSOP_DESCENDANTS)

BEGIN_CASE(JSOP_FILTER)
{
    JS_ASSERT(!script->strictModeCode);

    /*
     * We push the hole value before jumping to [enditer] so we can detect the
     * first iteration and direct js_StepXMLListFilter to initialize filter's
     * state.
     */
    PUSH_HOLE();
    len = GET_JUMP_OFFSET(regs.pc);
    JS_ASSERT(len > 0);
}
END_VARLEN_CASE

BEGIN_CASE(JSOP_ENDFILTER)
{
    JS_ASSERT(!script->strictModeCode);

    bool cond = !regs.sp[-1].isMagic();
    if (cond) {
        /* Exit the "with" block left from the previous iteration. */
        regs.fp()->popWith(cx);
    }
    if (!js_StepXMLListFilter(cx, cond))
        goto error;
    if (!regs.sp[-1].isNull()) {
        /*
         * Decrease sp after EnterWith returns as we use sp[-1] there to root
         * temporaries.
         */
        JS_ASSERT(IsXML(regs.sp[-1]));
        if (!EnterWith(cx, -2))
            goto error;
        regs.sp--;
        len = GET_JUMP_OFFSET(regs.pc);
        JS_ASSERT(len < 0);
        BRANCH(len);
    }
    regs.sp--;
}
END_CASE(JSOP_ENDFILTER);

BEGIN_CASE(JSOP_TOXML)
{
    JS_ASSERT(!script->strictModeCode);

    cx->runtime->gcExactScanningEnabled = false;

    Value rval = regs.sp[-1];
    JSObject *obj = js_ValueToXMLObject(cx, rval);
    if (!obj)
        goto error;
    regs.sp[-1].setObject(*obj);
}
END_CASE(JSOP_TOXML)

BEGIN_CASE(JSOP_TOXMLLIST)
{
    JS_ASSERT(!script->strictModeCode);

    Value rval = regs.sp[-1];
    JSObject *obj = js_ValueToXMLListObject(cx, rval);
    if (!obj)
        goto error;
    regs.sp[-1].setObject(*obj);
}
END_CASE(JSOP_TOXMLLIST)

BEGIN_CASE(JSOP_XMLTAGEXPR)
{
    JS_ASSERT(!script->strictModeCode);

    Value rval = regs.sp[-1];
    JSString *str = ToString(cx, rval);
    if (!str)
        goto error;
    regs.sp[-1].setString(str);
}
END_CASE(JSOP_XMLTAGEXPR)

BEGIN_CASE(JSOP_XMLELTEXPR)
{
    JS_ASSERT(!script->strictModeCode);

    Value rval = regs.sp[-1];
    JSString *str;
    if (IsXML(rval)) {
        str = js_ValueToXMLString(cx, rval);
    } else {
        str = ToString(cx, rval);
        if (str)
            str = js_EscapeElementValue(cx, str);
    }
    if (!str)
        goto error;
    regs.sp[-1].setString(str);
}
END_CASE(JSOP_XMLELTEXPR)

BEGIN_CASE(JSOP_XMLCDATA)
{
    JS_ASSERT(!script->strictModeCode);

    JSAtom *atom = script->getAtom(regs.pc);
    JSObject *obj = js_NewXMLSpecialObject(cx, JSXML_CLASS_TEXT, NULL, atom);
    if (!obj)
        goto error;
    PUSH_OBJECT(*obj);
}
END_CASE(JSOP_XMLCDATA)

BEGIN_CASE(JSOP_XMLCOMMENT)
{
    JS_ASSERT(!script->strictModeCode);

    JSAtom *atom = script->getAtom(regs.pc);
    JSObject *obj = js_NewXMLSpecialObject(cx, JSXML_CLASS_COMMENT, NULL, atom);
    if (!obj)
        goto error;
    PUSH_OBJECT(*obj);
}
END_CASE(JSOP_XMLCOMMENT)

BEGIN_CASE(JSOP_XMLPI)
{
    JS_ASSERT(!script->strictModeCode);

    JSAtom *atom = script->getAtom(regs.pc);
    Value rval = regs.sp[-1];
    JSString *str2 = rval.toString();
    JSObject *obj = js_NewXMLSpecialObject(cx, JSXML_CLASS_PROCESSING_INSTRUCTION, atom, str2);
    if (!obj)
        goto error;
    regs.sp[-1].setObject(*obj);
}
END_CASE(JSOP_XMLPI)

BEGIN_CASE(JSOP_GETFUNNS)
{
    JS_ASSERT(!script->strictModeCode);

    Value rval;
    if (!cx->fp()->global().getFunctionNamespace(cx, &rval))
        goto error;
    PUSH_COPY(rval);
}
END_CASE(JSOP_GETFUNNS)
#endif /* JS_HAS_XML_SUPPORT */

BEGIN_CASE(JSOP_ENTERBLOCK)
BEGIN_CASE(JSOP_ENTERLET0)
BEGIN_CASE(JSOP_ENTERLET1)
{
    StaticBlockObject &blockObj = script->getObject(regs.pc)->asStaticBlock();

    if (op == JSOP_ENTERBLOCK) {
        JS_ASSERT(regs.stackDepth() == blockObj.stackDepth());
        JS_ASSERT(regs.stackDepth() + blockObj.slotCount() <= script->nslots);
        Value *vp = regs.sp + blockObj.slotCount();
        SetValueRangeToUndefined(regs.sp, vp);
        regs.sp = vp;
    }

    /* Clone block iff there are any closed-over variables. */
    if (!regs.fp()->pushBlock(cx, blockObj))
        goto error;
}
END_CASE(JSOP_ENTERBLOCK)

BEGIN_CASE(JSOP_LEAVEBLOCK)
BEGIN_CASE(JSOP_LEAVEFORLETIN)
BEGIN_CASE(JSOP_LEAVEBLOCKEXPR)
{
    DebugOnly<uint32_t> blockDepth = regs.fp()->blockChain().stackDepth();

    regs.fp()->popBlock(cx);

    if (op == JSOP_LEAVEBLOCK) {
        /* Pop the block's slots. */
        regs.sp -= GET_UINT16(regs.pc);
        JS_ASSERT(regs.stackDepth() == blockDepth);
    } else if (op == JSOP_LEAVEBLOCKEXPR) {
        /* Pop the block's slots maintaining the topmost expr. */
        Value *vp = &regs.sp[-1];
        regs.sp -= GET_UINT16(regs.pc);
        JS_ASSERT(regs.stackDepth() == blockDepth + 1);
        regs.sp[-1] = *vp;
    } else {
        /* Another op will pop; nothing to do here. */
        len = JSOP_LEAVEFORLETIN_LENGTH;
        DO_NEXT_OP(len);
    }
}
END_CASE(JSOP_LEAVEBLOCK)

#if JS_HAS_GENERATORS
BEGIN_CASE(JSOP_GENERATOR)
{
    JS_ASSERT(!cx->isExceptionPending());
    regs.fp()->initGeneratorFrame();
    regs.pc += JSOP_GENERATOR_LENGTH;
    JSObject *obj = js_NewGenerator(cx);
    if (!obj)
        goto error;
    regs.fp()->setReturnValue(ObjectValue(*obj));
    regs.fp()->setYielding();
    interpReturnOK = true;
    if (entryFrame != regs.fp())
        goto inline_return;
    goto exit;
}

BEGIN_CASE(JSOP_YIELD)
    JS_ASSERT(!cx->isExceptionPending());
    JS_ASSERT(regs.fp()->isNonEvalFunctionFrame());
    if (cx->innermostGenerator()->state == JSGEN_CLOSING) {
        RootedValue &val = rootValue0;
        val.setObject(regs.fp()->callee());
        js_ReportValueError(cx, JSMSG_BAD_GENERATOR_YIELD, JSDVG_SEARCH_STACK, val, NullPtr());
        goto error;
    }
    regs.fp()->setReturnValue(regs.sp[-1]);
    regs.fp()->setYielding();
    regs.pc += JSOP_YIELD_LENGTH;
    interpReturnOK = true;
    goto exit;

BEGIN_CASE(JSOP_ARRAYPUSH)
{
    uint32_t slot = GET_UINT16(regs.pc);
    JS_ASSERT(script->nfixed <= slot);
    JS_ASSERT(slot < script->nslots);
    RootedObject &obj = rootObject0;
    obj = &regs.fp()->unaliasedLocal(slot).toObject();
    if (!js_NewbornArrayPush(cx, obj, regs.sp[-1]))
        goto error;
    regs.sp--;
}
END_CASE(JSOP_ARRAYPUSH)
#endif /* JS_HAS_GENERATORS */

#if JS_THREADED_INTERP
  L_JSOP_BACKPATCH:
  L_JSOP_BACKPATCH_POP:

# if !JS_HAS_GENERATORS
  L_JSOP_GENERATOR:
  L_JSOP_YIELD:
  L_JSOP_ARRAYPUSH:
# endif

# if !JS_HAS_DESTRUCTURING
  L_JSOP_ENUMCONSTELEM:
# endif

# if !JS_HAS_XML_SUPPORT
  L_JSOP_CALLXMLNAME:
  L_JSOP_STARTXMLEXPR:
  L_JSOP_STARTXML:
  L_JSOP_DELDESC:
  L_JSOP_GETFUNNS:
  L_JSOP_XMLPI:
  L_JSOP_XMLCOMMENT:
  L_JSOP_XMLCDATA:
  L_JSOP_XMLELTEXPR:
  L_JSOP_XMLTAGEXPR:
  L_JSOP_TOXMLLIST:
  L_JSOP_TOXML:
  L_JSOP_ENDFILTER:
  L_JSOP_FILTER:
  L_JSOP_DESCENDANTS:
  L_JSOP_XMLNAME:
  L_JSOP_SETXMLNAME:
  L_JSOP_BINDXMLNAME:
  L_JSOP_ADDATTRVAL:
  L_JSOP_ADDATTRNAME:
  L_JSOP_TOATTRVAL:
  L_JSOP_TOATTRNAME:
  L_JSOP_QNAME:
  L_JSOP_QNAMECONST:
  L_JSOP_ANYNAME:
  L_JSOP_DEFXMLNS:
# endif

#endif /* !JS_THREADED_INTERP */
#if !JS_THREADED_INTERP
          default:
#endif
          {
            char numBuf[12];
            JS_snprintf(numBuf, sizeof numBuf, "%d", op);
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_BAD_BYTECODE, numBuf);
            goto error;
          }

#if !JS_THREADED_INTERP
        } /* switch (op) */
    } /* for (;;) */
#endif /* !JS_THREADED_INTERP */

  error:
    JS_ASSERT(&cx->regs() == &regs);
    JS_ASSERT(uint32_t(regs.pc - script->code) < script->length);

    /* When rejoining, we must not err before finishing Interpret's prologue. */
    JS_ASSERT(interpMode != JSINTERP_REJOIN);

    if (cx->isExceptionPending()) {
        /* Call debugger throw hook if set. */
        if (cx->runtime->debugHooks.throwHook || !cx->compartment->getDebuggees().empty()) {
            Value rval;
            JSTrapStatus st = Debugger::onExceptionUnwind(cx, &rval);
            if (st == JSTRAP_CONTINUE) {
                if (JSThrowHook handler = cx->runtime->debugHooks.throwHook)
                    st = handler(cx, script, regs.pc, &rval, cx->runtime->debugHooks.throwHookData);
            }

            switch (st) {
              case JSTRAP_ERROR:
                cx->clearPendingException();
                goto error;
              case JSTRAP_RETURN:
                cx->clearPendingException();
                regs.fp()->setReturnValue(rval);
                interpReturnOK = true;
                goto forced_return;
              case JSTRAP_THROW:
                cx->setPendingException(rval);
              case JSTRAP_CONTINUE:
              default:;
            }
        }

        for (TryNoteIter tni(regs); !tni.done(); ++tni) {
            JSTryNote *tn = *tni;

            UnwindScope(cx, tn->stackDepth);

            /*
             * Set pc to the first bytecode after the the try note to point
             * to the beginning of catch or finally or to [enditer] closing
             * the for-in loop.
             */
            regs.pc = (script)->main() + tn->start + tn->length;
            regs.sp = regs.spForStackDepth(tn->stackDepth);

            switch (tn->kind) {
              case JSTRY_CATCH:
                  JS_ASSERT(*regs.pc == JSOP_ENTERBLOCK);

#if JS_HAS_GENERATORS
                /* Catch cannot intercept the closing of a generator. */
                  if (JS_UNLIKELY(cx->getPendingException().isMagic(JS_GENERATOR_CLOSING)))
                    break;
#endif

                /*
                 * Don't clear exceptions to save cx->exception from GC
                 * until it is pushed to the stack via [exception] in the
                 * catch block.
                 */
                len = 0;
                DO_NEXT_OP(len);

              case JSTRY_FINALLY:
                /*
                 * Push (true, exception) pair for finally to indicate that
                 * [retsub] should rethrow the exception.
                 */
                PUSH_BOOLEAN(true);
                PUSH_COPY(cx->getPendingException());
                cx->clearPendingException();
                len = 0;
                DO_NEXT_OP(len);

              case JSTRY_ITER: {
                /* This is similar to JSOP_ENDITER in the interpreter loop. */
                JS_ASSERT(JSOp(*regs.pc) == JSOP_ENDITER);
                bool ok = UnwindIteratorForException(cx, &regs.sp[-1].toObject());
                regs.sp -= 1;
                if (!ok)
                    goto error;
              }
           }
        }

        /*
         * Propagate the exception or error to the caller unless the exception
         * is an asynchronous return from a generator.
         */
        interpReturnOK = false;
#if JS_HAS_GENERATORS
        if (JS_UNLIKELY(cx->isExceptionPending() &&
                        cx->getPendingException().isMagic(JS_GENERATOR_CLOSING))) {
            cx->clearPendingException();
            interpReturnOK = true;
            regs.fp()->clearReturnValue();
        }
#endif
    } else {
        UnwindForUncatchableException(cx, regs);
        interpReturnOK = false;
    }

  forced_return:
    UnwindScope(cx, 0);
    regs.setToEndOfScript();

    if (entryFrame != regs.fp())
        goto inline_return;

  exit:
    if (cx->compartment->debugMode())
        interpReturnOK = ScriptDebugEpilogue(cx, regs.fp(), interpReturnOK);
    if (!regs.fp()->isYielding())
        regs.fp()->epilogue(cx);
    else
        Probes::exitScript(cx, script, script->function(), regs.fp());
    regs.fp()->setFinishedInInterpreter();

#ifdef JS_METHODJIT
    /*
     * This path is used when it's guaranteed the method can be finished
     * inside the JIT.
     */
  leave_on_safe_point:
#endif

    gc::MaybeVerifyBarriers(cx, true);
    return interpReturnOK;
}
