/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS math package.
 */

#include "mozilla/Constants.h"
#include "mozilla/FloatingPoint.h"

#include <stdlib.h>
#include "jstypes.h"
#include "prmjtime.h"
#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsversion.h"
#include "jslock.h"
#include "jsmath.h"
#include "jsnum.h"
#include "jslibmath.h"
#include "jscompartment.h"

#include "jsinferinlines.h"
#include "jsobjinlines.h"

using namespace js;

#ifndef M_E
#define M_E             2.7182818284590452354
#endif
#ifndef M_LOG2E
#define M_LOG2E         1.4426950408889634074
#endif
#ifndef M_LOG10E
#define M_LOG10E        0.43429448190325182765
#endif
#ifndef M_LN2
#define M_LN2           0.69314718055994530942
#endif
#ifndef M_LN10
#define M_LN10          2.30258509299404568402
#endif
#ifndef M_SQRT2
#define M_SQRT2         1.41421356237309504880
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2       0.70710678118654752440
#endif

static JSConstDoubleSpec math_constants[] = {
    {M_E,       "E",            0, {0,0,0}},
    {M_LOG2E,   "LOG2E",        0, {0,0,0}},
    {M_LOG10E,  "LOG10E",       0, {0,0,0}},
    {M_LN2,     "LN2",          0, {0,0,0}},
    {M_LN10,    "LN10",         0, {0,0,0}},
    {M_PI,      "PI",           0, {0,0,0}},
    {M_SQRT2,   "SQRT2",        0, {0,0,0}},
    {M_SQRT1_2, "SQRT1_2",      0, {0,0,0}},
    {0,0,0,{0,0,0}}
};

MathCache::MathCache() {
    memset(table, 0, sizeof(table));

    /* See comments in lookup(). */
    JS_ASSERT(MOZ_DOUBLE_IS_NEGATIVE_ZERO(-0.0));
    JS_ASSERT(!MOZ_DOUBLE_IS_NEGATIVE_ZERO(+0.0));
    JS_ASSERT(hash(-0.0) != hash(+0.0));
}

size_t
MathCache::sizeOfIncludingThis(JSMallocSizeOfFun mallocSizeOf)
{
    return mallocSizeOf(this);
}

Class js::MathClass = {
    js_Math_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_Math),
    JS_PropertyStub,         /* addProperty */
    JS_PropertyStub,         /* delProperty */
    JS_PropertyStub,         /* getProperty */
    JS_StrictPropertyStub,   /* setProperty */
    JS_EnumerateStub,
    JS_ResolveStub,
    JS_ConvertStub
};

JSBool
js_math_abs(JSContext *cx, unsigned argc, Value *vp)
{
    double x, z;

    if (argc == 0) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    if (!ToNumber(cx, vp[2], &x))
        return JS_FALSE;
    z = fabs(x);
    vp->setNumber(z);
    return JS_TRUE;
}

static JSBool
math_acos(JSContext *cx, unsigned argc, Value *vp)
{
    double x, z;

    if (argc == 0) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    if (!ToNumber(cx, vp[2], &x))
        return JS_FALSE;
#if defined(SOLARIS) && defined(__GNUC__)
    if (x < -1 || 1 < x) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
#endif
    MathCache *mathCache = cx->runtime->getMathCache(cx);
    if (!mathCache)
        return JS_FALSE;
    z = mathCache->lookup(acos, x);
    vp->setDouble(z);
    return JS_TRUE;
}

static JSBool
math_asin(JSContext *cx, unsigned argc, Value *vp)
{
    double x, z;

    if (argc == 0) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    if (!ToNumber(cx, vp[2], &x))
        return JS_FALSE;
#if defined(SOLARIS) && defined(__GNUC__)
    if (x < -1 || 1 < x) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
#endif
    MathCache *mathCache = cx->runtime->getMathCache(cx);
    if (!mathCache)
        return JS_FALSE;
    z = mathCache->lookup(asin, x);
    vp->setDouble(z);
    return JS_TRUE;
}

static JSBool
math_atan(JSContext *cx, unsigned argc, Value *vp)
{
    double x, z;

    if (argc == 0) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    if (!ToNumber(cx, vp[2], &x))
        return JS_FALSE;
    MathCache *mathCache = cx->runtime->getMathCache(cx);
    if (!mathCache)
        return JS_FALSE;
    z = mathCache->lookup(atan, x);
    vp->setDouble(z);
    return JS_TRUE;
}

static inline double JS_FASTCALL
math_atan2_kernel(double x, double y)
{
#if defined(_MSC_VER)
    /*
     * MSVC's atan2 does not yield the result demanded by ECMA when both x
     * and y are infinite.
     * - The result is a multiple of pi/4.
     * - The sign of x determines the sign of the result.
     * - The sign of y determines the multiplicator, 1 or 3.
     */
    if (MOZ_DOUBLE_IS_INFINITE(x) && MOZ_DOUBLE_IS_INFINITE(y)) {
        double z = js_copysign(M_PI / 4, x);
        if (y < 0)
            z *= 3;
        return z;
    }
#endif

#if defined(SOLARIS) && defined(__GNUC__)
    if (x == 0) {
        if (MOZ_DOUBLE_IS_NEGZERO(y))
            return js_copysign(M_PI, x);
        if (y == 0)
            return x;
    }
#endif
    return atan2(x, y);
}

static JSBool
math_atan2(JSContext *cx, unsigned argc, Value *vp)
{
    double x, y, z;

    if (argc <= 1) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    if (!ToNumber(cx, vp[2], &x) || !ToNumber(cx, vp[3], &y))
        return JS_FALSE;
    z = math_atan2_kernel(x, y);
    vp->setDouble(z);
    return JS_TRUE;
}

double
js_math_ceil_impl(double x)
{
#ifdef __APPLE__
    if (x < 0 && x > -1.0)
        return js_copysign(0, -1);
#endif
    return ceil(x);
}

JSBool
js_math_ceil(JSContext *cx, unsigned argc, Value *vp)
{
    double x, z;

    if (argc == 0) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    if (!ToNumber(cx, vp[2], &x))
        return JS_FALSE;
    z = js_math_ceil_impl(x);
    vp->setNumber(z);
    return JS_TRUE;
}

static JSBool
math_cos(JSContext *cx, unsigned argc, Value *vp)
{
    double x, z;

    if (argc == 0) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    if (!ToNumber(cx, vp[2], &x))
        return JS_FALSE;
    MathCache *mathCache = cx->runtime->getMathCache(cx);
    if (!mathCache)
        return JS_FALSE;
    z = mathCache->lookup(cos, x);
    vp->setDouble(z);
    return JS_TRUE;
}

static double
math_exp_body(double d)
{
#ifdef _WIN32
    if (!MOZ_DOUBLE_IS_NaN(d)) {
        if (d == js_PositiveInfinity)
            return js_PositiveInfinity;
        if (d == js_NegativeInfinity)
            return 0.0;
    }
#endif
    return exp(d);
}

static JSBool
math_exp(JSContext *cx, unsigned argc, Value *vp)
{
    double x, z;

    if (argc == 0) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    if (!ToNumber(cx, vp[2], &x))
        return JS_FALSE;
    MathCache *mathCache = cx->runtime->getMathCache(cx);
    if (!mathCache)
        return JS_FALSE;
    z = mathCache->lookup(math_exp_body, x);
    vp->setNumber(z);
    return JS_TRUE;
}

double
js_math_floor_impl(double x)
{
    return floor(x);
}

JSBool
js_math_floor(JSContext *cx, unsigned argc, Value *vp)
{
    double x, z;

    if (argc == 0) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    if (!ToNumber(cx, vp[2], &x))
        return JS_FALSE;
    z = js_math_floor_impl(x);
    vp->setNumber(z);
    return JS_TRUE;
}

static JSBool
math_log(JSContext *cx, unsigned argc, Value *vp)
{
    double x, z;

    if (argc == 0) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    if (!ToNumber(cx, vp[2], &x))
        return JS_FALSE;
#if defined(SOLARIS) && defined(__GNUC__)
    if (x < 0) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
#endif
    MathCache *mathCache = cx->runtime->getMathCache(cx);
    if (!mathCache)
        return JS_FALSE;
    z = mathCache->lookup(log, x);
    vp->setNumber(z);
    return JS_TRUE;
}

JSBool
js_math_max(JSContext *cx, unsigned argc, Value *vp)
{
    double x, z = js_NegativeInfinity;
    Value *argv;
    unsigned i;

    if (argc == 0) {
        vp->setDouble(js_NegativeInfinity);
        return JS_TRUE;
    }
    argv = vp + 2;
    for (i = 0; i < argc; i++) {
        if (!ToNumber(cx, argv[i], &x))
            return JS_FALSE;
        if (MOZ_DOUBLE_IS_NaN(x)) {
            vp->setDouble(js_NaN);
            return JS_TRUE;
        }
        if (x == 0 && x == z) {
            if (js_copysign(1.0, z) == -1)
                z = x;
        } else {
            z = (x > z) ? x : z;
        }
    }
    vp->setNumber(z);
    return JS_TRUE;
}

JSBool
js_math_min(JSContext *cx, unsigned argc, Value *vp)
{
    double x, z = js_PositiveInfinity;
    Value *argv;
    unsigned i;

    if (argc == 0) {
        vp->setDouble(js_PositiveInfinity);
        return JS_TRUE;
    }
    argv = vp + 2;
    for (i = 0; i < argc; i++) {
        if (!ToNumber(cx, argv[i], &x))
            return JS_FALSE;
        if (MOZ_DOUBLE_IS_NaN(x)) {
            vp->setDouble(js_NaN);
            return JS_TRUE;
        }
        if (x == 0 && x == z) {
            if (js_copysign(1.0, x) == -1)
                z = x;
        } else {
            z = (x < z) ? x : z;
        }
    }
    vp->setNumber(z);
    return JS_TRUE;
}

static double
powi(double x, int y)
{
    unsigned n = (y < 0) ? -y : y;
    double m = x;
    double p = 1;
    while (true) {
        if ((n & 1) != 0) p *= m;
        n >>= 1;
        if (n == 0) {
            if (y < 0) {
                // Unfortunately, we have to be careful when p has reached
                // infinity in the computation, because sometimes the higher
                // internal precision in the pow() implementation would have
                // given us a finite p. This happens very rarely.

                double result = 1.0 / p;
                return (result == 0 && MOZ_DOUBLE_IS_INFINITE(p))
                       ? pow(x, static_cast<double>(y))  // Avoid pow(double, int).
                       : result;
            }

            return p;
        }
        m *= m;
    }
}

JSBool
js_math_pow(JSContext *cx, unsigned argc, Value *vp)
{
    double x, y, z;

    if (argc <= 1) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    if (!ToNumber(cx, vp[2], &x) || !ToNumber(cx, vp[3], &y))
        return JS_FALSE;
    /*
     * Special case for square roots. Note that pow(x, 0.5) != sqrt(x)
     * when x = -0.0, so we have to guard for this.
     */
    if (MOZ_DOUBLE_IS_FINITE(x) && x != 0.0) {
        if (y == 0.5) {
            vp->setNumber(sqrt(x));
            return JS_TRUE;
        }
        if (y == -0.5) {
            vp->setNumber(1.0/sqrt(x));
            return JS_TRUE;
        }
    }
    /*
     * Because C99 and ECMA specify different behavior for pow(),
     * we need to wrap the libm call to make it ECMA compliant.
     */
    if (!MOZ_DOUBLE_IS_FINITE(y) && (x == 1.0 || x == -1.0)) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    /* pow(x, +-0) is always 1, even for x = NaN. */
    if (y == 0) {
        vp->setInt32(1);
        return JS_TRUE;
    }

    /*
     * Use powi if the exponent is an integer or an integer-valued double.
     * We don't have to check for NaN since a comparison with NaN is always
     * false.
     */
    if (int32_t(y) == y)
        z = powi(x, int32_t(y));
    else
        z = pow(x, y);

    vp->setNumber(z);
    return JS_TRUE;
}

static const int64_t RNG_MULTIPLIER = 0x5DEECE66DLL;
static const int64_t RNG_ADDEND = 0xBLL;
static const int64_t RNG_MASK = (1LL << 48) - 1;
static const double RNG_DSCALE = double(1LL << 53);

/*
 * Math.random() support, lifted from java.util.Random.java.
 */
extern void
random_setSeed(int64_t *rngSeed, int64_t seed)
{
    *rngSeed = (seed ^ RNG_MULTIPLIER) & RNG_MASK;
}

void
js_InitRandom(JSContext *cx)
{
    /*
     * Set the seed from current time. Since we have a RNG per context and we often bring
     * up several contexts at the same time, we xor in some additional values, namely
     * the context and its successor. We don't just use the context because it might be
     * possible to reverse engineer the context pointer if one guesses the time right.
     */
    random_setSeed(&cx->rngSeed, (PRMJ_Now() / 1000) ^ int64_t(cx) ^ int64_t(cx->link.next));
}

extern uint64_t
random_next(int64_t *rngSeed, int bits)
{
    uint64_t nextseed = *rngSeed * RNG_MULTIPLIER;
    nextseed += RNG_ADDEND;
    nextseed &= RNG_MASK;
    *rngSeed = nextseed;
    return nextseed >> (48 - bits);
}

static inline double
random_nextDouble(JSContext *cx)
{
    return double((random_next(&cx->rngSeed, 26) << 27) + random_next(&cx->rngSeed, 27)) /
           RNG_DSCALE;
}

static JSBool
math_random(JSContext *cx, unsigned argc, Value *vp)
{
    double z = random_nextDouble(cx);
    vp->setDouble(z);
    return JS_TRUE;
}

JSBool /* ES5 15.8.2.15. */
js_math_round(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() == 0) {
        args.rval().setDouble(js_NaN);
        return true;
    }

    double x;
    if (!ToNumber(cx, args[0], &x))
        return false;

    int32_t i;
    if (MOZ_DOUBLE_IS_INT32(x, &i)) {
        args.rval().setInt32(i);
        return true;
    }

    /* Some numbers are so big that adding 0.5 would give the wrong number */
    if (MOZ_DOUBLE_EXPONENT(x) >= 52) {
        args.rval().setNumber(x);
        return true;
    }

    args.rval().setNumber(js_copysign(floor(x + 0.5), x));
    return true;
}

static JSBool
math_sin(JSContext *cx, unsigned argc, Value *vp)
{
    double x, z;

    if (argc == 0) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    if (!ToNumber(cx, vp[2], &x))
        return JS_FALSE;
    MathCache *mathCache = cx->runtime->getMathCache(cx);
    if (!mathCache)
        return JS_FALSE;
    z = mathCache->lookup(sin, x);
    vp->setDouble(z);
    return JS_TRUE;
}

JSBool
js_math_sqrt(JSContext *cx, unsigned argc, Value *vp)
{
    double x, z;

    if (argc == 0) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    if (!ToNumber(cx, vp[2], &x))
        return JS_FALSE;
    MathCache *mathCache = cx->runtime->getMathCache(cx);
    if (!mathCache)
        return JS_FALSE;
    z = mathCache->lookup(sqrt, x);
    vp->setDouble(z);
    return JS_TRUE;
}

static JSBool
math_tan(JSContext *cx, unsigned argc, Value *vp)
{
    double x, z;

    if (argc == 0) {
        vp->setDouble(js_NaN);
        return JS_TRUE;
    }
    if (!ToNumber(cx, vp[2], &x))
        return JS_FALSE;
    MathCache *mathCache = cx->runtime->getMathCache(cx);
    if (!mathCache)
        return JS_FALSE;
    z = mathCache->lookup(tan, x);
    vp->setDouble(z);
    return JS_TRUE;
}

#if JS_HAS_TOSOURCE
static JSBool
math_toSource(JSContext *cx, unsigned argc, Value *vp)
{
    vp->setString(CLASS_NAME(cx, Math));
    return JS_TRUE;
}
#endif

static JSFunctionSpec math_static_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,  math_toSource,        0, 0),
#endif
    JS_FN("abs",            js_math_abs,          1, 0),
    JS_FN("acos",           math_acos,            1, 0),
    JS_FN("asin",           math_asin,            1, 0),
    JS_FN("atan",           math_atan,            1, 0),
    JS_FN("atan2",          math_atan2,           2, 0),
    JS_FN("ceil",           js_math_ceil,         1, 0),
    JS_FN("cos",            math_cos,             1, 0),
    JS_FN("exp",            math_exp,             1, 0),
    JS_FN("floor",          js_math_floor,        1, 0),
    JS_FN("log",            math_log,             1, 0),
    JS_FN("max",            js_math_max,          2, 0),
    JS_FN("min",            js_math_min,          2, 0),
    JS_FN("pow",            js_math_pow,          2, 0),
    JS_FN("random",         math_random,          0, 0),
    JS_FN("round",          js_math_round,        1, 0),
    JS_FN("sin",            math_sin,             1, 0),
    JS_FN("sqrt",           js_math_sqrt,         1, 0),
    JS_FN("tan",            math_tan,             1, 0),
    JS_FS_END
};

JSObject *
js_InitMathClass(JSContext *cx, JSObject *obj_)
{
    RootedObject obj(cx, obj_);
    RootedObject Math(cx, NewObjectWithClassProto(cx, &MathClass, NULL, obj));
    if (!Math || !JSObject::setSingletonType(cx, Math))
        return NULL;

    if (!JS_DefineProperty(cx, obj, js_Math_str, OBJECT_TO_JSVAL(Math),
                           JS_PropertyStub, JS_StrictPropertyStub, 0)) {
        return NULL;
    }

    if (!JS_DefineFunctions(cx, Math, math_static_methods))
        return NULL;
    if (!JS_DefineConstDoubles(cx, Math, math_constants))
        return NULL;

    MarkStandardClassInitializedNoProto(obj, &MathClass);

    return Math;
}
