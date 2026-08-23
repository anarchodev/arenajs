/*---
description: per-request stack config still behaves
esid: x
---*/
var bad = [];
Error.stackTraceLimit = 3;
if (Error.stackTraceLimit !== 3) bad.push("limit readback");
Error.prepareStackTrace = function (e, frames) { return "CUSTOM"; };
if (Error.prepareStackTrace === undefined) bad.push("prepare readback");
try { null.x; } catch (e) { if (e.stack !== "CUSTOM") bad.push("prepare not applied: " + e.stack); }
Error.prepareStackTrace = undefined;
if (Error.prepareStackTrace !== undefined) bad.push("assigning undefined ignored");
try { null.x; } catch (e) { if (e.stack === "CUSTOM") bad.push("undefined did not disable the hook"); }
if (bad.length) throw new Error(bad.join("; "));
