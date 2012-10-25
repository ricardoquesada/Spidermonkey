/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "tests.h"

BEGIN_TEST(testDeepFreeze_bug535703)
{
    jsval v;
    EVAL("var x = {}; x;", &v);
    JS::RootedObject obj(cx, JSVAL_TO_OBJECT(v));
    CHECK(JS_DeepFreezeObject(cx, obj));  // don't crash
    EVAL("Object.isFrozen(x)", &v);
    CHECK_SAME(v, JSVAL_TRUE);
    return true;
}
END_TEST(testDeepFreeze_bug535703)

BEGIN_TEST(testDeepFreeze_deep)
{
    jsval a, o;
    EXEC("var a = {}, o = a;\n"
         "for (var i = 0; i < 5000; i++)\n"
         "    a = {x: a, y: a};\n");
    EVAL("a", &a);
    EVAL("o", &o);

    JS::RootedObject aobj(cx, JSVAL_TO_OBJECT(a));
    CHECK(JS_DeepFreezeObject(cx, aobj));

    jsval b;
    EVAL("Object.isFrozen(a)", &b);
    CHECK_SAME(b, JSVAL_TRUE);
    EVAL("Object.isFrozen(o)", &b);
    CHECK_SAME(b, JSVAL_TRUE);
    return true;
}
END_TEST(testDeepFreeze_deep)

BEGIN_TEST(testDeepFreeze_loop)
{
    jsval x, y;
    EXEC("var x = [], y = {x: x}; y.y = y; x.push(x, y);");
    EVAL("x", &x);
    EVAL("y", &y);

    JS::RootedObject xobj(cx, JSVAL_TO_OBJECT(x));
    CHECK(JS_DeepFreezeObject(cx, xobj));

    jsval b;
    EVAL("Object.isFrozen(x)", &b);
    CHECK_SAME(b, JSVAL_TRUE);
    EVAL("Object.isFrozen(y)", &b);
    CHECK_SAME(b, JSVAL_TRUE);
    return true;
}
END_TEST(testDeepFreeze_loop)
