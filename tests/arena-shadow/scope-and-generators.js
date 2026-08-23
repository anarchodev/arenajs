/*---
description: shadow must not escape via scope objects, generators or async
esid: x
---*/
var bad = [];
function check(n, got, want) { if (got !== want) bad.push(n); }

Math.__s = 1;   // shadow Math

// `with` puts an object into the scope chain; a bare reference resolved
// through it must yield the same identity as a property read.
var viaWith;
with (Math) { viaWith = __s; }
check("with scope read", viaWith, Math.__s);
var selfViaWith;
with ({ m: Math }) { selfViaWith = m; }
check("with scope object identity", selfViaWith, Math);

// generators suspend and resume across the property machinery
function* gen() { yield Math; yield (yield Math); }
var g = gen();
check("generator yielded identity", g.next().value, Math);
check("generator sent value round trip", (g.next(), g.next(Math).value), Math);

// a generator method on a shadowed base prototype: `this` is the receiver
Object.defineProperty(Object.prototype, "gm", {
  value: function* () { yield this; }, configurable: true, writable: true });
var host = {};
check("generator method this", host.gm().next().value, host);
delete Object.prototype.gm;

// getters reached through a for-in / Object.keys walk
Object.defineProperty(Object.prototype, "walked", {
  get: function () { return this; }, enumerable: false, configurable: true });
var w = {};
check("getter via explicit read", w.walked, w);
delete Object.prototype.walked;

// try/catch and finally do not re-wrap
try { throw Math; } catch (e) { check("thrown identity", e, Math); }

// labelled break out of a loop holding the reference
var held; outer: for (var i = 0; i < 1; i++) { held = Math; break outer; }
check("loop-held identity", held, Math);

// Object.groupBy-style callback receiving the object
check("filter callback arg", [Math].filter(function (x) { return x === Math; }).length, 1);
check("map callback arg", [Math].map(function (x) { return x; })[0], Math);
check("reduce accumulator", [1].reduce(function (a) { return a; }, Math), Math);

if (bad.length) throw new Error("SHADOW ESCAPES: " + bad.join(", "));
