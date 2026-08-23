/*---
description: shadow must never escape into a JS-visible identity
esid: x
---*/
var bad = [];
function check(name, got, want) { if (got !== want) bad.push(name); }

// Math is a base object; writing to it creates a shadow.
var before = Math;
Math.__probe = 1;
check("global read after shadowing", Math, before);
check("captured reference", before, Math);

// method receiver
Math.m = function () { return this; };
check("method `this`", Math.m(), Math);

// accessor receivers
Object.defineProperty(Math, "g", { get: function () { return this; }, configurable: true });
check("getter `this`", Math.g, Math);
var setterThis = null;
Object.defineProperty(Math, "s", { set: function (v) { setterThis = this; }, configurable: true });
Math.s = 1;
check("setter `this`", setterThis, Math);

// explicit receivers
check("Reflect.get receiver", Reflect.get(Math, "g", Math), Math);
check("call/apply thisArg", Math.m.call(Math), Math);
check("bound thisArg", Math.m.bind(Math)(), Math);
check("Array forEach thisArg", (function(){ var t; [1].forEach(function(){ t = this; }, Math); return t; })(), Math);

// prototype plumbing
var O = Object.prototype;
O.__probe2 = 1;
check("getPrototypeOf", Object.getPrototypeOf({}), O);
check("instanceof proto identity", Object.getPrototypeOf(Object.getPrototypeOf(new Error())), O);

// proxy traps
var seen = {};
var P = new Proxy(Math, {
  get: function (t, k, r) { seen.get = r; return Reflect.get(t, k, r); },
  set: function (t, k, v, r) { seen.set = r; return Reflect.set(t, k, v, r); }
});
P.g; check("proxy get receiver", seen.get, P);
P.s = 1; check("proxy set receiver", seen.set, P);

if (bad.length) throw new Error("SHADOW ESCAPES: " + bad.join(", "));
