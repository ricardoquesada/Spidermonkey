/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "tests.h"

struct ScriptObjectFixture : public JSAPITest {
    static const int code_size;
    static const char code[];
    static jschar uc_code[];

    ScriptObjectFixture()
    {
        for (int i = 0; i < code_size; i++)
            uc_code[i] = code[i];
    }

    bool tryScript(JS::HandleObject global, JSScript *scriptArg)
    {
        JS::RootedScript script(cx, scriptArg);
        CHECK(script);

        JS_GC(rt);

        /* After a garbage collection, the script should still work. */
        jsval result;
        CHECK(JS_ExecuteScript(cx, global, script, &result));

        return true;
    }
};

const char ScriptObjectFixture::code[] =
    "(function(a, b){return a+' '+b;}('hello', 'world'))";
const int ScriptObjectFixture::code_size = sizeof(ScriptObjectFixture::code) - 1;
jschar ScriptObjectFixture::uc_code[ScriptObjectFixture::code_size];

BEGIN_FIXTURE_TEST(ScriptObjectFixture, bug438633_CompileScript)
{
    return tryScript(global, JS_CompileScript(cx, global, code, code_size,
                                              __FILE__, __LINE__));
}
END_FIXTURE_TEST(ScriptObjectFixture, bug438633_CompileScript)

BEGIN_FIXTURE_TEST(ScriptObjectFixture, bug438633_CompileScript_empty)
{
    return tryScript(global, JS_CompileScript(cx, global, "", 0,
                                              __FILE__, __LINE__));
}
END_FIXTURE_TEST(ScriptObjectFixture, bug438633_CompileScript_empty)

BEGIN_FIXTURE_TEST(ScriptObjectFixture, bug438633_CompileScriptForPrincipals)
{
    return tryScript(global, JS_CompileScriptForPrincipals(cx, global, NULL,
                                                           code, code_size,
                                                           __FILE__, __LINE__));
}
END_FIXTURE_TEST(ScriptObjectFixture, bug438633_CompileScriptForPrincipals)

BEGIN_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileUCScript)
{
    return tryScript(global, JS_CompileUCScript(cx, global,
                                                uc_code, code_size,
                                                __FILE__, __LINE__));
}
END_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileUCScript)

BEGIN_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileUCScript_empty)
{
    return tryScript(global, JS_CompileUCScript(cx, global,
                                                uc_code, 0,
                                                __FILE__, __LINE__));
}
END_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileUCScript_empty)

BEGIN_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileUCScriptForPrincipals)
{
    return tryScript(global, JS_CompileUCScriptForPrincipals(cx, global, NULL,
                                                             uc_code, code_size,
                                                             __FILE__, __LINE__));
}
END_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileUCScriptForPrincipals)

BEGIN_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileFile)
{
    TempFile tempScript;
    static const char script_filename[] = "temp-bug438633_JS_CompileFile";
    FILE *script_stream = tempScript.open(script_filename);
    CHECK(fputs(code, script_stream) != EOF);
    tempScript.close();
    JSScript *script = JS_CompileUTF8File(cx, global, script_filename);
    tempScript.remove();
    return tryScript(global, script);
}
END_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileFile)

BEGIN_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileFile_empty)
{
    TempFile tempScript;
    static const char script_filename[] = "temp-bug438633_JS_CompileFile_empty";
    tempScript.open(script_filename);
    tempScript.close();
    JSScript *script = JS_CompileUTF8File(cx, global, script_filename);
    tempScript.remove();
    return tryScript(global, script);
}
END_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileFile_empty)

BEGIN_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileFileHandle)
{
    TempFile tempScript;
    FILE *script_stream = tempScript.open("temp-bug438633_JS_CompileFileHandle");
    CHECK(fputs(code, script_stream) != EOF);
    CHECK(fseek(script_stream, 0, SEEK_SET) != EOF);
    return tryScript(global, JS_CompileUTF8FileHandle(cx, global, "temporary file",
                                                      script_stream));
}
END_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileFileHandle)

BEGIN_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileFileHandle_empty)
{
    TempFile tempScript;
    FILE *script_stream = tempScript.open("temp-bug438633_JS_CompileFileHandle_empty");
    return tryScript(global, JS_CompileUTF8FileHandle(cx, global, "empty temporary file",
                                              script_stream));
}
END_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileFileHandle_empty)

BEGIN_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileFileHandleForPrincipals)
{
    TempFile tempScript;
    FILE *script_stream = tempScript.open("temp-bug438633_JS_CompileFileHandleForPrincipals");
    CHECK(fputs(code, script_stream) != EOF);
    CHECK(fseek(script_stream, 0, SEEK_SET) != EOF);
    return tryScript(global, JS_CompileUTF8FileHandleForPrincipals(cx, global,
                                                                   "temporary file",
                                                                   script_stream, NULL));
}
END_FIXTURE_TEST(ScriptObjectFixture, bug438633_JS_CompileFileHandleForPrincipals)
