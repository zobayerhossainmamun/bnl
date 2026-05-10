#pragma once

namespace bnl {

class Interpreter;

// Each registers one native module under its conventional name on
// the given interpreter (so `import "sys" as sys;` finds it).
//
// Core ships only the four intrinsic modules:
//   sys     — process meta (args, env, platform, exit)
//   io      — local file ops (sync + async)
//   timers  — event-loop scheduling (set / interval)
//   regex   — pattern matching (compile / test / match / replace / split)
//
// Anything else (crypto, net, tls, http, json, sqlite, …) is delivered as
// a plugin by the separate package manager.
void register_sys   (Interpreter& interp);
void register_io    (Interpreter& interp);
void register_timers(Interpreter& interp);
void register_regex (Interpreter& interp);

}  // namespace bnl
