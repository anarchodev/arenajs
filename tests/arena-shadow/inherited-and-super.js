/*---
description: harder shadow-escape paths
esid: x
---*/
var bad = [];
function check(n, got, want) { if (got !== want) bad.push(n); }

// accessor INHERITED from a shadowed base prototype: `this` must be the instance
Object.defineProperty(Object.prototype, "inh", {
  get: function () { return this; }, set: function (v) { this.__got = this; }, configurable: true });
var inst = {};
check("inherited getter this", inst.inh, inst);
inst.inh = 1; check("inherited setter this", inst.__got, inst);

// the shadowed prototype itself as receiver
check("proto as receiver", Object.prototype.inh, Object.prototype);

// coercion hooks on a base prototype
Number.prototype.vo = function () { return this.valueOf(); };
Object.defineProperty(Boolean.prototype, "tp", { get: function () { return this; }, configurable: true });
var b = Object(true);
check("boxed receiver", b.tp, b);

// __proto__ accessor round trip
var o2 = {};
Object.setPrototypeOf(o2, Math);
check("__proto__ after setPrototypeOf", o2.__proto__, Math);
check("getPrototypeOf after setPrototypeOf", Object.getPrototypeOf(o2), Math);

// descriptor functions round trip
var d = Object.getOwnPropertyDescriptor(Object.prototype, "inh");
check("descriptor getter identity", d.get.call(Math), Math);

// sort comparator / replace replacer receivers are undefined in strict; check base obj passes through
check("Object.assign target", Object.assign(Math, {}), Math);
check("Object.keys sees shadow props", Object.keys(Math).indexOf("inh") === -1, true);

// class super on a shadowed base prototype
Object.defineProperty(Object.prototype, "sup", { get: function () { return this; }, configurable: true });
class C { constructor() { this.viaSuper = super.sup; } }
var c = new C();
check("super receiver", c.viaSuper, c);

// freeze/seal identity
check("isFrozen on base", Object.isFrozen(Math), false);

if (bad.length) throw new Error("SHADOW ESCAPES: " + bad.join(", "));
