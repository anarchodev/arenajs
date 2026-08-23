/*---
description: converting the shadowed Array.prototype must not dirty base
esid: x
---*/
/* forces convert_fast_array_to_array on Array.prototype */
Object.defineProperty(Array.prototype, "0", { get: function () { return 1; }, configurable: true });
if ([][0] !== 1) throw new Error("fast path not invalidated: got " + [][0]);
