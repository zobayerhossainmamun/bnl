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
//   json    — JSON parse / stringify (nlohmann_json)
//   _exec   — child process spawn with stdin/stdout/stderr pipes (libuv).
//             Registered with a leading underscore so the public "exec"
//             import resolves to lib/exec.bnl (which adds run helpers).
//   _dns    — DNS lookup / reverse via uv_getaddrinfo + uv_getnameinfo.
//             Same convention: public "dns" name belongs to lib/dns.bnl.
//   _sqlite — SQLite open / exec / query / etc (vcpkg sqlite3).
//             Public "sqlite" name belongs to lib/sqlite.bnl
//             (transaction / migrate / insert helpers).
//   _pg     — PostgreSQL connect / exec / query / etc (vcpkg libpq).
//             Public "pg" name belongs to lib/pg.bnl
//             (transaction / migrate / insert helpers + map-form connect).
//   _mysql  — MySQL/MariaDB via vcpkg libmariadb (LGPL, wire-compatible
//             with MySQL servers). Public "mysql" name belongs to
//             lib/mysql.bnl. Uses prepared statements + `?` placeholders.
//   _mongo  — MongoDB via vcpkg mongo-c-driver (Apache-2.0). Collection-
//             shaped API rather than SQL: connect → client → db →
//             collection → {insert_one/find/...}. Public "mongo" name
//             belongs to lib/mongo.bnl.
//   _math   — libm wrapper (sqrt/sin/cos/log/...). Public "math" belongs
//             to lib/math.bnl (adds min/max/clamp/lerp helpers).
//   _random — Mersenne-Twister PRNG primitives. Public "random" belongs
//             to lib/random.bnl (int/float/choice/shuffle/sample helpers).
//   _time   — system + monotonic clocks, tm-struct conversion. Public
//             "time" belongs to lib/time.bnl (ISO 8601 + arithmetic).
//   _zlib   — gzip + raw deflate/inflate via zlib (transitive dep of
//             libpq, no new vcpkg port). Public "zlib" in lib/zlib.bnl.
//
// Anything else is delivered as a plugin by the separate package manager.
void register_sys   (Interpreter& interp);
void register_io    (Interpreter& interp);
void register_timers(Interpreter& interp);
void register_regex (Interpreter& interp);
void register_crypto(Interpreter& interp);
void register_net   (Interpreter& interp);
void register_http  (Interpreter& interp);
void register_tls   (Interpreter& interp);
void register_json  (Interpreter& interp);
void register_exec  (Interpreter& interp);
void register_dns   (Interpreter& interp);
void register_sqlite(Interpreter& interp);
void register_pg    (Interpreter& interp);
void register_mysql (Interpreter& interp);
void register_mongo (Interpreter& interp);
void register_math  (Interpreter& interp);
void register_random(Interpreter& interp);
void register_time  (Interpreter& interp);
void register_zlib  (Interpreter& interp);

}  // namespace bnl
