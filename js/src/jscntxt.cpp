/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=80:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS execution context.
 */

#include <limits.h> /* make sure that <features.h> is included and we can use
                       __GLIBC__ to detect glibc presence */
#include <new>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifdef ANDROID
# include <android/log.h>
# include <fstream>
# include <string>
#endif  // ANDROID

#include "jstypes.h"
#include "jsutil.h"
#include "jsclist.h"
#include "jsprf.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsversion.h"
#include "jsdbgapi.h"
#include "jsexn.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsiter.h"
#include "jslock.h"
#include "jsmath.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jspubtd.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstr.h"

#ifdef JS_METHODJIT
# include "assembler/assembler/MacroAssembler.h"
# include "methodjit/MethodJIT.h"
#endif
#include "gc/Marking.h"
#include "js/MemoryMetrics.h"
#include "frontend/TokenStream.h"
#include "frontend/ParseMaps.h"
#include "yarr/BumpPointerAllocator.h"

#include "jsatominlines.h"
#include "jscntxtinlines.h"
#include "jscompartment.h"
#include "jsobjinlines.h"

#include "selfhosted.out.h"

using namespace js;
using namespace js::gc;

struct CallbackData
{
    CallbackData(JSMallocSizeOfFun f) : mallocSizeOf(f), n(0) {}
    JSMallocSizeOfFun mallocSizeOf;
    size_t n;
};

void CompartmentCallback(JSRuntime *rt, void *vdata, JSCompartment *compartment)
{
    CallbackData *data = (CallbackData *) vdata;
    data->n += data->mallocSizeOf(compartment);
}

void
JSRuntime::sizeOfIncludingThis(JSMallocSizeOfFun mallocSizeOf, RuntimeSizes *runtime)
{
    runtime->object = mallocSizeOf(this);

    runtime->atomsTable = atomState.atoms.sizeOfExcludingThis(mallocSizeOf);

    runtime->contexts = 0;
    for (ContextIter acx(this); !acx.done(); acx.next())
        runtime->contexts += acx->sizeOfIncludingThis(mallocSizeOf);

    runtime->dtoa = mallocSizeOf(dtoaState);

    runtime->temporary = tempLifoAlloc.sizeOfExcludingThis(mallocSizeOf);

    if (execAlloc_)
        execAlloc_->sizeOfCode(&runtime->mjitCode, &runtime->regexpCode,
                               &runtime->unusedCodeMemory);
    else
        runtime->mjitCode = runtime->regexpCode = runtime->unusedCodeMemory = 0;

    runtime->stackCommitted = stackSpace.sizeOfCommitted();

    runtime->gcMarker = gcMarker.sizeOfExcludingThis(mallocSizeOf);

    runtime->mathCache = mathCache_ ? mathCache_->sizeOfIncludingThis(mallocSizeOf) : 0;

    runtime->scriptFilenames = scriptFilenameTable.sizeOfExcludingThis(mallocSizeOf);
    for (ScriptFilenameTable::Range r = scriptFilenameTable.all(); !r.empty(); r.popFront())
        runtime->scriptFilenames += mallocSizeOf(r.front());

    runtime->compartmentObjects = 0;
    CallbackData data(mallocSizeOf);
    JS_IterateCompartments(this, &data, CompartmentCallback);
    runtime->compartmentObjects = data.n;
}

size_t
JSRuntime::sizeOfExplicitNonHeap()
{
    if (!execAlloc_)
        return 0;

    size_t mjitCode, regexpCode, unusedCodeMemory;
    execAlloc_->sizeOfCode(&mjitCode, &regexpCode, &unusedCodeMemory);
    return mjitCode + regexpCode + unusedCodeMemory + stackSpace.sizeOfCommitted();
}

void
JSRuntime::triggerOperationCallback()
{
    /*
     * Use JS_ATOMIC_SET in the hope that it ensures the write will become
     * immediately visible to other processors polling the flag.
     */
    JS_ATOMIC_SET(&interrupt, 1);
}

void
JSRuntime::setJitHardening(bool enabled)
{
    jitHardening = enabled;
    if (execAlloc_)
        execAlloc_->setRandomize(enabled);
}

JSC::ExecutableAllocator *
JSRuntime::createExecutableAllocator(JSContext *cx)
{
    JS_ASSERT(!execAlloc_);
    JS_ASSERT(cx->runtime == this);

    JSC::AllocationBehavior randomize =
        jitHardening ? JSC::AllocationCanRandomize : JSC::AllocationDeterministic;
    execAlloc_ = new_<JSC::ExecutableAllocator>(randomize);
    if (!execAlloc_)
        js_ReportOutOfMemory(cx);
    return execAlloc_;
}

WTF::BumpPointerAllocator *
JSRuntime::createBumpPointerAllocator(JSContext *cx)
{
    JS_ASSERT(!bumpAlloc_);
    JS_ASSERT(cx->runtime == this);

    bumpAlloc_ = new_<WTF::BumpPointerAllocator>();
    if (!bumpAlloc_)
        js_ReportOutOfMemory(cx);
    return bumpAlloc_;
}

MathCache *
JSRuntime::createMathCache(JSContext *cx)
{
    JS_ASSERT(!mathCache_);
    JS_ASSERT(cx->runtime == this);

    MathCache *newMathCache = new_<MathCache>();
    if (!newMathCache) {
        js_ReportOutOfMemory(cx);
        return NULL;
    }

    mathCache_ = newMathCache;
    return mathCache_;
}

#ifdef JS_METHODJIT
mjit::JaegerRuntime *
JSRuntime::createJaegerRuntime(JSContext *cx)
{
    JS_ASSERT(!jaegerRuntime_);
    JS_ASSERT(cx->runtime == this);

    mjit::JaegerRuntime *jr = new_<mjit::JaegerRuntime>();
    if (!jr || !jr->init(cx)) {
        js_ReportOutOfMemory(cx);
        delete_(jr);
        return NULL;
    }

    jaegerRuntime_ = jr;
    return jaegerRuntime_;
}
#endif


static JSClass self_hosting_global_class = {
    "self-hosting-global", JSCLASS_GLOBAL_FLAGS,
    JS_PropertyStub,  JS_PropertyStub,
    JS_PropertyStub,  JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub,
    JS_ConvertStub,   NULL
};
bool
JSRuntime::initSelfHosting(JSContext *cx)
{
    JS_ASSERT(!selfHostedGlobal_);
    RootedObject savedGlobal(cx, JS_GetGlobalObject(cx));
    if (!(selfHostedGlobal_ = JS_NewGlobalObject(cx, &self_hosting_global_class, NULL)))
        return false;
    JS_SetGlobalObject(cx, selfHostedGlobal_);

    const char *src = selfhosted::raw_sources;
    uint32_t srcLen = selfhosted::GetRawScriptsSize();

    CompileOptions options(cx);
    options.setFileAndLine("self-hosted", 1);
    options.setSelfHostingMode(true);

    RootedObject shg(cx, selfHostedGlobal_);
    Value rv;
    if (!Evaluate(cx, shg, options, src, srcLen, &rv))
        return false;

    JS_SetGlobalObject(cx, savedGlobal);
    return true;
}

void
JSRuntime::markSelfHostedGlobal(JSTracer *trc)
{
    MarkObjectRoot(trc, &selfHostedGlobal_, "self-hosting global");
}

JSFunction *
JSRuntime::getSelfHostedFunction(JSContext *cx, const char *name)
{
    RootedObject holder(cx, cx->global()->getIntrinsicsHolder());
    JSAtom *atom = Atomize(cx, name, strlen(name));
    if (!atom)
        return NULL;
    Value funVal = NullValue();
    JSAutoByteString bytes;
    if (!cloneSelfHostedValueById(cx, AtomToId(atom), holder, &funVal))
        return NULL;
    return funVal.toObject().toFunction();
}

bool
JSRuntime::cloneSelfHostedValueById(JSContext *cx, jsid id, HandleObject holder, Value *vp)
{
    Value funVal;
    {
        RootedObject shg(cx, selfHostedGlobal_);
        AutoCompartment ac(cx, shg);
        if (!JS_GetPropertyById(cx, shg, id, &funVal) || !funVal.isObject())
            return false;
    }

    RootedObject clone(cx, JS_CloneFunctionObject(cx, &funVal.toObject(), cx->global()));
    if (!clone)
        return false;

    vp->setObjectOrNull(clone);
    DebugOnly<bool> ok = JS_DefinePropertyById(cx, holder, id, *vp, NULL, NULL, 0);
    JS_ASSERT(ok);
    return true;
}

JSContext *
js::NewContext(JSRuntime *rt, size_t stackChunkSize)
{
    JS_AbortIfWrongThread(rt);

    JSContext *cx = OffTheBooks::new_<JSContext>(rt);
    if (!cx)
        return NULL;

    JS_ASSERT(cx->findVersion() == JSVERSION_DEFAULT);

    if (!cx->busyArrays.init()) {
        Foreground::delete_(cx);
        return NULL;
    }

    /*
     * Here the GC lock is still held after js_InitContextThreadAndLockGC took it and
     * the GC is not running on another thread.
     */
    bool first = JS_CLIST_IS_EMPTY(&rt->contextList);
    JS_APPEND_LINK(&cx->link, &rt->contextList);

    js_InitRandom(cx);

    /*
     * If cx is the first context on this runtime, initialize well-known atoms,
     * keywords, numbers, strings and self-hosted scripts. If one of these
     * steps should fail, the runtime will be left in a partially initialized
     * state, with zeroes and nulls stored in the default-initialized remainder
     * of the struct. We'll clean the runtime up under DestroyContext, because
     * cx will be "last" as well as "first".
     */
    if (first) {
#ifdef JS_THREADSAFE
        JS_BeginRequest(cx);
#endif
        bool ok = rt->staticStrings.init(cx);
        if (ok)
            ok = InitCommonAtoms(cx);
        if (ok)
            ok = rt->initSelfHosting(cx);

#ifdef JS_THREADSAFE
        JS_EndRequest(cx);
#endif
        if (!ok) {
            DestroyContext(cx, DCM_NEW_FAILED);
            return NULL;
        }
    }

    JSContextCallback cxCallback = rt->cxCallback;
    if (cxCallback && !cxCallback(cx, JSCONTEXT_NEW)) {
        DestroyContext(cx, DCM_NEW_FAILED);
        return NULL;
    }

    return cx;
}

void
js::DestroyContext(JSContext *cx, DestroyContextMode mode)
{
    JSRuntime *rt = cx->runtime;
    JS_AbortIfWrongThread(rt);

    JS_ASSERT(!cx->enumerators);

#ifdef JS_THREADSAFE
    JS_ASSERT(cx->outstandingRequests == 0);
#endif

    if (mode != DCM_NEW_FAILED) {
        if (JSContextCallback cxCallback = rt->cxCallback) {
            /*
             * JSCONTEXT_DESTROY callback is not allowed to fail and must
             * return true.
             */
            JS_ALWAYS_TRUE(cxCallback(cx, JSCONTEXT_DESTROY));
        }
    }

    JS_REMOVE_LINK(&cx->link);
    bool last = !rt->hasContexts();
    if (last) {
        JS_ASSERT(!rt->isHeapBusy());

        /*
         * Dump remaining type inference results first. This printing
         * depends on atoms still existing.
         */
        for (CompartmentsIter c(rt); !c.done(); c.next())
            c->types.print(cx, false);

        /* Unpin all common atoms before final GC. */
        FinishCommonAtoms(rt);

        /* Clear debugging state to remove GC roots. */
        for (CompartmentsIter c(rt); !c.done(); c.next())
            c->clearTraps(rt->defaultFreeOp());
        JS_ClearAllWatchPoints(cx);

        PrepareForFullGC(rt);
        GC(rt, GC_NORMAL, gcreason::LAST_CONTEXT);
    } else if (mode == DCM_FORCE_GC) {
        JS_ASSERT(!rt->isHeapBusy());
        PrepareForFullGC(rt);
        GC(rt, GC_NORMAL, gcreason::DESTROY_CONTEXT);
    }
    Foreground::delete_(cx);
}

namespace js {

bool
AutoResolving::alreadyStartedSlow() const
{
    JS_ASSERT(link);
    AutoResolving *cursor = link;
    do {
        JS_ASSERT(this != cursor);
        if (object.get() == cursor->object && id.get() == cursor->id && kind == cursor->kind)
            return true;
    } while (!!(cursor = cursor->link));
    return false;
}

} /* namespace js */

static void
ReportError(JSContext *cx, const char *message, JSErrorReport *reportp,
            JSErrorCallback callback, void *userRef)
{
    /*
     * Check the error report, and set a JavaScript-catchable exception
     * if the error is defined to have an associated exception.  If an
     * exception is thrown, then the JSREPORT_EXCEPTION flag will be set
     * on the error report, and exception-aware hosts should ignore it.
     */
    JS_ASSERT(reportp);
    if ((!callback || callback == js_GetErrorMessage) &&
        reportp->errorNumber == JSMSG_UNCAUGHT_EXCEPTION)
        reportp->flags |= JSREPORT_EXCEPTION;

    /*
     * Call the error reporter only if an exception wasn't raised.
     *
     * If an exception was raised, then we call the debugErrorHook
     * (if present) to give it a chance to see the error before it
     * propagates out of scope.  This is needed for compatibility
     * with the old scheme.
     */
    if (!JS_IsRunning(cx) ||
        !js_ErrorToException(cx, message, reportp, callback, userRef)) {
        js_ReportErrorAgain(cx, message, reportp);
    } else if (JSDebugErrorHook hook = cx->runtime->debugHooks.debugErrorHook) {
        /*
         * If we've already chewed up all the C stack, don't call into the
         * error reporter since this may trigger an infinite recursion where
         * the reporter triggers an over-recursion.
         */
        int stackDummy;
        if (!JS_CHECK_STACK_SIZE(cx->runtime->nativeStackLimit, &stackDummy))
            return;

        if (cx->errorReporter)
            hook(cx, message, reportp, cx->runtime->debugHooks.debugErrorHookData);
    }
}

/*
 * The given JSErrorReport object have been zeroed and must not outlive
 * cx->fp() (otherwise report->originPrincipals may become invalid).
 */
static void
PopulateReportBlame(JSContext *cx, JSErrorReport *report)
{
    /*
     * Walk stack until we find a frame that is associated with a non-builtin
     * rather than a builtin frame.
     */
    NonBuiltinScriptFrameIter iter(cx);
    if (iter.done())
        return;

    report->filename = iter.script()->filename;
    report->lineno = PCToLineNumber(iter.script(), iter.pc(), &report->column);
    report->originPrincipals = iter.script()->originPrincipals;
}

/*
 * We don't post an exception in this case, since doing so runs into
 * complications of pre-allocating an exception object which required
 * running the Exception class initializer early etc.
 * Instead we just invoke the errorReporter with an "Out Of Memory"
 * type message, and then hope the process ends swiftly.
 */
void
js_ReportOutOfMemory(JSContext *cx)
{
    cx->runtime->hadOutOfMemory = true;

    JSErrorReport report;
    JSErrorReporter onError = cx->errorReporter;

    /* Get the message for this error, but we won't expand any arguments. */
    const JSErrorFormatString *efs =
        js_GetLocalizedErrorMessage(cx, NULL, NULL, JSMSG_OUT_OF_MEMORY);
    const char *msg = efs ? efs->format : "Out of memory";

    /* Fill out the report, but don't do anything that requires allocation. */
    PodZero(&report);
    report.flags = JSREPORT_ERROR;
    report.errorNumber = JSMSG_OUT_OF_MEMORY;
    PopulateReportBlame(cx, &report);

    /*
     * If debugErrorHook is present then we give it a chance to veto sending
     * the error on to the regular ErrorReporter. We also clear a pending
     * exception if any now so the hooks can replace the out-of-memory error
     * by a script-catchable exception.
     */
    cx->clearPendingException();
    if (onError) {
        JSDebugErrorHook hook = cx->runtime->debugHooks.debugErrorHook;
        if (hook &&
            !hook(cx, msg, &report, cx->runtime->debugHooks.debugErrorHookData)) {
            onError = NULL;
        }
    }

    if (onError) {
        AutoAtomicIncrement incr(&cx->runtime->inOOMReport);
        onError(cx, msg, &report);
    }
}

JS_FRIEND_API(void)
js_ReportOverRecursed(JSContext *maybecx)
{
#ifdef JS_MORE_DETERMINISTIC
    /*
     * We cannot make stack depth deterministic across different
     * implementations (e.g. JIT vs. interpreter will differ in
     * their maximum stack depth).
     * However, we can detect externally when we hit the maximum
     * stack depth which is useful for external testing programs
     * like fuzzers.
     */
    fprintf(stderr, "js_ReportOverRecursed called\n");
#endif
    if (maybecx)
        JS_ReportErrorNumber(maybecx, js_GetErrorMessage, NULL, JSMSG_OVER_RECURSED);
}

void
js_ReportAllocationOverflow(JSContext *maybecx)
{
    if (maybecx)
        JS_ReportErrorNumber(maybecx, js_GetErrorMessage, NULL, JSMSG_ALLOC_OVERFLOW);
}

/*
 * Given flags and the state of cx, decide whether we should report an
 * error, a warning, or just continue execution normally.  Return
 * true if we should continue normally, without reporting anything;
 * otherwise, adjust *flags as appropriate and return false.
 */
static bool
checkReportFlags(JSContext *cx, unsigned *flags)
{
    if (JSREPORT_IS_STRICT_MODE_ERROR(*flags)) {
        /*
         * Error in strict code; warning with strict option; okay otherwise.
         * We assume that if the top frame is a native, then it is strict if
         * the nearest scripted frame is strict, see bug 536306.
         */
        JSScript *script = cx->stack.currentScript();
        if (script && script->strictModeCode)
            *flags &= ~JSREPORT_WARNING;
        else if (cx->hasStrictOption())
            *flags |= JSREPORT_WARNING;
        else
            return true;
    } else if (JSREPORT_IS_STRICT(*flags)) {
        /* Warning/error only when JSOPTION_STRICT is set. */
        if (!cx->hasStrictOption())
            return true;
    }

    /* Warnings become errors when JSOPTION_WERROR is set. */
    if (JSREPORT_IS_WARNING(*flags) && cx->hasWErrorOption())
        *flags &= ~JSREPORT_WARNING;

    return false;
}

JSBool
js_ReportErrorVA(JSContext *cx, unsigned flags, const char *format, va_list ap)
{
    char *message;
    jschar *ucmessage;
    size_t messagelen;
    JSErrorReport report;
    JSBool warning;

    if (checkReportFlags(cx, &flags))
        return JS_TRUE;

    message = JS_vsmprintf(format, ap);
    if (!message)
        return JS_FALSE;
    messagelen = strlen(message);

    PodZero(&report);
    report.flags = flags;
    report.errorNumber = JSMSG_USER_DEFINED_ERROR;
    report.ucmessage = ucmessage = InflateString(cx, message, &messagelen);
    PopulateReportBlame(cx, &report);

    warning = JSREPORT_IS_WARNING(report.flags);

    ReportError(cx, message, &report, NULL, NULL);
    Foreground::free_(message);
    Foreground::free_(ucmessage);
    return warning;
}

namespace js {

/* |callee| requires a usage string provided by JS_DefineFunctionsWithHelp. */
void
ReportUsageError(JSContext *cx, HandleObject callee, const char *msg)
{
    const char *usageStr = "usage";
    PropertyName *usageAtom = Atomize(cx, usageStr, strlen(usageStr))->asPropertyName();
    DebugOnly<Shape *> shape = callee->nativeLookup(cx, NameToId(usageAtom));
    JS_ASSERT(!shape->configurable());
    JS_ASSERT(!shape->writable());
    JS_ASSERT(shape->hasDefaultGetter());

    jsval usage;
    if (!JS_LookupProperty(cx, callee, "usage", &usage))
        return;

    if (JSVAL_IS_VOID(usage)) {
        JS_ReportError(cx, "%s", msg);
    } else {
        JSString *str = JSVAL_TO_STRING(usage);
        JS::Anchor<JSString *> a_str(str);
        const jschar *chars = JS_GetStringCharsZ(cx, str);
        if (!chars)
            return;
        JS_ReportError(cx, "%s. Usage: %hs", msg, chars);
    }
}

} /* namespace js */

/*
 * The arguments from ap need to be packaged up into an array and stored
 * into the report struct.
 *
 * The format string addressed by the error number may contain operands
 * identified by the format {N}, where N is a decimal digit. Each of these
 * is to be replaced by the Nth argument from the va_list. The complete
 * message is placed into reportp->ucmessage converted to a JSString.
 *
 * Returns true if the expansion succeeds (can fail if out of memory).
 */
JSBool
js_ExpandErrorArguments(JSContext *cx, JSErrorCallback callback,
                        void *userRef, const unsigned errorNumber,
                        char **messagep, JSErrorReport *reportp,
                        bool charArgs, va_list ap)
{
    const JSErrorFormatString *efs;
    int i;
    int argCount;

    *messagep = NULL;

    /* Most calls supply js_GetErrorMessage; if this is so, assume NULL. */
    if (!callback || callback == js_GetErrorMessage)
        efs = js_GetLocalizedErrorMessage(cx, userRef, NULL, errorNumber);
    else
        efs = callback(userRef, NULL, errorNumber);
    if (efs) {
        reportp->exnType = efs->exnType;

        size_t totalArgsLength = 0;
        size_t argLengths[10]; /* only {0} thru {9} supported */
        argCount = efs->argCount;
        JS_ASSERT(argCount <= 10);
        if (argCount > 0) {
            /*
             * Gather the arguments into an array, and accumulate
             * their sizes. We allocate 1 more than necessary and
             * null it out to act as the caboose when we free the
             * pointers later.
             */
            reportp->messageArgs = (const jschar **)
                cx->malloc_(sizeof(jschar *) * (argCount + 1));
            if (!reportp->messageArgs)
                return JS_FALSE;
            reportp->messageArgs[argCount] = NULL;
            for (i = 0; i < argCount; i++) {
                if (charArgs) {
                    char *charArg = va_arg(ap, char *);
                    size_t charArgLength = strlen(charArg);
                    reportp->messageArgs[i] = InflateString(cx, charArg, &charArgLength);
                    if (!reportp->messageArgs[i])
                        goto error;
                } else {
                    reportp->messageArgs[i] = va_arg(ap, jschar *);
                }
                argLengths[i] = js_strlen(reportp->messageArgs[i]);
                totalArgsLength += argLengths[i];
            }
            /* NULL-terminate for easy copying. */
            reportp->messageArgs[i] = NULL;
        }
        /*
         * Parse the error format, substituting the argument X
         * for {X} in the format.
         */
        if (argCount > 0) {
            if (efs->format) {
                jschar *buffer, *fmt, *out;
                int expandedArgs = 0;
                size_t expandedLength;
                size_t len = strlen(efs->format);

                buffer = fmt = InflateString(cx, efs->format, &len);
                if (!buffer)
                    goto error;
                expandedLength = len
                                 - (3 * argCount)       /* exclude the {n} */
                                 + totalArgsLength;

                /*
                * Note - the above calculation assumes that each argument
                * is used once and only once in the expansion !!!
                */
                reportp->ucmessage = out = (jschar *)
                    cx->malloc_((expandedLength + 1) * sizeof(jschar));
                if (!out) {
                    cx->free_(buffer);
                    goto error;
                }
                while (*fmt) {
                    if (*fmt == '{') {
                        if (isdigit(fmt[1])) {
                            int d = JS7_UNDEC(fmt[1]);
                            JS_ASSERT(d < argCount);
                            js_strncpy(out, reportp->messageArgs[d],
                                       argLengths[d]);
                            out += argLengths[d];
                            fmt += 3;
                            expandedArgs++;
                            continue;
                        }
                    }
                    *out++ = *fmt++;
                }
                JS_ASSERT(expandedArgs == argCount);
                *out = 0;
                cx->free_(buffer);
                *messagep = DeflateString(cx, reportp->ucmessage,
                                          size_t(out - reportp->ucmessage));
                if (!*messagep)
                    goto error;
            }
        } else {
            /*
             * Zero arguments: the format string (if it exists) is the
             * entire message.
             */
            if (efs->format) {
                size_t len;
                *messagep = JS_strdup(cx, efs->format);
                if (!*messagep)
                    goto error;
                len = strlen(*messagep);
                reportp->ucmessage = InflateString(cx, *messagep, &len);
                if (!reportp->ucmessage)
                    goto error;
            }
        }
    }
    if (*messagep == NULL) {
        /* where's the right place for this ??? */
        const char *defaultErrorMessage
            = "No error message available for error number %d";
        size_t nbytes = strlen(defaultErrorMessage) + 16;
        *messagep = (char *)cx->malloc_(nbytes);
        if (!*messagep)
            goto error;
        JS_snprintf(*messagep, nbytes, defaultErrorMessage, errorNumber);
    }
    return JS_TRUE;

error:
    if (reportp->messageArgs) {
        /* free the arguments only if we allocated them */
        if (charArgs) {
            i = 0;
            while (reportp->messageArgs[i])
                cx->free_((void *)reportp->messageArgs[i++]);
        }
        cx->free_((void *)reportp->messageArgs);
        reportp->messageArgs = NULL;
    }
    if (reportp->ucmessage) {
        cx->free_((void *)reportp->ucmessage);
        reportp->ucmessage = NULL;
    }
    if (*messagep) {
        cx->free_((void *)*messagep);
        *messagep = NULL;
    }
    return JS_FALSE;
}

JSBool
js_ReportErrorNumberVA(JSContext *cx, unsigned flags, JSErrorCallback callback,
                       void *userRef, const unsigned errorNumber,
                       JSBool charArgs, va_list ap)
{
    JSErrorReport report;
    char *message;
    JSBool warning;

    if (checkReportFlags(cx, &flags))
        return JS_TRUE;
    warning = JSREPORT_IS_WARNING(flags);

    PodZero(&report);
    report.flags = flags;
    report.errorNumber = errorNumber;
    PopulateReportBlame(cx, &report);

    if (!js_ExpandErrorArguments(cx, callback, userRef, errorNumber,
                                 &message, &report, !!charArgs, ap)) {
        return JS_FALSE;
    }

    ReportError(cx, message, &report, callback, userRef);

    if (message)
        cx->free_(message);
    if (report.messageArgs) {
        /*
         * js_ExpandErrorArguments owns its messageArgs only if it had to
         * inflate the arguments (from regular |char *|s).
         */
        if (charArgs) {
            int i = 0;
            while (report.messageArgs[i])
                cx->free_((void *)report.messageArgs[i++]);
        }
        cx->free_((void *)report.messageArgs);
    }
    if (report.ucmessage)
        cx->free_((void *)report.ucmessage);

    return warning;
}

JS_FRIEND_API(void)
js_ReportErrorAgain(JSContext *cx, const char *message, JSErrorReport *reportp)
{
    JSErrorReporter onError;

    if (!message)
        return;

    if (cx->lastMessage)
        Foreground::free_(cx->lastMessage);
    cx->lastMessage = JS_strdup(cx, message);
    if (!cx->lastMessage)
        return;
    onError = cx->errorReporter;

    /*
     * If debugErrorHook is present then we give it a chance to veto
     * sending the error on to the regular ErrorReporter.
     */
    if (onError) {
        JSDebugErrorHook hook = cx->runtime->debugHooks.debugErrorHook;
        if (hook && !hook(cx, cx->lastMessage, reportp, cx->runtime->debugHooks.debugErrorHookData))
            onError = NULL;
    }
    if (onError)
        onError(cx, cx->lastMessage, reportp);
}

void
js_ReportIsNotDefined(JSContext *cx, const char *name)
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NOT_DEFINED, name);
}

JSBool
js_ReportIsNullOrUndefined(JSContext *cx, int spindex, HandleValue v,
                           HandleString fallback)
{
    char *bytes;
    JSBool ok;

    bytes = DecompileValueGenerator(cx, spindex, v, fallback);
    if (!bytes)
        return JS_FALSE;

    if (strcmp(bytes, js_undefined_str) == 0 ||
        strcmp(bytes, js_null_str) == 0) {
        ok = JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR,
                                          js_GetErrorMessage, NULL,
                                          JSMSG_NO_PROPERTIES, bytes,
                                          NULL, NULL);
    } else if (v.isUndefined()) {
        ok = JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR,
                                          js_GetErrorMessage, NULL,
                                          JSMSG_UNEXPECTED_TYPE, bytes,
                                          js_undefined_str, NULL);
    } else {
        JS_ASSERT(v.isNull());
        ok = JS_ReportErrorFlagsAndNumber(cx, JSREPORT_ERROR,
                                          js_GetErrorMessage, NULL,
                                          JSMSG_UNEXPECTED_TYPE, bytes,
                                          js_null_str, NULL);
    }

    cx->free_(bytes);
    return ok;
}

void
js_ReportMissingArg(JSContext *cx, HandleValue v, unsigned arg)
{
    char argbuf[11];
    char *bytes;
    RootedAtom atom(cx);

    JS_snprintf(argbuf, sizeof argbuf, "%u", arg);
    bytes = NULL;
    if (IsFunctionObject(v)) {
        atom = v.toObject().toFunction()->atom();
        bytes = DecompileValueGenerator(cx, JSDVG_SEARCH_STACK,
                                        v, atom);
        if (!bytes)
            return;
    }
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                         JSMSG_MISSING_FUN_ARG, argbuf,
                         bytes ? bytes : "");
    cx->free_(bytes);
}

JSBool
js_ReportValueErrorFlags(JSContext *cx, unsigned flags, const unsigned errorNumber,
                         int spindex, HandleValue v, HandleString fallback,
                         const char *arg1, const char *arg2)
{
    char *bytes;
    JSBool ok;

    JS_ASSERT(js_ErrorFormatString[errorNumber].argCount >= 1);
    JS_ASSERT(js_ErrorFormatString[errorNumber].argCount <= 3);
    bytes = DecompileValueGenerator(cx, spindex, v, fallback);
    if (!bytes)
        return JS_FALSE;

    ok = JS_ReportErrorFlagsAndNumber(cx, flags, js_GetErrorMessage,
                                      NULL, errorNumber, bytes, arg1, arg2);
    cx->free_(bytes);
    return ok;
}

JSErrorFormatString js_ErrorFormatString[JSErr_Limit] = {
#define MSG_DEF(name, number, count, exception, format) \
    { format, count, exception } ,
#include "js.msg"
#undef MSG_DEF
};

JS_FRIEND_API(const JSErrorFormatString *)
js_GetErrorMessage(void *userRef, const char *locale, const unsigned errorNumber)
{
    if ((errorNumber > 0) && (errorNumber < JSErr_Limit))
        return &js_ErrorFormatString[errorNumber];
    return NULL;
}

JSBool
js_InvokeOperationCallback(JSContext *cx)
{
    JS_ASSERT_REQUEST_DEPTH(cx);

    JSRuntime *rt = cx->runtime;
    JS_ASSERT(rt->interrupt != 0);

    /*
     * Reset the callback counter first, then run GC and yield. If another
     * thread is racing us here we will accumulate another callback request
     * which will be serviced at the next opportunity.
     */
    JS_ATOMIC_SET(&rt->interrupt, 0);

    if (rt->gcIsNeeded)
        GCSlice(rt, GC_NORMAL, rt->gcTriggerReason);

    /*
     * Important: Additional callbacks can occur inside the callback handler
     * if it re-enters the JS engine. The embedding must ensure that the
     * callback is disconnected before attempting such re-entry.
     */
    JSOperationCallback cb = cx->operationCallback;
    return !cb || cb(cx);
}

JSBool
js_HandleExecutionInterrupt(JSContext *cx)
{
    JSBool result = JS_TRUE;
    if (cx->runtime->interrupt)
        result = js_InvokeOperationCallback(cx) && result;
    return result;
}

jsbytecode*
js_GetCurrentBytecodePC(JSContext* cx)
{
    return cx->hasfp() ? cx->regs().pc : NULL;
}

void
DSTOffsetCache::purge()
{
    /*
     * NB: The initial range values are carefully chosen to result in a cache
     *     miss on first use given the range of possible values.  Be careful
     *     to keep these values and the caching algorithm in sync!
     */
    offsetMilliseconds = 0;
    rangeStartSeconds = rangeEndSeconds = INT64_MIN;
    oldOffsetMilliseconds = 0;
    oldRangeStartSeconds = oldRangeEndSeconds = INT64_MIN;

    sanityCheck();
}

/*
 * Since getDSTOffsetMilliseconds guarantees that all times seen will be
 * positive, we can initialize the range at construction time with large
 * negative numbers to ensure the first computation is always a cache miss and
 * doesn't return a bogus offset.
 */
DSTOffsetCache::DSTOffsetCache()
{
    purge();
}

JSContext::JSContext(JSRuntime *rt)
  : ContextFriendFields(rt),
    defaultVersion(JSVERSION_DEFAULT),
    hasVersionOverride(false),
    throwing(false),
    exception(UndefinedValue()),
    runOptions(0),
    reportGranularity(JS_DEFAULT_JITREPORT_GRANULARITY),
    localeCallbacks(NULL),
    resolvingList(NULL),
    generatingError(false),
#ifdef DEBUG
    rootingUnnecessary(false),
#endif
    compartment(NULL),
    enterCompartmentDepth_(0),
    savedFrameChains_(),
    defaultCompartmentObject_(NULL),
    stack(thisDuringConstruction()),
    parseMapPool_(NULL),
    sharpObjectMap(thisDuringConstruction()),
    argumentFormatMap(NULL),
    lastMessage(NULL),
    errorReporter(NULL),
    operationCallback(NULL),
    data(NULL),
    data2(NULL),
#ifdef JS_THREADSAFE
    outstandingRequests(0),
#endif
    resolveFlags(0),
    rngSeed(0),
    iterValue(MagicValue(JS_NO_ITER_VALUE)),
#ifdef JS_METHODJIT
    methodJitEnabled(false),
#endif
#ifdef MOZ_TRACE_JSCALLS
    functionCallback(NULL),
#endif
    enumerators(NULL),
    innermostGenerator_(NULL),
#ifdef DEBUG
    stackIterAssertionEnabled(true),
#endif
    activeCompilations(0)
{
    PodZero(&link);
#ifdef JSGC_ROOT_ANALYSIS
    PodArrayZero(thingGCRooters);
#ifdef DEBUG
    skipGCRooters = NULL;
#endif
#endif
}

JSContext::~JSContext()
{
    /* Free the stuff hanging off of cx. */
    if (parseMapPool_)
        Foreground::delete_(parseMapPool_);

    if (lastMessage)
        Foreground::free_(lastMessage);

    /* Remove any argument formatters. */
    JSArgumentFormatMap *map = argumentFormatMap;
    while (map) {
        JSArgumentFormatMap *temp = map;
        map = map->next;
        Foreground::free_(temp);
    }

    JS_ASSERT(!resolvingList);
}

#ifdef DEBUG
namespace JS {

JS_FRIEND_API(void)
SetRootingUnnecessaryForContext(JSContext *cx, bool value)
{
    cx->rootingUnnecessary = value;
}

JS_FRIEND_API(bool)
IsRootingUnnecessaryForContext(JSContext *cx)
{
    return cx->rootingUnnecessary;
}

JS_FRIEND_API(bool)
RelaxRootChecksForContext(JSContext *cx)
{
    return cx->runtime->relaxRootChecks;
}

} /* namespace JS */
#endif

/*
 * Since this function is only called in the context of a pending exception,
 * the caller must subsequently take an error path. If wrapping fails, it will
 * set a new (uncatchable) exception to be used in place of the original.
 */
void
JSContext::wrapPendingException()
{
    Value v = getPendingException();
    clearPendingException();
    if (compartment->wrap(this, &v))
        setPendingException(v);
}


void
JSContext::enterGenerator(JSGenerator *gen)
{
    JS_ASSERT(!gen->prevGenerator);
    gen->prevGenerator = innermostGenerator_;
    innermostGenerator_ = gen;
}

void
JSContext::leaveGenerator(JSGenerator *gen)
{
    JS_ASSERT(innermostGenerator_ == gen);
    innermostGenerator_ = innermostGenerator_->prevGenerator;
    gen->prevGenerator = NULL;
}


bool
JSContext::runningWithTrustedPrincipals() const
{
    return !compartment || compartment->principals == runtime->trustedPrincipals();
}

bool
JSContext::saveFrameChain()
{
    if (!stack.saveFrameChain())
        return false;

    if (!savedFrameChains_.append(SavedFrameChain(compartment, enterCompartmentDepth_))) {
        stack.restoreFrameChain();
        return false;
    }

    if (defaultCompartmentObject_)
        compartment = defaultCompartmentObject_->compartment();
    else
        compartment = NULL;
    enterCompartmentDepth_ = 0;

    if (isExceptionPending())
        wrapPendingException();
    return true;
}

void
JSContext::restoreFrameChain()
{
    SavedFrameChain sfc = savedFrameChains_.popCopy();
    compartment = sfc.compartment;
    enterCompartmentDepth_ = sfc.enterCompartmentCount;

    stack.restoreFrameChain();

    if (isExceptionPending())
        wrapPendingException();
}

void
JSRuntime::setGCMaxMallocBytes(size_t value)
{
    /*
     * For compatibility treat any value that exceeds PTRDIFF_T_MAX to
     * mean that value.
     */
    gcMaxMallocBytes = (ptrdiff_t(value) >= 0) ? value : size_t(-1) >> 1;
    for (CompartmentsIter c(this); !c.done(); c.next())
        c->setGCMaxMallocBytes(value);
}

void
JSRuntime::updateMallocCounter(JSContext *cx, size_t nbytes)
{
    if (cx && cx->compartment) {
        cx->compartment->updateMallocCounter(nbytes);
    } else {
        ptrdiff_t oldCount = gcMallocBytes;
        ptrdiff_t newCount = oldCount - ptrdiff_t(nbytes);
        gcMallocBytes = newCount;
        if (JS_UNLIKELY(newCount <= 0 && oldCount > 0))
            onTooMuchMalloc();
    }
}

JS_FRIEND_API(void)
JSRuntime::onTooMuchMalloc()
{
    TriggerGC(this, gcreason::TOO_MUCH_MALLOC);
}

JS_FRIEND_API(void *)
JSRuntime::onOutOfMemory(void *p, size_t nbytes, JSContext *cx)
{
    if (isHeapBusy())
        return NULL;

    /*
     * Retry when we are done with the background sweeping and have stopped
     * all the allocations and released the empty GC chunks.
     */
    ShrinkGCBuffers(this);
    gcHelperThread.waitBackgroundSweepOrAllocEnd();
    if (!p)
        p = OffTheBooks::malloc_(nbytes);
    else if (p == reinterpret_cast<void *>(1))
        p = OffTheBooks::calloc_(nbytes);
    else
      p = OffTheBooks::realloc_(p, nbytes);
    if (p)
        return p;
    if (cx)
        js_ReportOutOfMemory(cx);
    return NULL;
}

void
JSContext::purge()
{
    if (!activeCompilations) {
        Foreground::delete_(parseMapPool_);
        parseMapPool_ = NULL;
    }
}

#if defined(JS_METHODJIT)
static bool
ComputeIsJITBroken()
{
#if !defined(ANDROID) || defined(GONK)
    return false;
#else  // ANDROID
    if (getenv("JS_IGNORE_JIT_BROKENNESS")) {
        return false;
    }

    std::string line;

    // Check for the known-bad kernel version (2.6.29).
    std::ifstream osrelease("/proc/sys/kernel/osrelease");
    std::getline(osrelease, line);
    __android_log_print(ANDROID_LOG_INFO, "Gecko", "Detected osrelease `%s'",
                        line.c_str());

    if (line.npos == line.find("2.6.29")) {
        // We're using something other than 2.6.29, so the JITs should work.
        __android_log_print(ANDROID_LOG_INFO, "Gecko", "JITs are not broken");
        return false;
    }

    // We're using 2.6.29, and this causes trouble with the JITs on i9000.
    line = "";
    bool broken = false;
    std::ifstream cpuinfo("/proc/cpuinfo");
    do {
        if (0 == line.find("Hardware")) {
            const char* blacklist[] = {
                "SCH-I400",     // Samsung Continuum
                "SGH-T959",     // Samsung i9000, Vibrant device
                "SGH-I897",     // Samsung i9000, Captivate device
                "SCH-I500",     // Samsung i9000, Fascinate device
                "SPH-D700",     // Samsung i9000, Epic device
                "GT-I9000",     // Samsung i9000, UK/Europe device
                NULL
            };
            for (const char** hw = &blacklist[0]; *hw; ++hw) {
                if (line.npos != line.find(*hw)) {
                    __android_log_print(ANDROID_LOG_INFO, "Gecko",
                                        "Blacklisted device `%s'", *hw);
                    broken = true;
                    break;
                }
            }
            break;
        }
        std::getline(cpuinfo, line);
    } while(!cpuinfo.fail() && !cpuinfo.eof());

    __android_log_print(ANDROID_LOG_INFO, "Gecko", "JITs are %sbroken",
                        broken ? "" : "not ");

    return broken;
#endif  // ifndef ANDROID
}

static bool
IsJITBrokenHere()
{
    static bool computedIsBroken = false;
    static bool isBroken = false;
    if (!computedIsBroken) {
        isBroken = ComputeIsJITBroken();
        computedIsBroken = true;
    }
    return isBroken;
}
#endif

void
JSContext::updateJITEnabled()
{
#ifdef JS_METHODJIT
    methodJitEnabled = (runOptions & JSOPTION_METHODJIT) && !IsJITBrokenHere();
#endif
}

size_t
JSContext::sizeOfIncludingThis(JSMallocSizeOfFun mallocSizeOf) const
{
    /*
     * There are other JSContext members that could be measured; the following
     * ones have been found by DMD to be worth measuring.  More stuff may be
     * added later.
     */
    return mallocSizeOf(this) + busyArrays.sizeOfExcludingThis(mallocSizeOf);
}

void
JSContext::mark(JSTracer *trc)
{
    /* Stack frames and slots are traced by StackSpace::mark. */

    /* Mark other roots-by-definition in the JSContext. */
    if (defaultCompartmentObject_ && !hasRunOption(JSOPTION_UNROOTED_GLOBAL))
        MarkObjectRoot(trc, &defaultCompartmentObject_, "default compartment object");
    if (isExceptionPending())
        MarkValueRoot(trc, &exception, "exception");

    if (sharpObjectMap.depth > 0)
        js_TraceSharpMap(trc, &sharpObjectMap);

    MarkValueRoot(trc, &iterValue, "iterValue");
}

namespace JS {

#if defined JS_THREADSAFE && defined DEBUG

AutoCheckRequestDepth::AutoCheckRequestDepth(JSContext *cx)
    : cx(cx)
{
    JS_ASSERT(cx->runtime->requestDepth || cx->runtime->isHeapBusy());
    JS_ASSERT(cx->runtime->onOwnerThread());
    cx->runtime->checkRequestDepth++;
}

AutoCheckRequestDepth::~AutoCheckRequestDepth()
{
    JS_ASSERT(cx->runtime->checkRequestDepth != 0);
    cx->runtime->checkRequestDepth--;
}

#endif

} // namespace JS
