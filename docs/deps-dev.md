# Dep development

How to author a pure-bnl dep: a directory of `.bnl` source that another project
imports by name.

## When to write a dep (vs a plugin)

| Choose this | When you need |
|---|---|
| Pure-bnl dep | Pure-bnl logic, no system calls beyond what built-ins (`sys`, `io`, `timers`) cover |
| FFI plugin | A C library binding, raw OS APIs, or hot-path performance |

You can mix: a dep can re-export functionality from a plugin it bundles.

## Anatomy

A dep is just a directory under `deps/<name>/`. The runtime decides what to load
by trying entry-point forms in this order:

1. `bnl.json` `"native"` field — covered in [plugin development](./plugin-dev.md).
2. `bnl.json` `"main"` field → resolves to a bnl source file.
3. `index.bnl` (Node-style fallback).
4. `<dep_name>.bnl` (single-file dep).

The first form that resolves wins.

## The four shapes

### Shape A — `bnl.json` with `main`

The standard layout. Recommended once your dep has more than a handful of
files.

```
deps/mylib/
├── bnl.json
└── src/
    └── index.bnl
```

`bnl.json`:

```json
{
    "name": "mylib",
    "version": "0.1.0",
    "description": "A pure-bnl helper library",
    "main": "src/index.bnl"
}
```

The `main` value is **relative to the dep dir**. Other fields (`name`,
`version`, `description`) are tolerated but currently ignored by the runtime;
a future package manager will use them.

### Shape B — `index.bnl` fallback

No manifest needed. The runtime falls back to `index.bnl` when `bnl.json`
is absent or doesn't have a usable `main` field.

```
deps/utils/
└── index.bnl
```

Good for small deps that don't need versioning yet.

### Shape C — `<name>.bnl` single-file fallback

Simplest possible drop-in: name the file after the dep dir.

```
deps/tag-helper/
└── tag-helper.bnl
```

This is the convention for one-file experiments where even `index.bnl` is
overkill.

### Shape D — `bnl.json` with `native` (FFI)

When the dep ships a compiled shared library — see
[plugin development](./plugin-dev.md). The same dep dir can host both a
`native` and a `main` field; `native` takes precedence.

## What goes in the entry file

Anything the dep wants to expose. The whole module is the file's top-level
environment. Names declared with `var`, `function`, or `class` become exports.

```bnl
// deps/mylib/src/index.bnl

var PI = 3.14;

function add(a, b) {
    return a + b;
}

class Point {
    function init(self, x, y) { self.x = x; self.y = y; }
}
```

Then in the consumer:

```bnl
import "mylib" as mylib;

print(mylib.PI);             // 3.14
print(mylib.add(2, 3));      // 5
var p = mylib.Point(1, 2);
print(p.x);                  // 1
```

There is no `export` keyword; everything top-level is reachable. There is no
named-import syntax (`import { add } from "mylib"`); access exports via the
module value.

## Internal modules

Split a dep across multiple files using ordinary relative imports — same as
inside an application:

```
deps/mylib/
├── bnl.json                    {"main": "src/index.bnl"}
└── src/
    ├── index.bnl
    ├── private.bnl             ← internal helpers
    └── public.bnl              ← re-exported pieces
```

`src/index.bnl`:

```bnl
import "./public.bnl"  as public;
import "./private.bnl" as priv;

var PI         = 3.14;
var add        = public.add;          // re-export
function build() {
    return priv.format(public.add(1, 2));
}
```

Relative imports inside a dep resolve **inside the dep**. Bare-name imports
inside a dep are resolved by the same chain as from the application — built-ins
first, then walk-up `deps/`. So a dep can `import "sys" as sys;` for built-ins,
and can also depend on other deps as long as those deps live in a shared
ancestor's `deps/` directory.

## Caching

Each module is cached by canonical path. If your dep is imported from multiple
files, the entry runs **once** per Interpreter — so:

- Top-level side effects (timers, file I/O, server start) fire once.
- Cycles between modules are detected and reported with a clear error.

## Project root and global deps

A directory becomes a *project* by having a `bnl.json` at the root that is not
itself inside a `deps/` directory. Inside a project, the runtime never falls
back to the global `~/.bnl/deps/` — every dep must be local. This guarantees
that a project's behavior is fully determined by its own contents.

To author a dep that's importable globally (outside any project), drop it
under `~/.bnl/deps/<name>/` (or `$BNL_HOME/deps/<name>/` if `BNL_HOME` is set).
The same four entry-point shapes apply.

## Distribution

For now, dep distribution is manual: zip the dep dir, share it, and the
consumer drops it into their `deps/` directory. A package manager is being
built that will automate this; until then, the format is stable so packages
won't need rewriting later.

## Reference examples

The integration test fixture at `tests/_fixtures/proj_full/deps/` exercises
all three pure-bnl shapes (A: `mathlib`, B: `greeter`, C: `flat-utils`) plus
the FFI shape (D: `mathx-plugin`). Read those for canonical layouts.
