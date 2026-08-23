# arena-shadow

Identity probes for the base/shadow split, run by `arena-test262` like any
other test262 tree (they use test262 frontmatter and `assert`-free plain
`throw`, so they need no harness beyond the walker's preloads).

They exist because test262 cannot reach this. The invariant under test is
not a language rule — it is arenajs's own:

> **Base is the identity JS observes. The shadow is storage. It must never
> cross into JS.**

A shadow is a separate allocation created lazily on first write, so a
reference JS already holds points at base and can never be rewritten.
Shadow-identity would flip mid-request at an arbitrary moment; base is the
only stable one.

## Files

- `receivers.js` — method/getter/setter `this`, `Reflect.get`/`set`,
  `call`/`apply`/`bind`, `forEach` thisArg, Proxy traps
- `inherited-and-super.js` — accessors inherited from a shadowed prototype,
  `super`, boxed primitives, `__proto__` round-trips, descriptor functions
- `primitive-prototypes.js` — `Object.getPrototypeOf` on every primitive
- `coercion-and-iteration.js` — `Symbol.toPrimitive`, `valueOf`, `toJSON`,
  `Symbol.iterator`, and containers/callbacks that carry the object as a value
- `scope-and-generators.js` — `with` scope chains, generators suspending
  across the property machinery, `throw`/`catch`, array callbacks

Each file asserts that a shadowed base object compares equal to itself
however JS reaches it: method/getter/setter receivers, `Reflect.get`/`set`,
`call`/`apply`/`bind`, `forEach` thisArg, inherited accessors, `super`,
boxed primitives, `__proto__` round-trips, descriptor functions, Proxy
traps, and `Object.getPrototypeOf` on every primitive.

Run both ways to tell an arena bug from an engine gap:

    build/arena-test262 tests/arena-shadow
    ARENA_RUNTIME=vanilla build/arena-test262 tests/arena-shadow

Run them both ways when adding a case: a check that fails identically on
both runtimes is a bug in the probe, not an escape. That is how the
`JSON.stringify` replacer case here got caught -- returning `undefined`
from a replacer drops the root before any key is visited, so the check was
wrong rather than the engine.

Every one of these corresponds to a bug that shipped. `primitive-prototypes.js`
in particular catches a leak introduced *by the fix for another leak* --
resolving inside `JS_GetPrototypePrimitive` served its two lookup callers
and leaked to its third, whose result goes straight back to JS.
