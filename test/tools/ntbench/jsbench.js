// Alphabox NT application benchmark, run by cscript (Windows Script Host).
//
// The point of this file is to exercise datapaths a batch loop never touches:
// floating point, string and array handling, property lookup and allocation,
// inside a large real DLL (jscript.dll) rather than cmd.exe's parser. Each
// section is sized to run a few seconds on an emulated 800 MHz Alpha; scale
// them all with the first argument.
//
//   cscript //nologo jsbench.js [scale] [section]
//
// Sections: int, fp, str, arr, obj, all (default).
var args = WScript.Arguments;
var scale = args.length > 0 ? parseFloat(args(0)) : 1.0;
var only = args.length > 1 ? args(1) : "all";
function want(s) { return only == "all" || only == s; }

function bench_int(n) {           // integer ALU and branches
  var a = 1, b = 0;
  for (var i = 0; i < n; i++) { a = (a * 1103515245 + 12345) & 0x7fffffff; b ^= a; }
  return b;
}
function bench_fp(n) {            // IEEE double: the FP emitter and FPCR path
  var s = 0.0;
  for (var i = 1; i <= n; i++) s += Math.sqrt(i) / (i + 0.5);
  return s;
}
function bench_str(n) {           // string build, compare, search: memory traffic
  var parts = [];
  for (var i = 0; i < n; i++) parts.push("item" + (i % 997) + ":");
  var s = parts.join("");
  var hits = 0, at = 0;
  while ((at = s.indexOf("item42:", at)) >= 0) { hits++; at += 7; }
  return hits + s.length;
}
function bench_arr(n) {           // array fill + sort: a big working set
  var v = new Array(n);
  for (var i = 0; i < n; i++) v[i] = (i * 2654435761) % 1000003;
  v.sort(function (x, y) { return x - y; });
  return v[0] + v[n - 1];
}
function bench_obj(n) {           // allocation, property lookup, collection
  var acc = 0;
  for (var i = 0; i < n; i++) {
    var o = { a: i, b: i * 2, tag: "n" + (i & 63) };
    acc += o.a + o.b + o.tag.length;
  }
  return acc;
}

var work = [["int", bench_int, 400000], ["fp", bench_fp, 200000],
            ["str", bench_str, 60000], ["arr", bench_arr, 120000],
            ["obj", bench_obj, 150000]];
// Seconds to two places. Not toFixed: Windows 2000's JScript 5.1 predates it.
function secs(ms) { return Math.round(ms / 10) / 100; }
var t0 = new Date();
for (var w = 0; w < work.length; w++) {
  if (!want(work[w][0])) continue;
  var s0 = new Date();
  var r = work[w][1](Math.round(work[w][2] * scale));
  WScript.Echo(work[w][0] + " " + secs(new Date() - s0) + "s (" + r + ")");
}
WScript.Echo("total " + secs(new Date() - t0) + "s");
