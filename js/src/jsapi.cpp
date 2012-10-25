/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JavaScript API.
 */

#include "mozilla/FloatingPoint.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "jstypes.h"
#include "jsutil.h"
#include "jsclist.h"
#include "jsprf.h"
#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jsbool.h"
#include "jsclone.h"
#include "jscntxt.h"
#include "jsversion.h"
#include "jsdate.h"
#include "jsdtoa.h"
#include "jsexn.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinterp.h"
#include "jsiter.h"
#include "jslock.h"
#include "jsmath.h"
#include "jsnativestack.h"
#include "jsnum.h"
#include "json.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jsprobes.h"
#include "jsproxy.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstr.h"
#include "prmjtime.h"
#include "jsweakmap.h"
#include "jswrapper.h"
#include "jstypedarray.h"
#include "jsxml.h"

#include "builtin/Eval.h"
#include "builtin/MapObject.h"
#include "builtin/RegExp.h"
#include "builtin/ParallelArray.h"
#include "ds/LifoAlloc.h"
#include "frontend/BytecodeCompiler.h"
#include "gc/Marking.h"
#include "gc/Memory.h"
#include "js/MemoryMetrics.h"
#include "vm/NumericConversions.h"
#include "vm/StringBuffer.h"
#include "vm/Xdr.h"
#include "yarr/BumpPointerAllocator.h"

#include "jsatominlines.h"
#include "jsinferinlines.h"
#include "jsobjinlines.h"
#include "jsscopeinlines.h"
#include "jsscriptinlines.h"

#include "vm/ObjectImpl-inl.h"
#include "vm/RegExpObject-inl.h"
#include "vm/RegExpStatics-inl.h"
#include "vm/Stack-inl.h"
#include "vm/String-inl.h"

#if ENABLE_YARR_JIT
#include "assembler/jit/ExecutableAllocator.h"
#include "methodjit/Logging.h"
#endif

using namespace js;
using namespace js::gc;
using namespace js::types;
using js::frontend::Parser;

bool
JS::detail::CallMethodIfWrapped(JSContext *cx, IsAcceptableThis test, NativeImpl impl,
                               CallArgs args)
{
    const Value &thisv = args.thisv();
    JS_ASSERT(!test(thisv));

    if (thisv.isObject()) {
        JSObject &thisObj = args.thisv().toObject();
        if (thisObj.isProxy())
            return Proxy::nativeCall(cx, test, impl, args);
    }

    ReportIncompatible(cx, args);
    return false;
}


/*
 * This class is a version-establishing barrier at the head of a VM entry or
 * re-entry. It ensures that:
 *
 * - |newVersion| is the starting (default) version used for the context.
 * - The starting version state is not an override.
 * - Overrides in the VM session are not propagated to the caller.
 */
class AutoVersionAPI
{
    JSContext   * const cx;
    JSVersion   oldDefaultVersion;
    bool        oldHasVersionOverride;
    JSVersion   oldVersionOverride;
#ifdef DEBUG
    unsigned       oldCompileOptions;
#endif
    JSVersion   newVersion;

  public:
    AutoVersionAPI(JSContext *cx, JSVersion newVersion)
      : cx(cx),
        oldDefaultVersion(cx->getDefaultVersion()),
        oldHasVersionOverride(cx->isVersionOverridden()),
        oldVersionOverride(oldHasVersionOverride ? cx->findVersion() : JSVERSION_UNKNOWN)
#ifdef DEBUG
        , oldCompileOptions(cx->getCompileOptions())
#endif
    {
#if JS_HAS_XML_SUPPORT
        // For backward compatibility, AutoVersionAPI clobbers the
        // JSOPTION_MOAR_XML bit in cx, but not the JSOPTION_ALLOW_XML bit.
        newVersion = JSVersion(newVersion | (oldDefaultVersion & VersionFlags::ALLOW_XML));
#endif
        this->newVersion = newVersion;
        cx->clearVersionOverride();
        cx->setDefaultVersion(newVersion);
    }

    ~AutoVersionAPI() {
        cx->setDefaultVersion(oldDefaultVersion);
        if (oldHasVersionOverride)
            cx->overrideVersion(oldVersionOverride);
        else
            cx->clearVersionOverride();
        JS_ASSERT(oldCompileOptions == cx->getCompileOptions());
    }

    /* The version that this scoped-entity establishes. */
    JSVersion version() const { return newVersion; }
};

#ifdef HAVE_VA_LIST_AS_ARRAY
#define JS_ADDRESSOF_VA_LIST(ap) ((va_list *)(ap))
#else
#define JS_ADDRESSOF_VA_LIST(ap) (&(ap))
#endif

#ifdef JS_USE_JSID_STRUCT_TYPES
jsid JS_DEFAULT_XML_NAMESPACE_ID = { size_t(JSID_TYPE_DEFAULT_XML_NAMESPACE) };
jsid JSID_VOID  = { size_t(JSID_TYPE_VOID) };
jsid JSID_EMPTY = { size_t(JSID_TYPE_OBJECT) };
#endif

const jsval JSVAL_NULL  = IMPL_TO_JSVAL(BUILD_JSVAL(JSVAL_TAG_NULL,      0));
const jsval JSVAL_ZERO  = IMPL_TO_JSVAL(BUILD_JSVAL(JSVAL_TAG_INT32,     0));
const jsval JSVAL_ONE   = IMPL_TO_JSVAL(BUILD_JSVAL(JSVAL_TAG_INT32,     1));
const jsval JSVAL_FALSE = IMPL_TO_JSVAL(BUILD_JSVAL(JSVAL_TAG_BOOLEAN,   JS_FALSE));
const jsval JSVAL_TRUE  = IMPL_TO_JSVAL(BUILD_JSVAL(JSVAL_TAG_BOOLEAN,   JS_TRUE));
const jsval JSVAL_VOID  = IMPL_TO_JSVAL(BUILD_JSVAL(JSVAL_TAG_UNDEFINED, 0));

/* Make sure that jschar is two bytes unsigned integer */
JS_STATIC_ASSERT((jschar)-1 > 0);
JS_STATIC_ASSERT(sizeof(jschar) == 2);

JS_PUBLIC_API(int64_t)
JS_Now()
{
    return PRMJ_Now();
}

JS_PUBLIC_API(jsval)
JS_GetNaNValue(JSContext *cx)
{
    return cx->runtime->NaNValue;
}

JS_PUBLIC_API(jsval)
JS_GetNegativeInfinityValue(JSContext *cx)
{
    return cx->runtime->negativeInfinityValue;
}

JS_PUBLIC_API(jsval)
JS_GetPositiveInfinityValue(JSContext *cx)
{
    return cx->runtime->positiveInfinityValue;
}

JS_PUBLIC_API(jsval)
JS_GetEmptyStringValue(JSContext *cx)
{
    return STRING_TO_JSVAL(cx->runtime->emptyString);
}

JS_PUBLIC_API(JSString *)
JS_GetEmptyString(JSRuntime *rt)
{
    JS_ASSERT(rt->hasContexts());
    return rt->emptyString;
}

static JSBool
TryArgumentFormatter(JSContext *cx, const char **formatp, JSBool fromJS, jsval **vpp, va_list *app)
{
    const char *format;
    JSArgumentFormatMap *map;

    format = *formatp;
    for (map = cx->argumentFormatMap; map; map = map->next) {
        if (!strncmp(format, map->format, map->length)) {
            *formatp = format + map->length;
            return map->formatter(cx, format, fromJS, vpp, app);
        }
    }
    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_CHAR, format);
    return JS_FALSE;
}

static void
AssertHeapIsIdle(JSRuntime *rt)
{
    JS_ASSERT(rt->heapState == JSRuntime::Idle);
}

static void
AssertHeapIsIdle(JSContext *cx)
{
    AssertHeapIsIdle(cx->runtime);
}

static void
AssertHeapIsIdleOrIterating(JSRuntime *rt)
{
    JS_ASSERT(rt->heapState != JSRuntime::Collecting);
}

static void
AssertHeapIsIdleOrIterating(JSContext *cx)
{
    AssertHeapIsIdleOrIterating(cx->runtime);
}

static void
AssertHeapIsIdleOrStringIsFlat(JSContext *cx, JSString *str)
{
    /*
     * We allow some functions to be called during a GC as long as the argument
     * is a flat string, since that will not cause allocation.
     */
    JS_ASSERT_IF(cx->runtime->isHeapBusy(), str->isFlat());
}

JS_PUBLIC_API(JSBool)
JS_ConvertArguments(JSContext *cx, unsigned argc, jsval *argv, const char *format, ...)
{
    va_list ap;
    JSBool ok;

    AssertHeapIsIdle(cx);

    va_start(ap, format);
    ok = JS_ConvertArgumentsVA(cx, argc, argv, format, ap);
    va_end(ap);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_ConvertArgumentsVA(JSContext *cx, unsigned argc, jsval *argv, const char *format, va_list ap)
{
    jsval *sp;
    JSBool required;
    char c;
    double d;
    JSString *str;
    RootedObject obj(cx);

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, JSValueArray(argv - 2, argc + 2));
    sp = argv;
    required = JS_TRUE;
    while ((c = *format++) != '\0') {
        if (isspace(c))
            continue;
        if (c == '/') {
            required = JS_FALSE;
            continue;
        }
        if (sp == argv + argc) {
            if (required) {
                if (JSFunction *fun = ReportIfNotFunction(cx, argv[-2])) {
                    char numBuf[12];
                    JS_snprintf(numBuf, sizeof numBuf, "%u", argc);
                    JSAutoByteString funNameBytes;
                    if (const char *name = GetFunctionNameBytes(cx, fun, &funNameBytes)) {
                        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_MORE_ARGS_NEEDED,
                                             name, numBuf, (argc == 1) ? "" : "s");
                    }
                }
                return JS_FALSE;
            }
            break;
        }
        switch (c) {
          case 'b':
            *va_arg(ap, JSBool *) = ToBoolean(*sp);
            break;
          case 'c':
            if (!JS_ValueToUint16(cx, *sp, va_arg(ap, uint16_t *)))
                return JS_FALSE;
            break;
          case 'i':
            if (!JS_ValueToECMAInt32(cx, *sp, va_arg(ap, int32_t *)))
                return JS_FALSE;
            break;
          case 'u':
            if (!JS_ValueToECMAUint32(cx, *sp, va_arg(ap, uint32_t *)))
                return JS_FALSE;
            break;
          case 'j':
            if (!JS_ValueToInt32(cx, *sp, va_arg(ap, int32_t *)))
                return JS_FALSE;
            break;
          case 'd':
            if (!JS_ValueToNumber(cx, *sp, va_arg(ap, double *)))
                return JS_FALSE;
            break;
          case 'I':
            if (!JS_ValueToNumber(cx, *sp, &d))
                return JS_FALSE;
            *va_arg(ap, double *) = ToInteger(d);
            break;
          case 'S':
          case 'W':
            str = ToString(cx, *sp);
            if (!str)
                return JS_FALSE;
            *sp = STRING_TO_JSVAL(str);
            if (c == 'W') {
                JSFixedString *fixed = str->ensureFixed(cx);
                if (!fixed)
                    return JS_FALSE;
                *va_arg(ap, const jschar **) = fixed->chars();
            } else {
                *va_arg(ap, JSString **) = str;
            }
            break;
          case 'o':
            if (!js_ValueToObjectOrNull(cx, *sp, &obj))
                return JS_FALSE;
            *sp = OBJECT_TO_JSVAL(obj);
            *va_arg(ap, JSObject **) = obj;
            break;
          case 'f':
            obj = ReportIfNotFunction(cx, *sp);
            if (!obj)
                return JS_FALSE;
            *sp = OBJECT_TO_JSVAL(obj);
            *va_arg(ap, JSFunction **) = obj->toFunction();
            break;
          case 'v':
            *va_arg(ap, jsval *) = *sp;
            break;
          case '*':
            break;
          default:
            format--;
            if (!TryArgumentFormatter(cx, &format, JS_TRUE, &sp,
                                      JS_ADDRESSOF_VA_LIST(ap))) {
                return JS_FALSE;
            }
            /* NB: the formatter already updated sp, so we continue here. */
            continue;
        }
        sp++;
    }
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_AddArgumentFormatter(JSContext *cx, const char *format, JSArgumentFormatter formatter)
{
    size_t length;
    JSArgumentFormatMap **mpp, *map;

    length = strlen(format);
    mpp = &cx->argumentFormatMap;
    while ((map = *mpp) != NULL) {
        /* Insert before any shorter string to match before prefixes. */
        if (map->length < length)
            break;
        if (map->length == length && !strcmp(map->format, format))
            goto out;
        mpp = &map->next;
    }
    map = (JSArgumentFormatMap *) cx->malloc_(sizeof *map);
    if (!map)
        return JS_FALSE;
    map->format = format;
    map->length = length;
    map->next = *mpp;
    *mpp = map;
out:
    map->formatter = formatter;
    return JS_TRUE;
}

JS_PUBLIC_API(void)
JS_RemoveArgumentFormatter(JSContext *cx, const char *format)
{
    size_t length;
    JSArgumentFormatMap **mpp, *map;

    length = strlen(format);
    mpp = &cx->argumentFormatMap;
    while ((map = *mpp) != NULL) {
        if (map->length == length && !strcmp(map->format, format)) {
            *mpp = map->next;
            cx->free_(map);
            return;
        }
        mpp = &map->next;
    }
}

JS_PUBLIC_API(JSBool)
JS_ConvertValue(JSContext *cx, jsval v, JSType type, jsval *vp)
{
    JSBool ok;
    RootedObject obj(cx);
    JSString *str;
    double d;

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v);
    switch (type) {
      case JSTYPE_VOID:
        *vp = JSVAL_VOID;
        ok = JS_TRUE;
        break;
      case JSTYPE_OBJECT:
        ok = js_ValueToObjectOrNull(cx, v, &obj);
        if (ok)
            *vp = OBJECT_TO_JSVAL(obj);
        break;
      case JSTYPE_FUNCTION:
        *vp = v;
        obj = ReportIfNotFunction(cx, *vp);
        ok = (obj != NULL);
        break;
      case JSTYPE_STRING:
        str = ToString(cx, v);
        ok = (str != NULL);
        if (ok)
            *vp = STRING_TO_JSVAL(str);
        break;
      case JSTYPE_NUMBER:
        ok = JS_ValueToNumber(cx, v, &d);
        if (ok)
            *vp = DOUBLE_TO_JSVAL(d);
        break;
      case JSTYPE_BOOLEAN:
        *vp = BooleanValue(ToBoolean(v));
        return JS_TRUE;
      default: {
        char numBuf[12];
        JS_snprintf(numBuf, sizeof numBuf, "%d", (int)type);
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_TYPE, numBuf);
        ok = JS_FALSE;
        break;
      }
    }
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_ValueToObject(JSContext *cx, jsval v, JSObject **objpArg)
{
    RootedObject objp(cx, *objpArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v);
    if (!js_ValueToObjectOrNull(cx, v, &objp))
        return false;
    *objpArg = objp;
    return true;
}

JS_PUBLIC_API(JSFunction *)
JS_ValueToFunction(JSContext *cx, jsval v)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v);
    return ReportIfNotFunction(cx, v);
}

JS_PUBLIC_API(JSFunction *)
JS_ValueToConstructor(JSContext *cx, jsval v)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v);
    return ReportIfNotFunction(cx, v);
}

JS_PUBLIC_API(JSString *)
JS_ValueToString(JSContext *cx, jsval v)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v);
    return ToString(cx, v);
}

JS_PUBLIC_API(JSString *)
JS_ValueToSource(JSContext *cx, jsval v)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v);
    return js_ValueToSource(cx, v);
}

JS_PUBLIC_API(JSBool)
JS_ValueToNumber(JSContext *cx, jsval v, double *dp)
{
    RootedValue value(cx, v);
    return JS::ToNumber(cx, value, dp);
}

JS_PUBLIC_API(JSBool)
JS_DoubleIsInt32(double d, int32_t *ip)
{
    return MOZ_DOUBLE_IS_INT32(d, ip);
}

JS_PUBLIC_API(int32_t)
JS_DoubleToInt32(double d)
{
    return ToInt32(d);
}

JS_PUBLIC_API(uint32_t)
JS_DoubleToUint32(double d)
{
    return ToUint32(d);
}

JS_PUBLIC_API(JSBool)
JS_ValueToECMAInt32(JSContext *cx, jsval v, int32_t *ip)
{
    RootedValue value(cx, v);
    return JS::ToInt32(cx, value, ip);
}

JS_PUBLIC_API(JSBool)
JS_ValueToECMAUint32(JSContext *cx, jsval v, uint32_t *ip)
{
    RootedValue value(cx, v);
    return JS::ToUint32(cx, value, ip);
}

JS_PUBLIC_API(JSBool)
JS_ValueToInt64(JSContext *cx, jsval v, int64_t *ip)
{
    RootedValue value(cx, v);
    return JS::ToInt64(cx, value, ip);
}

JS_PUBLIC_API(JSBool)
JS_ValueToUint64(JSContext *cx, jsval v, uint64_t *ip)
{
    RootedValue value(cx, v);
    return JS::ToUint64(cx, value, ip);
}

JS_PUBLIC_API(JSBool)
JS_ValueToInt32(JSContext *cx, jsval vArg, int32_t *ip)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    RootedValue v(cx, vArg);
    assertSameCompartment(cx, v);

    if (v.isInt32()) {
        *ip = v.toInt32();
        return true;
    }

    double d;
    if (v.isDouble()) {
        d = v.toDouble();
    } else if (!ToNumberSlow(cx, v, &d)) {
        return false;
    }

    if (MOZ_DOUBLE_IS_NaN(d) || d <= -2147483649.0 || 2147483648.0 <= d) {
        js_ReportValueError(cx, JSMSG_CANT_CONVERT,
                            JSDVG_SEARCH_STACK, v, NullPtr());
        return false;
    }

    *ip = (int32_t) floor(d + 0.5);  /* Round to nearest */
    return true;
}

JS_PUBLIC_API(JSBool)
JS_ValueToUint16(JSContext *cx, jsval v, uint16_t *ip)
{
    return ToUint16(cx, v, ip);
}

JS_PUBLIC_API(JSBool)
JS_ValueToBoolean(JSContext *cx, jsval v, JSBool *bp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v);
    *bp = ToBoolean(v);
    return JS_TRUE;
}

JS_PUBLIC_API(JSType)
JS_TypeOfValue(JSContext *cx, jsval v)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v);
    return TypeOfValue(cx, v);
}

JS_PUBLIC_API(const char *)
JS_GetTypeName(JSContext *cx, JSType type)
{
    if ((unsigned)type >= (unsigned)JSTYPE_LIMIT)
        return NULL;
    return JS_TYPE_STR(type);
}

JS_PUBLIC_API(JSBool)
JS_StrictlyEqual(JSContext *cx, jsval v1, jsval v2, JSBool *equal)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v1, v2);
    bool eq;
    if (!StrictlyEqual(cx, v1, v2, &eq))
        return false;
    *equal = eq;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_LooselyEqual(JSContext *cx, jsval v1, jsval v2, JSBool *equal)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v1, v2);
    bool eq;
    if (!LooselyEqual(cx, v1, v2, &eq))
        return false;
    *equal = eq;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_SameValue(JSContext *cx, jsval v1, jsval v2, JSBool *same)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v1, v2);
    bool s;
    if (!SameValue(cx, v1, v2, &s))
        return false;
    *same = s;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_IsBuiltinEvalFunction(JSFunction *fun)
{
    return IsAnyBuiltinEval(fun);
}

JS_PUBLIC_API(JSBool)
JS_IsBuiltinFunctionConstructor(JSFunction *fun)
{
    return IsBuiltinFunctionConstructor(fun);
}

/************************************************************************/

/*
 * Has a new runtime ever been created?  This flag is used to detect unsafe
 * changes to js_CStringsAreUTF8 after a runtime has been created, and to
 * control things that should happen only once across all runtimes.
 */
static JSBool js_NewRuntimeWasCalled = JS_FALSE;

static const JSSecurityCallbacks NullSecurityCallbacks = { };

JSRuntime::JSRuntime()
  : atomsCompartment(NULL),
#ifdef JS_THREADSAFE
    ownerThread_(NULL),
#endif
    tempLifoAlloc(TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE),
    freeLifoAlloc(TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE),
    execAlloc_(NULL),
    bumpAlloc_(NULL),
#ifdef JS_METHODJIT
    jaegerRuntime_(NULL),
#endif
    selfHostedGlobal_(NULL),
    nativeStackBase(0),
    nativeStackQuota(0),
    interpreterFrames(NULL),
    cxCallback(NULL),
    destroyCompartmentCallback(NULL),
    compartmentNameCallback(NULL),
    activityCallback(NULL),
    activityCallbackArg(NULL),
#ifdef JS_THREADSAFE
    suspendCount(0),
    requestDepth(0),
# ifdef DEBUG
    checkRequestDepth(0),
# endif
#endif
    gcSystemAvailableChunkListHead(NULL),
    gcUserAvailableChunkListHead(NULL),
    gcKeepAtoms(0),
    gcBytes(0),
    gcMaxBytes(0),
    gcMaxMallocBytes(0),
    gcNumArenasFreeCommitted(0),
    gcVerifyPreData(NULL),
    gcVerifyPostData(NULL),
    gcChunkAllocationSinceLastGC(false),
    gcNextFullGCTime(0),
    gcLastGCTime(0),
    gcJitReleaseTime(0),
    gcMode(JSGC_MODE_GLOBAL),
    gcHighFrequencyGC(false),
    gcHighFrequencyTimeThreshold(1000),
    gcHighFrequencyLowLimitBytes(100 * 1024 * 1024),
    gcHighFrequencyHighLimitBytes(500 * 1024 * 1024),
    gcHighFrequencyHeapGrowthMax(3.0),
    gcHighFrequencyHeapGrowthMin(1.5),
    gcLowFrequencyHeapGrowth(1.5),
    gcDynamicHeapGrowth(false),
    gcDynamicMarkSlice(false),
    gcShouldCleanUpEverything(false),
    gcIsNeeded(0),
    gcWeakMapList(NULL),
    gcStats(thisFromCtor()),
    gcNumber(0),
    gcStartNumber(0),
    gcIsFull(false),
    gcTriggerReason(gcreason::NO_REASON),
    gcStrictCompartmentChecking(false),
    gcDisableStrictProxyCheckingCount(0),
    gcIncrementalState(gc::NO_INCREMENTAL),
    gcLastMarkSlice(false),
    gcSweepOnBackgroundThread(false),
    gcSweepingCompartments(NULL),
    gcSweepPhase(0),
    gcSweepCompartmentIndex(0),
    gcSweepKindIndex(0),
    gcArenasAllocatedDuringSweep(NULL),
    gcInterFrameGC(0),
    gcSliceBudget(SliceBudget::Unlimited),
    gcIncrementalEnabled(true),
    gcExactScanningEnabled(true),
    gcPoke(false),
    heapState(Idle),
#ifdef JS_GC_ZEAL
    gcZeal_(0),
    gcZealFrequency(0),
    gcNextScheduled(0),
    gcDeterministicOnly(false),
    gcIncrementalLimit(0),
#endif
    gcValidate(true),
    gcCallback(NULL),
    gcSliceCallback(NULL),
    gcFinalizeCallback(NULL),
    analysisPurgeCallback(NULL),
    analysisPurgeTriggerBytes(0),
    gcMallocBytes(0),
    gcBlackRootsTraceOp(NULL),
    gcBlackRootsData(NULL),
    gcGrayRootsTraceOp(NULL),
    gcGrayRootsData(NULL),
    autoGCRooters(NULL),
    scriptAndCountsVector(NULL),
    NaNValue(UndefinedValue()),
    negativeInfinityValue(UndefinedValue()),
    positiveInfinityValue(UndefinedValue()),
    emptyString(NULL),
    sourceHook(NULL),
    debugMode(false),
    spsProfiler(thisFromCtor()),
    profilingScripts(false),
    alwaysPreserveCode(false),
    hadOutOfMemory(false),
    debugScopes(NULL),
    data(NULL),
    gcLock(NULL),
    gcHelperThread(thisFromCtor()),
#ifdef JS_THREADSAFE
    sourceCompressorThread(thisFromCtor()),
#endif
    defaultFreeOp_(thisFromCtor(), false),
    debuggerMutations(0),
    securityCallbacks(const_cast<JSSecurityCallbacks *>(&NullSecurityCallbacks)),
    DOMcallbacks(NULL),
    destroyPrincipals(NULL),
    structuredCloneCallbacks(NULL),
    telemetryCallback(NULL),
    propertyRemovals(0),
    thousandsSeparator(0),
    decimalSeparator(0),
    numGrouping(0),
    waiveGCQuota(false),
    mathCache_(NULL),
    dtoaState(NULL),
    pendingProxyOperation(NULL),
    trustedPrincipals_(NULL),
    wrapObjectCallback(TransparentObjectWrapper),
    sameCompartmentWrapObjectCallback(NULL),
    preWrapObjectCallback(NULL),
    preserveWrapperCallback(NULL),
#ifdef DEBUG
    noGCOrAllocationCheck(0),
#endif
    inOOMReport(0),
    jitHardening(false)
{
    /* Initialize infallibly first, so we can goto bad and JS_DestroyRuntime. */
    JS_INIT_CLIST(&contextList);
    JS_INIT_CLIST(&debuggerList);

    PodZero(&debugHooks);
    PodZero(&atomState);

#if JS_STACK_GROWTH_DIRECTION > 0
    nativeStackLimit = UINTPTR_MAX;
#endif
}

bool
JSRuntime::init(uint32_t maxbytes)
{
#ifdef JS_THREADSAFE
    ownerThread_ = PR_GetCurrentThread();
#endif

#ifdef JS_METHODJIT_SPEW
    JMCheckLogging();
#endif

    if (!js_InitGC(this, maxbytes))
        return false;

    if (!gcMarker.init())
        return false;

    const char *size = getenv("JSGC_MARK_STACK_LIMIT");
    if (size)
        SetMarkStackLimit(this, atoi(size));

    if (!(atomsCompartment = this->new_<JSCompartment>(this)) ||
        !atomsCompartment->init(NULL) ||
        !compartments.append(atomsCompartment))
    {
        Foreground::delete_(atomsCompartment);
        return false;
    }

    atomsCompartment->isSystemCompartment = true;
    atomsCompartment->setGCLastBytes(8192, 8192, GC_NORMAL);

    if (!InitAtomState(this))
        return false;

    if (!InitRuntimeNumberState(this))
        return false;

    dtoaState = js_NewDtoaState();
    if (!dtoaState)
        return false;

    if (!stackSpace.init())
        return false;

    if (!scriptFilenameTable.init())
        return false;

#ifdef JS_THREADSAFE
    if (!sourceCompressorThread.init())
        return false;
#endif

    if (!evalCache.init())
        return false;

    debugScopes = this->new_<DebugScopes>(this);
    if (!debugScopes || !debugScopes->init()) {
        Foreground::delete_(debugScopes);
        return false;
    }

    nativeStackBase = GetNativeStackBase();
    return true;
}

JSRuntime::~JSRuntime()
{
    JS_ASSERT(onOwnerThread());

    delete_(debugScopes);

    /*
     * Even though all objects in the compartment are dead, we may have keep
     * some filenames around because of gcKeepAtoms.
     */
    FreeScriptFilenames(this);

#ifdef JS_THREADSAFE
    sourceCompressorThread.finish();
#endif

#ifdef DEBUG
    /* Don't hurt everyone in leaky ol' Mozilla with a fatal JS_ASSERT! */
    if (!JS_CLIST_IS_EMPTY(&contextList)) {
        unsigned cxcount = 0;
        for (ContextIter acx(this); !acx.done(); acx.next()) {
            fprintf(stderr,
"JS API usage error: found live context at %p\n",
                    (void *) acx.get());
            cxcount++;
        }
        fprintf(stderr,
"JS API usage error: %u context%s left in runtime upon JS_DestroyRuntime.\n",
                cxcount, (cxcount == 1) ? "" : "s");
    }
#endif

    FinishRuntimeNumberState(this);
    FinishAtomState(this);

    if (dtoaState)
        js_DestroyDtoaState(dtoaState);

    js_FinishGC(this);
#ifdef JS_THREADSAFE
    if (gcLock)
        PR_DestroyLock(gcLock);
#endif

    delete_(bumpAlloc_);
    delete_(mathCache_);
#ifdef JS_METHODJIT
    delete_(jaegerRuntime_);
#endif
    delete_(execAlloc_);  /* Delete after jaegerRuntime_. */
}

#ifdef JS_THREADSAFE
void
JSRuntime::setOwnerThread()
{
    JS_ASSERT(ownerThread_ == (void *)0xc1ea12);  /* "clear" */
    JS_ASSERT(requestDepth == 0);
    ownerThread_ = PR_GetCurrentThread();
    nativeStackBase = GetNativeStackBase();
    if (nativeStackQuota)
        JS_SetNativeStackQuota(this, nativeStackQuota);
}

void
JSRuntime::clearOwnerThread()
{
    JS_ASSERT(onOwnerThread());
    JS_ASSERT(requestDepth == 0);
    ownerThread_ = (void *)0xc1ea12;  /* "clear" */
    nativeStackBase = 0;
#if JS_STACK_GROWTH_DIRECTION > 0
    nativeStackLimit = UINTPTR_MAX;
#else
    nativeStackLimit = 0;
#endif
}

JS_FRIEND_API(bool)
JSRuntime::onOwnerThread() const
{
    return ownerThread_ == PR_GetCurrentThread();
}
#endif  /* JS_THREADSAFE */

JS_PUBLIC_API(JSRuntime *)
JS_NewRuntime(uint32_t maxbytes)
{
    if (!js_NewRuntimeWasCalled) {
#ifdef DEBUG
        /*
         * This code asserts that the numbers associated with the error names
         * in jsmsg.def are monotonically increasing.  It uses values for the
         * error names enumerated in jscntxt.c.  It's not a compile-time check
         * but it's better than nothing.
         */
        int errorNumber = 0;
#define MSG_DEF(name, number, count, exception, format)                       \
    JS_ASSERT(name == errorNumber++);
#include "js.msg"
#undef MSG_DEF

#define MSG_DEF(name, number, count, exception, format)                       \
    JS_BEGIN_MACRO                                                            \
        unsigned numfmtspecs = 0;                                                \
        const char *fmt;                                                      \
        for (fmt = format; *fmt != '\0'; fmt++) {                             \
            if (*fmt == '{' && isdigit(fmt[1]))                               \
                ++numfmtspecs;                                                \
        }                                                                     \
        JS_ASSERT(count == numfmtspecs);                                      \
    JS_END_MACRO;
#include "js.msg"
#undef MSG_DEF
#endif /* DEBUG */

        InitMemorySubsystem();

        js_NewRuntimeWasCalled = JS_TRUE;
    }

    JSRuntime *rt = OffTheBooks::new_<JSRuntime>();
    if (!rt)
        return NULL;

    if (!rt->init(maxbytes)) {
        JS_DestroyRuntime(rt);
        return NULL;
    }

    Probes::createRuntime(rt);
    return rt;
}

JS_PUBLIC_API(void)
JS_DestroyRuntime(JSRuntime *rt)
{
    Probes::destroyRuntime(rt);
    Foreground::delete_(rt);
}

JS_PUBLIC_API(void)
JS_ShutDown(void)
{
    Probes::shutdown();
    PRMJ_NowShutdown();
}

JS_PUBLIC_API(void *)
JS_GetRuntimePrivate(JSRuntime *rt)
{
    return rt->data;
}

JS_PUBLIC_API(void)
JS_SetRuntimePrivate(JSRuntime *rt, void *data)
{
    rt->data = data;
}

#ifdef JS_THREADSAFE
static void
StartRequest(JSContext *cx)
{
    JSRuntime *rt = cx->runtime;
    JS_ASSERT(rt->onOwnerThread());

    if (rt->requestDepth) {
        rt->requestDepth++;
    } else {
        /* Indicate that a request is running. */
        rt->requestDepth = 1;

        if (rt->activityCallback)
            rt->activityCallback(rt->activityCallbackArg, true);
    }
}

static void
StopRequest(JSContext *cx)
{
    JSRuntime *rt = cx->runtime;
    JS_ASSERT(rt->onOwnerThread());
    JS_ASSERT(rt->requestDepth != 0);
    if (rt->requestDepth != 1) {
        rt->requestDepth--;
    } else {
        rt->conservativeGC.updateForRequestEnd(rt->suspendCount);
        rt->requestDepth = 0;

        if (rt->activityCallback)
            rt->activityCallback(rt->activityCallbackArg, false);
    }
}
#endif /* JS_THREADSAFE */

JS_PUBLIC_API(void)
JS_BeginRequest(JSContext *cx)
{
#ifdef JS_THREADSAFE
    cx->outstandingRequests++;
    StartRequest(cx);
#endif
}

JS_PUBLIC_API(void)
JS_EndRequest(JSContext *cx)
{
#ifdef JS_THREADSAFE
    JS_ASSERT(cx->outstandingRequests != 0);
    cx->outstandingRequests--;
    StopRequest(cx);
#endif
}

/* Yield to pending GC operations, regardless of request depth */
JS_PUBLIC_API(void)
JS_YieldRequest(JSContext *cx)
{
#ifdef JS_THREADSAFE
    CHECK_REQUEST(cx);
    JS_ResumeRequest(cx, JS_SuspendRequest(cx));
#endif
}

JS_PUBLIC_API(unsigned)
JS_SuspendRequest(JSContext *cx)
{
#ifdef JS_THREADSAFE
    JSRuntime *rt = cx->runtime;
    JS_ASSERT(rt->onOwnerThread());

    unsigned saveDepth = rt->requestDepth;
    if (!saveDepth)
        return 0;

    rt->suspendCount++;
    rt->requestDepth = 1;
    StopRequest(cx);
    return saveDepth;
#else
    return 0;
#endif
}

JS_PUBLIC_API(void)
JS_ResumeRequest(JSContext *cx, unsigned saveDepth)
{
#ifdef JS_THREADSAFE
    JSRuntime *rt = cx->runtime;
    JS_ASSERT(rt->onOwnerThread());
    if (saveDepth == 0)
        return;
    JS_ASSERT(saveDepth >= 1);
    JS_ASSERT(!rt->requestDepth);
    JS_ASSERT(rt->suspendCount);
    StartRequest(cx);
    rt->requestDepth = saveDepth;
    rt->suspendCount--;
#endif
}

JS_PUBLIC_API(JSBool)
JS_IsInRequest(JSRuntime *rt)
{
#ifdef JS_THREADSAFE
    JS_ASSERT(rt->onOwnerThread());
    return rt->requestDepth != 0;
#else
    return false;
#endif
}

JS_PUBLIC_API(JSBool)
JS_IsInSuspendedRequest(JSRuntime *rt)
{
#ifdef JS_THREADSAFE
    JS_ASSERT(rt->onOwnerThread());
    return rt->suspendCount != 0;
#else
    return false;
#endif
}

JS_PUBLIC_API(JSContextCallback)
JS_SetContextCallback(JSRuntime *rt, JSContextCallback cxCallback)
{
    JSContextCallback old;

    old = rt->cxCallback;
    rt->cxCallback = cxCallback;
    return old;
}

JS_PUBLIC_API(JSContext *)
JS_NewContext(JSRuntime *rt, size_t stackChunkSize)
{
    return NewContext(rt, stackChunkSize);
}

JS_PUBLIC_API(void)
JS_DestroyContext(JSContext *cx)
{
    DestroyContext(cx, DCM_FORCE_GC);
}

JS_PUBLIC_API(void)
JS_DestroyContextNoGC(JSContext *cx)
{
    DestroyContext(cx, DCM_NO_GC);
}

JS_PUBLIC_API(void *)
JS_GetContextPrivate(JSContext *cx)
{
    return cx->data;
}

JS_PUBLIC_API(void)
JS_SetContextPrivate(JSContext *cx, void *data)
{
    cx->data = data;
}

JS_PUBLIC_API(void *)
JS_GetSecondContextPrivate(JSContext *cx)
{
    return cx->data2;
}

JS_PUBLIC_API(void)
JS_SetSecondContextPrivate(JSContext *cx, void *data)
{
    cx->data2 = data;
}

JS_PUBLIC_API(JSRuntime *)
JS_GetRuntime(JSContext *cx)
{
    return cx->runtime;
}

JS_PUBLIC_API(JSContext *)
JS_ContextIterator(JSRuntime *rt, JSContext **iterp)
{
    JSContext *cx = *iterp;
    JSCList *next = cx ? cx->link.next : rt->contextList.next;
    cx = (next == &rt->contextList) ? NULL : JSContext::fromLinkField(next);
    *iterp = cx;
    return cx;
}

JS_PUBLIC_API(JSVersion)
JS_GetVersion(JSContext *cx)
{
    return VersionNumber(cx->findVersion());
}

JS_PUBLIC_API(JSVersion)
JS_SetVersion(JSContext *cx, JSVersion newVersion)
{
    JS_ASSERT(VersionIsKnown(newVersion));
    JS_ASSERT(!VersionHasFlags(newVersion));
    JSVersion newVersionNumber = newVersion;

#ifdef DEBUG
    unsigned coptsBefore = cx->getCompileOptions();
#endif
    JSVersion oldVersion = cx->findVersion();
    JSVersion oldVersionNumber = VersionNumber(oldVersion);
    if (oldVersionNumber == newVersionNumber)
        return oldVersionNumber; /* No override actually occurs! */

    /* We no longer support 1.4 or below. */
    if (newVersionNumber != JSVERSION_DEFAULT && newVersionNumber <= JSVERSION_1_4)
        return oldVersionNumber;

    VersionCopyFlags(&newVersion, oldVersion);
    cx->maybeOverrideVersion(newVersion);
    JS_ASSERT(cx->getCompileOptions() == coptsBefore);
    return oldVersionNumber;
}

static struct v2smap {
    JSVersion   version;
    const char  *string;
} v2smap[] = {
    {JSVERSION_1_0,     "1.0"},
    {JSVERSION_1_1,     "1.1"},
    {JSVERSION_1_2,     "1.2"},
    {JSVERSION_1_3,     "1.3"},
    {JSVERSION_1_4,     "1.4"},
    {JSVERSION_ECMA_3,  "ECMAv3"},
    {JSVERSION_1_5,     "1.5"},
    {JSVERSION_1_6,     "1.6"},
    {JSVERSION_1_7,     "1.7"},
    {JSVERSION_1_8,     "1.8"},
    {JSVERSION_ECMA_5,  "ECMAv5"},
    {JSVERSION_DEFAULT, js_default_str},
    {JSVERSION_UNKNOWN, NULL},          /* must be last, NULL is sentinel */
};

JS_PUBLIC_API(const char *)
JS_VersionToString(JSVersion version)
{
    int i;

    for (i = 0; v2smap[i].string; i++)
        if (v2smap[i].version == version)
            return v2smap[i].string;
    return "unknown";
}

JS_PUBLIC_API(JSVersion)
JS_StringToVersion(const char *string)
{
    int i;

    for (i = 0; v2smap[i].string; i++)
        if (strcmp(v2smap[i].string, string) == 0)
            return v2smap[i].version;
    return JSVERSION_UNKNOWN;
}

JS_PUBLIC_API(uint32_t)
JS_GetOptions(JSContext *cx)
{
    /*
     * Can't check option/version synchronization here.
     * We may have been synchronized with a script version that was formerly on
     * the stack, but has now been popped.
     */
    return cx->allOptions();
}

static unsigned
SetOptionsCommon(JSContext *cx, unsigned options)
{
    JS_ASSERT((options & JSALLOPTION_MASK) == options);
    unsigned oldopts = cx->allOptions();
    unsigned newropts = options & JSRUNOPTION_MASK;
    unsigned newcopts = options & JSCOMPILEOPTION_MASK;
    cx->setRunOptions(newropts);
    cx->setCompileOptions(newcopts);
    cx->updateJITEnabled();
    return oldopts;
}

JS_PUBLIC_API(uint32_t)
JS_SetOptions(JSContext *cx, uint32_t options)
{
    return SetOptionsCommon(cx, options);
}

JS_PUBLIC_API(uint32_t)
JS_ToggleOptions(JSContext *cx, uint32_t options)
{
    unsigned oldopts = cx->allOptions();
    unsigned newopts = oldopts ^ options;
    return SetOptionsCommon(cx, newopts);
}

JS_PUBLIC_API(void)
JS_SetJitHardening(JSRuntime *rt, JSBool enabled)
{
    rt->setJitHardening(!!enabled);
}

JS_PUBLIC_API(const char *)
JS_GetImplementationVersion(void)
{
    return "JavaScript-C 1.8.5+ 2011-04-16";
}

JS_PUBLIC_API(void)
JS_SetDestroyCompartmentCallback(JSRuntime *rt, JSDestroyCompartmentCallback callback)
{
    rt->destroyCompartmentCallback = callback;
}

JS_PUBLIC_API(void)
JS_SetCompartmentNameCallback(JSRuntime *rt, JSCompartmentNameCallback callback)
{
    rt->compartmentNameCallback = callback;
}

JS_PUBLIC_API(JSWrapObjectCallback)
JS_SetWrapObjectCallbacks(JSRuntime *rt,
                          JSWrapObjectCallback callback,
                          JSSameCompartmentWrapObjectCallback sccallback,
                          JSPreWrapCallback precallback)
{
    JSWrapObjectCallback old = rt->wrapObjectCallback;
    rt->wrapObjectCallback = callback;
    rt->sameCompartmentWrapObjectCallback = sccallback;
    rt->preWrapObjectCallback = precallback;
    return old;
}

struct JSCrossCompartmentCall
{
    JSContext *context;
    JSCompartment *oldCompartment;
};

JS_PUBLIC_API(JSCrossCompartmentCall *)
JS_EnterCrossCompartmentCall(JSContext *cx, JSRawObject target)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    JSCrossCompartmentCall *call = OffTheBooks::new_<JSCrossCompartmentCall>();
    if (!call)
        return NULL;

    call->context = cx;
    call->oldCompartment = cx->compartment;

    cx->enterCompartment(target->compartment());
    return call;
}

JS_PUBLIC_API(JSCrossCompartmentCall *)
JS_EnterCrossCompartmentCallScript(JSContext *cx, JSScript *target)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    GlobalObject &global = target->global();
    return JS_EnterCrossCompartmentCall(cx, &global);
}

JS_PUBLIC_API(JSCrossCompartmentCall *)
JS_EnterCrossCompartmentCallStackFrame(JSContext *cx, JSStackFrame *target)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    HandleObject global(HandleObject::fromMarkedLocation((JSObject**) &Valueify(target)->global()));
    return JS_EnterCrossCompartmentCall(cx, global);
}

JS_PUBLIC_API(void)
JS_LeaveCrossCompartmentCall(JSCrossCompartmentCall *call)
{
    AssertHeapIsIdle(call->context);
    CHECK_REQUEST(call->context);
    call->context->leaveCompartment(call->oldCompartment);
    Foreground::delete_(call);
}

JSAutoCompartment::JSAutoCompartment(JSContext *cx, JSRawObject target)
  : cx_(cx),
    oldCompartment_(cx->compartment)
{
    AssertHeapIsIdleOrIterating(cx_);
    cx_->enterCompartment(target->compartment());
}

JSAutoCompartment::~JSAutoCompartment()
{
    cx_->leaveCompartment(oldCompartment_);
}

bool
AutoEnterScriptCompartment::enter(JSContext *cx, JSScript *target)
{
    JS_ASSERT(!call);
    if (cx->compartment == target->compartment()) {
        call = reinterpret_cast<JSCrossCompartmentCall*>(1);
        return true;
    }
    call = JS_EnterCrossCompartmentCallScript(cx, target);
    return call != NULL;
}

bool
AutoEnterFrameCompartment::enter(JSContext *cx, JSStackFrame *target)
{
    JS_ASSERT(!call);
    if (cx->compartment == Valueify(target)->scopeChain()->compartment()) {
        call = reinterpret_cast<JSCrossCompartmentCall*>(1);
        return true;
    }
    call = JS_EnterCrossCompartmentCallStackFrame(cx, target);
    return call != NULL;
}

JS_PUBLIC_API(void)
JS_SetCompartmentPrivate(JSCompartment *compartment, void *data)
{
    compartment->data = data;
}

JS_PUBLIC_API(void *)
JS_GetCompartmentPrivate(JSCompartment *compartment)
{
    return compartment->data;
}

JS_PUBLIC_API(JSBool)
JS_WrapObject(JSContext *cx, JSObject **objp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return cx->compartment->wrap(cx, objp);
}

JS_PUBLIC_API(JSBool)
JS_WrapValue(JSContext *cx, jsval *vp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return cx->compartment->wrap(cx, vp);
}

/*
 * Identity remapping. Not for casual consumers.
 *
 * Normally, an object's contents and its identity are inextricably linked.
 * Identity is determined by the address of the JSObject* in the heap, and
 * the contents are what is located at that address. Transplanting allows these
 * concepts to be separated through a combination of swapping (exchanging the
 * contents of two same-compartment objects) and remapping cross-compartment
 * identities by altering wrappers.
 *
 * The |origobj| argument should be the object whose identity needs to be
 * remapped, usually to another compartment. The contents of |origobj| are
 * destroyed.
 *
 * The |target| argument serves two purposes:
 *
 * First, |target| serves as a hint for the new identity of the object. The new
 * identity object will always be in the same compartment as |target|, but
 * if that compartment already had an object representing |origobj| (either a
 * cross-compartment wrapper for it, or |origobj| itself if the two arguments
 * are same-compartment), the existing object is used. Otherwise, |target|
 * itself is used. To avoid ambiguity, JS_TransplantObject always returns the
 * new identity.
 *
 * Second, the new identity object's contents will be those of |target|. A swap()
 * is used to make this happen if an object other than |target| is used.
 */

JS_PUBLIC_API(JSObject *)
JS_TransplantObject(JSContext *cx, JSObject *origobjArg, JSObject *targetArg)
{
    RootedObject origobj(cx, origobjArg);
    RootedObject target(cx, targetArg);
    AssertHeapIsIdle(cx);
    JS_ASSERT(origobj != target);
    JS_ASSERT(!IsCrossCompartmentWrapper(origobj));
    JS_ASSERT(!IsCrossCompartmentWrapper(target));

    /*
     * Transplantation typically allocates new wrappers in every compartment. If
     * an incremental GC is active, this causes every compartment to be leaked
     * for that GC. Hence, we finish any ongoing incremental GC before the
     * transplant to avoid leaks.
     */
    if (cx->runtime->gcIncrementalState != NO_INCREMENTAL) {
        PrepareForIncrementalGC(cx->runtime);
        FinishIncrementalGC(cx->runtime, gcreason::TRANSPLANT);
    }

    JSCompartment *destination = target->compartment();
    WrapperMap &map = destination->crossCompartmentWrappers;
    Value origv = ObjectValue(*origobj);
    JSObject *newIdentity;

    if (origobj->compartment() == destination) {
        // If the original object is in the same compartment as the
        // destination, then we know that we won't find a wrapper in the
        // destination's cross compartment map and that the same
        // object will continue to work.
        if (!origobj->swap(cx, target))
            return NULL;
        newIdentity = origobj;
    } else if (WrapperMap::Ptr p = map.lookup(origv)) {
        // There might already be a wrapper for the original object in
        // the new compartment. If there is, we use its identity and swap
        // in the contents of |target|.
        newIdentity = &p->value.toObject();

        // When we remove origv from the wrapper map, its wrapper, newIdentity,
        // must immediately cease to be a cross-compartment wrapper. Neuter it.
        map.remove(p);
        NukeCrossCompartmentWrapper(newIdentity);

        if (!newIdentity->swap(cx, target))
            return NULL;
    } else {
        // Otherwise, we use |target| for the new identity object.
        newIdentity = target;
    }

    // Now, iterate through other scopes looking for references to the
    // old object, and update the relevant cross-compartment wrappers.
    if (!RemapAllWrappersForObject(cx, origobj, newIdentity))
        return NULL;

    // Lastly, update the original object to point to the new one.
    if (origobj->compartment() != destination) {
        RootedObject newIdentityWrapper(cx, newIdentity);
        AutoCompartment ac(cx, origobj);
        if (!JS_WrapObject(cx, newIdentityWrapper.address()))
            return NULL;
        if (!origobj->swap(cx, newIdentityWrapper))
            return NULL;
        origobj->compartment()->crossCompartmentWrappers.put(ObjectValue(*newIdentity), origv);
    }

    // The new identity object might be one of several things. Return it to avoid
    // ambiguity.
    return newIdentity;
}

/*
 * Some C++ objects (such as the location object and XBL) require both an XPConnect
 * reflector and a security wrapper for that reflector. We expect that there are
 * no live references to the reflector, so when we perform the transplant we turn
 * the security wrapper into a cross-compartment wrapper. Just in case there
 * happen to be live references to the reflector, we swap it out to limit the harm.
 */
JS_FRIEND_API(JSObject *)
js_TransplantObjectWithWrapper(JSContext *cx,
                               JSObject *origobjArg,
                               JSObject *origwrapperArg,
                               JSObject *targetobjArg,
                               JSObject *targetwrapperArg)
{
    RootedObject origobj(cx, origobjArg);
    RootedObject origwrapper(cx, origwrapperArg);
    RootedObject targetobj(cx, targetobjArg);
    RootedObject targetwrapper(cx, targetwrapperArg);

    AssertHeapIsIdle(cx);
    JS_ASSERT(!IsCrossCompartmentWrapper(origobj));
    JS_ASSERT(!IsCrossCompartmentWrapper(origwrapper));
    JS_ASSERT(!IsCrossCompartmentWrapper(targetobj));
    JS_ASSERT(!IsCrossCompartmentWrapper(targetwrapper));

    JSObject *newWrapper;
    JSCompartment *destination = targetobj->compartment();
    WrapperMap &map = destination->crossCompartmentWrappers;

    // |origv| is the map entry we're looking up. The map entries are going to
    // be for |origobj|, not |origwrapper|.
    Value origv = ObjectValue(*origobj);

    // There might already be a wrapper for the original object in the new
    // compartment.
    if (WrapperMap::Ptr p = map.lookup(origv)) {
        // There is. Make the existing cross-compartment wrapper a same-
        // compartment wrapper.
        newWrapper = &p->value.toObject();

        // When we remove origv from the wrapper map, its wrapper, newWrapper,
        // must immediately cease to be a cross-compartment wrapper. Neuter it.
        map.remove(p);
        NukeCrossCompartmentWrapper(newWrapper);

        if (!newWrapper->swap(cx, targetwrapper))
            return NULL;
    } else {
        // Otherwise, use the passed-in wrapper as the same-compartment wrapper.
        newWrapper = targetwrapper;
    }

    // Now, iterate through other scopes looking for references to the old
    // object. Note that the entries in the maps are for |origobj| and not
    // |origwrapper|. They need to be updated to point at the new object.
    if (!RemapAllWrappersForObject(cx, origobj, targetobj))
        return NULL;

    // Lastly, update things in the original compartment. Our invariants dictate
    // that the original compartment can only have one cross-compartment wrapper
    // to the new object. So we choose to update |origwrapper|, not |origobj|,
    // since there are probably no live direct intra-compartment references to
    // |origobj|.
    {
        AutoCompartment ac(cx, origobj);

        // We can't be sure that the reflector is completely dead. This is bad,
        // because it is in a weird state. To minimize potential harm we create
        // a new unreachable dummy object and swap it with the reflector.
        // After the swap we have a possibly-live object that isn't dangerous,
        // and a possibly-dangerous object that isn't live.
        RootedObject reflectorGuts(cx, NewDeadProxyObject(cx, JS_GetGlobalForObject(cx, origobj)));
        if (!reflectorGuts || !origobj->swap(cx, reflectorGuts))
            return NULL;

        // Turn origwrapper into a CCW to the new object.
        RootedObject wrapperGuts(cx, targetobj);
        if (!JS_WrapObject(cx, wrapperGuts.address()))
            return NULL;
        if (!origwrapper->swap(cx, wrapperGuts))
            return NULL;
        origwrapper->compartment()->crossCompartmentWrappers.put(ObjectValue(*targetobj),
                                                                 ObjectValue(*origwrapper));
    }

    return newWrapper;
}

/*
 * Recompute all cross-compartment wrappers for an object, resetting state.
 * Gecko uses this to clear Xray wrappers when doing a navigation that reuses
 * the inner window and global object.
 */
JS_PUBLIC_API(JSBool)
JS_RefreshCrossCompartmentWrappers(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);
    return RemapAllWrappersForObject(cx, obj, obj);
}

JS_PUBLIC_API(JSObject *)
JS_GetGlobalObject(JSContext *cx)
{
    return cx->maybeDefaultCompartmentObject();
}

JS_PUBLIC_API(void)
JS_SetGlobalObject(JSContext *cx, JSRawObject obj)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    cx->setDefaultCompartmentObject(obj);
}

JS_PUBLIC_API(JSBool)
JS_InitStandardClasses(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    cx->setDefaultCompartmentObjectIfUnset(obj);
    assertSameCompartment(cx, obj);

    Rooted<GlobalObject*> global(cx, &obj->global());
    return GlobalObject::initStandardClasses(cx, global);
}

#define CLASP(name)                 (&name##Class)
#define TYPED_ARRAY_CLASP(type)     (&TypedArray::classes[TypedArray::type])
#define EAGER_ATOM(name)            NAME_OFFSET(name)
#define EAGER_CLASS_ATOM(name)      CLASS_NAME_OFFSET(name)
#define EAGER_ATOM_AND_CLASP(name)  EAGER_CLASS_ATOM(name), CLASP(name)

typedef struct JSStdName {
    JSClassInitializerOp init;
    size_t      atomOffset;     /* offset of atom pointer in JSAtomState */
    Class       *clasp;
} JSStdName;

static PropertyName *
StdNameToPropertyName(JSContext *cx, JSStdName *stdn)
{
    return OFFSET_TO_NAME(cx->runtime, stdn->atomOffset);
}

/*
 * Table of class initializers and their atom offsets in rt->atomState.
 * If you add a "standard" class, remember to update this table.
 */
static JSStdName standard_class_atoms[] = {
    {js_InitFunctionClass,              EAGER_ATOM_AND_CLASP(Function)},
    {js_InitObjectClass,                EAGER_ATOM_AND_CLASP(Object)},
    {js_InitArrayClass,                 EAGER_ATOM_AND_CLASP(Array)},
    {js_InitBooleanClass,               EAGER_ATOM_AND_CLASP(Boolean)},
    {js_InitDateClass,                  EAGER_ATOM_AND_CLASP(Date)},
    {js_InitMathClass,                  EAGER_ATOM_AND_CLASP(Math)},
    {js_InitNumberClass,                EAGER_ATOM_AND_CLASP(Number)},
    {js_InitStringClass,                EAGER_ATOM_AND_CLASP(String)},
    {js_InitExceptionClasses,           EAGER_ATOM_AND_CLASP(Error)},
    {js_InitRegExpClass,                EAGER_ATOM_AND_CLASP(RegExp)},
#if JS_HAS_XML_SUPPORT
    {js_InitXMLClass,                   EAGER_ATOM_AND_CLASP(XML)},
    {js_InitNamespaceClass,             EAGER_ATOM_AND_CLASP(Namespace)},
    {js_InitQNameClass,                 EAGER_ATOM_AND_CLASP(QName)},
#endif
#if JS_HAS_GENERATORS
    {js_InitIteratorClasses,            EAGER_ATOM_AND_CLASP(StopIteration)},
#endif
    {js_InitJSONClass,                  EAGER_ATOM_AND_CLASP(JSON)},
    {js_InitTypedArrayClasses,          EAGER_CLASS_ATOM(ArrayBuffer), &js::ArrayBufferObject::protoClass},
    {js_InitWeakMapClass,               EAGER_CLASS_ATOM(WeakMap), &js::WeakMapClass},
    {js_InitMapClass,                   EAGER_CLASS_ATOM(Map), &js::MapObject::class_},
    {js_InitSetClass,                   EAGER_CLASS_ATOM(Set), &js::SetObject::class_},
    {js_InitParallelArrayClass,         EAGER_CLASS_ATOM(ParallelArray), &js::ParallelArrayObject::class_},
    {NULL,                              0, NULL}
};

/*
 * Table of top-level function and constant names and their init functions.
 * If you add a "standard" global function or property, remember to update
 * this table.
 */
static JSStdName standard_class_names[] = {
    {js_InitObjectClass,        EAGER_ATOM(eval), CLASP(Object)},

    /* Global properties and functions defined by the Number class. */
    {js_InitNumberClass,        EAGER_ATOM(NaN), CLASP(Number)},
    {js_InitNumberClass,        EAGER_ATOM(Infinity), CLASP(Number)},
    {js_InitNumberClass,        EAGER_ATOM(isNaN), CLASP(Number)},
    {js_InitNumberClass,        EAGER_ATOM(isFinite), CLASP(Number)},
    {js_InitNumberClass,        EAGER_ATOM(parseFloat), CLASP(Number)},
    {js_InitNumberClass,        EAGER_ATOM(parseInt), CLASP(Number)},

    /* String global functions. */
    {js_InitStringClass,        EAGER_ATOM(escape), CLASP(String)},
    {js_InitStringClass,        EAGER_ATOM(unescape), CLASP(String)},
    {js_InitStringClass,        EAGER_ATOM(decodeURI), CLASP(String)},
    {js_InitStringClass,        EAGER_ATOM(encodeURI), CLASP(String)},
    {js_InitStringClass,        EAGER_ATOM(decodeURIComponent), CLASP(String)},
    {js_InitStringClass,        EAGER_ATOM(encodeURIComponent), CLASP(String)},
#if JS_HAS_UNEVAL
    {js_InitStringClass,        EAGER_ATOM(uneval), CLASP(String)},
#endif

    /* Exception constructors. */
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(Error), CLASP(Error)},
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(InternalError), CLASP(Error)},
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(EvalError), CLASP(Error)},
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(RangeError), CLASP(Error)},
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(ReferenceError), CLASP(Error)},
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(SyntaxError), CLASP(Error)},
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(TypeError), CLASP(Error)},
    {js_InitExceptionClasses,   EAGER_CLASS_ATOM(URIError), CLASP(Error)},

#if JS_HAS_XML_SUPPORT
    {js_InitXMLClass,           EAGER_ATOM(XMLList), CLASP(XML)},
    {js_InitXMLClass,           EAGER_ATOM(isXMLName), CLASP(XML)},
#endif

    {js_InitIteratorClasses,    EAGER_CLASS_ATOM(Iterator), &PropertyIteratorObject::class_},

    /* Typed Arrays */
    {js_InitTypedArrayClasses,  EAGER_CLASS_ATOM(ArrayBuffer),  &ArrayBufferClass},
    {js_InitTypedArrayClasses,  EAGER_CLASS_ATOM(Int8Array),    TYPED_ARRAY_CLASP(TYPE_INT8)},
    {js_InitTypedArrayClasses,  EAGER_CLASS_ATOM(Uint8Array),   TYPED_ARRAY_CLASP(TYPE_UINT8)},
    {js_InitTypedArrayClasses,  EAGER_CLASS_ATOM(Int16Array),   TYPED_ARRAY_CLASP(TYPE_INT16)},
    {js_InitTypedArrayClasses,  EAGER_CLASS_ATOM(Uint16Array),  TYPED_ARRAY_CLASP(TYPE_UINT16)},
    {js_InitTypedArrayClasses,  EAGER_CLASS_ATOM(Int32Array),   TYPED_ARRAY_CLASP(TYPE_INT32)},
    {js_InitTypedArrayClasses,  EAGER_CLASS_ATOM(Uint32Array),  TYPED_ARRAY_CLASP(TYPE_UINT32)},
    {js_InitTypedArrayClasses,  EAGER_CLASS_ATOM(Float32Array), TYPED_ARRAY_CLASP(TYPE_FLOAT32)},
    {js_InitTypedArrayClasses,  EAGER_CLASS_ATOM(Float64Array), TYPED_ARRAY_CLASP(TYPE_FLOAT64)},
    {js_InitTypedArrayClasses,  EAGER_CLASS_ATOM(Uint8ClampedArray),
                                TYPED_ARRAY_CLASP(TYPE_UINT8_CLAMPED)},
    {js_InitTypedArrayClasses,  EAGER_CLASS_ATOM(DataView),     &DataViewClass},

    {js_InitWeakMapClass,       EAGER_ATOM_AND_CLASP(WeakMap)},
    {js_InitProxyClass,         EAGER_ATOM_AND_CLASP(Proxy)},

    {NULL,                      0, NULL}
};

static JSStdName object_prototype_names[] = {
    /* Object.prototype properties (global delegates to Object.prototype). */
    {js_InitObjectClass,        EAGER_ATOM(proto), CLASP(Object)},
#if JS_HAS_TOSOURCE
    {js_InitObjectClass,        EAGER_ATOM(toSource), CLASP(Object)},
#endif
    {js_InitObjectClass,        EAGER_ATOM(toString), CLASP(Object)},
    {js_InitObjectClass,        EAGER_ATOM(toLocaleString), CLASP(Object)},
    {js_InitObjectClass,        EAGER_ATOM(valueOf), CLASP(Object)},
#if JS_HAS_OBJ_WATCHPOINT
    {js_InitObjectClass,        EAGER_ATOM(watch), CLASP(Object)},
    {js_InitObjectClass,        EAGER_ATOM(unwatch), CLASP(Object)},
#endif
    {js_InitObjectClass,        EAGER_ATOM(hasOwnProperty), CLASP(Object)},
    {js_InitObjectClass,        EAGER_ATOM(isPrototypeOf), CLASP(Object)},
    {js_InitObjectClass,        EAGER_ATOM(propertyIsEnumerable), CLASP(Object)},
#if OLD_GETTER_SETTER_METHODS
    {js_InitObjectClass,        EAGER_ATOM(defineGetter), CLASP(Object)},
    {js_InitObjectClass,        EAGER_ATOM(defineSetter), CLASP(Object)},
    {js_InitObjectClass,        EAGER_ATOM(lookupGetter), CLASP(Object)},
    {js_InitObjectClass,        EAGER_ATOM(lookupSetter), CLASP(Object)},
#endif

    {NULL,                      0, NULL}
};

JS_PUBLIC_API(JSBool)
JS_ResolveStandardClass(JSContext *cx, JSObject *objArg, jsid id, JSBool *resolved)
{
    RootedObject obj(cx, objArg);
    JSString *idstr;
    JSRuntime *rt;
    JSAtom *atom;
    JSStdName *stdnm;
    unsigned i;

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, id);
    *resolved = false;

    rt = cx->runtime;
    if (!rt->hasContexts() || !JSID_IS_ATOM(id))
        return true;

    idstr = JSID_TO_STRING(id);

    /* Check whether we're resolving 'undefined', and define it if so. */
    atom = rt->atomState.typeAtoms[JSTYPE_VOID];
    if (idstr == atom) {
        *resolved = true;
        RootedValue undefinedValue(cx, UndefinedValue());
        return JSObject::defineProperty(cx, obj, atom->asPropertyName(), undefinedValue,
                                        JS_PropertyStub, JS_StrictPropertyStub,
                                        JSPROP_PERMANENT | JSPROP_READONLY);
    }

    /* Try for class constructors/prototypes named by well-known atoms. */
    stdnm = NULL;
    for (i = 0; standard_class_atoms[i].init; i++) {
        JS_ASSERT(standard_class_atoms[i].clasp);
        atom = OFFSET_TO_NAME(rt, standard_class_atoms[i].atomOffset);
        if (idstr == atom) {
            stdnm = &standard_class_atoms[i];
            break;
        }
    }

    if (!stdnm) {
        /* Try less frequently used top-level functions and constants. */
        for (i = 0; standard_class_names[i].init; i++) {
            JS_ASSERT(standard_class_names[i].clasp);
            atom = StdNameToPropertyName(cx, &standard_class_names[i]);
            if (!atom)
                return false;
            if (idstr == atom) {
                stdnm = &standard_class_names[i];
                break;
            }
        }

        if (!stdnm && !obj->getProto()) {
            /*
             * Try even less frequently used names delegated from the global
             * object to Object.prototype, but only if the Object class hasn't
             * yet been initialized.
             */
            for (i = 0; object_prototype_names[i].init; i++) {
                JS_ASSERT(object_prototype_names[i].clasp);
                atom = StdNameToPropertyName(cx, &object_prototype_names[i]);
                if (!atom)
                    return false;
                if (idstr == atom) {
                    stdnm = &object_prototype_names[i];
                    break;
                }
            }
        }
    }

    if (stdnm) {
        /*
         * If this standard class is anonymous, then we don't want to resolve
         * by name.
         */
        JS_ASSERT(obj->isGlobal());
        if (stdnm->clasp->flags & JSCLASS_IS_ANONYMOUS)
            return true;

        if (IsStandardClassResolved(obj, stdnm->clasp))
            return true;

#if JS_HAS_XML_SUPPORT
        if ((stdnm->init == js_InitXMLClass ||
             stdnm->init == js_InitNamespaceClass ||
             stdnm->init == js_InitQNameClass) &&
            !VersionHasAllowXML(cx->findVersion()))
        {
            return true;
        }
#endif

        if (!stdnm->init(cx, obj))
            return false;
        *resolved = true;
    }
    return true;
}

JS_PUBLIC_API(JSBool)
JS_EnumerateStandardClasses(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);

    /*
     * Check whether we need to bind 'undefined' and define it if so.
     * Since ES5 15.1.1.3 undefined can't be deleted.
     */
    RootedPropertyName undefinedName(cx, cx->runtime->atomState.typeAtoms[JSTYPE_VOID]);
    RootedId undefinedId(cx, NameToId(undefinedName));
    RootedValue undefinedValue(cx, UndefinedValue());
    if (!obj->nativeContains(cx, undefinedId) &&
        !JSObject::defineProperty(cx, obj, undefinedName, undefinedValue,
                                  JS_PropertyStub, JS_StrictPropertyStub,
                                  JSPROP_PERMANENT | JSPROP_READONLY)) {
        return false;
    }

    /* Initialize any classes that have not been initialized yet. */
    for (unsigned i = 0; standard_class_atoms[i].init; i++) {
        const JSStdName &stdnm = standard_class_atoms[i];
        if (!js::IsStandardClassResolved(obj, stdnm.clasp)
#if JS_HAS_XML_SUPPORT
            && ((stdnm.init != js_InitXMLClass &&
                 stdnm.init != js_InitNamespaceClass &&
                 stdnm.init != js_InitQNameClass) ||
                VersionHasAllowXML(cx->findVersion()))
#endif
            )
        {
            if (!stdnm.init(cx, obj))
                return false;
        }
    }

    return true;
}

static JSIdArray *
NewIdArray(JSContext *cx, int length)
{
    JSIdArray *ida;

    ida = (JSIdArray *)
        cx->calloc_(offsetof(JSIdArray, vector) + length * sizeof(jsval));
    if (ida)
        ida->length = length;
    return ida;
}

/*
 * Unlike realloc(3), this function frees ida on failure.
 */
static JSIdArray *
SetIdArrayLength(JSContext *cx, JSIdArray *ida, int length)
{
    JSIdArray *rida;

    rida = (JSIdArray *)
           JS_realloc(cx, ida,
                      offsetof(JSIdArray, vector) + length * sizeof(jsval));
    if (!rida) {
        JS_DestroyIdArray(cx, ida);
    } else {
        rida->length = length;
    }
    return rida;
}

static JSIdArray *
AddNameToArray(JSContext *cx, PropertyName *name, JSIdArray *ida, int *ip)
{
    int i = *ip;
    int length = ida->length;
    if (i >= length) {
        ida = SetIdArrayLength(cx, ida, Max(length * 2, 8));
        if (!ida)
            return NULL;
        JS_ASSERT(i < ida->length);
    }
    ida->vector[i].init(NameToId(name));
    *ip = i + 1;
    return ida;
}

static JSIdArray *
EnumerateIfResolved(JSContext *cx, JSHandleObject obj, PropertyName *name, JSIdArray *ida,
                    int *ip, JSBool *foundp)
{
    RootedId id(cx, NameToId(name));
    *foundp = obj->nativeContains(cx, id);
    if (*foundp)
        ida = AddNameToArray(cx, name, ida, ip);
    return ida;
}

JS_PUBLIC_API(JSIdArray *)
JS_EnumerateResolvedStandardClasses(JSContext *cx, JSObject *objArg, JSIdArray *ida)
{
    RootedObject obj(cx, objArg);
    JSRuntime *rt;
    int i, j, k;
    PropertyName *name;
    JSBool found;
    JSClassInitializerOp init;

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, ida);
    rt = cx->runtime;
    if (ida) {
        i = ida->length;
    } else {
        ida = NewIdArray(cx, 8);
        if (!ida)
            return NULL;
        i = 0;
    }

    /* Check whether 'undefined' has been resolved and enumerate it if so. */
    name = rt->atomState.typeAtoms[JSTYPE_VOID];
    ida = EnumerateIfResolved(cx, obj, name, ida, &i, &found);
    if (!ida)
        return NULL;

    /* Enumerate only classes that *have* been resolved. */
    for (j = 0; standard_class_atoms[j].init; j++) {
        name = OFFSET_TO_NAME(rt, standard_class_atoms[j].atomOffset);
        ida = EnumerateIfResolved(cx, obj, name, ida, &i, &found);
        if (!ida)
            return NULL;

        if (found) {
            init = standard_class_atoms[j].init;

            for (k = 0; standard_class_names[k].init; k++) {
                if (standard_class_names[k].init == init) {
                    name = StdNameToPropertyName(cx, &standard_class_names[k]);
                    ida = AddNameToArray(cx, name, ida, &i);
                    if (!ida)
                        return NULL;
                }
            }

            if (init == js_InitObjectClass) {
                for (k = 0; object_prototype_names[k].init; k++) {
                    name = StdNameToPropertyName(cx, &object_prototype_names[k]);
                    ida = AddNameToArray(cx, name, ida, &i);
                    if (!ida)
                        return NULL;
                }
            }
        }
    }

    /* Trim to exact length. */
    return SetIdArrayLength(cx, ida, i);
}

#undef CLASP
#undef EAGER_ATOM
#undef EAGER_CLASS_ATOM
#undef EAGER_ATOM_CLASP

JS_PUBLIC_API(JSBool)
JS_GetClassObject(JSContext *cx, JSRawObject obj, JSProtoKey key, JSObject **objpArg)
{
    RootedObject objp(cx, *objpArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    assertSameCompartment(cx, obj);
    if (!js_GetClassObject(cx, obj, key, &objp))
        return false;
    *objpArg = objp;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_GetClassPrototype(JSContext *cx, JSProtoKey key, JSObject **objp_)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    RootedObject objp(cx);
    bool result = js_GetClassPrototype(cx, key, &objp);
    *objp_ = objp;
    return result;
}

JS_PUBLIC_API(JSProtoKey)
JS_IdentifyClassPrototype(JSContext *cx, JSObject *obj)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    JS_ASSERT(!IsCrossCompartmentWrapper(obj));
    return js_IdentifyClassPrototype(obj);
}

JS_PUBLIC_API(JSObject *)
JS_GetObjectPrototype(JSContext *cx, JSRawObject forObj)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, forObj);
    return forObj->global().getOrCreateObjectPrototype(cx);
}

JS_PUBLIC_API(JSObject *)
JS_GetFunctionPrototype(JSContext *cx, JSRawObject forObj)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, forObj);
    return forObj->global().getOrCreateFunctionPrototype(cx);
}

JS_PUBLIC_API(JSObject *)
JS_GetGlobalForObject(JSContext *cx, JSRawObject obj)
{
    AssertHeapIsIdle(cx);
    assertSameCompartment(cx, obj);
    return &obj->global();
}

JS_PUBLIC_API(JSObject *)
JS_GetGlobalForCompartmentOrNull(JSContext *cx, JSCompartment *c)
{
    AssertHeapIsIdleOrIterating(cx);
    assertSameCompartment(cx, c);
    return c->maybeGlobal();
}

JS_PUBLIC_API(JSObject *)
JS_GetGlobalForScopeChain(JSContext *cx)
{
    AssertHeapIsIdleOrIterating(cx);
    CHECK_REQUEST(cx);
    return cx->global();
}

JS_PUBLIC_API(jsval)
JS_ComputeThis(JSContext *cx, jsval *vp)
{
    AssertHeapIsIdle(cx);
    assertSameCompartment(cx, JSValueArray(vp, 2));
    CallReceiver call = CallReceiverFromVp(vp);
    if (!BoxNonStrictThis(cx, call))
        return JSVAL_NULL;
    return call.thisv();
}

JS_PUBLIC_API(void)
JS_MallocInCompartment(JSCompartment *comp, size_t nbytes)
{
    comp->mallocInCompartment(nbytes);
}

JS_PUBLIC_API(void)
JS_FreeInCompartment(JSCompartment *comp, size_t nbytes)
{
    comp->freeInCompartment(nbytes);
}

JS_PUBLIC_API(void *)
JS_malloc(JSContext *cx, size_t nbytes)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return cx->malloc_(nbytes);
}

JS_PUBLIC_API(void *)
JS_realloc(JSContext *cx, void *p, size_t nbytes)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return cx->realloc_(p, nbytes);
}

JS_PUBLIC_API(void)
JS_free(JSContext *cx, void *p)
{
    return cx->free_(p);
}

JS_PUBLIC_API(void)
JS_freeop(JSFreeOp *fop, void *p)
{
    return FreeOp::get(fop)->free_(p);
}

JS_PUBLIC_API(JSFreeOp *)
JS_GetDefaultFreeOp(JSRuntime *rt)
{
    return rt->defaultFreeOp();
}

JS_PUBLIC_API(void)
JS_updateMallocCounter(JSContext *cx, size_t nbytes)
{
    return cx->runtime->updateMallocCounter(cx, nbytes);
}

JS_PUBLIC_API(char *)
JS_strdup(JSContext *cx, const char *s)
{
    AssertHeapIsIdle(cx);
    size_t n = strlen(s) + 1;
    void *p = cx->malloc_(n);
    if (!p)
        return NULL;
    return (char *)js_memcpy(p, s, n);
}

#undef JS_AddRoot

JS_PUBLIC_API(JSBool)
JS_AddValueRoot(JSContext *cx, jsval *vp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_AddRoot(cx, vp, NULL);
}

JS_PUBLIC_API(JSBool)
JS_AddStringRoot(JSContext *cx, JSString **rp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_AddGCThingRoot(cx, (void **)rp, NULL);
}

JS_PUBLIC_API(JSBool)
JS_AddObjectRoot(JSContext *cx, JSObject **rp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_AddGCThingRoot(cx, (void **)rp, NULL);
}

JS_PUBLIC_API(JSBool)
JS_AddGCThingRoot(JSContext *cx, void **rp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_AddGCThingRoot(cx, (void **)rp, NULL);
}

JS_PUBLIC_API(JSBool)
JS_AddNamedValueRoot(JSContext *cx, jsval *vp, const char *name)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_AddRoot(cx, vp, name);
}

JS_PUBLIC_API(JSBool)
JS_AddNamedStringRoot(JSContext *cx, JSString **rp, const char *name)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_AddGCThingRoot(cx, (void **)rp, name);
}

JS_PUBLIC_API(JSBool)
JS_AddNamedObjectRoot(JSContext *cx, JSObject **rp, const char *name)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_AddGCThingRoot(cx, (void **)rp, name);
}

JS_PUBLIC_API(JSBool)
JS_AddNamedScriptRoot(JSContext *cx, JSScript **rp, const char *name)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_AddGCThingRoot(cx, (void **)rp, name);
}

JS_PUBLIC_API(JSBool)
JS_AddNamedGCThingRoot(JSContext *cx, void **rp, const char *name)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_AddGCThingRoot(cx, (void **)rp, name);
}

/* We allow unrooting from finalizers within the GC */

JS_PUBLIC_API(void)
JS_RemoveValueRoot(JSContext *cx, jsval *vp)
{
    CHECK_REQUEST(cx);
    js_RemoveRoot(cx->runtime, (void *)vp);
}

JS_PUBLIC_API(void)
JS_RemoveStringRoot(JSContext *cx, JSString **rp)
{
    CHECK_REQUEST(cx);
    js_RemoveRoot(cx->runtime, (void *)rp);
}

JS_PUBLIC_API(void)
JS_RemoveObjectRoot(JSContext *cx, JSObject **rp)
{
    CHECK_REQUEST(cx);
    js_RemoveRoot(cx->runtime, (void *)rp);
}

JS_PUBLIC_API(void)
JS_RemoveScriptRoot(JSContext *cx, JSScript **rp)
{
    CHECK_REQUEST(cx);
    js_RemoveRoot(cx->runtime, (void *)rp);
}

JS_PUBLIC_API(void)
JS_RemoveGCThingRoot(JSContext *cx, void **rp)
{
    CHECK_REQUEST(cx);
    js_RemoveRoot(cx->runtime, (void *)rp);
}

JS_PUBLIC_API(void)
JS_RemoveValueRootRT(JSRuntime *rt, jsval *vp)
{
    js_RemoveRoot(rt, (void *)vp);
}

JS_PUBLIC_API(void)
JS_RemoveStringRootRT(JSRuntime *rt, JSString **rp)
{
    js_RemoveRoot(rt, (void *)rp);
}

JS_PUBLIC_API(void)
JS_RemoveObjectRootRT(JSRuntime *rt, JSObject **rp)
{
    js_RemoveRoot(rt, (void *)rp);
}

JS_PUBLIC_API(void)
JS_RemoveScriptRoot(JSRuntime *rt, JSScript **rp)
{
    js_RemoveRoot(rt, (void *)rp);
}

JS_NEVER_INLINE JS_PUBLIC_API(void)
JS_AnchorPtr(void *p)
{
}

#ifdef DEBUG

JS_PUBLIC_API(void)
JS_DumpNamedRoots(JSRuntime *rt,
                  void (*dump)(const char *name, void *rp, JSGCRootType type, void *data),
                  void *data)
{
    js_DumpNamedRoots(rt, dump, data);
}

#endif /* DEBUG */

JS_PUBLIC_API(uint32_t)
JS_MapGCRoots(JSRuntime *rt, JSGCRootMapFun map, void *data)
{
    return js_MapGCRoots(rt, map, data);
}

JS_PUBLIC_API(JSBool)
JS_LockGCThing(JSContext *cx, void *thing)
{
    JSBool ok;

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    ok = js_LockGCThingRT(cx->runtime, thing);
    if (!ok)
        JS_ReportOutOfMemory(cx);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_LockGCThingRT(JSRuntime *rt, void *thing)
{
    return js_LockGCThingRT(rt, thing);
}

JS_PUBLIC_API(JSBool)
JS_UnlockGCThing(JSContext *cx, void *thing)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    js_UnlockGCThingRT(cx->runtime, thing);
    return true;
}

JS_PUBLIC_API(JSBool)
JS_UnlockGCThingRT(JSRuntime *rt, void *thing)
{
    js_UnlockGCThingRT(rt, thing);
    return true;
}

JS_PUBLIC_API(void)
JS_SetExtraGCRootsTracer(JSRuntime *rt, JSTraceDataOp traceOp, void *data)
{
    AssertHeapIsIdle(rt);
    rt->gcBlackRootsTraceOp = traceOp;
    rt->gcBlackRootsData = data;
}

JS_PUBLIC_API(void)
JS_TracerInit(JSTracer *trc, JSRuntime *rt, JSTraceCallback callback)
{
    InitTracer(trc, rt, callback);
}

JS_PUBLIC_API(void)
JS_TraceRuntime(JSTracer *trc)
{
    AssertHeapIsIdle(trc->runtime);
    TraceRuntime(trc);
}

JS_PUBLIC_API(void)
JS_TraceChildren(JSTracer *trc, void *thing, JSGCTraceKind kind)
{
    js::TraceChildren(trc, thing, kind);
}

JS_PUBLIC_API(void)
JS_CallTracer(JSTracer *trc, void *thing, JSGCTraceKind kind)
{
    js::CallTracer(trc, thing, kind);
}

JS_PUBLIC_API(void)
JS_GetTraceThingInfo(char *buf, size_t bufsize, JSTracer *trc, void *thing,
                     JSGCTraceKind kind, JSBool details)
{
    const char *name = NULL; /* silence uninitialized warning */
    size_t n;

    if (bufsize == 0)
        return;

    switch (kind) {
      case JSTRACE_OBJECT:
      {
        name = static_cast<JSObject *>(thing)->getClass()->name;
        break;
      }

      case JSTRACE_STRING:
        name = ((JSString *)thing)->isDependent()
               ? "substring"
               : "string";
        break;

      case JSTRACE_SCRIPT:
        name = "script";
        break;

      case JSTRACE_SHAPE:
        name = "shape";
        break;

      case JSTRACE_BASE_SHAPE:
        name = "base_shape";
        break;

      case JSTRACE_TYPE_OBJECT:
        name = "type_object";
        break;

#if JS_HAS_XML_SUPPORT
      case JSTRACE_XML:
        name = "xml";
        break;
#endif
    }

    n = strlen(name);
    if (n > bufsize - 1)
        n = bufsize - 1;
    js_memcpy(buf, name, n + 1);
    buf += n;
    bufsize -= n;
    *buf = '\0';

    if (details && bufsize > 2) {
        switch (kind) {
          case JSTRACE_OBJECT:
          {
            JSObject *obj = (JSObject *)thing;
            Class *clasp = obj->getClass();
            if (clasp == &FunctionClass) {
                JSFunction *fun = obj->toFunction();
                if (!fun) {
                    JS_snprintf(buf, bufsize, " <newborn>");
                } else if (fun != obj) {
                    JS_snprintf(buf, bufsize, " %p", fun);
                } else {
                    if (fun->displayAtom()) {
                        *buf++ = ' ';
                        bufsize--;
                        PutEscapedString(buf, bufsize, fun->displayAtom(), 0);
                    }
                }
            } else if (clasp->flags & JSCLASS_HAS_PRIVATE) {
                JS_snprintf(buf, bufsize, " %p", obj->getPrivate());
            } else {
                JS_snprintf(buf, bufsize, " <no private>");
            }
            break;
          }

          case JSTRACE_STRING:
          {
            *buf++ = ' ';
            bufsize--;
            JSString *str = (JSString *)thing;
            if (str->isLinear())
                PutEscapedString(buf, bufsize, &str->asLinear(), 0);
            else
                JS_snprintf(buf, bufsize, "<rope: length %d>", (int)str->length());
            break;
          }

          case JSTRACE_SCRIPT:
          {
            JSScript *script = static_cast<JSScript *>(thing);
            JS_snprintf(buf, bufsize, " %s:%u", script->filename, unsigned(script->lineno));
            break;
          }

          case JSTRACE_SHAPE:
          case JSTRACE_BASE_SHAPE:
          case JSTRACE_TYPE_OBJECT:
            break;

#if JS_HAS_XML_SUPPORT
          case JSTRACE_XML:
          {
            extern const char *js_xml_class_str[];
            JSXML *xml = (JSXML *)thing;

            JS_snprintf(buf, bufsize, " %s", js_xml_class_str[xml->xml_class]);
            break;
          }
#endif
        }
    }
    buf[bufsize - 1] = '\0';
}

extern JS_PUBLIC_API(const char *)
JS_GetTraceEdgeName(JSTracer *trc, char *buffer, int bufferSize)
{
    if (trc->debugPrinter) {
        trc->debugPrinter(trc, buffer, bufferSize);
        return buffer;
    }
    if (trc->debugPrintIndex != (size_t) - 1) {
        JS_snprintf(buffer, bufferSize, "%s[%lu]",
                    (const char *)trc->debugPrintArg,
                    trc->debugPrintIndex);
        return buffer;
    }
    return (const char*)trc->debugPrintArg;
}

#ifdef DEBUG

typedef struct JSHeapDumpNode JSHeapDumpNode;

struct JSHeapDumpNode {
    void            *thing;
    JSGCTraceKind   kind;
    JSHeapDumpNode  *next;          /* next sibling */
    JSHeapDumpNode  *parent;        /* node with the thing that refer to thing
                                       from this node */
    char            edgeName[1];    /* name of the edge from parent->thing
                                       into thing */
};

typedef HashSet<void *, PointerHasher<void *, 3>, SystemAllocPolicy> VisitedSet;

typedef struct JSDumpingTracer {
    JSTracer            base;
    VisitedSet          visited;
    bool                ok;
    void                *startThing;
    void                *thingToFind;
    void                *thingToIgnore;
    JSHeapDumpNode      *parentNode;
    JSHeapDumpNode      **lastNodep;
    char                buffer[200];
} JSDumpingTracer;

static void
DumpNotify(JSTracer *trc, void **thingp, JSGCTraceKind kind)
{
    JS_ASSERT(trc->callback == DumpNotify);

    JSDumpingTracer *dtrc = (JSDumpingTracer *)trc;
    void *thing = *thingp;

    if (!dtrc->ok || thing == dtrc->thingToIgnore)
        return;

    /*
     * Check if we have already seen thing unless it is thingToFind to include
     * it to the graph each time we reach it and print all live things that
     * refer to thingToFind.
     *
     * This does not print all possible paths leading to thingToFind since
     * when a thing A refers directly or indirectly to thingToFind and A is
     * present several times in the graph, we will print only the first path
     * leading to A and thingToFind, other ways to reach A will be ignored.
     */
    if (dtrc->thingToFind != thing) {
        /*
         * The startThing check allows to avoid putting startThing into the
         * hash table before tracing startThing in JS_DumpHeap.
         */
        if (thing == dtrc->startThing)
            return;
        VisitedSet::AddPtr p = dtrc->visited.lookupForAdd(thing);
        if (p)
            return;
        if (!dtrc->visited.add(p, thing)) {
            dtrc->ok = false;
            return;
        }
    }

    const char *edgeName = JS_GetTraceEdgeName(&dtrc->base, dtrc->buffer, sizeof(dtrc->buffer));
    size_t edgeNameSize = strlen(edgeName) + 1;
    size_t bytes = offsetof(JSHeapDumpNode, edgeName) + edgeNameSize;
    JSHeapDumpNode *node = (JSHeapDumpNode *) OffTheBooks::malloc_(bytes);
    if (!node) {
        dtrc->ok = false;
        return;
    }

    node->thing = thing;
    node->kind = kind;
    node->next = NULL;
    node->parent = dtrc->parentNode;
    js_memcpy(node->edgeName, edgeName, edgeNameSize);

    JS_ASSERT(!*dtrc->lastNodep);
    *dtrc->lastNodep = node;
    dtrc->lastNodep = &node->next;
}

/* Dump node and the chain that leads to thing it contains. */
static JSBool
DumpNode(JSDumpingTracer *dtrc, FILE* fp, JSHeapDumpNode *node)
{
    JSHeapDumpNode *prev, *following;
    size_t chainLimit;
    enum { MAX_PARENTS_TO_PRINT = 10 };

    JS_GetTraceThingInfo(dtrc->buffer, sizeof dtrc->buffer,
                         &dtrc->base, node->thing, node->kind, JS_TRUE);
    if (fprintf(fp, "%p %-22s via ", node->thing, dtrc->buffer) < 0)
        return JS_FALSE;

    /*
     * We need to print the parent chain in the reverse order. To do it in
     * O(N) time where N is the chain length we first reverse the chain while
     * searching for the top and then print each node while restoring the
     * chain order.
     */
    chainLimit = MAX_PARENTS_TO_PRINT;
    prev = NULL;
    for (;;) {
        following = node->parent;
        node->parent = prev;
        prev = node;
        node = following;
        if (!node)
            break;
        if (chainLimit == 0) {
            if (fputs("...", fp) < 0)
                return JS_FALSE;
            break;
        }
        --chainLimit;
    }

    node = prev;
    prev = following;
    bool ok = true;
    do {
        /* Loop must continue even when !ok to restore the parent chain. */
        if (ok) {
            if (!prev) {
                /* Print edge from some runtime root or startThing. */
                if (fputs(node->edgeName, fp) < 0)
                    ok = false;
            } else {
                JS_GetTraceThingInfo(dtrc->buffer, sizeof dtrc->buffer,
                                     &dtrc->base, prev->thing, prev->kind,
                                     JS_FALSE);
                if (fprintf(fp, "(%p %s).%s",
                           prev->thing, dtrc->buffer, node->edgeName) < 0) {
                    ok = false;
                }
            }
        }
        following = node->parent;
        node->parent = prev;
        prev = node;
        node = following;
    } while (node);

    return ok && putc('\n', fp) >= 0;
}

JS_PUBLIC_API(JSBool)
JS_DumpHeap(JSRuntime *rt, FILE *fp, void* startThing, JSGCTraceKind startKind,
            void *thingToFind, size_t maxDepth, void *thingToIgnore)
{
    if (maxDepth == 0)
        return true;

    JSDumpingTracer dtrc;
    if (!dtrc.visited.init())
        return false;
    JS_TracerInit(&dtrc.base, rt, DumpNotify);
    dtrc.ok = true;
    dtrc.startThing = startThing;
    dtrc.thingToFind = thingToFind;
    dtrc.thingToIgnore = thingToIgnore;
    dtrc.parentNode = NULL;
    JSHeapDumpNode *node = NULL;
    dtrc.lastNodep = &node;
    if (!startThing) {
        JS_ASSERT(startKind == JSTRACE_OBJECT);
        TraceRuntime(&dtrc.base);
    } else {
        JS_TraceChildren(&dtrc.base, startThing, startKind);
    }

    if (!node)
        return dtrc.ok;

    size_t depth = 1;
    JSHeapDumpNode *children, *next, *parent;
    bool thingToFindWasTraced = thingToFind && thingToFind == startThing;
    for (;;) {
        /*
         * Loop must continue even when !dtrc.ok to free all nodes allocated
         * so far.
         */
        if (dtrc.ok) {
            if (thingToFind == NULL || thingToFind == node->thing)
                dtrc.ok = DumpNode(&dtrc, fp, node);

            /* Descend into children. */
            if (dtrc.ok &&
                depth < maxDepth &&
                (thingToFind != node->thing || !thingToFindWasTraced)) {
                dtrc.parentNode = node;
                children = NULL;
                dtrc.lastNodep = &children;
                JS_TraceChildren(&dtrc.base, node->thing, node->kind);
                if (thingToFind == node->thing)
                    thingToFindWasTraced = JS_TRUE;
                if (children != NULL) {
                    ++depth;
                    node = children;
                    continue;
                }
            }
        }

        /* Move to next or parents next and free the node. */
        for (;;) {
            next = node->next;
            parent = node->parent;
            Foreground::free_(node);
            node = next;
            if (node)
                break;
            if (!parent)
                return dtrc.ok;
            JS_ASSERT(depth > 1);
            --depth;
            node = parent;
        }
    }

    JS_ASSERT(depth == 1);
    return dtrc.ok;
}

#endif /* DEBUG */

extern JS_PUBLIC_API(JSBool)
JS_IsGCMarkingTracer(JSTracer *trc)
{
    return IS_GC_MARKING_TRACER(trc);
}

JS_PUBLIC_API(void)
JS_GC(JSRuntime *rt)
{
    AssertHeapIsIdle(rt);
    PrepareForFullGC(rt);
    GC(rt, GC_NORMAL, gcreason::API);
}

JS_PUBLIC_API(void)
JS_MaybeGC(JSContext *cx)
{
    MaybeGC(cx);
}

JS_PUBLIC_API(void)
JS_SetGCCallback(JSRuntime *rt, JSGCCallback cb)
{
    AssertHeapIsIdle(rt);
    rt->gcCallback = cb;
}

JS_PUBLIC_API(void)
JS_SetFinalizeCallback(JSRuntime *rt, JSFinalizeCallback cb)
{
    AssertHeapIsIdle(rt);
    rt->gcFinalizeCallback = cb;
}

JS_PUBLIC_API(JSBool)
JS_IsAboutToBeFinalized(void *thing)
{
    gc::Cell *t = static_cast<gc::Cell *>(thing);
    bool isMarked = IsCellMarked(&t);
    JS_ASSERT(t == thing);
    return !isMarked;
}

JS_PUBLIC_API(void)
JS_SetGCParameter(JSRuntime *rt, JSGCParamKey key, uint32_t value)
{
    switch (key) {
      case JSGC_MAX_BYTES: {
        JS_ASSERT(value >= rt->gcBytes);
        rt->gcMaxBytes = value;
        break;
      }
      case JSGC_MAX_MALLOC_BYTES:
        rt->setGCMaxMallocBytes(value);
        break;
      case JSGC_SLICE_TIME_BUDGET:
        rt->gcSliceBudget = SliceBudget::TimeBudget(value);
        break;
      case JSGC_MARK_STACK_LIMIT:
        js::SetMarkStackLimit(rt, value);
        break;
      case JSGC_HIGH_FREQUENCY_TIME_LIMIT:
        rt->gcHighFrequencyTimeThreshold = value;
        break;
      case JSGC_HIGH_FREQUENCY_LOW_LIMIT:
        rt->gcHighFrequencyLowLimitBytes = value * 1024 * 1024;
        break;
      case JSGC_HIGH_FREQUENCY_HIGH_LIMIT:
        rt->gcHighFrequencyHighLimitBytes = value * 1024 * 1024;
        break;
      case JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MAX:
        rt->gcHighFrequencyHeapGrowthMax = value / 100.0;
        break;
      case JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MIN:
        rt->gcHighFrequencyHeapGrowthMin = value / 100.0;
        break;
      case JSGC_LOW_FREQUENCY_HEAP_GROWTH:
        rt->gcLowFrequencyHeapGrowth = value / 100.0;
        break;
      case JSGC_DYNAMIC_HEAP_GROWTH:
        rt->gcDynamicHeapGrowth = value;
        break;
      case JSGC_DYNAMIC_MARK_SLICE:
        rt->gcDynamicMarkSlice = value;
        break;
      case JSGC_ANALYSIS_PURGE_TRIGGER:
        rt->analysisPurgeTriggerBytes = value * 1024 * 1024;
        break;
      default:
        JS_ASSERT(key == JSGC_MODE);
        rt->gcMode = JSGCMode(value);
        JS_ASSERT(rt->gcMode == JSGC_MODE_GLOBAL ||
                  rt->gcMode == JSGC_MODE_COMPARTMENT ||
                  rt->gcMode == JSGC_MODE_INCREMENTAL);
        return;
    }
}

JS_PUBLIC_API(uint32_t)
JS_GetGCParameter(JSRuntime *rt, JSGCParamKey key)
{
    switch (key) {
      case JSGC_MAX_BYTES:
        return uint32_t(rt->gcMaxBytes);
      case JSGC_MAX_MALLOC_BYTES:
        return rt->gcMaxMallocBytes;
      case JSGC_BYTES:
        return uint32_t(rt->gcBytes);
      case JSGC_MODE:
        return uint32_t(rt->gcMode);
      case JSGC_UNUSED_CHUNKS:
        return uint32_t(rt->gcChunkPool.getEmptyCount());
      case JSGC_TOTAL_CHUNKS:
        return uint32_t(rt->gcChunkSet.count() + rt->gcChunkPool.getEmptyCount());
      case JSGC_SLICE_TIME_BUDGET:
        return uint32_t(rt->gcSliceBudget > 0 ? rt->gcSliceBudget / PRMJ_USEC_PER_MSEC : 0);
      case JSGC_MARK_STACK_LIMIT:
        return rt->gcMarker.sizeLimit();
      case JSGC_HIGH_FREQUENCY_TIME_LIMIT:
        return rt->gcHighFrequencyTimeThreshold;
      case JSGC_HIGH_FREQUENCY_LOW_LIMIT:
        return rt->gcHighFrequencyLowLimitBytes / 1024 / 1024;
      case JSGC_HIGH_FREQUENCY_HIGH_LIMIT:
        return rt->gcHighFrequencyHighLimitBytes / 1024 / 1024;
      case JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MAX:
        return uint32_t(rt->gcHighFrequencyHeapGrowthMax * 100);
      case JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MIN:
        return uint32_t(rt->gcHighFrequencyHeapGrowthMin * 100);
      case JSGC_LOW_FREQUENCY_HEAP_GROWTH:
        return uint32_t(rt->gcLowFrequencyHeapGrowth * 100);
      case JSGC_DYNAMIC_HEAP_GROWTH:
        return rt->gcDynamicHeapGrowth;
      case JSGC_DYNAMIC_MARK_SLICE:
        return rt->gcDynamicMarkSlice;
      case JSGC_ANALYSIS_PURGE_TRIGGER:
        return rt->analysisPurgeTriggerBytes / 1024 / 1024;
      default:
        JS_ASSERT(key == JSGC_NUMBER);
        return uint32_t(rt->gcNumber);
    }
}

JS_PUBLIC_API(void)
JS_SetGCParameterForThread(JSContext *cx, JSGCParamKey key, uint32_t value)
{
    JS_ASSERT(key == JSGC_MAX_CODE_CACHE_BYTES);
}

JS_PUBLIC_API(uint32_t)
JS_GetGCParameterForThread(JSContext *cx, JSGCParamKey key)
{
    JS_ASSERT(key == JSGC_MAX_CODE_CACHE_BYTES);
    return 0;
}

JS_PUBLIC_API(JSString *)
JS_NewExternalString(JSContext *cx, const jschar *chars, size_t length,
                     const JSStringFinalizer *fin)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    JSString *s = JSExternalString::new_(cx, chars, length, fin);
    Probes::createString(cx, s, length);
    return s;
}

extern JS_PUBLIC_API(JSBool)
JS_IsExternalString(JSString *str)
{
    return str->isExternal();
}

extern JS_PUBLIC_API(const JSStringFinalizer *)
JS_GetExternalStringFinalizer(JSString *str)
{
    return str->asExternal().externalFinalizer();
}

JS_PUBLIC_API(void)
JS_SetNativeStackQuota(JSRuntime *rt, size_t stackSize)
{
    rt->nativeStackQuota = stackSize;
    if (!rt->nativeStackBase)
        return;

#if JS_STACK_GROWTH_DIRECTION > 0
    if (stackSize == 0) {
        rt->nativeStackLimit = UINTPTR_MAX;
    } else {
        JS_ASSERT(rt->nativeStackBase <= size_t(-1) - stackSize);
        rt->nativeStackLimit = rt->nativeStackBase + stackSize - 1;
    }
#else
    if (stackSize == 0) {
        rt->nativeStackLimit = 0;
    } else {
        JS_ASSERT(rt->nativeStackBase >= stackSize);
        rt->nativeStackLimit = rt->nativeStackBase - (stackSize - 1);
    }
#endif
}

/************************************************************************/

JS_PUBLIC_API(int)
JS_IdArrayLength(JSContext *cx, JSIdArray *ida)
{
    return ida->length;
}

JS_PUBLIC_API(jsid)
JS_IdArrayGet(JSContext *cx, JSIdArray *ida, int index)
{
    JS_ASSERT(index >= 0 && index < ida->length);
    return ida->vector[index];
}

JS_PUBLIC_API(void)
JS_DestroyIdArray(JSContext *cx, JSIdArray *ida)
{
    DestroyIdArray(cx->runtime->defaultFreeOp(), ida);
}

JS_PUBLIC_API(JSBool)
JS_ValueToId(JSContext *cx, jsval v, jsid *idp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v);
    return ValueToId(cx, v, idp);
}

JS_PUBLIC_API(JSBool)
JS_IdToValue(JSContext *cx, jsid id, jsval *vp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    *vp = IdToJsval(id);
    assertSameCompartment(cx, *vp);
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_DefaultValue(JSContext *cx, JSObject *objArg, JSType hint, jsval *vp)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    JS_ASSERT(obj != NULL);
    JS_ASSERT(hint == JSTYPE_VOID || hint == JSTYPE_STRING || hint == JSTYPE_NUMBER);

    RootedValue value(cx);
    if (!JSObject::defaultValue(cx, obj, hint, &value))
        return false;

    *vp = value;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_PropertyStub(JSContext *cx, JSHandleObject obj, JSHandleId id, JSMutableHandleValue vp)
{
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_StrictPropertyStub(JSContext *cx, JSHandleObject obj, JSHandleId id, JSBool strict, JSMutableHandleValue vp)
{
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_EnumerateStub(JSContext *cx, JSHandleObject obj)
{
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_ResolveStub(JSContext *cx, JSHandleObject obj, JSHandleId id)
{
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_ConvertStub(JSContext *cx, JSHandleObject obj, JSType type, JSMutableHandleValue vp)
{
    JS_ASSERT(type != JSTYPE_OBJECT && type != JSTYPE_FUNCTION);
    JS_ASSERT(obj);
    return DefaultValue(cx, obj, type, vp);
}

JS_PUBLIC_API(JSObject *)
JS_InitClass(JSContext *cx, JSObject *objArg, JSObject *parent_protoArg,
             JSClass *clasp, JSNative constructor, unsigned nargs,
             JSPropertySpec *ps, JSFunctionSpec *fs,
             JSPropertySpec *static_ps, JSFunctionSpec *static_fs)
{
    RootedObject obj(cx, objArg);
    RootedObject parent_proto(cx, parent_protoArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, parent_proto);
    return js_InitClass(cx, obj, parent_proto, Valueify(clasp), constructor,
                        nargs, ps, fs, static_ps, static_fs);
}

JS_PUBLIC_API(JSBool)
JS_LinkConstructorAndPrototype(JSContext *cx, JSObject *ctorArg, JSObject *protoArg)
{
    RootedObject ctor(cx, ctorArg);
    RootedObject proto(cx, protoArg);
    return LinkConstructorAndPrototype(cx, ctor, proto);
}

JS_PUBLIC_API(JSClass *)
JS_GetClass(RawObject obj)
{
    return obj->getJSClass();
}

JS_PUBLIC_API(JSBool)
JS_InstanceOf(JSContext *cx, JSObject *objArg, JSClass *clasp, jsval *argv)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
#ifdef DEBUG
    if (argv) {
        assertSameCompartment(cx, obj);
        assertSameCompartment(cx, JSValueArray(argv - 2, 2));
    }
#endif
    if (!obj || obj->getJSClass() != clasp) {
        if (argv)
            ReportIncompatibleMethod(cx, CallReceiverFromArgv(argv), Valueify(clasp));
        return false;
    }
    return true;
}

JS_PUBLIC_API(JSBool)
JS_HasInstance(JSContext *cx, JSObject *objArg, jsval v, JSBool *bp)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    assertSameCompartment(cx, obj, v);
    return HasInstance(cx, obj, &v, bp);
}

JS_PUBLIC_API(void *)
JS_GetPrivate(RawObject obj)
{
    /* This function can be called by a finalizer. */
    return obj->getPrivate();
}

JS_PUBLIC_API(void)
JS_SetPrivate(RawObject obj, void *data)
{
    /* This function can be called by a finalizer. */
    obj->setPrivate(data);
}

JS_PUBLIC_API(void *)
JS_GetInstancePrivate(JSContext *cx, JSObject *objArg, JSClass *clasp, jsval *argv)
{
    RootedObject obj(cx, objArg);
    if (!JS_InstanceOf(cx, obj, clasp, argv))
        return NULL;
    return obj->getPrivate();
}

JS_PUBLIC_API(JSObject *)
JS_GetPrototype(RawObject obj)
{
    return obj->getProto();
}

JS_PUBLIC_API(JSBool)
JS_SetPrototype(JSContext *cx, JSObject *objArg, JSObject *protoArg)
{
    RootedObject obj(cx, objArg);
    RootedObject proto(cx, protoArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, proto);

    return SetProto(cx, obj, proto, JS_FALSE);
}

JS_PUBLIC_API(JSObject *)
JS_GetParent(RawObject obj)
{
    JS_ASSERT(!obj->isScope());
    return obj->getParent();
}

JS_PUBLIC_API(JSBool)
JS_SetParent(JSContext *cx, JSObject *objArg, JSObject *parentArg)
{
    RootedObject obj(cx, objArg);
    RootedObject parent(cx, parentArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    JS_ASSERT(!obj->isScope());
    JS_ASSERT(parent || !obj->getParent());
    assertSameCompartment(cx, obj, parent);

    return JSObject::setParent(cx, obj, parent);
}

JS_PUBLIC_API(JSObject *)
JS_GetConstructor(JSContext *cx, JSObject *protoArg)
{
    RootedObject proto(cx, protoArg);
    RootedValue cval(cx);

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, proto);
    {
        JSAutoResolveFlags rf(cx, JSRESOLVE_QUALIFIED);

        if (!JSObject::getProperty(cx, proto, proto, cx->runtime->atomState.constructorAtom, &cval))
            return NULL;
    }
    if (!IsFunctionObject(cval)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NO_CONSTRUCTOR,
                             proto->getClass()->name);
        return NULL;
    }
    return &cval.toObject();
}

JS_PUBLIC_API(JSBool)
JS_GetObjectId(JSContext *cx, JSRawObject obj, jsid *idp)
{
    AssertHeapIsIdle(cx);
    assertSameCompartment(cx, obj);
    *idp = OBJECT_TO_JSID(obj);
    return JS_TRUE;
}

class AutoHoldCompartment {
  public:
    explicit AutoHoldCompartment(JSCompartment *compartment JS_GUARD_OBJECT_NOTIFIER_PARAM)
      : holdp(&compartment->hold)
    {
        JS_GUARD_OBJECT_NOTIFIER_INIT;
        *holdp = true;
    }

    ~AutoHoldCompartment() {
        *holdp = false;
    }
  private:
    bool *holdp;
    JS_DECL_USE_GUARD_OBJECT_NOTIFIER
};

JS_PUBLIC_API(JSObject *)
JS_NewGlobalObject(JSContext *cx, JSClass *clasp, JSPrincipals *principals)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);

    JSCompartment *compartment = NewCompartment(cx, principals);
    if (!compartment)
        return NULL;

    AutoHoldCompartment hold(compartment);

    JSCompartment *saved = cx->compartment;
    cx->setCompartment(compartment);
    GlobalObject *global = GlobalObject::create(cx, Valueify(clasp));
    cx->setCompartment(saved);

    return global;
}

JS_PUBLIC_API(JSObject *)
JS_NewObject(JSContext *cx, JSClass *jsclasp, JSObject *protoArg, JSObject *parentArg)
{
    RootedObject proto(cx, protoArg);
    RootedObject parent(cx, parentArg);
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, proto, parent);

    Class *clasp = Valueify(jsclasp);
    if (!clasp)
        clasp = &ObjectClass;    /* default class is Object */

    JS_ASSERT(clasp != &FunctionClass);
    JS_ASSERT(!(clasp->flags & JSCLASS_IS_GLOBAL));

    JSObject *obj = NewObjectWithClassProto(cx, clasp, proto, parent);
    AssertRootingUnnecessary safe(cx);
    if (obj) {
        if (clasp->ext.equality)
            MarkTypeObjectFlags(cx, obj, OBJECT_FLAG_SPECIAL_EQUALITY);
    }

    JS_ASSERT_IF(obj, obj->getParent());
    return obj;
}

JS_PUBLIC_API(JSObject *)
JS_NewObjectWithGivenProto(JSContext *cx, JSClass *jsclasp, JSObject *protoArg, JSObject *parentArg)
{
    RootedObject proto(cx, protoArg);
    RootedObject parent(cx, parentArg);
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, proto, parent);

    Class *clasp = Valueify(jsclasp);
    if (!clasp)
        clasp = &ObjectClass;    /* default class is Object */

    JS_ASSERT(clasp != &FunctionClass);
    JS_ASSERT(!(clasp->flags & JSCLASS_IS_GLOBAL));

    JSObject *obj = NewObjectWithGivenProto(cx, clasp, proto, parent);
    AssertRootingUnnecessary safe(cx);
    if (obj)
        MarkTypeObjectUnknownProperties(cx, obj->type());
    return obj;
}

JS_PUBLIC_API(JSObject *)
JS_NewObjectForConstructor(JSContext *cx, JSClass *clasp, const jsval *vp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, *vp);

    RootedObject obj(cx, JSVAL_TO_OBJECT(*vp));
    return js_CreateThis(cx, Valueify(clasp), obj);
}

JS_PUBLIC_API(JSBool)
JS_IsExtensible(JSRawObject obj)
{
    return obj->isExtensible();
}

JS_PUBLIC_API(JSBool)
JS_IsNative(JSRawObject obj)
{
    return obj->isNative();
}

JS_PUBLIC_API(JSRuntime *)
JS_GetObjectRuntime(JSRawObject obj)
{
    return obj->compartment()->rt;
}

JS_PUBLIC_API(JSBool)
JS_FreezeObject(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);

    return JSObject::freeze(cx, obj);
}

JS_PUBLIC_API(JSBool)
JS_DeepFreezeObject(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);

    /* Assume that non-extensible objects are already deep-frozen, to avoid divergence. */
    if (!obj->isExtensible())
        return true;

    if (!JSObject::freeze(cx, obj))
        return false;

    /* Walk slots in obj and if any value is a non-null object, seal it. */
    for (uint32_t i = 0, n = obj->slotSpan(); i < n; ++i) {
        const Value &v = obj->getSlot(i);
        if (v.isPrimitive())
            continue;
        RootedObject obj(cx, &v.toObject());
        if (!JS_DeepFreezeObject(cx, obj))
            return false;
    }

    return true;
}

static JSBool
LookupPropertyById(JSContext *cx, HandleObject obj, HandleId id, unsigned flags,
                   MutableHandleObject objp, MutableHandleShape propp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, id);

    JSAutoResolveFlags rf(cx, flags);
    return JSObject::lookupGeneric(cx, obj, id, objp, propp);
}

#define AUTO_NAMELEN(s,n)   (((n) == (size_t)-1) ? js_strlen(s) : (n))

static JSBool
LookupResult(JSContext *cx, HandleObject obj, HandleObject obj2, jsid id,
             HandleShape shape, Value *vp)
{
    if (!shape) {
        /* XXX bad API: no way to tell "not defined" from "void value" */
        vp->setUndefined();
        return JS_TRUE;
    }

    if (obj2->isNative()) {
        /* Peek at the native property's slot value, without doing a Get. */
        if (shape->hasSlot()) {
            *vp = obj2->nativeGetSlot(shape->slot());
            return true;
        }
    } else {
        if (obj2->isDenseArray())
            return js_GetDenseArrayElementValue(cx, obj2, id, vp);
        if (obj2->isProxy()) {
            AutoPropertyDescriptorRooter desc(cx);
            if (!Proxy::getPropertyDescriptor(cx, obj2, id, false, &desc))
                return false;
            if (!(desc.attrs & JSPROP_SHARED)) {
                *vp = desc.value;
                return true;
            }
        }
    }

    /* XXX bad API: no way to return "defined but value unknown" */
    vp->setBoolean(true);
    return true;
}

JS_PUBLIC_API(JSBool)
JS_LookupPropertyById(JSContext *cx, JSObject *objArg, jsid idArg, jsval *vp)
{
    RootedId id(cx, idArg);
    RootedObject obj(cx, objArg);
    RootedObject obj2(cx);
    RootedShape prop(cx);

    return LookupPropertyById(cx, obj, id, JSRESOLVE_QUALIFIED, &obj2, &prop) &&
           LookupResult(cx, obj, obj2, id, prop, vp);
}

JS_PUBLIC_API(JSBool)
JS_LookupElement(JSContext *cx, JSObject *objArg, uint32_t index, jsval *vp)
{
    RootedObject obj(cx, objArg);
    CHECK_REQUEST(cx);
    jsid id;
    if (!IndexToId(cx, index, &id))
        return false;
    return JS_LookupPropertyById(cx, obj, id, vp);
}

JS_PUBLIC_API(JSBool)
JS_LookupProperty(JSContext *cx, JSObject *objArg, const char *name, jsval *vp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = Atomize(cx, name, strlen(name));
    return atom && JS_LookupPropertyById(cx, obj, AtomToId(atom), vp);
}

JS_PUBLIC_API(JSBool)
JS_LookupUCProperty(JSContext *cx, JSObject *objArg, const jschar *name, size_t namelen, jsval *vp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
    return atom && JS_LookupPropertyById(cx, obj, AtomToId(atom), vp);
}

JS_PUBLIC_API(JSBool)
JS_LookupPropertyWithFlagsById(JSContext *cx, JSObject *objArg, jsid id_, unsigned flags,
                               JSObject **objpArg, jsval *vp)
{
    RootedObject obj(cx, objArg);
    RootedObject objp(cx, *objpArg);
    RootedId id(cx, id_);
    RootedShape prop(cx);

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, id);
    if (!(obj->isNative()
          ? LookupPropertyWithFlags(cx, obj, id, flags, &objp, &prop)
          : JSObject::lookupGeneric(cx, obj, id, &objp, &prop)))
        return false;

    if (!LookupResult(cx, obj, objp, id, prop, vp))
        return false;

    *objpArg = objp;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_LookupPropertyWithFlags(JSContext *cx, JSObject *objArg, const char *name, unsigned flags, jsval *vp)
{
    RootedObject obj(cx, objArg);
    JSObject *obj2;
    JSAtom *atom = Atomize(cx, name, strlen(name));
    return atom && JS_LookupPropertyWithFlagsById(cx, obj, AtomToId(atom), flags, &obj2, vp);
}

JS_PUBLIC_API(JSBool)
JS_HasPropertyById(JSContext *cx, JSObject *objArg, jsid idArg, JSBool *foundp)
{
    RootedObject obj(cx, objArg);
    RootedId id(cx, idArg);
    RootedObject obj2(cx);
    RootedShape prop(cx);
    JSBool ok = LookupPropertyById(cx, obj, id, JSRESOLVE_QUALIFIED | JSRESOLVE_DETECTING,
                                   &obj2, &prop);
    *foundp = (prop != NULL);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_HasElement(JSContext *cx, JSObject *objArg, uint32_t index, JSBool *foundp)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    jsid id;
    if (!IndexToId(cx, index, &id))
        return false;
    return JS_HasPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API(JSBool)
JS_HasProperty(JSContext *cx, JSObject *objArg, const char *name, JSBool *foundp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = Atomize(cx, name, strlen(name));
    return atom && JS_HasPropertyById(cx, obj, AtomToId(atom), foundp);
}

JS_PUBLIC_API(JSBool)
JS_HasUCProperty(JSContext *cx, JSObject *objArg, const jschar *name, size_t namelen, JSBool *foundp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
    return atom && JS_HasPropertyById(cx, obj, AtomToId(atom), foundp);
}

JS_PUBLIC_API(JSBool)
JS_AlreadyHasOwnPropertyById(JSContext *cx, JSObject *objArg, jsid id_, JSBool *foundp)
{
    RootedObject obj(cx, objArg);
    RootedId id(cx, id_);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, id);

    if (!obj->isNative()) {
        RootedObject obj2(cx);
        RootedShape prop(cx);

        if (!LookupPropertyById(cx, obj, id, JSRESOLVE_QUALIFIED | JSRESOLVE_DETECTING,
                                &obj2, &prop)) {
            return JS_FALSE;
        }
        *foundp = (obj == obj2);
        return JS_TRUE;
    }

    *foundp = obj->nativeContains(cx, id);
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_AlreadyHasOwnElement(JSContext *cx, JSObject *objArg, uint32_t index, JSBool *foundp)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    jsid id;
    if (!IndexToId(cx, index, &id))
        return false;
    return JS_AlreadyHasOwnPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API(JSBool)
JS_AlreadyHasOwnProperty(JSContext *cx, JSObject *objArg, const char *name, JSBool *foundp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = Atomize(cx, name, strlen(name));
    return atom && JS_AlreadyHasOwnPropertyById(cx, obj, AtomToId(atom), foundp);
}

JS_PUBLIC_API(JSBool)
JS_AlreadyHasOwnUCProperty(JSContext *cx, JSObject *objArg, const jschar *name, size_t namelen,
                           JSBool *foundp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
    return atom && JS_AlreadyHasOwnPropertyById(cx, obj, AtomToId(atom), foundp);
}

/* Wrapper functions to create wrappers with no corresponding JSJitInfo from API
 * function arguments.
 */
static JSPropertyOpWrapper
GetterWrapper(JSPropertyOp getter)
{
    JSPropertyOpWrapper ret;
    ret.op = getter;
    ret.info = NULL;
    return ret;
}

static JSStrictPropertyOpWrapper
SetterWrapper(JSStrictPropertyOp setter)
{
    JSStrictPropertyOpWrapper ret;
    ret.op = setter;
    ret.info = NULL;
    return ret;
}

static JSBool
DefinePropertyById(JSContext *cx, HandleObject obj, HandleId id, HandleValue value,
                   const JSPropertyOpWrapper &get, const JSStrictPropertyOpWrapper &set,
                   unsigned attrs, unsigned flags, int tinyid)
{
    PropertyOp getter = get.op;
    StrictPropertyOp setter = set.op;
    /*
     * JSPROP_READONLY has no meaning when accessors are involved. Ideally we'd
     * throw if this happens, but we've accepted it for long enough that it's
     * not worth trying to make callers change their ways. Just flip it off on
     * its way through the API layer so that we can enforce this internally.
     */
    if (attrs & (JSPROP_GETTER | JSPROP_SETTER))
        attrs &= ~JSPROP_READONLY;

    /*
     * When we use DefineProperty, we need full scriptable Function objects rather
     * than JSNatives. However, we might be pulling this property descriptor off
     * of something with JSNative property descriptors. If we are, wrap them in
     * JS Function objects.
     */
    if (attrs & JSPROP_NATIVE_ACCESSORS) {
        JS_ASSERT(!(attrs & (JSPROP_GETTER | JSPROP_SETTER)));
        attrs &= ~JSPROP_NATIVE_ACCESSORS;
        if (getter) {
            RootedObject global(cx, (JSObject*) &obj->global());
            JSFunction *getobj = JS_NewFunction(cx, (Native) getter, 0, 0, global, NULL);
            if (!getobj)
                return false;

            if (get.info)
                getobj->setJitInfo(get.info);

            getter = JS_DATA_TO_FUNC_PTR(PropertyOp, getobj);
            attrs |= JSPROP_GETTER;
        }
        if (setter) {
            // Root just the getter, since the setter is not yet a JSObject.
            AutoRooterGetterSetter getRoot(cx, JSPROP_GETTER, &getter, NULL);
            RootedObject global(cx, (JSObject*) &obj->global());
            JSFunction *setobj = JS_NewFunction(cx, (Native) setter, 1, 0, global, NULL);
            if (!setobj)
                return false;

            if (set.info)
                setobj->setJitInfo(set.info);

            setter = JS_DATA_TO_FUNC_PTR(StrictPropertyOp, setobj);
            attrs |= JSPROP_SETTER;
        }
    }


    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, id, value,
                            (attrs & JSPROP_GETTER)
                            ? JS_FUNC_TO_DATA_PTR(JSObject *, getter)
                            : NULL,
                            (attrs & JSPROP_SETTER)
                            ? JS_FUNC_TO_DATA_PTR(JSObject *, setter)
                            : NULL);

    JSAutoResolveFlags rf(cx, JSRESOLVE_QUALIFIED);
    if (flags != 0 && obj->isNative()) {
        return !!DefineNativeProperty(cx, obj, id, value, getter, setter,
                                      attrs, flags, tinyid);
    }
    return JSObject::defineGeneric(cx, obj, id, value, getter, setter, attrs);
}

JS_PUBLIC_API(JSBool)
JS_DefinePropertyById(JSContext *cx, JSObject *objArg, jsid idArg, jsval value_,
                      JSPropertyOp getter, JSStrictPropertyOp setter, unsigned attrs)
{
    RootedObject obj(cx, objArg);
    RootedId id(cx, idArg);
    RootedValue value(cx, value_);
    return DefinePropertyById(cx, obj, id, value, GetterWrapper(getter),
                              SetterWrapper(setter), attrs, 0, 0);
}

JS_PUBLIC_API(JSBool)
JS_DefineElement(JSContext *cx, JSObject *objArg, uint32_t index, jsval valueArg,
                 JSPropertyOp getter, JSStrictPropertyOp setter, unsigned attrs)
{
    RootedObject obj(cx, objArg);
    RootedValue value(cx, valueArg);
    AutoRooterGetterSetter gsRoot(cx, attrs, &getter, &setter);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    RootedId id(cx);
    if (!IndexToId(cx, index, id.address()))
        return false;
    return DefinePropertyById(cx, obj, id, value, GetterWrapper(getter),
                              SetterWrapper(setter), attrs, 0, 0);
}

static JSBool
DefineProperty(JSContext *cx, JSHandleObject obj, const char *name, const Value &value_,
               const JSPropertyOpWrapper &getter, const JSStrictPropertyOpWrapper &setter,
               unsigned attrs, unsigned flags, int tinyid)
{
    RootedValue value(cx, value_);
    AutoRooterGetterSetter gsRoot(cx, attrs, const_cast<JSPropertyOp *>(&getter.op),
                                  const_cast<JSStrictPropertyOp *>(&setter.op));
    RootedId id(cx);

    if (attrs & JSPROP_INDEX) {
        id = INT_TO_JSID(intptr_t(name));
        attrs &= ~JSPROP_INDEX;
    } else {
        JSAtom *atom = Atomize(cx, name, strlen(name));
        if (!atom)
            return JS_FALSE;
        id = AtomToId(atom);
    }

    return DefinePropertyById(cx, obj, id, value, getter, setter, attrs, flags, tinyid);
}

JS_PUBLIC_API(JSBool)
JS_DefineProperty(JSContext *cx, JSObject *objArg, const char *name, jsval value,
                  PropertyOp getter, JSStrictPropertyOp setter, unsigned attrs)
{
    RootedObject obj(cx, objArg);
    return DefineProperty(cx, obj, name, value, GetterWrapper(getter),
                          SetterWrapper(setter), attrs, 0, 0);
}

JS_PUBLIC_API(JSBool)
JS_DefinePropertyWithTinyId(JSContext *cx, JSObject *objArg, const char *name, int8_t tinyid,
                            jsval value, PropertyOp getter, JSStrictPropertyOp setter, unsigned attrs)
{
    RootedObject obj(cx, objArg);
    return DefineProperty(cx, obj, name, value, GetterWrapper(getter),
                          SetterWrapper(setter), attrs, Shape::HAS_SHORTID, tinyid);
}

static JSBool
DefineUCProperty(JSContext *cx, JSHandleObject obj, const jschar *name, size_t namelen,
                 const Value &value_, PropertyOp getter, StrictPropertyOp setter, unsigned attrs,
                 unsigned flags, int tinyid)
{
    RootedValue value(cx, value_);
    AutoRooterGetterSetter gsRoot(cx, attrs, &getter, &setter);
    JSAtom *atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
    if (!atom)
        return false;
    RootedId id(cx, AtomToId(atom));
    return DefinePropertyById(cx, obj, id, value, GetterWrapper(getter),
                              SetterWrapper(setter), attrs, flags, tinyid);
}

JS_PUBLIC_API(JSBool)
JS_DefineUCProperty(JSContext *cx, JSObject *objArg, const jschar *name, size_t namelen,
                    jsval value, JSPropertyOp getter, JSStrictPropertyOp setter, unsigned attrs)
{
    RootedObject obj(cx, objArg);
    return DefineUCProperty(cx, obj, name, namelen, value, getter, setter, attrs, 0, 0);
}

JS_PUBLIC_API(JSBool)
JS_DefineUCPropertyWithTinyId(JSContext *cx, JSObject *objArg, const jschar *name, size_t namelen,
                              int8_t tinyid, jsval value,
                              JSPropertyOp getter, JSStrictPropertyOp setter, unsigned attrs)
{
    RootedObject obj(cx, objArg);
    return DefineUCProperty(cx, obj, name, namelen, value, getter, setter, attrs,
                            Shape::HAS_SHORTID, tinyid);
}

JS_PUBLIC_API(JSBool)
JS_DefineOwnProperty(JSContext *cx, JSObject *objArg, jsid idArg, jsval descriptor, JSBool *bp)
{
    RootedObject obj(cx, objArg);
    RootedId id(cx, idArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, id, descriptor);

    return js_DefineOwnProperty(cx, obj, id, descriptor, bp);
}

JS_PUBLIC_API(JSObject *)
JS_DefineObject(JSContext *cx, JSObject *objArg, const char *name, JSClass *jsclasp,
                JSObject *protoArg, unsigned attrs)
{
    RootedObject obj(cx, objArg);
    RootedObject proto(cx, protoArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, proto);

    Class *clasp = Valueify(jsclasp);
    if (!clasp)
        clasp = &ObjectClass;    /* default class is Object */

    RootedObject nobj(cx, NewObjectWithClassProto(cx, clasp, proto, obj));
    if (!nobj)
        return NULL;

    if (!DefineProperty(cx, obj, name, ObjectValue(*nobj), GetterWrapper(NULL),
                        SetterWrapper(NULL), attrs, 0, 0))
    {
        return NULL;
    }

    return nobj;
}

JS_PUBLIC_API(JSBool)
JS_DefineConstDoubles(JSContext *cx, JSObject *objArg, JSConstDoubleSpec *cds)
{
    RootedObject obj(cx, objArg);
    JSBool ok;
    unsigned attrs;

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    JSPropertyOpWrapper noget = GetterWrapper(NULL);
    JSStrictPropertyOpWrapper noset = SetterWrapper(NULL);
    for (ok = JS_TRUE; cds->name; cds++) {
        Value value = DoubleValue(cds->dval);
        attrs = cds->flags;
        if (!attrs)
            attrs = JSPROP_READONLY | JSPROP_PERMANENT;
        ok = DefineProperty(cx, obj, cds->name, value, noget, noset, attrs, 0, 0);
        if (!ok)
            break;
    }
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_DefineProperties(JSContext *cx, JSObject *objArg, JSPropertySpec *ps)
{
    RootedObject obj(cx, objArg);
    JSBool ok;
    for (ok = true; ps->name; ps++) {
        ok = DefineProperty(cx, obj, ps->name, UndefinedValue(), ps->getter, ps->setter,
                            ps->flags, Shape::HAS_SHORTID, ps->tinyid);
        if (!ok)
            break;
    }
    return ok;
}

static JSBool
GetPropertyDescriptorById(JSContext *cx, HandleObject obj, HandleId id, unsigned flags,
                          JSBool own, PropertyDescriptor *desc)
{
    RootedObject obj2(cx);
    RootedShape shape(cx);

    if (!LookupPropertyById(cx, obj, id, flags, &obj2, &shape))
        return JS_FALSE;

    if (!shape || (own && obj != obj2)) {
        desc->obj = NULL;
        desc->attrs = 0;
        desc->getter = NULL;
        desc->setter = NULL;
        desc->value.setUndefined();
        return JS_TRUE;
    }

    desc->obj = obj2;
    if (obj2->isNative()) {
        desc->attrs = shape->attributes();
        desc->getter = shape->getter();
        desc->setter = shape->setter();
        if (shape->hasSlot())
            desc->value = obj2->nativeGetSlot(shape->slot());
        else
            desc->value.setUndefined();
    } else {
        if (obj2->isProxy()) {
            JSAutoResolveFlags rf(cx, flags);
            return own
                   ? Proxy::getOwnPropertyDescriptor(cx, obj2, id, false, desc)
                   : Proxy::getPropertyDescriptor(cx, obj2, id, false, desc);
        }
        if (!JSObject::getGenericAttributes(cx, obj2, id, &desc->attrs))
            return false;
        desc->getter = NULL;
        desc->setter = NULL;
        desc->value.setUndefined();
    }
    return true;
}

JS_PUBLIC_API(JSBool)
JS_GetPropertyDescriptorById(JSContext *cx, JSObject *objArg, jsid idArg, unsigned flags,
                             JSPropertyDescriptor *desc_)
{
    RootedObject obj(cx, objArg);
    RootedId id(cx, idArg);
    AutoPropertyDescriptorRooter desc(cx);
    if (!GetPropertyDescriptorById(cx, obj, id, flags, JS_FALSE, &desc))
        return false;
    *desc_ = desc;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_GetPropertyAttrsGetterAndSetterById(JSContext *cx, JSObject *objArg, jsid idArg,
                                       unsigned *attrsp, JSBool *foundp,
                                       JSPropertyOp *getterp, JSStrictPropertyOp *setterp)
{
    RootedObject obj(cx, objArg);
    RootedId id(cx, idArg);
    AutoPropertyDescriptorRooter desc(cx);
    if (!GetPropertyDescriptorById(cx, obj, id, JSRESOLVE_QUALIFIED, JS_FALSE, &desc))
        return false;

    *attrsp = desc.attrs;
    *foundp = (desc.obj != NULL);
    if (getterp)
        *getterp = desc.getter;
    if (setterp)
        *setterp = desc.setter;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_GetPropertyAttributes(JSContext *cx, JSObject *objArg, const char *name,
                         unsigned *attrsp, JSBool *foundp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = Atomize(cx, name, strlen(name));
    return atom && JS_GetPropertyAttrsGetterAndSetterById(cx, obj, AtomToId(atom),
                                                          attrsp, foundp, NULL, NULL);
}

JS_PUBLIC_API(JSBool)
JS_GetUCPropertyAttributes(JSContext *cx, JSObject *objArg, const jschar *name, size_t namelen,
                           unsigned *attrsp, JSBool *foundp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
    return atom && JS_GetPropertyAttrsGetterAndSetterById(cx, obj, AtomToId(atom),
                                                          attrsp, foundp, NULL, NULL);
}

JS_PUBLIC_API(JSBool)
JS_GetPropertyAttrsGetterAndSetter(JSContext *cx, JSObject *objArg, const char *name,
                                   unsigned *attrsp, JSBool *foundp,
                                   JSPropertyOp *getterp, JSStrictPropertyOp *setterp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = Atomize(cx, name, strlen(name));
    return atom && JS_GetPropertyAttrsGetterAndSetterById(cx, obj, AtomToId(atom),
                                                          attrsp, foundp, getterp, setterp);
}

JS_PUBLIC_API(JSBool)
JS_GetUCPropertyAttrsGetterAndSetter(JSContext *cx, JSObject *objArg,
                                     const jschar *name, size_t namelen,
                                     unsigned *attrsp, JSBool *foundp,
                                     JSPropertyOp *getterp, JSStrictPropertyOp *setterp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
    return atom && JS_GetPropertyAttrsGetterAndSetterById(cx, obj, AtomToId(atom),
                                                          attrsp, foundp, getterp, setterp);
}

JS_PUBLIC_API(JSBool)
JS_GetOwnPropertyDescriptor(JSContext *cx, JSObject *objArg, jsid idArg, jsval *vp)
{
    RootedObject obj(cx, objArg);
    RootedId id(cx, idArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    return GetOwnPropertyDescriptor(cx, obj, id, vp);
}

static JSBool
SetPropertyAttributesById(JSContext *cx, HandleObject obj, HandleId id, unsigned attrs, JSBool *foundp)
{
    RootedObject obj2(cx);
    RootedShape shape(cx);

    if (!LookupPropertyById(cx, obj, id, JSRESOLVE_QUALIFIED, &obj2, &shape))
        return false;
    if (!shape || obj != obj2) {
        *foundp = false;
        return true;
    }
    JSBool ok = obj->isNative()
                ? JSObject::changePropertyAttributes(cx, obj, shape, attrs)
                : JSObject::setGenericAttributes(cx, obj, id, &attrs);
    if (ok)
        *foundp = true;
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_SetPropertyAttributes(JSContext *cx, JSObject *objArg, const char *name,
                         unsigned attrs, JSBool *foundp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = Atomize(cx, name, strlen(name));
    RootedId id(cx, AtomToId(atom));
    return atom && SetPropertyAttributesById(cx, obj, id, attrs, foundp);
}

JS_PUBLIC_API(JSBool)
JS_SetUCPropertyAttributes(JSContext *cx, JSObject *objArg, const jschar *name, size_t namelen,
                           unsigned attrs, JSBool *foundp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
    RootedId id(cx, AtomToId(atom));
    return atom && SetPropertyAttributesById(cx, obj, id, attrs, foundp);
}

JS_PUBLIC_API(JSBool)
JS_GetPropertyById(JSContext *cx, JSObject *objArg, jsid idArg, jsval *vp)
{
    return JS_ForwardGetPropertyTo(cx, objArg, idArg, objArg, vp);
}

JS_PUBLIC_API(JSBool)
JS_ForwardGetPropertyTo(JSContext *cx, JSObject *objArg, jsid idArg, JSObject *onBehalfOfArg, jsval *vp)
{
    RootedObject obj(cx, objArg);
    RootedObject onBehalfOf(cx, onBehalfOfArg);
    RootedId id(cx, idArg);

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, id);
    assertSameCompartment(cx, onBehalfOf);
    JSAutoResolveFlags rf(cx, JSRESOLVE_QUALIFIED);

    RootedValue value(cx);
    if (!JSObject::getGeneric(cx, obj, onBehalfOf, id, &value))
        return false;

    *vp = value;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_GetPropertyByIdDefault(JSContext *cx, JSObject *objArg, jsid idArg, jsval defArg, jsval *vp)
{
    RootedObject obj(cx, objArg);
    RootedId id(cx, idArg);
    RootedValue def(cx, defArg);

    RootedValue value(cx);
    if (!baseops::GetPropertyDefault(cx, obj, id, def, &value))
        return false;

    *vp = value;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_GetElement(JSContext *cx, JSObject *objArg, uint32_t index, jsval *vp)
{
    return JS_ForwardGetElementTo(cx, objArg, index, objArg, vp);
}

JS_PUBLIC_API(JSBool)
JS_ForwardGetElementTo(JSContext *cx, JSObject *objArg, uint32_t index, JSObject *onBehalfOfArg, jsval *vp)
{
    RootedObject obj(cx, objArg);
    RootedObject onBehalfOf(cx, onBehalfOfArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    JSAutoResolveFlags rf(cx, JSRESOLVE_QUALIFIED);

    RootedValue value(cx);
    if (!JSObject::getElement(cx, obj, onBehalfOf, index, &value))
        return false;

    *vp = value;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_GetElementIfPresent(JSContext *cx, JSObject *objArg, uint32_t index, JSObject *onBehalfOfArg, jsval *vp, JSBool* present)
{
    RootedObject obj(cx, objArg);
    RootedObject onBehalfOf(cx, onBehalfOfArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    JSAutoResolveFlags rf(cx, JSRESOLVE_QUALIFIED);

    RootedValue value(cx);
    bool isPresent;
    if (!JSObject::getElementIfPresent(cx, obj, onBehalfOf, index, &value, &isPresent))
        return false;

    *vp = value;
    *present = isPresent;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_GetProperty(JSContext *cx, JSObject *objArg, const char *name, jsval *vp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = Atomize(cx, name, strlen(name));
    return atom && JS_GetPropertyById(cx, obj, AtomToId(atom), vp);
}

JS_PUBLIC_API(JSBool)
JS_GetPropertyDefault(JSContext *cx, JSObject *objArg, const char *name, jsval def, jsval *vp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = Atomize(cx, name, strlen(name));
    return atom && JS_GetPropertyByIdDefault(cx, obj, AtomToId(atom), def, vp);
}

JS_PUBLIC_API(JSBool)
JS_GetUCProperty(JSContext *cx, JSObject *objArg, const jschar *name, size_t namelen, jsval *vp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
    return atom && JS_GetPropertyById(cx, obj, AtomToId(atom), vp);
}

JS_PUBLIC_API(JSBool)
JS_GetMethodById(JSContext *cx, JSObject *objArg, jsid idArg, JSObject **objp, jsval *vp)
{
    RootedObject obj(cx, objArg);
    RootedId id(cx, idArg);

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, id);

    RootedValue value(cx);
    if (!GetMethod(cx, obj, id, 0, &value))
        return JS_FALSE;
    *vp = value;

    if (objp)
        *objp = obj;
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_GetMethod(JSContext *cx, JSObject *objArg, const char *name, JSObject **objp, jsval *vp)
{
    JSAtom *atom = Atomize(cx, name, strlen(name));
    return atom && JS_GetMethodById(cx, objArg, AtomToId(atom), objp, vp);
}

JS_PUBLIC_API(JSBool)
JS_SetPropertyById(JSContext *cx, JSObject *objArg, jsid idArg, jsval *vp)
{
    RootedObject obj(cx, objArg);
    RootedId id(cx, idArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, id);
    JSAutoResolveFlags rf(cx, JSRESOLVE_QUALIFIED | JSRESOLVE_ASSIGNING);

    RootedValue value(cx, *vp);
    if (!JSObject::setGeneric(cx, obj, obj, id, &value, false))
        return false;

    *vp = value;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_SetElement(JSContext *cx, JSObject *objArg, uint32_t index, jsval *vp)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, *vp);
    JSAutoResolveFlags rf(cx, JSRESOLVE_QUALIFIED | JSRESOLVE_ASSIGNING);

    RootedValue value(cx, *vp);
    if (!JSObject::setElement(cx, obj, obj, index, &value, false))
        return false;

    *vp = value;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_SetProperty(JSContext *cx, JSObject *objArg, const char *name, jsval *vp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = Atomize(cx, name, strlen(name));
    return atom && JS_SetPropertyById(cx, obj, AtomToId(atom), vp);
}

JS_PUBLIC_API(JSBool)
JS_SetUCProperty(JSContext *cx, JSObject *objArg, const jschar *name, size_t namelen, jsval *vp)
{
    RootedObject obj(cx, objArg);
    JSAtom *atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
    return atom && JS_SetPropertyById(cx, obj, AtomToId(atom), vp);
}

JS_PUBLIC_API(JSBool)
JS_DeletePropertyById2(JSContext *cx, JSObject *objArg, jsid id, jsval *rval)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, id);
    JSAutoResolveFlags rf(cx, JSRESOLVE_QUALIFIED);

    RootedValue value(cx);

    if (JSID_IS_SPECIAL(id)) {
        Rooted<SpecialId> sid(cx, JSID_TO_SPECIALID(id));
        if (!JSObject::deleteSpecial(cx, obj, sid, &value, false))
            return false;
    } else {
        if (!JSObject::deleteByValue(cx, obj, IdToValue(id), &value, false))
            return false;
    }

    *rval = value;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_DeleteElement2(JSContext *cx, JSObject *objArg, uint32_t index, jsval *rval)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    JSAutoResolveFlags rf(cx, JSRESOLVE_QUALIFIED);

    RootedValue value(cx);
    if (!JSObject::deleteElement(cx, obj, index, &value, false))
        return false;

    *rval = value;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_DeleteProperty2(JSContext *cx, JSObject *objArg, const char *name, jsval *rval)
{
    RootedObject obj(cx, objArg);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    JSAutoResolveFlags rf(cx, JSRESOLVE_QUALIFIED);

    JSAtom *atom = Atomize(cx, name, strlen(name));
    if (!atom)
        return false;

    RootedValue value(cx);
    if (!JSObject::deleteByValue(cx, obj, StringValue(atom), &value, false))
        return false;

    *rval = value;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_DeleteUCProperty2(JSContext *cx, JSObject *objArg, const jschar *name, size_t namelen, jsval *rval)
{
    RootedObject obj(cx, objArg);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    JSAutoResolveFlags rf(cx, JSRESOLVE_QUALIFIED);

    JSAtom *atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
    if (!atom)
        return false;

    RootedValue value(cx);
    if (!JSObject::deleteByValue(cx, obj, StringValue(atom), &value, false))
        return false;

    *rval = value;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_DeletePropertyById(JSContext *cx, JSObject *objArg, jsid idArg)
{
    jsval junk;
    return JS_DeletePropertyById2(cx, objArg, idArg, &junk);
}

JS_PUBLIC_API(JSBool)
JS_DeleteElement(JSContext *cx, JSObject *objArg, uint32_t index)
{
    jsval junk;
    return JS_DeleteElement2(cx, objArg, index, &junk);
}

JS_PUBLIC_API(JSBool)
JS_DeleteProperty(JSContext *cx, JSObject *objArg, const char *name)
{
    jsval junk;
    return JS_DeleteProperty2(cx, objArg, name, &junk);
}

JS_PUBLIC_API(void)
JS_ClearScope(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);

    ClearOp clearOp = obj->getOps()->clear;
    if (clearOp)
        clearOp(cx, obj);

    if (obj->isNative())
        js_ClearNative(cx, obj);

    /* Clear cached class objects on the global object. */
    if (obj->isGlobal())
        obj->asGlobal().clear(cx);

    js_InitRandom(cx);
}

JS_PUBLIC_API(JSIdArray *)
JS_Enumerate(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);

    AutoIdVector props(cx);
    JSIdArray *ida;
    if (!GetPropertyNames(cx, obj, JSITER_OWNONLY, &props) || !VectorToIdArray(cx, props, &ida))
        return NULL;
    return ida;
}

/*
 * XXX reverse iterator for properties, unreverse and meld with jsinterp.c's
 *     prop_iterator_class somehow...
 * + preserve the obj->enumerate API while optimizing the native object case
 * + native case here uses a Shape *, but that iterates in reverse!
 * + so we make non-native match, by reverse-iterating after JS_Enumerating
 */
const uint32_t JSSLOT_ITER_INDEX = 0;

static void
prop_iter_finalize(FreeOp *fop, JSObject *obj)
{
    void *pdata = obj->getPrivate();
    if (!pdata)
        return;

    if (obj->getSlot(JSSLOT_ITER_INDEX).toInt32() >= 0) {
        /* Non-native case: destroy the ida enumerated when obj was created. */
        JSIdArray *ida = (JSIdArray *) pdata;
        DestroyIdArray(fop, ida);
    }
}

static void
prop_iter_trace(JSTracer *trc, JSObject *obj)
{
    void *pdata = obj->getPrivate();
    if (!pdata)
        return;

    if (obj->getSlot(JSSLOT_ITER_INDEX).toInt32() < 0) {
        /*
         * Native case: just mark the next property to visit. We don't need a
         * barrier here because the pointer is updated via setPrivate, which
         * always takes a barrier.
         */
        Shape *tmp = (Shape *)pdata;
        MarkShapeUnbarriered(trc, &tmp, "prop iter shape");
        obj->setPrivateUnbarriered(tmp);
    } else {
        /* Non-native case: mark each id in the JSIdArray private. */
        JSIdArray *ida = (JSIdArray *) pdata;
        MarkIdRange(trc, ida->length, ida->vector, "prop iter");
    }
}

static Class prop_iter_class = {
    "PropertyIterator",
    JSCLASS_HAS_PRIVATE | JSCLASS_IMPLEMENTS_BARRIERS | JSCLASS_HAS_RESERVED_SLOTS(1),
    JS_PropertyStub,         /* addProperty */
    JS_PropertyStub,         /* delProperty */
    JS_PropertyStub,         /* getProperty */
    JS_StrictPropertyStub,   /* setProperty */
    JS_EnumerateStub,
    JS_ResolveStub,
    JS_ConvertStub,
    prop_iter_finalize,
    NULL,           /* checkAccess */
    NULL,           /* call        */
    NULL,           /* construct   */
    NULL,           /* hasInstance */
    prop_iter_trace
};

JS_PUBLIC_API(JSObject *)
JS_NewPropertyIterator(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);

    RootedObject iterobj(cx, NewObjectWithClassProto(cx, &prop_iter_class, NULL, obj));
    if (!iterobj)
        return NULL;

    int index;
    if (obj->isNative()) {
        /* Native case: start with the last property in obj. */
        iterobj->setPrivateGCThing(obj->lastProperty());
        index = -1;
    } else {
        /* Non-native case: enumerate a JSIdArray and keep it via private. */
        JSIdArray *ida = JS_Enumerate(cx, obj);
        if (!ida)
            return NULL;
        iterobj->setPrivate((void *)ida);
        index = ida->length;
    }

    /* iterobj cannot escape to other threads here. */
    iterobj->setSlot(JSSLOT_ITER_INDEX, Int32Value(index));
    return iterobj;
}

JS_PUBLIC_API(JSBool)
JS_NextProperty(JSContext *cx, JSObject *iterobjArg, jsid *idp)
{
    RootedObject iterobj(cx, iterobjArg);
    AssertRootingUnnecessary safe(cx);
    int32_t i;
    Shape *shape;
    JSIdArray *ida;

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, iterobj);
    i = iterobj->getSlot(JSSLOT_ITER_INDEX).toInt32();
    if (i < 0) {
        /* Native case: private data is a property tree node pointer. */
        JS_ASSERT(iterobj->getParent()->isNative());
        shape = (Shape *) iterobj->getPrivate();

        while (shape->previous() && !shape->enumerable())
            shape = shape->previous();

        if (!shape->previous()) {
            JS_ASSERT(shape->isEmptyShape());
            *idp = JSID_VOID;
        } else {
            iterobj->setPrivateGCThing(const_cast<Shape *>(shape->previous().get()));
            *idp = shape->propid();
        }
    } else {
        /* Non-native case: use the ida enumerated when iterobj was created. */
        ida = (JSIdArray *) iterobj->getPrivate();
        JS_ASSERT(i <= ida->length);
        STATIC_ASSUME(i <= ida->length);
        if (i == 0) {
            *idp = JSID_VOID;
        } else {
            *idp = ida->vector[--i];
            iterobj->setSlot(JSSLOT_ITER_INDEX, Int32Value(i));
        }
    }
    return JS_TRUE;
}

JS_PUBLIC_API(JSBool)
JS_ArrayIterator(JSContext *cx, unsigned argc, jsval *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    Rooted<Value> target(cx, args.thisv());
    AssertHeapIsIdle(cx);
    assertSameCompartment(cx, target);
    CHECK_REQUEST(cx);

    JSObject *iterobj = ElementIteratorObject::create(cx, target);
    if (!iterobj)
        return false;
    vp->setObject(*iterobj);
    return true;
}

JS_PUBLIC_API(jsval)
JS_GetReservedSlot(RawObject obj, uint32_t index)
{
    return obj->getReservedSlot(index);
}

JS_PUBLIC_API(void)
JS_SetReservedSlot(RawObject obj, uint32_t index, jsval v)
{
    obj->setReservedSlot(index, v);
}

JS_PUBLIC_API(JSObject *)
JS_NewArrayObject(JSContext *cx, int length, jsval *vector)
{
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    assertSameCompartment(cx, JSValueArray(vector, vector ? (uint32_t)length : 0));
    return NewDenseCopiedArray(cx, (uint32_t)length, vector);
}

JS_PUBLIC_API(JSBool)
JS_IsArrayObject(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);
    assertSameCompartment(cx, obj);
    return ObjectClassIs(*obj, ESClass_Array, cx);
}

JS_PUBLIC_API(JSBool)
JS_GetArrayLength(JSContext *cx, JSObject *objArg, uint32_t *lengthp)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    return GetLengthProperty(cx, obj, lengthp);
}

JS_PUBLIC_API(JSBool)
JS_SetArrayLength(JSContext *cx, JSObject *objArg, uint32_t length)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    return SetLengthProperty(cx, obj, length);
}

JS_PUBLIC_API(JSBool)
JS_CheckAccess(JSContext *cx, JSObject *objArg, jsid idArg, JSAccessMode mode,
               jsval *vp, unsigned *attrsp)
{
    RootedObject obj(cx, objArg);
    RootedId id(cx, idArg);

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, id);
    return CheckAccess(cx, obj, id, mode, vp, attrsp);
}

JS_PUBLIC_API(void)
JS_HoldPrincipals(JSPrincipals *principals)
{
    JS_ATOMIC_INCREMENT(&principals->refcount);
}

JS_PUBLIC_API(void)
JS_DropPrincipals(JSRuntime *rt, JSPrincipals *principals)
{
    int rc = JS_ATOMIC_DECREMENT(&principals->refcount);
    if (rc == 0)
        rt->destroyPrincipals(principals);
}

JS_PUBLIC_API(void)
JS_SetSecurityCallbacks(JSRuntime *rt, const JSSecurityCallbacks *scb)
{
    JS_ASSERT(scb != &NullSecurityCallbacks);
    rt->securityCallbacks = scb ? scb : &NullSecurityCallbacks;
}

JS_PUBLIC_API(const JSSecurityCallbacks *)
JS_GetSecurityCallbacks(JSRuntime *rt)
{
    return (rt->securityCallbacks != &NullSecurityCallbacks) ? rt->securityCallbacks : NULL;
}

JS_PUBLIC_API(void)
JS_SetTrustedPrincipals(JSRuntime *rt, JSPrincipals *prin)
{
    rt->setTrustedPrincipals(prin);
}

extern JS_PUBLIC_API(void)
JS_InitDestroyPrincipalsCallback(JSRuntime *rt, JSDestroyPrincipalsOp destroyPrincipals)
{
    JS_ASSERT(destroyPrincipals);
    JS_ASSERT(!rt->destroyPrincipals);
    rt->destroyPrincipals = destroyPrincipals;
}

JS_PUBLIC_API(JSFunction *)
JS_NewFunction(JSContext *cx, JSNative native, unsigned nargs, unsigned flags,
               JSObject *parentArg, const char *name)
{
    RootedObject parent(cx, parentArg);
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    JSAtom *atom;

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, parent);

    if (!name) {
        atom = NULL;
    } else {
        atom = Atomize(cx, name, strlen(name));
        if (!atom)
            return NULL;
    }

    return js_NewFunction(cx, NULL, native, nargs, flags, parent, atom);
}

JS_PUBLIC_API(JSFunction *)
JS_NewFunctionById(JSContext *cx, JSNative native, unsigned nargs, unsigned flags, JSObject *parentArg,
                   jsid id)
{
    RootedObject parent(cx, parentArg);
    JS_ASSERT(JSID_IS_STRING(id));
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, parent);

    return js_NewFunction(cx, NULL, native, nargs, flags, parent, JSID_TO_ATOM(id));
}

JS_PUBLIC_API(JSObject *)
JS_CloneFunctionObject(JSContext *cx, JSObject *funobjArg, JSRawObject parentArg)
{
    RootedObject funobj(cx, funobjArg);
    RootedObject parent(cx, parentArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, parent);  // XXX no funobj for now

    if (!parent)
        parent = cx->global();

    if (!funobj->isFunction()) {
        ReportIsNotFunction(cx, ObjectValue(*funobj));
        return NULL;
    }

    /*
     * If a function was compiled to be lexically nested inside some other
     * script, we cannot clone it without breaking the compiler's assumptions.
     */
    RootedFunction fun(cx, funobj->toFunction());
    if (fun->isInterpreted() && (fun->script()->enclosingStaticScope() ||
        (fun->script()->compileAndGo && !parent->isGlobal())))
    {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_CLONE_FUNOBJ_SCOPE);
        return NULL;
    }

    if (fun->isBoundFunction()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_CLONE_OBJECT);
        return NULL;
    }

    return CloneFunctionObject(cx, fun, parent, fun->getAllocKind());
}

JS_PUBLIC_API(JSObject *)
JS_GetFunctionObject(JSFunction *fun)
{
    return fun;
}

JS_PUBLIC_API(JSString *)
JS_GetFunctionId(JSFunction *fun)
{
    return fun->atom();
}

JS_PUBLIC_API(JSString *)
JS_GetFunctionDisplayId(JSFunction *fun)
{
    return fun->displayAtom();
}

JS_PUBLIC_API(unsigned)
JS_GetFunctionFlags(JSFunction *fun)
{
    return fun->flags;
}

JS_PUBLIC_API(uint16_t)
JS_GetFunctionArity(JSFunction *fun)
{
    return fun->nargs;
}

JS_PUBLIC_API(JSBool)
JS_ObjectIsFunction(JSContext *cx, RawObject obj)
{
    return obj->isFunction();
}

JS_PUBLIC_API(JSBool)
JS_ObjectIsCallable(JSContext *cx, RawObject obj)
{
    return obj->isCallable();
}

JS_PUBLIC_API(JSBool)
JS_IsNativeFunction(JSRawObject funobj, JSNative call)
{
    if (!funobj->isFunction())
        return false;
    JSFunction *fun = funobj->toFunction();
    return fun->isNative() && fun->native() == call;
}

JS_PUBLIC_API(JSObject*)
JS_BindCallable(JSContext *cx, JSObject *targetArg, JSRawObject newThis)
{
    RootedObject target(cx, targetArg);
    RootedValue thisArg(cx, ObjectValue(*newThis));
    return js_fun_bind(cx, target, thisArg, NULL, 0);
}

JSBool
js_generic_native_method_dispatcher(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    JSFunctionSpec *fs = (JSFunctionSpec *)
        vp->toObject().toFunction()->getExtendedSlot(0).toPrivate();
    JS_ASSERT((fs->flags & JSFUN_GENERIC_NATIVE) != 0);

    if (argc < 1) {
        js_ReportMissingArg(cx, args.calleev(), 0);
        return JS_FALSE;
    }

    /*
     * Copy all actual (argc) arguments down over our |this| parameter, vp[1],
     * which is almost always the class constructor object, e.g. Array.  Then
     * call the corresponding prototype native method with our first argument
     * passed as |this|.
     */
    memmove(vp + 1, vp + 2, argc * sizeof(jsval));

    /* Clear the last parameter in case too few arguments were passed. */
    vp[2 + --argc].setUndefined();

    return fs->call.op(cx, argc, vp);
}

JS_PUBLIC_API(JSBool)
JS_DefineFunctions(JSContext *cx, JSObject *objArg, JSFunctionSpec *fs)
{
    RootedObject obj(cx, objArg);
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    unsigned flags;
    RootedObject ctor(cx);
    JSFunction *fun;

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    for (; fs->name; fs++) {
        flags = fs->flags;

        RootedAtom atom(cx, Atomize(cx, fs->name, strlen(fs->name)));
        if (!atom)
            return JS_FALSE;

        Rooted<jsid> id(cx, AtomToId(atom));

        /*
         * Define a generic arity N+1 static method for the arity N prototype
         * method if flags contains JSFUN_GENERIC_NATIVE.
         */
        if (flags & JSFUN_GENERIC_NATIVE) {
            if (!ctor) {
                ctor = JS_GetConstructor(cx, obj);
                if (!ctor)
                    return JS_FALSE;
            }

            flags &= ~JSFUN_GENERIC_NATIVE;
            fun = js_DefineFunction(cx, ctor, id, js_generic_native_method_dispatcher,
                                    fs->nargs + 1, flags, NULL, JSFunction::ExtendedFinalizeKind);
            if (!fun)
                return JS_FALSE;

            /*
             * As jsapi.h notes, fs must point to storage that lives as long
             * as fun->object lives.
             */
            fun->setExtendedSlot(0, PrivateValue(fs));
        }

        fun = js_DefineFunction(cx, obj, id, fs->call.op, fs->nargs, flags, fs->selfHostedName);
        if (!fun)
            return JS_FALSE;
        if (fs->call.info)
            fun->setJitInfo(fs->call.info);
    }
    return JS_TRUE;
}

JS_PUBLIC_API(JSFunction *)
JS_DefineFunction(JSContext *cx, JSObject *objArg, const char *name, JSNative call,
                  unsigned nargs, unsigned attrs)
{
    RootedObject obj(cx, objArg);
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    JSAtom *atom = Atomize(cx, name, strlen(name));
    if (!atom)
        return NULL;
    Rooted<jsid> id(cx, AtomToId(atom));
    return js_DefineFunction(cx, obj, id, call, nargs, attrs);
}

JS_PUBLIC_API(JSFunction *)
JS_DefineUCFunction(JSContext *cx, JSObject *objArg,
                    const jschar *name, size_t namelen, JSNative call,
                    unsigned nargs, unsigned attrs)
{
    RootedObject obj(cx, objArg);
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    JSAtom *atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
    if (!atom)
        return NULL;
    Rooted<jsid> id(cx, AtomToId(atom));
    return js_DefineFunction(cx, obj, id, call, nargs, attrs);
}

extern JS_PUBLIC_API(JSFunction *)
JS_DefineFunctionById(JSContext *cx, JSObject *objArg, jsid id_, JSNative call,
                      unsigned nargs, unsigned attrs)
{
    RootedObject obj(cx, objArg);
    RootedId id(cx, id_);
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    return js_DefineFunction(cx, obj, id, call, nargs, attrs);
}

struct AutoLastFrameCheck {
    AutoLastFrameCheck(JSContext *cx JS_GUARD_OBJECT_NOTIFIER_PARAM)
      : cx(cx) {
        JS_ASSERT(cx);
        JS_GUARD_OBJECT_NOTIFIER_INIT;
    }

    ~AutoLastFrameCheck() {
        if (cx->isExceptionPending() &&
            !JS_IsRunning(cx) &&
            !cx->hasRunOption(JSOPTION_DONT_REPORT_UNCAUGHT)) {
            js_ReportUncaughtException(cx);
        }
    }

  private:
    JSContext       *cx;
    JS_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/* Use the fastest available getc. */
#if defined(HAVE_GETC_UNLOCKED)
# define fast_getc getc_unlocked
#elif defined(HAVE__GETC_NOLOCK)
# define fast_getc _getc_nolock
#else
# define fast_getc getc
#endif

typedef Vector<char, 8, TempAllocPolicy> FileContents;

static bool
ReadCompleteFile(JSContext *cx, FILE *fp, FileContents &buffer)
{
    /* Get the complete length of the file, if possible. */
    struct stat st;
    int ok = fstat(fileno(fp), &st);
    if (ok != 0)
        return false;
    if (st.st_size > 0) {
        if (!buffer.reserve(st.st_size))
            return false;
    }

    // Read in the whole file. Note that we can't assume the data's length
    // is actually st.st_size, because 1) some files lie about their size
    // (/dev/zero and /dev/random), and 2) reading files in text mode on
    // Windows collapses "\r\n" pairs to single \n characters.
    for (;;) {
        int c = fast_getc(fp);
        if (c == EOF)
            break;
        if (!buffer.append(c))
            return false;
    }

    return true;
}

class AutoFile
{
    FILE *fp_;
  public:
    AutoFile()
      : fp_(NULL)
    {}
    ~AutoFile()
    {
        if (fp_ && fp_ != stdin)
            fclose(fp_);
    }
    FILE *fp() const { return fp_; }
    bool open(JSContext *cx, const char *filename);
    bool readAll(JSContext *cx, FileContents &buffer)
    {
        JS_ASSERT(fp_);
        return ReadCompleteFile(cx, fp_, buffer);
    }
};

/*
 * Open a source file for reading. Supports "-" and NULL to mean stdin. The
 * return value must be fclosed unless it is stdin.
 */
bool
AutoFile::open(JSContext *cx, const char *filename)
{
    if (!filename || strcmp(filename, "-") == 0) {
        fp_ = stdin;
    } else {
        fp_ = fopen(filename, "r");
        if (!fp_) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_OPEN,
                                 filename, "No such file or directory");
            return false;
        }
    }
    return true;
}


JS::CompileOptions::CompileOptions(JSContext *cx)
    : principals(NULL),
      originPrincipals(NULL),
      version(cx->findVersion()),
      versionSet(false),
      utf8(false),
      filename(NULL),
      lineno(1),
      compileAndGo(cx->hasRunOption(JSOPTION_COMPILE_N_GO)),
      noScriptRval(cx->hasRunOption(JSOPTION_NO_SCRIPT_RVAL)),
      selfHostingMode(false),
      sourcePolicy(SAVE_SOURCE)
{
}

JSScript *
JS::Compile(JSContext *cx, HandleObject obj, CompileOptions options,
            const jschar *chars, size_t length)
{
    Maybe<AutoVersionAPI> mava;
    if (options.versionSet) {
        mava.construct(cx, options.version);
        // AutoVersionAPI propagates some compilation flags through.
        options.version = mava.ref().version();
    }

    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, options.principals, options.originPrincipals);
    AutoLastFrameCheck lfc(cx);

    return frontend::CompileScript(cx, obj, NULL, options, chars, length);
}

JSScript *
JS::Compile(JSContext *cx, HandleObject obj, CompileOptions options,
            const char *bytes, size_t length)
{
    jschar *chars;
    if (options.utf8)
        chars = InflateString(cx, bytes, &length, CESU8Encoding);
    else
        chars = InflateString(cx, bytes, &length);
    if (!chars)
        return NULL;

    JSScript *script = Compile(cx, obj, options, chars, length);
    cx->free_(chars);
    return script;
}

JSScript *
JS::Compile(JSContext *cx, HandleObject obj, CompileOptions options, FILE *fp)
{
    FileContents buffer(cx);
    if (!ReadCompleteFile(cx, fp, buffer))
        return NULL;

    JSScript *script = Compile(cx, obj, options, buffer.begin(), buffer.length());
    return script;
}

JSScript *
JS::Compile(JSContext *cx, HandleObject obj, CompileOptions options, const char *filename)
{
    AutoFile file;
    if (!file.open(cx, filename))
        return NULL;
    options = options.setFileAndLine(filename, 1);
    JSScript *script = Compile(cx, obj, options, file.fp());
    return script;
}

extern JS_PUBLIC_API(JSScript *)
JS_CompileUCScriptForPrincipalsVersion(JSContext *cx, JSObject *objArg,
                                       JSPrincipals *principals,
                                       const jschar *chars, size_t length,
                                       const char *filename, unsigned lineno,
                                       JSVersion version)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setPrincipals(principals)
           .setFileAndLine(filename, lineno)
           .setVersion(version);

    return Compile(cx, obj, options, chars, length);
}

extern JS_PUBLIC_API(JSScript *)
JS_CompileUCScriptForPrincipalsVersionOrigin(JSContext *cx, JSObject *objArg,
                                             JSPrincipals *principals,
                                             JSPrincipals *originPrincipals,
                                             const jschar *chars, size_t length,
                                             const char *filename, unsigned lineno,
                                             JSVersion version)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setPrincipals(principals)
           .setOriginPrincipals(originPrincipals)
           .setFileAndLine(filename, lineno)
           .setVersion(version);

    return Compile(cx, obj, options, chars, length);
}

JS_PUBLIC_API(JSScript *)
JS_CompileUCScriptForPrincipals(JSContext *cx, JSObject *objArg, JSPrincipals *principals,
                                const jschar *chars, size_t length,
                                const char *filename, unsigned lineno)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setPrincipals(principals)
           .setFileAndLine(filename, lineno);

    return Compile(cx, obj, options, chars, length);
}

JS_PUBLIC_API(JSScript *)
JS_CompileUCScript(JSContext *cx, JSObject *objArg, const jschar *chars, size_t length,
                   const char *filename, unsigned lineno)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setFileAndLine(filename, lineno);

    return Compile(cx, obj, options, chars, length);
}

JS_PUBLIC_API(JSScript *)
JS_CompileScriptForPrincipalsVersion(JSContext *cx, JSObject *objArg,
                                     JSPrincipals *principals,
                                     const char *bytes, size_t length,
                                     const char *filename, unsigned lineno,
                                     JSVersion version)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setPrincipals(principals)
           .setFileAndLine(filename, lineno)
           .setVersion(version);

    return Compile(cx, obj, options, bytes, length);
}

JS_PUBLIC_API(JSScript *)
JS_CompileScriptForPrincipals(JSContext *cx, JSObject *objArg,
                              JSPrincipals *principals,
                              const char *bytes, size_t length,
                              const char *filename, unsigned lineno)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setPrincipals(principals)
           .setFileAndLine(filename, lineno);

    return Compile(cx, obj, options, bytes, length);
}

JS_PUBLIC_API(JSScript *)
JS_CompileScript(JSContext *cx, JSObject *objArg, const char *bytes, size_t length,
                 const char *filename, unsigned lineno)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setFileAndLine(filename, lineno);

    return Compile(cx, obj, options, bytes, length);
}

JS_PUBLIC_API(JSBool)
JS_BufferIsCompilableUnit(JSContext *cx, JSBool bytes_are_utf8, JSObject *objArg, const char *bytes, size_t length)
{
    RootedObject obj(cx, objArg);
    jschar *chars;
    JSBool result;
    JSExceptionState *exnState;
    JSErrorReporter older;

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    if (bytes_are_utf8)
        chars = InflateString(cx, bytes, &length, CESU8Encoding);
    else
        chars = InflateString(cx, bytes, &length);
    if (!chars)
        return JS_TRUE;

    /*
     * Return true on any out-of-memory error, so our caller doesn't try to
     * collect more buffered source.
     */
    result = JS_TRUE;
    exnState = JS_SaveExceptionState(cx);
    {
        CompileOptions options(cx);
        options.setCompileAndGo(false);
        Parser parser(cx, options, chars, length, /* foldConstants = */ true);
        if (parser.init()) {
            older = JS_SetErrorReporter(cx, NULL);
            if (!parser.parse(obj) &&
                parser.tokenStream.isUnexpectedEOF()) {
                /*
                 * We ran into an error. If it was because we ran out of
                 * source, we return false so our caller knows to try to
                 * collect more buffered source.
                 */
                result = JS_FALSE;
            }
            JS_SetErrorReporter(cx, older);
        }
    }
    cx->free_(chars);
    JS_RestoreExceptionState(cx, exnState);
    return result;
}

JS_PUBLIC_API(JSScript *)
JS_CompileUTF8File(JSContext *cx, JSObject *objArg, const char *filename)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setUTF8(true)
           .setFileAndLine(filename, 1);

    return Compile(cx, obj, options, filename);
}

JS_PUBLIC_API(JSScript *)
JS_CompileUTF8FileHandleForPrincipals(JSContext *cx, JSObject *objArg, const char *filename,
                                      FILE *file, JSPrincipals *principals)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setUTF8(true)
           .setFileAndLine(filename, 1)
           .setPrincipals(principals);

    return Compile(cx, obj, options, file);
}

JS_PUBLIC_API(JSScript *)
JS_CompileUTF8FileHandleForPrincipalsVersion(JSContext *cx, JSObject *objArg, const char *filename,
                                             FILE *file, JSPrincipals *principals, JSVersion version)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setUTF8(true)
           .setFileAndLine(filename, 1)
           .setPrincipals(principals)
           .setVersion(version);

    return Compile(cx, obj, options, file);
}

JS_PUBLIC_API(JSScript *)
JS_CompileUTF8FileHandle(JSContext *cx, JSObject *objArg, const char *filename, FILE *file)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setUTF8(true)
           .setFileAndLine(filename, 1);

    return Compile(cx, obj, options, file);
}

JS_PUBLIC_API(JSObject *)
JS_GetGlobalFromScript(JSScript *script)
{
    JS_ASSERT(!script->isCachedEval);
    return &script->global();
}

JS_PUBLIC_API(JSFunction *)
JS::CompileFunction(JSContext *cx, HandleObject obj, CompileOptions options,
                    const char *name, unsigned nargs, const char **argnames,
                    const jschar *chars, size_t length)
{
    Maybe<AutoVersionAPI> mava;
    if (options.versionSet) {
        mava.construct(cx, options.version);
        // AutoVersionAPI propagates some compilation flags through.
        options.version = mava.ref().version();
    }

    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, options.principals, options.originPrincipals);
    AutoLastFrameCheck lfc(cx);

    RootedAtom funAtom(cx);
    if (name) {
        funAtom = Atomize(cx, name, strlen(name));
        if (!funAtom)
            return NULL;
    }

    AutoNameVector formals(cx);
    for (unsigned i = 0; i < nargs; i++) {
        RootedAtom argAtom(cx, Atomize(cx, argnames[i], strlen(argnames[i])));
        if (!argAtom || !formals.append(argAtom->asPropertyName()))
            return NULL;
    }

    RootedFunction fun(cx, js_NewFunction(cx, NULL, NULL, 0, JSFUN_INTERPRETED, obj, funAtom));
    if (!fun)
        return NULL;

    if (!frontend::CompileFunctionBody(cx, fun, options, formals, chars, length))
        return NULL;

    if (obj && funAtom) {
        Rooted<jsid> id(cx, AtomToId(funAtom));
        RootedValue value(cx, ObjectValue(*fun));
        if (!JSObject::defineGeneric(cx, obj, id, value, NULL, NULL, JSPROP_ENUMERATE))
            return NULL;
    }

    return fun;
}

JS_PUBLIC_API(JSFunction *)
JS::CompileFunction(JSContext *cx, HandleObject obj, CompileOptions options,
                    const char *name, unsigned nargs, const char **argnames,
                    const char *bytes, size_t length)
{
    jschar *chars;
    if (options.utf8)
        chars = InflateString(cx, bytes, &length, CESU8Encoding);
    else
        chars = InflateString(cx, bytes, &length);
    if (!chars)
        return NULL;

    JSFunction *fun = CompileFunction(cx, obj, options, name, nargs, argnames, chars, length);
    cx->free_(chars);
    return fun;
}

JS_PUBLIC_API(JSFunction *)
JS_CompileUCFunctionForPrincipalsVersion(JSContext *cx, JSObject *obj_,
                                         JSPrincipals *principals, const char *name,
                                         unsigned nargs, const char **argnames,
                                         const jschar *chars, size_t length,
                                         const char *filename, unsigned lineno,
                                         JSVersion version)
{
    RootedObject obj(cx, obj_);

    CompileOptions options(cx);
    options.setPrincipals(principals)
           .setFileAndLine(filename, lineno)
           .setVersion(version);

    return CompileFunction(cx, obj, options, name, nargs, argnames, chars, length);
}

JS_PUBLIC_API(JSFunction *)
JS_CompileUCFunctionForPrincipals(JSContext *cx, JSObject *objArg,
                                  JSPrincipals *principals, const char *name,
                                  unsigned nargs, const char **argnames,
                                  const jschar *chars, size_t length,
                                  const char *filename, unsigned lineno)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setPrincipals(principals)
           .setFileAndLine(filename, lineno);

    return CompileFunction(cx, obj, options, name, nargs, argnames, chars, length);
}

JS_PUBLIC_API(JSFunction *)
JS_CompileUCFunction(JSContext *cx, JSObject *objArg, const char *name,
                     unsigned nargs, const char **argnames,
                     const jschar *chars, size_t length,
                     const char *filename, unsigned lineno)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setFileAndLine(filename, lineno);

    return CompileFunction(cx, obj, options, name, nargs, argnames, chars, length);
}

JS_PUBLIC_API(JSFunction *)
JS_CompileFunctionForPrincipals(JSContext *cx, JSObject *objArg,
                                JSPrincipals *principals, const char *name,
                                unsigned nargs, const char **argnames,
                                const char *bytes, size_t length,
                                const char *filename, unsigned lineno)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setPrincipals(principals)
           .setFileAndLine(filename, lineno);

    return CompileFunction(cx, obj, options, name, nargs, argnames, bytes, length);
}

JS_PUBLIC_API(JSFunction *)
JS_CompileFunction(JSContext *cx, JSObject *objArg, const char *name,
                   unsigned nargs, const char **argnames,
                   const char *bytes, size_t length,
                   const char *filename, unsigned lineno)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setFileAndLine(filename, lineno);

    return CompileFunction(cx, obj, options, name, nargs, argnames, bytes, length);
}

JS_PUBLIC_API(JSString *)
JS_DecompileScript(JSContext *cx, JSScript *script, const char *name, unsigned indent)
{
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    RootedFunction fun(cx, script->function());
    if (fun)
        return JS_DecompileFunction(cx, fun, indent);
    return script->sourceData(cx);
}

JS_PUBLIC_API(JSString *)
JS_DecompileFunction(JSContext *cx, JSFunction *funArg, unsigned indent)
{
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, funArg);
    RootedFunction fun(cx, funArg);
    return FunctionToString(cx, fun, false, !(indent & JS_DONT_PRETTY_PRINT));
}

JS_PUBLIC_API(JSString *)
JS_DecompileFunctionBody(JSContext *cx, JSFunction *funArg, unsigned indent)
{
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, funArg);
    RootedFunction fun(cx, funArg);
    return FunctionToString(cx, fun, true, !(indent & JS_DONT_PRETTY_PRINT));
}

JS_NEVER_INLINE JS_PUBLIC_API(JSBool)
JS_ExecuteScript(JSContext *cx, JSObject *objArg, JSScript *scriptArg, jsval *rval)
{
    RootedObject obj(cx, objArg);
    RootedScript script(cx, scriptArg);

    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj);
    if (cx->compartment != obj->compartment())
        *(volatile int *) 0 = 0xf0;
    AutoLastFrameCheck lfc(cx);

    /*
     * Mozilla caches pre-compiled scripts (e.g., in the XUL prototype cache)
     * and runs them against multiple globals. With a compartment per global,
     * this requires cloning the pre-compiled script into each new global.
     * Since each script gets run once, there is no point in trying to cache
     * this clone. Ideally, this would be handled at some pinch point in
     * mozilla, but there doesn't seem to be one, so we handle it here.
     */
    if (script->compartment() != obj->compartment()) {
        script = CloneScript(cx, NullPtr(), NullPtr(), script);
        if (!script.get())
            return false;
    } else {
        script = scriptArg;
    }

    return Execute(cx, script, *obj, rval);
}

JS_PUBLIC_API(JSBool)
JS_ExecuteScriptVersion(JSContext *cx, JSObject *objArg, JSScript *script, jsval *rval,
                        JSVersion version)
{
    RootedObject obj(cx, objArg);
    AutoVersionAPI ava(cx, version);
    return JS_ExecuteScript(cx, obj, script, rval);
}

extern JS_PUBLIC_API(bool)
JS::Evaluate(JSContext *cx, HandleObject obj, CompileOptions options,
             const jschar *chars, size_t length, jsval *rval)
{
    Maybe<AutoVersionAPI> mava;
    if (options.versionSet) {
        mava.construct(cx, options.version);
        // AutoVersionAPI propagates some compilation flags through.
        options.version = mava.ref().version();
    }

    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, options.principals, options.originPrincipals);
    AutoLastFrameCheck lfc(cx);

    options.setCompileAndGo(true);
    options.setNoScriptRval(!rval);
    RootedScript script(cx, frontend::CompileScript(cx, obj, NULL, options, chars, length));
    if (!script)
        return false;

    JS_ASSERT(script->getVersion() == options.version);

    return Execute(cx, script, *obj, rval);
}

extern JS_PUBLIC_API(bool)
JS::Evaluate(JSContext *cx, HandleObject obj, CompileOptions options,
             const char *bytes, size_t length, jsval *rval)
{
    jschar *chars;
    if (options.utf8)
        chars = InflateString(cx, bytes, &length, CESU8Encoding);
    else
        chars = InflateString(cx, bytes, &length);
    if (!chars)
        return false;

    bool ok = Evaluate(cx, obj, options, chars, length, rval);
    cx->free_(chars);
    return ok;
}

extern JS_PUBLIC_API(bool)
JS::Evaluate(JSContext *cx, HandleObject obj, CompileOptions options,
             const char *filename, jsval *rval)
{
    FileContents buffer(cx);
    {
        AutoFile file;
        if (!file.open(cx, filename) || !file.readAll(cx, buffer))
            return false;
    }

    options = options.setFileAndLine(filename, 1);
    return Evaluate(cx, obj, options, buffer.begin(), buffer.length(), rval);
}

JS_PUBLIC_API(JSBool)
JS_EvaluateUCScriptForPrincipals(JSContext *cx, JSObject *objArg,
                                 JSPrincipals *principals,
                                 const jschar *chars, unsigned length,
                                 const char *filename, unsigned lineno,
                                 jsval *rval)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setPrincipals(principals)
           .setFileAndLine(filename, lineno);

    return Evaluate(cx, obj, options, chars, length, rval);
}

JS_PUBLIC_API(JSBool)
JS_EvaluateUCScriptForPrincipalsVersion(JSContext *cx, JSObject *objArg,
                                        JSPrincipals *principals,
                                        const jschar *chars, unsigned length,
                                        const char *filename, unsigned lineno,
                                        jsval *rval, JSVersion version)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setPrincipals(principals)
           .setFileAndLine(filename, lineno)
           .setVersion(version);

    return Evaluate(cx, obj, options, chars, length, rval);
}

extern JS_PUBLIC_API(JSBool)
JS_EvaluateUCScriptForPrincipalsVersionOrigin(JSContext *cx, JSObject *objArg,
                                              JSPrincipals *principals,
                                              JSPrincipals *originPrincipals,
                                              const jschar *chars, unsigned length,
                                              const char *filename, unsigned lineno,
                                              jsval *rval, JSVersion version)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setPrincipals(principals)
           .setOriginPrincipals(originPrincipals)
           .setFileAndLine(filename, lineno)
           .setVersion(version);

    return Evaluate(cx, obj, options, chars, length, rval);
}

JS_PUBLIC_API(JSBool)
JS_EvaluateUCScript(JSContext *cx, JSObject *objArg, const jschar *chars, unsigned length,
                    const char *filename, unsigned lineno, jsval *rval)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setFileAndLine(filename, lineno);

    return Evaluate(cx, obj, options, chars, length, rval);
}

/* Ancient unsigned nbytes is part of API/ABI, so use size_t length local. */
JS_PUBLIC_API(JSBool)
JS_EvaluateScriptForPrincipals(JSContext *cx, JSObject *objArg, JSPrincipals *principals,
                               const char *bytes, unsigned nbytes,
                               const char *filename, unsigned lineno, jsval *rval)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setPrincipals(principals)
           .setFileAndLine(filename, lineno);

    return Evaluate(cx, obj, options, bytes, nbytes, rval);
}

JS_PUBLIC_API(JSBool)
JS_EvaluateScriptForPrincipalsVersion(JSContext *cx, JSObject *objArg, JSPrincipals *principals,
                                      const char *bytes, unsigned nbytes,
                                      const char *filename, unsigned lineno, jsval *rval,
                                      JSVersion version)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setPrincipals(principals)
           .setVersion(version)
           .setFileAndLine(filename, lineno);

    return Evaluate(cx, obj, options, bytes, nbytes, rval);
}

JS_PUBLIC_API(JSBool)
JS_EvaluateScript(JSContext *cx, JSObject *objArg, const char *bytes, unsigned nbytes,
                  const char *filename, unsigned lineno, jsval *rval)
{
    RootedObject obj(cx, objArg);
    CompileOptions options(cx);
    options.setFileAndLine(filename, lineno);

    return Evaluate(cx, obj, options, bytes, nbytes, rval);
}

JS_PUBLIC_API(JSBool)
JS_CallFunction(JSContext *cx, JSObject *objArg, JSFunction *fun, unsigned argc, jsval *argv,
                jsval *rval)
{
    RootedObject obj(cx, objArg);
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, fun, JSValueArray(argv, argc));
    AutoLastFrameCheck lfc(cx);

    return Invoke(cx, ObjectOrNullValue(obj), ObjectValue(*fun), argc, argv, rval);
}

JS_PUBLIC_API(JSBool)
JS_CallFunctionName(JSContext *cx, JSObject *objArg, const char *name, unsigned argc, jsval *argv,
                    jsval *rval)
{
    RootedObject obj(cx, objArg);
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, JSValueArray(argv, argc));
    AutoLastFrameCheck lfc(cx);

    JSAtom *atom = Atomize(cx, name, strlen(name));
    if (!atom)
        return false;

    RootedValue v(cx);
    RootedId id(cx, AtomToId(atom));
    return GetMethod(cx, obj, id, 0, &v) &&
           Invoke(cx, ObjectOrNullValue(obj), v, argc, argv, rval);
}

JS_PUBLIC_API(JSBool)
JS_CallFunctionValue(JSContext *cx, JSObject *objArg, jsval fval, unsigned argc, jsval *argv,
                     jsval *rval)
{
    RootedObject obj(cx, objArg);
    JS_THREADSAFE_ASSERT(cx->compartment != cx->runtime->atomsCompartment);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, obj, fval, JSValueArray(argv, argc));
    AutoLastFrameCheck lfc(cx);

    return Invoke(cx, ObjectOrNullValue(obj), fval, argc, argv, rval);
}

namespace JS {

JS_PUBLIC_API(bool)
Call(JSContext *cx, jsval thisv, jsval fval, unsigned argc, jsval *argv, jsval *rval)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, thisv, fval, JSValueArray(argv, argc));
    AutoLastFrameCheck lfc(cx);

    return Invoke(cx, thisv, fval, argc, argv, rval);
}

} // namespace JS

JS_PUBLIC_API(JSObject *)
JS_New(JSContext *cx, JSObject *ctorArg, unsigned argc, jsval *argv)
{
    RootedObject ctor(cx, ctorArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, ctor, JSValueArray(argv, argc));
    AutoLastFrameCheck lfc(cx);

    // This is not a simple variation of JS_CallFunctionValue because JSOP_NEW
    // is not a simple variation of JSOP_CALL. We have to determine what class
    // of object to create, create it, and clamp the return value to an object,
    // among other details. InvokeConstructor does the hard work.
    InvokeArgsGuard args;
    if (!cx->stack.pushInvokeArgs(cx, argc, &args))
        return NULL;

    args.setCallee(ObjectValue(*ctor));
    args.setThis(NullValue());
    PodCopy(args.array(), argv, argc);

    if (!InvokeConstructor(cx, args))
        return NULL;

    if (!args.rval().isObject()) {
        /*
         * Although constructors may return primitives (via proxies), this
         * API is asking for an object, so we report an error.
         */
        JSAutoByteString bytes;
        if (js_ValueToPrintable(cx, args.rval(), &bytes)) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_NEW_RESULT,
                                 bytes.ptr());
        }
        return NULL;
    }

    return &args.rval().toObject();
}

JS_PUBLIC_API(JSOperationCallback)
JS_SetOperationCallback(JSContext *cx, JSOperationCallback callback)
{
    JSOperationCallback old = cx->operationCallback;
    cx->operationCallback = callback;
    return old;
}

JS_PUBLIC_API(JSOperationCallback)
JS_GetOperationCallback(JSContext *cx)
{
    return cx->operationCallback;
}

JS_PUBLIC_API(void)
JS_TriggerOperationCallback(JSRuntime *rt)
{
    rt->triggerOperationCallback();
}

JS_PUBLIC_API(JSBool)
JS_IsRunning(JSContext *cx)
{
    return cx->hasfp();
}

JS_PUBLIC_API(JSBool)
JS_SaveFrameChain(JSContext *cx)
{
    AssertHeapIsIdleOrIterating(cx);
    CHECK_REQUEST(cx);
    return cx->saveFrameChain();
}

JS_PUBLIC_API(void)
JS_RestoreFrameChain(JSContext *cx)
{
    AssertHeapIsIdleOrIterating(cx);
    CHECK_REQUEST(cx);
    cx->restoreFrameChain();
}

#ifdef MOZ_TRACE_JSCALLS
JS_PUBLIC_API(void)
JS_SetFunctionCallback(JSContext *cx, JSFunctionCallback fcb)
{
    cx->functionCallback = fcb;
}

JS_PUBLIC_API(JSFunctionCallback)
JS_GetFunctionCallback(JSContext *cx)
{
    return cx->functionCallback;
}
#endif

/************************************************************************/
JS_PUBLIC_API(JSString *)
JS_NewStringCopyN(JSContext *cx, const char *s, size_t n)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_NewStringCopyN(cx, s, n);
}

JS_PUBLIC_API(JSString *)
JS_NewStringCopyZ(JSContext *cx, const char *s)
{
    size_t n;
    jschar *js;
    JSString *str;

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    if (!s || !*s)
        return cx->runtime->emptyString;
    n = strlen(s);
    js = InflateString(cx, s, &n);
    if (!js)
        return NULL;
    str = js_NewString(cx, js, n);
    if (!str)
        cx->free_(js);
    return str;
}

JS_PUBLIC_API(JSBool)
JS_StringHasBeenInterned(JSContext *cx, JSString *str)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    if (!str->isAtom())
        return false;

    return AtomIsInterned(cx, &str->asAtom());
}

JS_PUBLIC_API(jsid)
INTERNED_STRING_TO_JSID(JSContext *cx, JSString *str)
{
    JS_ASSERT(str);
    JS_ASSERT(((size_t)str & JSID_TYPE_MASK) == 0);
    JS_ASSERT_IF(cx, JS_StringHasBeenInterned(cx, str));
    return AtomToId(&str->asAtom());
}

JS_PUBLIC_API(JSString *)
JS_InternJSString(JSContext *cx, JSString *str)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    JSAtom *atom = AtomizeString(cx, str, InternAtom);
    JS_ASSERT_IF(atom, JS_StringHasBeenInterned(cx, atom));
    return atom;
}

JS_PUBLIC_API(JSString *)
JS_InternString(JSContext *cx, const char *s)
{
    return JS_InternStringN(cx, s, strlen(s));
}

JS_PUBLIC_API(JSString *)
JS_InternStringN(JSContext *cx, const char *s, size_t length)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    JSAtom *atom = Atomize(cx, s, length, InternAtom);
    JS_ASSERT_IF(atom, JS_StringHasBeenInterned(cx, atom));
    return atom;
}

JS_PUBLIC_API(JSString *)
JS_NewUCString(JSContext *cx, jschar *chars, size_t length)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_NewString(cx, chars, length);
}

JS_PUBLIC_API(JSString *)
JS_NewUCStringCopyN(JSContext *cx, const jschar *s, size_t n)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_NewStringCopyN(cx, s, n);
}

JS_PUBLIC_API(JSString *)
JS_NewUCStringCopyZ(JSContext *cx, const jschar *s)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    if (!s)
        return cx->runtime->emptyString;
    return js_NewStringCopyZ(cx, s);
}

JS_PUBLIC_API(JSString *)
JS_InternUCStringN(JSContext *cx, const jschar *s, size_t length)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    JSAtom *atom = AtomizeChars(cx, s, length, InternAtom);
    JS_ASSERT_IF(atom, JS_StringHasBeenInterned(cx, atom));
    return atom;
}

JS_PUBLIC_API(JSString *)
JS_InternUCString(JSContext *cx, const jschar *s)
{
    return JS_InternUCStringN(cx, s, js_strlen(s));
}

JS_PUBLIC_API(size_t)
JS_GetStringLength(JSString *str)
{
    return str->length();
}

JS_PUBLIC_API(const jschar *)
JS_GetStringCharsZ(JSContext *cx, JSString *str)
{
    AssertHeapIsIdleOrStringIsFlat(cx, str);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, str);
    return str->getCharsZ(cx);
}

JS_PUBLIC_API(const jschar *)
JS_GetStringCharsZAndLength(JSContext *cx, JSString *str, size_t *plength)
{
    AssertHeapIsIdleOrStringIsFlat(cx, str);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, str);
    *plength = str->length();
    return str->getCharsZ(cx);
}

JS_PUBLIC_API(const jschar *)
JS_GetStringCharsAndLength(JSContext *cx, JSString *str, size_t *plength)
{
    AssertHeapIsIdleOrStringIsFlat(cx, str);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, str);
    *plength = str->length();
    return str->getChars(cx);
}

JS_PUBLIC_API(const jschar *)
JS_GetInternedStringChars(JSString *str)
{
    return str->asAtom().chars();
}

JS_PUBLIC_API(const jschar *)
JS_GetInternedStringCharsAndLength(JSString *str, size_t *plength)
{
    JSAtom &atom = str->asAtom();
    *plength = atom.length();
    return atom.chars();
}

extern JS_PUBLIC_API(JSFlatString *)
JS_FlattenString(JSContext *cx, JSString *str)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, str);
    return str->getCharsZ(cx) ? (JSFlatString *)str : NULL;
}

extern JS_PUBLIC_API(const jschar *)
JS_GetFlatStringChars(JSFlatString *str)
{
    return str->chars();
}

JS_PUBLIC_API(JSBool)
JS_CompareStrings(JSContext *cx, JSString *str1, JSString *str2, int32_t *result)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    return CompareStrings(cx, str1, str2, result);
}

JS_PUBLIC_API(JSBool)
JS_StringEqualsAscii(JSContext *cx, JSString *str, const char *asciiBytes, JSBool *match)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    JSLinearString *linearStr = str->ensureLinear(cx);
    if (!linearStr)
        return false;
    *match = StringEqualsAscii(linearStr, asciiBytes);
    return true;
}

JS_PUBLIC_API(JSBool)
JS_FlatStringEqualsAscii(JSFlatString *str, const char *asciiBytes)
{
    return StringEqualsAscii(str, asciiBytes);
}

JS_PUBLIC_API(size_t)
JS_PutEscapedFlatString(char *buffer, size_t size, JSFlatString *str, char quote)
{
    return PutEscapedString(buffer, size, str, quote);
}

JS_PUBLIC_API(size_t)
JS_PutEscapedString(JSContext *cx, char *buffer, size_t size, JSString *str, char quote)
{
    AssertHeapIsIdle(cx);
    JSLinearString *linearStr = str->ensureLinear(cx);
    if (!linearStr)
        return size_t(-1);
    return PutEscapedString(buffer, size, linearStr, quote);
}

JS_PUBLIC_API(JSBool)
JS_FileEscapedString(FILE *fp, JSString *str, char quote)
{
    JSLinearString *linearStr = str->ensureLinear(NULL);
    return linearStr && FileEscapedString(fp, linearStr, quote);
}

JS_PUBLIC_API(JSString *)
JS_NewGrowableString(JSContext *cx, jschar *chars, size_t length)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_NewString(cx, chars, length);
}

JS_PUBLIC_API(JSString *)
JS_NewDependentString(JSContext *cx, JSString *str, size_t start, size_t length)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_NewDependentString(cx, str, start, length);
}

JS_PUBLIC_API(JSString *)
JS_ConcatStrings(JSContext *cx, JSString *left, JSString *right)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    Rooted<JSString*> lstr(cx, left);
    Rooted<JSString*> rstr(cx, right);
    return js_ConcatStrings(cx, lstr, rstr);
}

JS_PUBLIC_API(const jschar *)
JS_UndependString(JSContext *cx, JSString *str)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return str->getCharsZ(cx);
}

JS_PUBLIC_API(JSBool)
JS_MakeStringImmutable(JSContext *cx, JSString *str)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return !!str->ensureFixed(cx);
}

JS_PUBLIC_API(JSBool)
JS_EncodeCharacters(JSContext *cx, const jschar *src, size_t srclen, char *dst, size_t *dstlenp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    size_t n;
    if (!dst) {
        n = GetDeflatedStringLength(cx, src, srclen);
        if (n == (size_t)-1) {
            *dstlenp = 0;
            return JS_FALSE;
        }
        *dstlenp = n;
        return JS_TRUE;
    }

    return DeflateStringToBuffer(cx, src, srclen, dst, dstlenp);
}

JS_PUBLIC_API(JSBool)
JS_DecodeBytes(JSContext *cx, const char *src, size_t srclen, jschar *dst, size_t *dstlenp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return InflateStringToBuffer(cx, src, srclen, dst, dstlenp);
}

JS_PUBLIC_API(JSBool)
JS_DecodeUTF8(JSContext *cx, const char *src, size_t srclen, jschar *dst,
              size_t *dstlenp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return InflateUTF8StringToBuffer(cx, src, srclen, dst, dstlenp);
}

JS_PUBLIC_API(char *)
JS_EncodeString(JSContext *cx, JSString *str)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    const jschar *chars = str->getChars(cx);
    if (!chars)
        return NULL;
    return DeflateString(cx, chars, str->length());
}

JS_PUBLIC_API(size_t)
JS_GetStringEncodingLength(JSContext *cx, JSString *str)
{
    /* jsd calls us with a NULL cx. Ugh. */
    if (cx) {
        AssertHeapIsIdle(cx);
        CHECK_REQUEST(cx);
    }

    const jschar *chars = str->getChars(cx);
    if (!chars)
        return size_t(-1);
    return GetDeflatedStringLength(cx, chars, str->length());
}

JS_PUBLIC_API(size_t)
JS_EncodeStringToBuffer(JSString *str, char *buffer, size_t length)
{
    /*
     * FIXME bug 612141 - fix DeflateStringToBuffer interface so the result
     * would allow to distinguish between insufficient buffer and encoding
     * error.
     */
    size_t writtenLength = length;
    const jschar *chars = str->getChars(NULL);
    if (!chars)
        return size_t(-1);
    if (DeflateStringToBuffer(NULL, chars, str->length(), buffer, &writtenLength)) {
        JS_ASSERT(writtenLength <= length);
        return writtenLength;
    }
    JS_ASSERT(writtenLength <= length);
    size_t necessaryLength = GetDeflatedStringLength(NULL, chars, str->length());
    if (necessaryLength == size_t(-1))
        return size_t(-1);
    if (writtenLength != length) {
        /* Make sure that the buffer contains only valid UTF-8 sequences. */
        JS_ASSERT(js_CStringsAreUTF8);
        PodZero(buffer + writtenLength, length - writtenLength);
    }
    return necessaryLength;
}

JS_PUBLIC_API(JSBool)
JS_Stringify(JSContext *cx, jsval *vp, JSObject *replacerArg, jsval space,
             JSONWriteCallback callback, void *data)
{
    RootedObject replacer(cx, replacerArg);
    RootedValue value(cx, *vp);

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, replacer, space);
    StringBuffer sb(cx);
    if (!js_Stringify(cx, &value, replacer, space, sb))
        return false;
    *vp = value;
    if (sb.empty()) {
        JSAtom *nullAtom = cx->runtime->atomState.nullAtom;
        return callback(nullAtom->chars(), nullAtom->length(), data);
    }
    return callback(sb.begin(), sb.length(), data);
}

JS_PUBLIC_API(JSBool)
JS_ParseJSON(JSContext *cx, const jschar *chars, uint32_t len, jsval *vp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    RootedValue reviver(cx, NullValue()), value(cx);
    if (!ParseJSONWithReviver(cx, chars, len, reviver, &value))
        return false;

    *vp = value;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_ParseJSONWithReviver(JSContext *cx, const jschar *chars, uint32_t len, jsval reviverArg, jsval *vp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    RootedValue reviver(cx, reviverArg), value(cx);
    if (!ParseJSONWithReviver(cx, chars, len, reviver, &value))
        return false;

    *vp = value;
    return true;
}

JS_PUBLIC_API(JSBool)
JS_ReadStructuredClone(JSContext *cx, const uint64_t *buf, size_t nbytes,
                       uint32_t version, jsval *vp,
                       const JSStructuredCloneCallbacks *optionalCallbacks,
                       void *closure)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    if (version > JS_STRUCTURED_CLONE_VERSION) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_BAD_CLONE_VERSION);
        return false;
    }
    const JSStructuredCloneCallbacks *callbacks =
        optionalCallbacks ?
        optionalCallbacks :
        cx->runtime->structuredCloneCallbacks;
    return ReadStructuredClone(cx, buf, nbytes, vp, callbacks, closure);
}

JS_PUBLIC_API(JSBool)
JS_WriteStructuredClone(JSContext *cx, jsval v, uint64_t **bufp, size_t *nbytesp,
                        const JSStructuredCloneCallbacks *optionalCallbacks,
                        void *closure)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v);

    const JSStructuredCloneCallbacks *callbacks =
        optionalCallbacks ?
        optionalCallbacks :
        cx->runtime->structuredCloneCallbacks;
    return WriteStructuredClone(cx, v, (uint64_t **) bufp, nbytesp, callbacks, closure);
}

JS_PUBLIC_API(JSBool)
JS_StructuredClone(JSContext *cx, jsval v, jsval *vp,
                   const JSStructuredCloneCallbacks *optionalCallbacks,
                   void *closure)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v);

    const JSStructuredCloneCallbacks *callbacks =
        optionalCallbacks ?
        optionalCallbacks :
        cx->runtime->structuredCloneCallbacks;
    JSAutoStructuredCloneBuffer buf;
    return buf.write(cx, v, callbacks, closure) &&
           buf.read(cx, vp, callbacks, closure);
}

void
JSAutoStructuredCloneBuffer::clear()
{
    if (data_) {
        Foreground::free_(data_);
        data_ = NULL;
        nbytes_ = 0;
        version_ = 0;
    }
}

void
JSAutoStructuredCloneBuffer::adopt(uint64_t *data, size_t nbytes, uint32_t version)
{
    clear();
    data_ = data;
    nbytes_ = nbytes;
    version_ = version;
}

bool
JSAutoStructuredCloneBuffer::copy(const uint64_t *srcData, size_t nbytes, uint32_t version)
{
    uint64_t *newData = static_cast<uint64_t *>(OffTheBooks::malloc_(nbytes));
    if (!newData)
        return false;

    js_memcpy(newData, srcData, nbytes);

    clear();
    data_ = newData;
    nbytes_ = nbytes;
    version_ = version;
    return true;
}
void
JSAutoStructuredCloneBuffer::steal(uint64_t **datap, size_t *nbytesp, uint32_t *versionp)
{
    *datap = data_;
    *nbytesp = nbytes_;
    if (versionp)
        *versionp = version_;

    data_ = NULL;
    nbytes_ = 0;
    version_ = 0;
}

bool
JSAutoStructuredCloneBuffer::read(JSContext *cx, jsval *vp,
                                  const JSStructuredCloneCallbacks *optionalCallbacks,
                                  void *closure) const
{
    JS_ASSERT(cx);
    JS_ASSERT(data_);
    return !!JS_ReadStructuredClone(cx, data_, nbytes_, version_, vp,
                                    optionalCallbacks, closure);
}

bool
JSAutoStructuredCloneBuffer::write(JSContext *cx, jsval v,
                                   const JSStructuredCloneCallbacks *optionalCallbacks,
                                   void *closure)
{
    clear();
    bool ok = !!JS_WriteStructuredClone(cx, v, &data_, &nbytes_,
                                        optionalCallbacks, closure);
    if (!ok) {
        data_ = NULL;
        nbytes_ = 0;
        version_ = JS_STRUCTURED_CLONE_VERSION;
    }
    return ok;
}

void
JSAutoStructuredCloneBuffer::swap(JSAutoStructuredCloneBuffer &other)
{
    uint64_t *data = other.data_;
    size_t nbytes = other.nbytes_;
    uint32_t version = other.version_;

    other.data_ = this->data_;
    other.nbytes_ = this->nbytes_;
    other.version_ = this->version_;

    this->data_ = data;
    this->nbytes_ = nbytes;
    this->version_ = version;
}

JS_PUBLIC_API(void)
JS_SetStructuredCloneCallbacks(JSRuntime *rt, const JSStructuredCloneCallbacks *callbacks)
{
    rt->structuredCloneCallbacks = callbacks;
}

JS_PUBLIC_API(JSBool)
JS_ReadUint32Pair(JSStructuredCloneReader *r, uint32_t *p1, uint32_t *p2)
{
    return r->input().readPair((uint32_t *) p1, (uint32_t *) p2);
}

JS_PUBLIC_API(JSBool)
JS_ReadBytes(JSStructuredCloneReader *r, void *p, size_t len)
{
    return r->input().readBytes(p, len);
}

JS_PUBLIC_API(JSBool)
JS_WriteUint32Pair(JSStructuredCloneWriter *w, uint32_t tag, uint32_t data)
{
    return w->output().writePair(tag, data);
}

JS_PUBLIC_API(JSBool)
JS_WriteBytes(JSStructuredCloneWriter *w, const void *p, size_t len)
{
    return w->output().writeBytes(p, len);
}

/*
 * The following determines whether C Strings are to be treated as UTF-8
 * or ISO-8859-1.  For correct operation, it must be set prior to the
 * first call to JS_NewRuntime.
 */
#ifndef JS_C_STRINGS_ARE_UTF8
JSBool js_CStringsAreUTF8 = JS_FALSE;
#endif

JS_PUBLIC_API(JSBool)
JS_CStringsAreUTF8()
{
    return js_CStringsAreUTF8;
}

JS_PUBLIC_API(void)
JS_SetCStringsAreUTF8()
{
    JS_ASSERT(!js_NewRuntimeWasCalled);

#ifndef JS_C_STRINGS_ARE_UTF8
    js_CStringsAreUTF8 = JS_TRUE;
#endif
}

/************************************************************************/

JS_PUBLIC_API(void)
JS_ReportError(JSContext *cx, const char *format, ...)
{
    va_list ap;

    AssertHeapIsIdle(cx);
    va_start(ap, format);
    js_ReportErrorVA(cx, JSREPORT_ERROR, format, ap);
    va_end(ap);
}

JS_PUBLIC_API(void)
JS_ReportErrorNumber(JSContext *cx, JSErrorCallback errorCallback,
                     void *userRef, const unsigned errorNumber, ...)
{
    va_list ap;
    va_start(ap, errorNumber);
    JS_ReportErrorNumberVA(cx, errorCallback, userRef, errorNumber, ap);
    va_end(ap);
}

JS_PUBLIC_API(void)
JS_ReportErrorNumberVA(JSContext *cx, JSErrorCallback errorCallback,
                       void *userRef, const unsigned errorNumber,
                       va_list ap)
{
    AssertHeapIsIdle(cx);
    js_ReportErrorNumberVA(cx, JSREPORT_ERROR, errorCallback, userRef,
                           errorNumber, JS_TRUE, ap);
}

JS_PUBLIC_API(void)
JS_ReportErrorNumberUC(JSContext *cx, JSErrorCallback errorCallback,
                     void *userRef, const unsigned errorNumber, ...)
{
    va_list ap;

    AssertHeapIsIdle(cx);
    va_start(ap, errorNumber);
    js_ReportErrorNumberVA(cx, JSREPORT_ERROR, errorCallback, userRef,
                           errorNumber, JS_FALSE, ap);
    va_end(ap);
}

JS_PUBLIC_API(JSBool)
JS_ReportWarning(JSContext *cx, const char *format, ...)
{
    va_list ap;
    JSBool ok;

    AssertHeapIsIdle(cx);
    va_start(ap, format);
    ok = js_ReportErrorVA(cx, JSREPORT_WARNING, format, ap);
    va_end(ap);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_ReportErrorFlagsAndNumber(JSContext *cx, unsigned flags,
                             JSErrorCallback errorCallback, void *userRef,
                             const unsigned errorNumber, ...)
{
    va_list ap;
    JSBool ok;

    AssertHeapIsIdle(cx);
    va_start(ap, errorNumber);
    ok = js_ReportErrorNumberVA(cx, flags, errorCallback, userRef,
                                errorNumber, JS_TRUE, ap);
    va_end(ap);
    return ok;
}

JS_PUBLIC_API(JSBool)
JS_ReportErrorFlagsAndNumberUC(JSContext *cx, unsigned flags,
                               JSErrorCallback errorCallback, void *userRef,
                               const unsigned errorNumber, ...)
{
    va_list ap;
    JSBool ok;

    AssertHeapIsIdle(cx);
    va_start(ap, errorNumber);
    ok = js_ReportErrorNumberVA(cx, flags, errorCallback, userRef,
                                errorNumber, JS_FALSE, ap);
    va_end(ap);
    return ok;
}

JS_PUBLIC_API(void)
JS_ReportOutOfMemory(JSContext *cx)
{
    js_ReportOutOfMemory(cx);
}

JS_PUBLIC_API(void)
JS_ReportAllocationOverflow(JSContext *cx)
{
    js_ReportAllocationOverflow(cx);
}

JS_PUBLIC_API(JSErrorReporter)
JS_GetErrorReporter(JSContext *cx)
{
    return cx->errorReporter;
}

JS_PUBLIC_API(JSErrorReporter)
JS_SetErrorReporter(JSContext *cx, JSErrorReporter er)
{
    JSErrorReporter older;

    older = cx->errorReporter;
    cx->errorReporter = er;
    return older;
}

/************************************************************************/

/*
 * Dates.
 */
JS_PUBLIC_API(JSObject *)
JS_NewDateObject(JSContext *cx, int year, int mon, int mday, int hour, int min, int sec)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_NewDateObject(cx, year, mon, mday, hour, min, sec);
}

JS_PUBLIC_API(JSObject *)
JS_NewDateObjectMsec(JSContext *cx, double msec)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return js_NewDateObjectMsec(cx, msec);
}

JS_PUBLIC_API(JSBool)
JS_ObjectIsDate(JSContext *cx, JSRawObject obj)
{
    AssertHeapIsIdle(cx);
    JS_ASSERT(obj);
    return obj->isDate();
}

JS_PUBLIC_API(void)
JS_ClearDateCaches(JSContext *cx)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    js_ClearDateCaches();
}

/************************************************************************/

/*
 * Regular Expressions.
 */
JS_PUBLIC_API(JSObject *)
JS_NewRegExpObject(JSContext *cx, JSObject *objArg, char *bytes, size_t length, unsigned flags)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    jschar *chars = InflateString(cx, bytes, &length);
    if (!chars)
        return NULL;

    RegExpStatics *res = obj->asGlobal().getRegExpStatics();
    RegExpObject *reobj = RegExpObject::create(cx, res, chars, length, RegExpFlag(flags), NULL);
    cx->free_(chars);
    return reobj;
}

JS_PUBLIC_API(JSObject *)
JS_NewUCRegExpObject(JSContext *cx, JSObject *objArg, jschar *chars, size_t length, unsigned flags)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    RegExpStatics *res = obj->asGlobal().getRegExpStatics();
    return RegExpObject::create(cx, res, chars, length, RegExpFlag(flags), NULL);
}

JS_PUBLIC_API(void)
JS_SetRegExpInput(JSContext *cx, JSObject *objArg, JSString *input, JSBool multiline)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, input);

    obj->asGlobal().getRegExpStatics()->reset(cx, input, !!multiline);
}

JS_PUBLIC_API(void)
JS_ClearRegExpStatics(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    JS_ASSERT(obj);

    obj->asGlobal().getRegExpStatics()->clear();
}

JS_PUBLIC_API(JSBool)
JS_ExecuteRegExp(JSContext *cx, JSObject *objArg, JSObject *reobjArg, jschar *chars, size_t length,
                 size_t *indexp, JSBool test, jsval *rval)
{
    RootedObject obj(cx, objArg);
    RootedObject reobj(cx, reobjArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    RegExpStatics *res = obj->asGlobal().getRegExpStatics();
    return ExecuteRegExp(cx, res, reobj->asRegExp(), NULL, chars, length,
                         indexp, test ? RegExpTest : RegExpExec, rval);
}

JS_PUBLIC_API(JSObject *)
JS_NewRegExpObjectNoStatics(JSContext *cx, char *bytes, size_t length, unsigned flags)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    jschar *chars = InflateString(cx, bytes, &length);
    if (!chars)
        return NULL;
    RegExpObject *reobj = RegExpObject::createNoStatics(cx, chars, length, RegExpFlag(flags), NULL);
    cx->free_(chars);
    return reobj;
}

JS_PUBLIC_API(JSObject *)
JS_NewUCRegExpObjectNoStatics(JSContext *cx, jschar *chars, size_t length, unsigned flags)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    return RegExpObject::createNoStatics(cx, chars, length, RegExpFlag(flags), NULL);
}

JS_PUBLIC_API(JSBool)
JS_ExecuteRegExpNoStatics(JSContext *cx, JSObject *objArg, jschar *chars, size_t length,
                          size_t *indexp, JSBool test, jsval *rval)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    return ExecuteRegExp(cx, NULL, obj->asRegExp(), NULL, chars, length, indexp,
                         test ? RegExpTest : RegExpExec, rval);
}

JS_PUBLIC_API(JSBool)
JS_ObjectIsRegExp(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    JS_ASSERT(obj);
    return obj->isRegExp();
}

JS_PUBLIC_API(unsigned)
JS_GetRegExpFlags(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    return obj->asRegExp().getFlags();
}

JS_PUBLIC_API(JSString *)
JS_GetRegExpSource(JSContext *cx, JSObject *objArg)
{
    RootedObject obj(cx, objArg);
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    return obj->asRegExp().getSource();
}

/************************************************************************/

JS_PUBLIC_API(void)
JS_SetLocaleCallbacks(JSContext *cx, JSLocaleCallbacks *callbacks)
{
    AssertHeapIsIdle(cx);
    cx->localeCallbacks = callbacks;
}

JS_PUBLIC_API(JSLocaleCallbacks *)
JS_GetLocaleCallbacks(JSContext *cx)
{
    /* This function can be called by a finalizer. */
    return cx->localeCallbacks;
}

/************************************************************************/

JS_PUBLIC_API(JSBool)
JS_IsExceptionPending(JSContext *cx)
{
    /* This function can be called by a finalizer. */
    return (JSBool) cx->isExceptionPending();
}

JS_PUBLIC_API(JSBool)
JS_GetPendingException(JSContext *cx, jsval *vp)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    if (!cx->isExceptionPending())
        return JS_FALSE;
    *vp = cx->getPendingException();
    assertSameCompartment(cx, *vp);
    return JS_TRUE;
}

JS_PUBLIC_API(void)
JS_SetPendingException(JSContext *cx, jsval v)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v);
    cx->setPendingException(v);
}

JS_PUBLIC_API(void)
JS_ClearPendingException(JSContext *cx)
{
    AssertHeapIsIdle(cx);
    cx->clearPendingException();
}

JS_PUBLIC_API(JSBool)
JS_ReportPendingException(JSContext *cx)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);

    return js_ReportUncaughtException(cx);
}

struct JSExceptionState {
    JSBool throwing;
    jsval  exception;
};

JS_PUBLIC_API(JSExceptionState *)
JS_SaveExceptionState(JSContext *cx)
{
    JSExceptionState *state;

    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    state = (JSExceptionState *) cx->malloc_(sizeof(JSExceptionState));
    if (state) {
        state->throwing = JS_GetPendingException(cx, &state->exception);
        if (state->throwing && JSVAL_IS_GCTHING(state->exception))
            js_AddRoot(cx, &state->exception, "JSExceptionState.exception");
    }
    return state;
}

JS_PUBLIC_API(void)
JS_RestoreExceptionState(JSContext *cx, JSExceptionState *state)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    if (state) {
        if (state->throwing)
            JS_SetPendingException(cx, state->exception);
        else
            JS_ClearPendingException(cx);
        JS_DropExceptionState(cx, state);
    }
}

JS_PUBLIC_API(void)
JS_DropExceptionState(JSContext *cx, JSExceptionState *state)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    if (state) {
        if (state->throwing && JSVAL_IS_GCTHING(state->exception)) {
            assertSameCompartment(cx, state->exception);
            JS_RemoveValueRoot(cx, &state->exception);
        }
        cx->free_(state);
    }
}

JS_PUBLIC_API(JSErrorReport *)
JS_ErrorFromException(JSContext *cx, jsval v)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v);
    return js_ErrorFromException(cx, v);
}

JS_PUBLIC_API(JSBool)
JS_ThrowReportedError(JSContext *cx, const char *message,
                      JSErrorReport *reportp)
{
    AssertHeapIsIdle(cx);
    return JS_IsRunning(cx) &&
           js_ErrorToException(cx, message, reportp, NULL, NULL);
}

JS_PUBLIC_API(JSBool)
JS_ThrowStopIteration(JSContext *cx)
{
    AssertHeapIsIdle(cx);
    return js_ThrowStopIteration(cx);
}

JS_PUBLIC_API(intptr_t)
JS_GetCurrentThread()
{
#ifdef JS_THREADSAFE
    return reinterpret_cast<intptr_t>(PR_GetCurrentThread());
#else
    return 0;
#endif
}

extern JS_PUBLIC_API(void)
JS_ClearRuntimeThread(JSRuntime *rt)
{
    AssertHeapIsIdle(rt);
#ifdef JS_THREADSAFE
    rt->clearOwnerThread();
#endif
}

extern JS_PUBLIC_API(void)
JS_SetRuntimeThread(JSRuntime *rt)
{
    AssertHeapIsIdle(rt);
#ifdef JS_THREADSAFE
    rt->setOwnerThread();
#endif
}

extern JS_NEVER_INLINE JS_PUBLIC_API(void)
JS_AbortIfWrongThread(JSRuntime *rt)
{
#ifdef JS_THREADSAFE
    if (!rt->onOwnerThread())
        MOZ_CRASH();
#endif
}

#ifdef JS_GC_ZEAL
JS_PUBLIC_API(void)
JS_SetGCZeal(JSContext *cx, uint8_t zeal, uint32_t frequency)
{
    const char *env = getenv("JS_GC_ZEAL");
    if (env) {
        if (0 == strcmp(env, "help")) {
            printf("Format: JS_GC_ZEAL=N[,F]\n"
                   "N indicates \"zealousness\":\n"
                   "  0: no additional GCs\n"
                   "  1: additional GCs at common danger points\n"
                   "  2: GC every F allocations (default: 100)\n"
                   "  3: GC when the window paints (browser only)\n"
                   "  4: Verify pre write barriers between instructions\n"
                   "  5: Verify pre write barriers between paints\n"
                   "  6: Verify stack rooting (ignoring XML and Reflect)\n"
                   "  7: Verify stack rooting (all roots)\n"
                   "  8: Incremental GC in two slices: 1) mark roots 2) finish collection\n"
                   "  9: Incremental GC in two slices: 1) mark all 2) new marking and finish\n"
                   " 10: Incremental GC in multiple slices\n"
                   " 11: Verify post write barriers between instructions\n"
                   " 12: Verify post write barriers between paints\n"
                   " 13: Purge analysis state every F allocations (default: 100)\n");
        }
        const char *p = strchr(env, ',');
        zeal = atoi(env);
        frequency = p ? atoi(p + 1) : JS_DEFAULT_ZEAL_FREQ;
    }

    JSRuntime *rt = cx->runtime;

    if (zeal == 0) {
        if (rt->gcVerifyPreData)
            VerifyBarriers(rt, PreBarrierVerifier);
        if (rt->gcVerifyPostData)
            VerifyBarriers(rt, PostBarrierVerifier);
    }

#ifdef JS_METHODJIT
    /* In case JSCompartment::compileBarriers() changed... */
    for (CompartmentsIter c(rt); !c.done(); c.next())
        mjit::ClearAllFrames(c);
#endif

    bool schedule = zeal >= js::gc::ZealAllocValue;
    rt->gcZeal_ = zeal;
    rt->gcZealFrequency = frequency;
    rt->gcNextScheduled = schedule ? frequency : 0;
}

JS_PUBLIC_API(void)
JS_ScheduleGC(JSContext *cx, uint32_t count)
{
    cx->runtime->gcNextScheduled = count;
}
#endif

/************************************************************************/

#if !defined(STATIC_EXPORTABLE_JS_API) && !defined(STATIC_JS_API) && defined(XP_WIN)

#include "jswin.h"

/*
 * Initialization routine for the JS DLL.
 */
BOOL WINAPI DllMain (HINSTANCE hDLL, DWORD dwReason, LPVOID lpReserved)
{
    return TRUE;
}

#endif

JS_PUBLIC_API(JSBool)
JS_IndexToId(JSContext *cx, uint32_t index, jsid *id)
{
    return IndexToId(cx, index, id);
}

JS_PUBLIC_API(JSBool)
JS_IsIdentifier(JSContext *cx, JSString *str, JSBool *isIdentifier)
{
    assertSameCompartment(cx, str);

    JSLinearString* linearStr = str->ensureLinear(cx);
    if (!linearStr)
        return false;

    *isIdentifier = js::frontend::IsIdentifier(linearStr);
    return true;
}

JS_PUBLIC_API(JSBool)
JS_DescribeScriptedCaller(JSContext *cx, JSScript **script, unsigned *lineno)
{
    if (script)
        *script = NULL;
    if (lineno)
        *lineno = 0;

    ScriptFrameIter i(cx);
    if (i.done())
        return JS_FALSE;

    if (script)
        *script = i.script();
    if (lineno)
        *lineno = js::PCToLineNumber(i.script(), i.pc());
    return JS_TRUE;
}

#ifdef JS_THREADSAFE
static PRStatus
CallOnce(void *func)
{
    JSInitCallback init = JS_DATA_TO_FUNC_PTR(JSInitCallback, func);
    return init() ? PR_SUCCESS : PR_FAILURE;
}
#endif

JS_PUBLIC_API(JSBool)
JS_CallOnce(JSCallOnceType *once, JSInitCallback func)
{
#ifdef JS_THREADSAFE
    return PR_CallOnceWithArg(once, CallOnce, JS_FUNC_TO_DATA_PTR(void *, func)) == PR_SUCCESS;
#else
    if (!*once) {
        *once = true;
        return func();
    } else {
        return JS_TRUE;
    }
#endif
}

namespace JS {

AutoGCRooter::AutoGCRooter(JSContext *cx, ptrdiff_t tag)
  : down(cx->runtime->autoGCRooters), tag(tag), stackTop(&cx->runtime->autoGCRooters)
{
    JS_ASSERT(this != *stackTop);
    *stackTop = this;
}

AutoEnumStateRooter::~AutoEnumStateRooter()
{
    if (!stateValue.isNull())
        MOZ_ALWAYS_TRUE(JSObject::enumerate(context, obj, JSENUMERATE_DESTROY, &stateValue, 0));
}

#ifdef DEBUG
JS_PUBLIC_API(void)
AssertArgumentsAreSane(JSContext *cx, const JS::Value &v)
{
    AssertHeapIsIdle(cx);
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, v);
}
#endif /* DEBUG */

} // namespace JS

JS_PUBLIC_API(void *)
JS_EncodeScript(JSContext *cx, JSScript *script, uint32_t *lengthp)
{
    XDREncoder encoder(cx);
    if (!encoder.codeScript(&script))
        return NULL;
    return encoder.forgetData(lengthp);
}

JS_PUBLIC_API(void *)
JS_EncodeInterpretedFunction(JSContext *cx, JSRawObject funobjArg, uint32_t *lengthp)
{
    XDREncoder encoder(cx);
    RootedObject funobj(cx, funobjArg);
    if (!encoder.codeFunction(&funobj))
        return NULL;
    return encoder.forgetData(lengthp);
}

JS_PUBLIC_API(JSScript *)
JS_DecodeScript(JSContext *cx, const void *data, uint32_t length,
                JSPrincipals *principals, JSPrincipals *originPrincipals)
{
    XDRDecoder decoder(cx, data, length, principals, originPrincipals);
    JSScript *script;
    if (!decoder.codeScript(&script))
        return NULL;
    return script;
}

JS_PUBLIC_API(JSObject *)
JS_DecodeInterpretedFunction(JSContext *cx, const void *data, uint32_t length,
                             JSPrincipals *principals, JSPrincipals *originPrincipals)
{
    XDRDecoder decoder(cx, data, length, principals, originPrincipals);
    RootedObject funobj(cx);
    if (!decoder.codeFunction(&funobj))
        return NULL;
    return funobj;
}

JS_PUBLIC_API(JSObject *)
JS_GetScriptedGlobal(JSContext *cx)
{
    ScriptFrameIter i(cx);
    if (i.done())
        return cx->global();
    return &i.fp()->global();
}

