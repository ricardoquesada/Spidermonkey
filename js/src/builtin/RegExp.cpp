/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jscntxt.h"

#include "vm/StringBuffer.h"

#include "builtin/RegExp.h"

#include "vm/RegExpObject-inl.h"
#include "vm/RegExpStatics-inl.h"

using namespace js;
using namespace js::types;

class RegExpMatchBuilder
{
    JSContext   * const cx;
    RootedObject array;

    bool setProperty(Handle<PropertyName*> name, HandleValue v) {
        return !!baseops::DefineProperty(cx, array, name, v,
                                         JS_PropertyStub, JS_StrictPropertyStub, JSPROP_ENUMERATE);
    }

  public:
    RegExpMatchBuilder(JSContext *cx, JSObject *array) : cx(cx), array(cx, array) {}

    bool append(uint32_t index, HandleValue v) {
        JS_ASSERT(!array->getOps()->getElement);
        return !!baseops::DefineElement(cx, array, index, v, JS_PropertyStub, JS_StrictPropertyStub,
                                        JSPROP_ENUMERATE);
    }

    bool setIndex(int index) {
        Rooted<PropertyName*> name(cx, cx->runtime->atomState.indexAtom);
        RootedValue value(cx, Int32Value(index));
        return setProperty(name, value);
    }

    bool setInput(JSString *str) {
        JS_ASSERT(str);
        Rooted<PropertyName*> name(cx, cx->runtime->atomState.inputAtom);
        RootedValue value(cx, StringValue(str));
        return setProperty(name, value);
    }
};

static bool
CreateRegExpMatchResult(JSContext *cx, JSString *input_, const jschar *chars, size_t length,
                        MatchPairs *matchPairs, Value *rval)
{
    RootedString input(cx, input_);

    /*
     * Create the (slow) result array for a match.
     *
     * Array contents:
     *  0:              matched string
     *  1..pairCount-1: paren matches
     *  input:          input string
     *  index:          start index for the match
     */
    RootedObject array(cx, NewSlowEmptyArray(cx));
    if (!array)
        return false;

    if (!input) {
        input = js_NewStringCopyN(cx, chars, length);
        if (!input)
            return false;
    }

    RegExpMatchBuilder builder(cx, array);
    RootedValue undefinedValue(cx, UndefinedValue());

    for (size_t i = 0; i < matchPairs->pairCount(); ++i) {
        MatchPair pair = matchPairs->pair(i);

        JSString *captured;
        if (pair.isUndefined()) {
            JS_ASSERT(i != 0); /* Since we had a match, first pair must be present. */
            if (!builder.append(i, undefinedValue))
                return false;
        } else {
            captured = js_NewDependentString(cx, input, pair.start, pair.length());
            RootedValue value(cx, StringValue(captured));
            if (!captured || !builder.append(i, value))
                return false;
        }
    }

    if (!builder.setIndex(matchPairs->pair(0).start) || !builder.setInput(input))
        return false;

    *rval = ObjectValue(*array);
    return true;
}

template <class T>
bool
ExecuteRegExpImpl(JSContext *cx, RegExpStatics *res, T &re, JSLinearString *input,
                  const jschar *chars, size_t length,
                  size_t *lastIndex, RegExpExecType type, Value *rval)
{
    LifoAllocScope allocScope(&cx->tempLifoAlloc());
    MatchPairs *matchPairs = NULL;
    RegExpRunStatus status = re.execute(cx, chars, length, lastIndex, &matchPairs);

    switch (status) {
      case RegExpRunStatus_Error:
        return false;
      case RegExpRunStatus_Success_NotFound:
        *rval = NullValue();
        return true;
      default:
        JS_ASSERT(status == RegExpRunStatus_Success);
        JS_ASSERT(matchPairs);
    }

    if (res)
        res->updateFromMatchPairs(cx, input, matchPairs);

    *lastIndex = matchPairs->pair(0).limit;

    if (type == RegExpTest) {
        *rval = BooleanValue(true);
        return true;
    }

    return CreateRegExpMatchResult(cx, input, chars, length, matchPairs, rval);
}

bool
js::ExecuteRegExp(JSContext *cx, RegExpStatics *res, RegExpShared &shared, JSLinearString *input,
                  const jschar *chars, size_t length,
                  size_t *lastIndex, RegExpExecType type, Value *rval)
{
    return ExecuteRegExpImpl(cx, res, shared, input, chars, length, lastIndex, type, rval);
}

bool
js::ExecuteRegExp(JSContext *cx, RegExpStatics *res, RegExpObject &reobj, JSLinearString *input,
                  const jschar *chars, size_t length,
                  size_t *lastIndex, RegExpExecType type, Value *rval)
{
    return ExecuteRegExpImpl(cx, res, reobj, input, chars, length, lastIndex, type, rval);
}

/* Note: returns the original if no escaping need be performed. */
static JSAtom *
EscapeNakedForwardSlashes(JSContext *cx, JSAtom *unescaped)
{
    size_t oldLen = unescaped->length();
    const jschar *oldChars = unescaped->chars();

    JS::Anchor<JSString *> anchor(unescaped);

    /* We may never need to use |sb|. Start using it lazily. */
    StringBuffer sb(cx);

    for (const jschar *it = oldChars; it < oldChars + oldLen; ++it) {
        if (*it == '/' && (it == oldChars || it[-1] != '\\')) {
            /* There's a forward slash that needs escaping. */
            if (sb.empty()) {
                /* This is the first one we've seen, copy everything up to this point. */
                if (!sb.reserve(oldLen + 1))
                    return NULL;
                sb.infallibleAppend(oldChars, size_t(it - oldChars));
            }
            if (!sb.append('\\'))
                return NULL;
        }

        if (!sb.empty() && !sb.append(*it))
            return NULL;
    }

    return sb.empty() ? unescaped : sb.finishAtom();
}

/*
 * Compile a new |RegExpShared| for the |RegExpObject|.
 *
 * Per ECMAv5 15.10.4.1, we act on combinations of (pattern, flags) as
 * arguments:
 *
 *  RegExp, undefined => flags := pattern.flags
 *  RegExp, _ => throw TypeError
 *  _ => pattern := ToString(pattern) if defined(pattern) else ''
 *       flags := ToString(flags) if defined(flags) else ''
 */
static bool
CompileRegExpObject(JSContext *cx, RegExpObjectBuilder &builder, CallArgs args)
{
    if (args.length() == 0) {
        RegExpStatics *res = cx->regExpStatics();
        Rooted<JSAtom*> empty(cx, cx->runtime->emptyString);
        RegExpObject *reobj = builder.build(empty, res->getFlags());
        if (!reobj)
            return false;
        args.rval().setObject(*reobj);
        return true;
    }

    Value sourceValue = args[0];

    /*
     * If we get passed in an object whose internal [[Class]] property is
     * "RegExp", return a new object with the same source/flags.
     */
    if (IsObjectWithClass(sourceValue, ESClass_RegExp, cx)) {
        /*
         * Beware, sourceObj may be a (transparent) proxy to a RegExp, so only
         * use generic (proxyable) operations on sourceObj that do not assume
         * sourceObj.isRegExp().
         */
        RootedObject sourceObj(cx, &sourceValue.toObject());

        if (args.hasDefined(1)) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_NEWREGEXP_FLAGGED);
            return false;
        }

        /*
         * Only extract the 'flags' out of sourceObj; do not reuse the
         * RegExpShared since it may be from a different compartment.
         */
        RegExpFlag flags;
        {
            RegExpGuard g;
            if (!RegExpToShared(cx, *sourceObj, &g))
                return false;

            flags = g->getFlags();
        }

        /*
         * 'toSource' is a permanent read-only property, so this is equivalent
         * to executing RegExpObject::getSource on the unwrapped object.
         */
        RootedValue v(cx);
        if (!JSObject::getProperty(cx, sourceObj, sourceObj, cx->runtime->atomState.sourceAtom, &v))
            return false;

        Rooted<JSAtom*> sourceAtom(cx, &v.toString()->asAtom());
        RegExpObject *reobj = builder.build(sourceAtom, flags);
        if (!reobj)
            return false;

        args.rval().setObject(*reobj);
        return true;
    }

    RootedAtom source(cx);
    if (sourceValue.isUndefined()) {
        source = cx->runtime->emptyString;
    } else {
        /* Coerce to string and compile. */
        JSString *str = ToString(cx, sourceValue);
        if (!str)
            return false;

        source = AtomizeString(cx, str);
        if (!source)
            return false;
    }

    RegExpFlag flags = RegExpFlag(0);
    if (args.hasDefined(1)) {
        JSString *flagStr = ToString(cx, args[1]);
        if (!flagStr)
            return false;
        args[1].setString(flagStr);
        if (!ParseRegExpFlags(cx, flagStr, &flags))
            return false;
    }

    RootedAtom escapedSourceStr(cx, EscapeNakedForwardSlashes(cx, source));
    if (!escapedSourceStr)
        return false;

    if (!js::detail::RegExpCode::checkSyntax(cx, NULL, escapedSourceStr))
        return false;

    RegExpStatics *res = cx->regExpStatics();
    RegExpObject *reobj = builder.build(escapedSourceStr, RegExpFlag(flags | res->getFlags()));
    if (!reobj)
        return false;

    args.rval().setObject(*reobj);
    return true;
}

JS_ALWAYS_INLINE bool
IsRegExp(const Value &v)
{
    return v.isObject() && v.toObject().hasClass(&RegExpClass);
}

JS_ALWAYS_INLINE bool
regexp_compile_impl(JSContext *cx, CallArgs args)
{
    JS_ASSERT(IsRegExp(args.thisv()));
    RegExpObjectBuilder builder(cx, &args.thisv().toObject().asRegExp());
    return CompileRegExpObject(cx, builder, args);
}

JSBool
regexp_compile(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsRegExp, regexp_compile_impl>(cx, args);
}

static JSBool
regexp_construct(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!IsConstructing(args)) {
        /*
         * If first arg is regexp and no flags are given, just return the arg.
         * Otherwise, delegate to the standard constructor.
         * See ECMAv5 15.10.3.1.
         */
        if (args.hasDefined(0) &&
            IsObjectWithClass(args[0], ESClass_RegExp, cx) &&
            !args.hasDefined(1))
        {
            args.rval().set(args[0]);
            return true;
        }
    }

    RegExpObjectBuilder builder(cx);
    return CompileRegExpObject(cx, builder, args);
}

JS_ALWAYS_INLINE bool
regexp_toString_impl(JSContext *cx, CallArgs args)
{
    JS_ASSERT(IsRegExp(args.thisv()));

    JSString *str = args.thisv().toObject().asRegExp().toString(cx);
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

JSBool
regexp_toString(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod<IsRegExp, regexp_toString_impl>(cx, args);
}

static JSFunctionSpec regexp_methods[] = {
#if JS_HAS_TOSOURCE
    JS_FN(js_toSource_str,  regexp_toString,    0,0),
#endif
    JS_FN(js_toString_str,  regexp_toString,    0,0),
    JS_FN("compile",        regexp_compile,     2,0),
    JS_FN("exec",           regexp_exec,        1,0),
    JS_FN("test",           regexp_test,        1,0),
    JS_FS_END
};

/*
 * RegExp static properties.
 *
 * RegExp class static properties and their Perl counterparts:
 *
 *  RegExp.input                $_
 *  RegExp.multiline            $*
 *  RegExp.lastMatch            $&
 *  RegExp.lastParen            $+
 *  RegExp.leftContext          $`
 *  RegExp.rightContext         $'
 */

#define DEFINE_STATIC_GETTER(name, code)                                        \
    static JSBool                                                               \
    name(JSContext *cx, HandleObject obj, HandleId id, MutableHandleValue vp)   \
    {                                                                           \
        RegExpStatics *res = cx->regExpStatics();                               \
        code;                                                                   \
    }

DEFINE_STATIC_GETTER(static_input_getter,        return res->createPendingInput(cx, vp.address()))
DEFINE_STATIC_GETTER(static_multiline_getter,    vp.set(BOOLEAN_TO_JSVAL(res->multiline()));
                                                 return true)
DEFINE_STATIC_GETTER(static_lastMatch_getter,    return res->createLastMatch(cx, vp.address()))
DEFINE_STATIC_GETTER(static_lastParen_getter,    return res->createLastParen(cx, vp.address()))
DEFINE_STATIC_GETTER(static_leftContext_getter,  return res->createLeftContext(cx, vp.address()))
DEFINE_STATIC_GETTER(static_rightContext_getter, return res->createRightContext(cx, vp.address()))

DEFINE_STATIC_GETTER(static_paren1_getter,       return res->createParen(cx, 1, vp.address()))
DEFINE_STATIC_GETTER(static_paren2_getter,       return res->createParen(cx, 2, vp.address()))
DEFINE_STATIC_GETTER(static_paren3_getter,       return res->createParen(cx, 3, vp.address()))
DEFINE_STATIC_GETTER(static_paren4_getter,       return res->createParen(cx, 4, vp.address()))
DEFINE_STATIC_GETTER(static_paren5_getter,       return res->createParen(cx, 5, vp.address()))
DEFINE_STATIC_GETTER(static_paren6_getter,       return res->createParen(cx, 6, vp.address()))
DEFINE_STATIC_GETTER(static_paren7_getter,       return res->createParen(cx, 7, vp.address()))
DEFINE_STATIC_GETTER(static_paren8_getter,       return res->createParen(cx, 8, vp.address()))
DEFINE_STATIC_GETTER(static_paren9_getter,       return res->createParen(cx, 9, vp.address()))

#define DEFINE_STATIC_SETTER(name, code)                                        \
    static JSBool                                                               \
    name(JSContext *cx, HandleObject obj, HandleId id, JSBool strict, MutableHandleValue vp)\
    {                                                                           \
        RegExpStatics *res = cx->regExpStatics();                               \
        code;                                                                   \
        return true;                                                            \
    }

DEFINE_STATIC_SETTER(static_input_setter,
                     if (!JSVAL_IS_STRING(vp) && !JS_ConvertValue(cx, vp, JSTYPE_STRING, vp.address()))
                         return false;
                     res->setPendingInput(JSVAL_TO_STRING(vp)))
DEFINE_STATIC_SETTER(static_multiline_setter,
                     if (!JSVAL_IS_BOOLEAN(vp) && !JS_ConvertValue(cx, vp, JSTYPE_BOOLEAN, vp.address()))
                         return false;
                     res->setMultiline(cx, !!JSVAL_TO_BOOLEAN(vp)))

const uint8_t REGEXP_STATIC_PROP_ATTRS    = JSPROP_PERMANENT | JSPROP_SHARED | JSPROP_ENUMERATE;
const uint8_t RO_REGEXP_STATIC_PROP_ATTRS = REGEXP_STATIC_PROP_ATTRS | JSPROP_READONLY;

const uint8_t HIDDEN_PROP_ATTRS = JSPROP_PERMANENT | JSPROP_SHARED;
const uint8_t RO_HIDDEN_PROP_ATTRS = HIDDEN_PROP_ATTRS | JSPROP_READONLY;

static JSPropertySpec regexp_static_props[] = {
    {"input",        0, REGEXP_STATIC_PROP_ATTRS,    JSOP_WRAPPER(static_input_getter),
                                                     JSOP_WRAPPER(static_input_setter)},
    {"multiline",    0, REGEXP_STATIC_PROP_ATTRS,    JSOP_WRAPPER(static_multiline_getter),
                                                     JSOP_WRAPPER(static_multiline_setter)},
    {"lastMatch",    0, RO_REGEXP_STATIC_PROP_ATTRS, JSOP_WRAPPER(static_lastMatch_getter),
                                                     JSOP_NULLWRAPPER},
    {"lastParen",    0, RO_REGEXP_STATIC_PROP_ATTRS, JSOP_WRAPPER(static_lastParen_getter),
                                                     JSOP_NULLWRAPPER},
    {"leftContext",  0, RO_REGEXP_STATIC_PROP_ATTRS, JSOP_WRAPPER(static_leftContext_getter),
                                                     JSOP_NULLWRAPPER},
    {"rightContext", 0, RO_REGEXP_STATIC_PROP_ATTRS, JSOP_WRAPPER(static_rightContext_getter),
                                                     JSOP_NULLWRAPPER},
    {"$1",           0, RO_REGEXP_STATIC_PROP_ATTRS, JSOP_WRAPPER(static_paren1_getter),
                                                     JSOP_NULLWRAPPER},
    {"$2",           0, RO_REGEXP_STATIC_PROP_ATTRS, JSOP_WRAPPER(static_paren2_getter),
                                                     JSOP_NULLWRAPPER},
    {"$3",           0, RO_REGEXP_STATIC_PROP_ATTRS, JSOP_WRAPPER(static_paren3_getter),
                                                     JSOP_NULLWRAPPER},
    {"$4",           0, RO_REGEXP_STATIC_PROP_ATTRS, JSOP_WRAPPER(static_paren4_getter),
                                                     JSOP_NULLWRAPPER},
    {"$5",           0, RO_REGEXP_STATIC_PROP_ATTRS, JSOP_WRAPPER(static_paren5_getter),
                                                     JSOP_NULLWRAPPER},
    {"$6",           0, RO_REGEXP_STATIC_PROP_ATTRS, JSOP_WRAPPER(static_paren6_getter),
                                                     JSOP_NULLWRAPPER},
    {"$7",           0, RO_REGEXP_STATIC_PROP_ATTRS, JSOP_WRAPPER(static_paren7_getter),
                                                     JSOP_NULLWRAPPER},
    {"$8",           0, RO_REGEXP_STATIC_PROP_ATTRS, JSOP_WRAPPER(static_paren8_getter),
                                                     JSOP_NULLWRAPPER},
    {"$9",           0, RO_REGEXP_STATIC_PROP_ATTRS, JSOP_WRAPPER(static_paren9_getter),
                                                     JSOP_NULLWRAPPER},
    {"$_",           0, HIDDEN_PROP_ATTRS,           JSOP_WRAPPER(static_input_getter),
                                                     JSOP_WRAPPER(static_input_setter)},
    {"$*",           0, HIDDEN_PROP_ATTRS,           JSOP_WRAPPER(static_multiline_getter),
                                                     JSOP_WRAPPER(static_multiline_setter)},
    {"$&",           0, RO_HIDDEN_PROP_ATTRS,        JSOP_WRAPPER(static_lastMatch_getter),
                                                     JSOP_NULLWRAPPER},
    {"$+",           0, RO_HIDDEN_PROP_ATTRS,        JSOP_WRAPPER(static_lastParen_getter),
                                                     JSOP_NULLWRAPPER},
    {"$`",           0, RO_HIDDEN_PROP_ATTRS,        JSOP_WRAPPER(static_leftContext_getter),
                                                     JSOP_NULLWRAPPER},
    {"$'",           0, RO_HIDDEN_PROP_ATTRS,        JSOP_WRAPPER(static_rightContext_getter),
                                                     JSOP_NULLWRAPPER},
    {0,0,0,JSOP_NULLWRAPPER,JSOP_NULLWRAPPER}
};

JSObject *
js_InitRegExpClass(JSContext *cx, JSObject *obj)
{
    JS_ASSERT(obj->isNative());

    Rooted<GlobalObject*> global(cx, &obj->asGlobal());

    RootedObject proto(cx, global->createBlankPrototype(cx, &RegExpClass));
    if (!proto)
        return NULL;
    proto->setPrivate(NULL);

    RegExpObjectBuilder builder(cx, &proto->asRegExp());
    Rooted<JSAtom*> empty(cx, cx->runtime->emptyString);
    if (!builder.build(empty, RegExpFlag(0)))
        return NULL;

    if (!DefinePropertiesAndBrand(cx, proto, NULL, regexp_methods))
        return NULL;

    RootedFunction ctor(cx);
    ctor = global->createConstructor(cx, regexp_construct, CLASS_NAME(cx, RegExp), 2);
    if (!ctor)
        return NULL;

    if (!LinkConstructorAndPrototype(cx, ctor, proto))
        return NULL;

    /* Add static properties to the RegExp constructor. */
    if (!JS_DefineProperties(cx, ctor, regexp_static_props))
        return NULL;

    /* Capture normal data properties pregenerated for RegExp objects. */
    TypeObject *type = proto->getNewType(cx);
    if (!type)
        return NULL;
    AddTypeProperty(cx, type, "source", Type::StringType());
    AddTypeProperty(cx, type, "global", Type::BooleanType());
    AddTypeProperty(cx, type, "ignoreCase", Type::BooleanType());
    AddTypeProperty(cx, type, "multiline", Type::BooleanType());
    AddTypeProperty(cx, type, "sticky", Type::BooleanType());
    AddTypeProperty(cx, type, "lastIndex", Type::Int32Type());

    if (!DefineConstructorAndPrototype(cx, global, JSProto_RegExp, ctor, proto))
        return NULL;

    return proto;
}


static const jschar GreedyStarChars[] = {'.', '*'};

static inline bool
StartsWithGreedyStar(JSAtom *source)
{
    return false;

#if 0
    if (source->length() < 3)
        return false;

    const jschar *chars = source->chars();
    return chars[0] == GreedyStarChars[0] &&
           chars[1] == GreedyStarChars[1] &&
           chars[2] != '?';
#endif
}

static inline bool
GetSharedForGreedyStar(JSContext *cx, JSAtom *source, RegExpFlag flags, RegExpGuard *g)
{
    if (cx->compartment->regExps.lookupHack(source, flags, cx, g))
        return true;

    JSAtom *hackedSource = AtomizeChars(cx, source->chars() + ArrayLength(GreedyStarChars),
                                        source->length() - ArrayLength(GreedyStarChars));
    if (!hackedSource)
        return false;

    return cx->compartment->regExps.getHack(cx, source, hackedSource, flags, g);
}

/*
 * ES5 15.10.6.2 (and 15.10.6.3, which calls 15.10.6.2).
 *
 * RegExp.prototype.test doesn't need to create a results array, and we use
 * |execType| to perform this optimization.
 */
static bool
ExecuteRegExp(JSContext *cx, RegExpExecType execType, CallArgs args)
{
    /* Step 1 was performed by CallNonGenericMethod. */
    Rooted<RegExpObject*> reobj(cx, &args.thisv().toObject().asRegExp());

    RegExpGuard re;
    if (StartsWithGreedyStar(reobj->getSource())) {
        if (!GetSharedForGreedyStar(cx, reobj->getSource(), reobj->getFlags(), &re))
            return false;
    } else {
        if (!reobj->getShared(cx, &re))
            return false;
    }

    RegExpStatics *res = cx->regExpStatics();

    /* Step 2. */
    JSString *input = ToString(cx, (args.length() > 0) ? args[0] : UndefinedValue());
    if (!input)
        return false;

    /* Step 3. */
    Rooted<JSLinearString*> linearInput(cx, input->ensureLinear(cx));
    if (!linearInput)
        return false;

    /* Step 4. */
    Value lastIndex = reobj->getLastIndex();

    /* Step 5. */
    double i;
    if (!ToInteger(cx, lastIndex, &i))
        return false;

    /* Steps 6-7 (with sticky extension). */
    if (!re->global() && !re->sticky())
        i = 0;

    const jschar *chars = linearInput->chars();
    size_t length = linearInput->length();

    /* Step 9a. */
    if (i < 0 || i > length) {
        reobj->zeroLastIndex();
        args.rval().setNull();
        return true;
    }

    /* Steps 8-21. */
    size_t lastIndexInt(i);
    if (!ExecuteRegExp(cx, res, *re, linearInput, chars, length, &lastIndexInt, execType,
                       args.rval().address())) {
        return false;
    }

    /* Step 11 (with sticky extension). */
    if (re->global() || (!args.rval().isNull() && re->sticky())) {
        if (args.rval().isNull())
            reobj->zeroLastIndex();
        else
            reobj->setLastIndex(lastIndexInt);
    }

    return true;
}

/* ES5 15.10.6.2. */
static bool
regexp_exec_impl(JSContext *cx, CallArgs args)
{
    return ExecuteRegExp(cx, RegExpExec, args);
}

JSBool
js::regexp_exec(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, IsRegExp, regexp_exec_impl, args);
}

/* ES5 15.10.6.3. */
static bool
regexp_test_impl(JSContext *cx, CallArgs args)
{
    if (!ExecuteRegExp(cx, RegExpTest, args))
        return false;
    if (!args.rval().isTrue())
        args.rval().setBoolean(false);
    return true;
}

JSBool
js::regexp_test(JSContext *cx, unsigned argc, Value *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return CallNonGenericMethod(cx, IsRegExp, regexp_test_impl, args);
}
