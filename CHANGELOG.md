# Changelog

All notable changes to the bnl language and runtime are documented in this
file. The format follows [Keep a Changelog](https://keepachangelog.com), and
the project follows [Semantic Versioning](https://semver.org).

## v2.0.0 — 2026-05-13

### Breaking changes

- **`lib/request.bnl` is now Future-native.** Every callback signature has
  been removed. `r.get(url, cb)` / `r.post(url, data, cb)` / etc. now return
  a `Future` and accept an optional axios-style `config` map.
  ```bnl
  // before
  r.get(url, function (err, resp) { ... });

  // after
  var resp = wait r.get(url);
  // or:
  var resp = wait r.get(url, { headers: { ... }, timeout: 3000 });
  ```
  Removed: `r.send` alias, `_async` variants (now the primaries),
  `r.download_with`, `r.upload_with` (replaced by config arg on
  `r.download(url, dest, config)` / `r.upload(url, file, config)`).

- **Native `io` module renamed to `_io`.** `import "io" as io;` now loads the
  new `lib/io.bnl` facade, which exposes the same sync API and Future-returning
  `*_async` variants. Low-level callback-style code (open_read/open_write with
  callbacks) must `import "_io"`.

- **`lib/async.bnl` removed.** The series/parallel/waterfall/each/map
  composition helpers are gone. Replace with `Future` + `wait` + `Future.all`:
  ```bnl
  // before
  async.parallel([t1, t2], function (err, results) { ... });

  // after
  var results = wait Future.all([t1, t2]);
  ```

### Added

- **`Future` built-in type + `wait` keyword.** The primary async story.
  - `Future(executor)` constructor, `Future.of(v)` / `Future.err(e)` /
    `Future.all([...])` / `Future.race([...])` / `Future.all_settled([...])`.
  - Methods `.next(on_ok)` / `.fail(on_err)` / `.always(fn)` / `.state`.
  - `wait expr` suspends the current function until a Future settles.
    Supported inside blocks, `if`/`else`, `while`, `for-of`, and `try`/`catch`.
- **`futurify(fn)`** — wraps an `(args..., (err, result) cb)` callback function
  as one that returns a Future.
- **`timers.delay(ms)`** — Future-returning sleep. Use with `wait`.
- **Default parameter values.** `function f(a, b = 1)` — defaults are
  tail-only and evaluated lazily per call in the function's call env.
- **Bangla aliases** for every module, global, and keyword (e.g.
  `import "অনুরোধ"` ↔ `import "request"`, `ভবিষ্যৎ` ↔ `Future`,
  `অপেক্ষা` ↔ `wait`). Single source of truth in
  `src/runtime/bn_aliases.h`.
- **`lib/io.bnl`** — public io facade re-exporting sync ops verbatim and
  wrapping every async/streaming op with `futurify`.

### Fixed

- Microtask drain handle (`uv_prepare_t`) was permanently `uv_unref`'d,
  causing the loop to exit before draining Future continuations when an
  I/O callback resolved a Future. Now refs while microtasks are pending
  and unrefs when drained.

### Migration

Run `git grep -E '(r|req|request)\.(get|post|put|patch|head|delete|send)\('`
in your project to find call sites that need updating, then convert each to
the Future-returning form (above).

For `async.X` calls: rewrite the surrounding function to use `wait` and
`Future.all` directly.

For `io.read_file_async(path, cb)` and `io.write_file_async(path, data, cb)`:
drop the callback, `wait` the return value.

For `io.open_read` / `io.open_write` callback-based streaming: switch to
`import "_io"` for the same callback API, or use the Future-returning stream
methods on the public `io` lib.


## v1.0.0

Initial public release.
