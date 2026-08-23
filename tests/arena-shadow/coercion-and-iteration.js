/*---
description: shadow must not escape via coercion hooks, iteration or callbacks
esid: x
---*/
var bad = [];
function check(n, got, want) { if (got !== want) bad.push(n); }

// Symbol.toPrimitive / valueOf / toString on a shadowed base prototype:
// `this` must be the receiver JS handed in, not a shadow of it.
Object.defineProperty(Number.prototype, Symbol.toPrimitive, {
  value: function () { return this === boxed ? 1 : 0; }, configurable: true });
var boxed = Object(5);
check("Symbol.toPrimitive this", +boxed, 1);

Object.defineProperty(Object.prototype, "valueOf", {
  value: function () { return this === vo ? 1 : 0; }, configurable: true, writable: true });
var vo = Object.create(Object.prototype);
check("valueOf this", +vo, 1);
delete Object.prototype.valueOf;

// toJSON is invoked with the value as receiver
Object.defineProperty(Object.prototype, "toJSON", {
  value: function () { return this === tj ? "yes" : "no"; }, configurable: true, writable: true });
var tj = {};
check("toJSON this", JSON.stringify(tj), '"yes"');
delete Object.prototype.toJSON;

// iteration protocol driven off a shadowed base prototype
Object.defineProperty(Object.prototype, Symbol.iterator, {
  value: function () { var self = this, done = false;
    return { next: function () { return done ? {done:true} : (done=true, {value:self, done:false}); } }; },
  configurable: true, writable: true });
var it = {};
check("Symbol.iterator this", [...it][0], it);
delete Object.prototype[Symbol.iterator];

// callbacks that receive the object as an argument, not as `this`
Math.__c = 1;
check("Object.entries value", Object.entries({ k: Math })[0][1], Math);
check("array element round trip", [Math][0], Math);
check("Map round trip", new Map([[1, Math]]).get(1), Math);
check("Set membership", new Set([Math]).has(Math), true);
check("WeakMap round trip", (function(){ var w = new WeakMap(); w.set(Math, 1); return w.has(Math); })(), true);
/* the replacer must return the root, or nothing below it is ever visited --
   this check failed identically on a vanilla runtime until it did */
check("JSON replacer value", (function(){ var seen;
  JSON.stringify({ k: Math }, function(k,v){ if (k==="k") { seen=v; return 0; } return v; });
  return seen; })(), Math);
check("closure capture", (function(){ var m = Math; return m; })(), Math);
check("array sort comparator arg", (function(){ var seen; [Math, Math].sort(function(a,b){ seen=a; return 0; }); return seen; })(), Math);

if (bad.length) throw new Error("SHADOW ESCAPES: " + bad.join(", "));
