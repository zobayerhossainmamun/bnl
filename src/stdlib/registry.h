#pragma once

namespace bnl {

class Interpreter;

// Each registers one native module under its conventional name on
// the given interpreter (so `import "sys" as sys;` finds it).
//
// Core ships these intrinsic modules:
//   sys     — process meta (args, env, platform, exit)
//   io      — local file ops (sync + async)
//   timers  — event-loop scheduling (set / interval)
//   regex   — pattern matching (compile / test / match / replace / split)
//   crypto  — randomness, hashes, HMAC, base64/hex codecs (OpenSSL)
//   net     — TCP sockets + IPv4 DNS (libuv)
//   http    — request/response parsing (llhttp)
//   tls     — TLS client + server over net (OpenSSL BIO-pair + libuv)
//
// Anything else (json, sqlite, …) is delivered as a plugin by the separate
// package manager.
void register_sys   (Interpreter& interp);
void register_io    (Interpreter& interp);
void register_timers(Interpreter& interp);
void register_regex (Interpreter& interp);
void register_crypto(Interpreter& interp);
void register_net   (Interpreter& interp);
void register_http  (Interpreter& interp);
void register_tls   (Interpreter& interp);

}  // namespace bnl
