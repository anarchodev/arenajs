/*---
description: request 2 must see neither
esid: x
---*/
var bad = [];
if (Error.stackTraceLimit !== 10) bad.push("stackTraceLimit leaked: " + Error.stackTraceLimit);
if (Error.prepareStackTrace !== undefined) bad.push("prepareStackTrace leaked");
try { null.x; } catch (e) { if (String(e.stack).indexOf("LEAKED") !== -1) bad.push("hook ran across requests"); }
if (bad.length) throw new Error(bad.join("; "));
