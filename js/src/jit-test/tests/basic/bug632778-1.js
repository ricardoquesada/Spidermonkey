// |jit-test| error: TypeError
function f() {
    "use strict";
}
g = wrap(f);
Object.defineProperty(g, "arguments", {set: function(){}});
