// Disabled pending bug 789036
// assertEq("abc".contains("a"), true);
// assertEq("abc".contains("b"), true);
// assertEq("abc".contains("abc"), true);
// assertEq("abc".contains("bc"), true);
// assertEq("abc".contains("d"), false);
// assertEq("abc".contains("abcd"), false);
// assertEq("abc".contains("ac"), false);
// assertEq("abc".contains("abc", 0), true);
// assertEq("abc".contains("bc", 0), true);
// assertEq("abc".contains("de", 0), false);
// assertEq("abc".contains("bc", 1), true);
// assertEq("abc".contains("c", 1), true);
// assertEq("abc".contains("a", 1), false);
// assertEq("abc".contains("abc", 1), false);
// assertEq("abc".contains("c", 2), true);
// assertEq("abc".contains("d", 2), false);
// assertEq("abc".contains("dcd", 2), false);
// assertEq("abc".contains("a", 42), false);
// assertEq("abc".contains("a", Infinity), false);
// assertEq("abc".contains("ab", -43), true);
// assertEq("abc".contains("cd", -42), false);
// assertEq("abc".contains("ab", -Infinity), true);
// assertEq("abc".contains("cd", -Infinity), false);
// assertEq("abc".contains("ab", NaN), true);
// assertEq("abc".contains("cd", NaN), false);
// var myobj = {toString : (function () "abc"), contains : String.prototype.contains};
// assertEq(myobj.contains("abc"), true);
// assertEq(myobj.contains("cd"), false);
// var gotStr = false, gotPos = false;
// myobj = {toString : (function () {
//     assertEq(gotPos, false);
//     gotStr = true;
//     return "xyz";
// }),
// contains : String.prototype.contains};
// var idx = {valueOf : (function () {
//     assertEq(gotStr, true);
//     gotPos = true;
//     return 42;
// })};
// myobj.contains("elephant", idx);
// assertEq(gotPos, true);
// assertEq("xyzzy".contains("zy\0", 2), false);
// var dots = Array(10000).join('.');
// assertEq(dots.contains("\x01", 10000), false);
// assertEq(dots.contains("\0", 10000), false);
