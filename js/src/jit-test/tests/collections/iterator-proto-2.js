// Iterators of different collection types have different prototypes.

var aproto = Object.getPrototypeOf(Array().iterator());
var mproto = Object.getPrototypeOf(Map().iterator());
var sproto = Object.getPrototypeOf(Set().iterator());
assertEq(aproto !== mproto, true);
assertEq(aproto !== sproto, true);
assertEq(mproto !== sproto, true);
assertEq(aproto.next !== mproto.next, true);
assertEq(aproto.next !== sproto.next, true);
assertEq(mproto.next !== sproto.next, true);
