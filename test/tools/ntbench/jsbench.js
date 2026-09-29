// Alphabox NT application benchmark, run by cscript (Windows Script Host).
//
// The point of this file is to exercise datapaths a batch loop never touches:
// floating point, string and array handling, property lookup and allocation,
// inside a large real DLL (jscript.dll) rather than cmd.exe's parser. Each
// section is sized to run about three seconds under the JIT; scale them all
// with the first argument. Output is axpbench's: "<section> <n> ms (<result>)"
// and "total <n> ms", results as integers, and a 300 ms sleep before each
// section so a MIPS trace shows the sections apart (test/tools/perf_ab.py).
//
// Windows 2000's JScript is 5.1: no Array.prototype.push, no toFixed.
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
  return s * 1e6; // printed rounded: keep six decimals of the sum
}
function bench_str(n) {           // string build, compare, search: memory traffic
  var parts = [];
  for (var i = 0; i < n; i++) parts[i] = "item" + (i % 997) + ":";
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

// Sized from a measured scale=1 run (build-jit, 2026-09-29): ~3 s each.
var work = [["int", bench_int, 2500000], ["fp", bench_fp, 2000000],
            ["str", bench_str, 130000], ["arr", bench_arr, 70000],
            ["obj", bench_obj, 500000]];
var total = 0;
for (var w = 0; w < work.length; w++) {
  if (!want(work[w][0])) continue;
  WScript.Sleep(300);
  var s0 = new Date();
  var r = work[w][1](Math.round(work[w][2] * scale));
  var ms = new Date() - s0;
  total += ms;
  WScript.Echo(work[w][0] + " " + ms + " ms (" + Math.round(r) + ")");
}
WScript.Echo("total " + total + " ms");
