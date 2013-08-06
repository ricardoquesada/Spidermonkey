// |jit-test| error:out of memory

gcPreserveCode();
evaluate("gcparam(\"maxBytes\", gcparam(\"gcBytes\") + 4*1024);");
evaluate("\
function testDontEnum(F) { \
  function test() {\
    typeof (new test(\"1\")) != 'function'\
  }\
  test();\
} \
var list = [];\
for (i in list)\
  var F = this[list[i]];\
actual = testDontEnum(F);\
");
