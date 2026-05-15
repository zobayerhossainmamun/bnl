# bnl — developer docs

Internal documentation for working on the bnl interpreter and runtime.
These docs are written for **developers reading the source**, not for end
users writing bnl scripts. (User-facing language reference lives elsewhere.)

| Doc | What it covers |
|---|---|
| [architecture.md](./architecture.md) | How bnl works internally — pipeline, value model, module system, async model, build system |
| [extending.md](./extending.md) | Practical: how to add a built-in module, a plugin, or a pure-bnl helper |

## 30-second pitch (what bnl is)

A **bilingual programming language** (Bangla + English identifiers and
keywords) implemented as a **tree-walking interpreter in C++20** on top of
**libuv** for async I/O. Ships as a self-contained single binary (~5 MB)
with a batteries-included standard library: file I/O, timers, regex,
crypto, networking (TCP/HTTP/TLS), JSON, sqlite/postgres/mongo drivers,
and 25 pure-bnl helper modules.

Source is closed; the binary is free to use for personal, educational,
and commercial work. The C plugin ABI (`include/bnl/plugin.h`) is public
and stable — anyone can ship plugins.
