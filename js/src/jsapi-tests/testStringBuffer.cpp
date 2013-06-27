/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "tests.h"

#include "jsatom.h"

#include "vm/StringBuffer.h"

#include "jsobjinlines.h"

BEGIN_TEST(testStringBuffer_finishString)
{
    JSString *str = JS_NewStringCopyZ(cx, "foopy");
    CHECK(str);

    JS::Rooted<JSAtom*> atom(cx, js::AtomizeString<js::CanGC>(cx, str));
    CHECK(atom);

    js::StringBuffer buffer(cx);
    CHECK(buffer.append("foopy"));

    JSAtom *finishedAtom = buffer.finishAtom();
    CHECK(finishedAtom);
    CHECK_EQUAL(atom, finishedAtom);
    return true;
}
END_TEST(testStringBuffer_finishString)
