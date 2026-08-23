/*---
description: request 1 installs stack hooks
esid: x
---*/
Error.stackTraceLimit = 1;
Error.prepareStackTrace = function () { return "LEAKED FROM REQUEST 1"; };
