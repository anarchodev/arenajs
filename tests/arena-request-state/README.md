# arena-request-state

ARENA_PLAN class 4: per-request mutable state that lives on the
base-resident `JSRuntime` / `JSContext`. Each field here writes the
snapshot unless it has a request-side twin, and the value then outlives
the request.

Order matters. `01_*` installs the hooks and `02_*` asserts a later
request sees neither, so the walker's alphabetical order is the test.

The `prepareStackTrace` case is the one that motivated this: it stores a
**function**, so base ends up holding a request pointer whose closure
dangles once the arena is rewound, and every later request's
`build_backtrace` calls it. Under an object-capability design it is also
an ambient-authority hook -- a dependency that sets it observes every
error thrown by every other module, including capability-holding code.

`03_*` covers the subtlety that made these need an explicit "was set"
flag rather than the UNDEFINED sentinel `error_back_trace_req` uses:
assigning `undefined` is meaningful for both fields (it disables the hook
/ the limit), so a value sentinel would fall through to the base value
and silently ignore the request's own assignment.

`04_*` covers `convert_fast_array_to_array`, which had both class-4 bugs
at once -- writing the base flag directly instead of the per-request
mark, and comparing a possibly-shadow pointer against the base
`class_proto`.

Run both ways; a failure under `ARENA_RUNTIME=vanilla` is an engine gap,
one under arena alone is ours.
