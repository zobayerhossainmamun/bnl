# bnl docs

Reference docs for the bnl language and runtime.

| Doc | When to read |
|---|---|
| [Guide](./guide.md) | First time using bnl. Install, first program, projects, async, classes. |
| [Syntax](./syntax.md) | Language reference. Tokens, grammar, types, operators, statements. |
| [Dep development](./deps-dev.md) | Authoring a pure-bnl package distributed under `deps/`. |

bnl is a tree-walking interpreter for a Bangla+English language with Python-flavored
semantics, semicolons, and brackets. The runtime ships with three native modules
(`sys`, `io`, `timers`); everything else — crypto, networking, HTTP, databases —
is delivered as plugins or pure-bnl packages.
