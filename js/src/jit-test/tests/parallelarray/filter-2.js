load(libdir + "parallelarray-helpers.js");

function testFilterNone() {
  // Test filtering (removing everything)
  var p = new ParallelArray([0,1,2,3,4]);
  var none = p.map(function () { return false; });
  var r = p.filter(none);
  assertEqParallelArray(r, new ParallelArray);
  var p = new ParallelArray([5,2], function(i,j) { return i+j; });
  var r = p.filter(none);
  assertEqParallelArray(r, new ParallelArray);
}

testFilterNone();
