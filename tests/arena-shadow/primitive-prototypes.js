/*---
description: primitive prototype identity must not leak a shadow
esid: x
---*/
var bad = [];
function check(n, got, want) { if (got !== want) bad.push(n); }

// shadow every primitive prototype, then ask for it back by every route
Number.prototype.__p = 1;
String.prototype.__p = 1;
Boolean.prototype.__p = 1;
Symbol.prototype.__p = 1;

check("getPrototypeOf(5)",        Object.getPrototypeOf(5),      Number.prototype);
check("getPrototypeOf('s')",      Object.getPrototypeOf("s"),    String.prototype);
check("getPrototypeOf(true)",     Object.getPrototypeOf(true),   Boolean.prototype);
check("getPrototypeOf(Object(5))",Object.getPrototypeOf(Object(5)), Number.prototype);
check("boxed __proto__",          Object(5).__proto__,           Number.prototype);
check("5 instanceof Number",      5 instanceof Number,           false);
check("Object(5) instanceof",     Object(5) instanceof Number,   true);
check("constructor via primitive",(5).constructor,               Number);
check("method this is boxed",     (function(){ Number.prototype.w = function(){ return Object.getPrototypeOf(this) }; return (5).w(); })(), Number.prototype);

if (bad.length) throw new Error("SHADOW ESCAPES: " + bad.join(", "));
