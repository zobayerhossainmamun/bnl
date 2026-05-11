#include "stdlib/registry.h"

#include <fmt/core.h>
#include <libpq-fe.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/native_module.h"

namespace bnl {

namespace {

// ===========================================================================
// PostgreSQL via libpq. Mirrors sqlite.cpp's structure: sync libpq calls,
// shared_ptr<DbState> lifetime so every method capture keeps the connection
// alive, RAII guards around PGresult* so step throws don't leak result memory.
//
// Differences from sqlite:
//   - Placeholders are $1, $2, ... (libpq's native syntax) — NOT ?.
//   - No global last_insert_id; bnl-side `lib/pg.bnl` exposes RETURNING-id
//     idioms via insert(). The native module only exposes `changes()`.
//   - All params are bound as text-format strings; the server casts to the
//     column type. This is the simplest correct path for v1; binary params
//     are a later optimization.
//   - Result decoding is also text-format. Numeric OIDs (int2/4/8, float4/8,
//     numeric, bool) come back as bnl number/bool. Everything else stays
//     as a string (date, timestamp, uuid, json, bytea text encoding, ...).
// ===========================================================================

struct DbState {
    PGconn* conn         = nullptr;
    bool    closed       = false;
    long    last_changes = 0;   // refreshed after each exec/query
};

void close_db(DbState* s) {
    if (s->closed) return;
    s->closed = true;
    if (s->conn) {
        PQfinish(s->conn);
        s->conn = nullptr;
    }
}

std::string strip_trailing_newline(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

[[noreturn]] void throw_pg_conn(PGconn* c, const char* op) {
    std::string msg = c ? strip_trailing_newline(PQerrorMessage(c)) : "(no conn)";
    throw std::runtime_error(std::string("pg ") + op + ": " + msg);
}

[[noreturn]] void throw_pg_result(PGresult* r, const char* op) {
    throw std::runtime_error(std::string("pg ") + op + ": "
                             + strip_trailing_newline(PQresultErrorMessage(r)));
}

// RAII clear so step exceptions don't leak result handles.
struct ResultGuard {
    PGresult* r;
    ~ResultGuard() { if (r) PQclear(r); }
};

// ---------- value -> text param marshalling --------------------------------

// Owns the per-call string buffer and the const char** array libpq wants.
// A null entry in ptrs[] means "send SQL NULL" for that position.
struct ParamBuf {
    std::vector<std::string> strs;
    std::vector<const char*> ptrs;
};

ParamBuf build_params(const std::vector<Value>& params) {
    ParamBuf b;
    b.strs.resize(params.size());
    b.ptrs.resize(params.size());
    for (std::size_t i = 0; i < params.size(); ++i) {
        const Value& v = params[i];
        if (v.is_null()) {
            b.ptrs[i] = nullptr;
        } else if (v.is_bool()) {
            b.strs[i] = v.as_bool() ? "t" : "f";
            b.ptrs[i] = b.strs[i].c_str();
        } else if (v.is_number()) {
            double n = v.as_number();
            double i_part;
            // Same integer-vs-decimal rule as sqlite/json: integer-valued
            // doubles bind as plain integers so the server can target an
            // int column without surprise REAL coercions.
            if (std::isfinite(n) && std::modf(n, &i_part) == 0.0
                && std::abs(n) < 9.0e15) {
                b.strs[i] = std::to_string(static_cast<long long>(n));
            } else {
                b.strs[i] = fmt::format("{}", n);
            }
            b.ptrs[i] = b.strs[i].c_str();
        } else if (v.is_string()) {
            b.strs[i] = v.as_string();
            b.ptrs[i] = b.strs[i].c_str();
        } else {
            throw std::runtime_error(
                std::string("pg bind: cannot bind value of type ") + v.type_name());
        }
    }
    return b;
}

// ---------- result -> bnl Value decoding -----------------------------------

// PG type OIDs we know how to decode beyond "just a string". Anything not in
// this list comes through as a bnl string — the user can parse it themselves.
constexpr Oid OID_BOOL    = 16;
constexpr Oid OID_INT2    = 21;
constexpr Oid OID_INT4    = 23;
constexpr Oid OID_INT8    = 20;
constexpr Oid OID_FLOAT4  = 700;
constexpr Oid OID_FLOAT8  = 701;
constexpr Oid OID_NUMERIC = 1700;

Value column_to_value(PGresult* r, int row, int col) {
    if (PQgetisnull(r, row, col)) return Value{};
    const char* val = PQgetvalue(r, row, col);
    int         len = PQgetlength(r, row, col);
    Oid         t   = PQftype(r, col);

    switch (t) {
        case OID_BOOL:
            // text format for bool is single-char "t" or "f".
            return Value{val && val[0] == 't'};
        case OID_INT2:
        case OID_INT4:
        case OID_INT8:
        case OID_FLOAT4:
        case OID_FLOAT8:
        case OID_NUMERIC: {
            char*  end = nullptr;
            double d   = std::strtod(val, &end);
            if (end != val) return Value{d};
            return Value{std::string(val, static_cast<std::size_t>(len))};
        }
        default:
            return Value{std::string(val, static_cast<std::size_t>(len))};
    }
}

Value row_to_map(PGresult* r, int row) {
    int  n = PQnfields(r);
    auto m = std::make_shared<std::unordered_map<std::string, Value>>();
    for (int c = 0; c < n; ++c) {
        const char* name = PQfname(r, c);
        (*m)[name ? name : ""] = column_to_value(r, row, c);
    }
    return Value{m};
}

// ---------- query runners --------------------------------------------------

// Single execution helper used by exec / query / query_one. Caller owns the
// result and must PQclear (ResultGuard). Throws on any non-OK status.
PGresult* run_sql(DbState& st, const std::string& sql,
                  const std::vector<Value>* params) {
    PGresult* r = nullptr;
    if (!params || params->empty()) {
        r = PQexecParams(st.conn, sql.c_str(), 0,
                         nullptr, nullptr, nullptr, nullptr, 0);
    } else {
        ParamBuf b = build_params(*params);
        r = PQexecParams(st.conn, sql.c_str(),
                         static_cast<int>(b.ptrs.size()),
                         nullptr,         // paramTypes — server infers
                         b.ptrs.data(),
                         nullptr,         // paramLengths (text mode)
                         nullptr,         // paramFormats (text mode)
                         0);              // resultFormat — text
    }
    if (!r) throw_pg_conn(st.conn, "exec");
    ExecStatusType s = PQresultStatus(r);
    if (s != PGRES_COMMAND_OK && s != PGRES_TUPLES_OK) {
        ResultGuard g{r};
        throw_pg_result(r, "exec");
    }
    // Refresh changes counter: PQcmdTuples is a decimal-text count for
    // INSERT/UPDATE/DELETE/MOVE/FETCH/COPY; empty for SELECT (we overwrite
    // with the tuple count below).
    const char* ct = PQcmdTuples(r);
    if (ct && *ct) {
        st.last_changes = std::strtol(ct, nullptr, 10);
    } else if (s == PGRES_TUPLES_OK) {
        st.last_changes = PQntuples(r);
    } else {
        st.last_changes = 0;
    }
    return r;
}

void exec_one(DbState& st, const std::string& sql, const std::vector<Value>* params) {
    PGresult* r = run_sql(st, sql, params);
    PQclear(r);
}

Value query_all(DbState& st, const std::string& sql, const std::vector<Value>* params) {
    PGresult* r = run_sql(st, sql, params);
    ResultGuard g{r};
    auto out  = std::make_shared<std::vector<Value>>();
    int  rows = PQntuples(r);
    out->reserve(static_cast<std::size_t>(rows));
    for (int i = 0; i < rows; ++i) out->push_back(row_to_map(r, i));
    return Value{out};
}

Value query_first(DbState& st, const std::string& sql, const std::vector<Value>* params) {
    PGresult* r = run_sql(st, sql, params);
    ResultGuard g{r};
    if (PQntuples(r) == 0) return Value{};
    return row_to_map(r, 0);
}

// ---------- argument parsing -----------------------------------------------

const std::string& require_string(const Value& v, const char* where) {
    if (!v.is_string())
        throw std::runtime_error(std::string(where) + ": expected string");
    return v.as_string();
}

void parse_sql_args(const std::vector<Value>& args, const char* fn,
                    std::string& sql, std::vector<Value>& params, bool& has_params) {
    if (args.empty() || args.size() > 2)
        throw std::runtime_error(std::string("db.") + fn + "(sql, params?): wrong arity");
    sql        = require_string(args[0], (std::string("db.") + fn + ": sql").c_str());
    has_params = args.size() == 2 && !args[1].is_null();
    if (has_params) {
        if (!args[1].is_list())
            throw std::runtime_error(std::string("db.") + fn + ": params must be a list");
        params = *args[1].as_list();
    }
}

// ---------- db module ------------------------------------------------------

ModulePtr build_db_module(PGconn* raw) {
    std::shared_ptr<DbState> st{new DbState{raw, false, 0},
        [](DbState* s) { close_db(s); delete s; }};

    auto require_open = [](const std::shared_ptr<DbState>& s, const char* fn) {
        if (s->closed)
            throw std::runtime_error(std::string("db.") + fn + ": db is closed");
    };

    return NativeModule("db")

        // db.exec(sql, params?) — non-query (CREATE / INSERT / UPDATE /
        // DELETE / BEGIN / COMMIT). Returns null. Updates changes().
        // Postgres uses $1, $2, ... placeholders — NOT ?.
        .add_function("exec", -1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "exec");
                std::string sql; std::vector<Value> params; bool has_params;
                parse_sql_args(args, "exec", sql, params, has_params);
                exec_one(*st, sql, has_params ? &params : nullptr);
                return Value{};
            })

        // db.query(sql, params?) — SELECT (or any tuples-returning statement
        // like INSERT ... RETURNING). Returns list of {col_name: value, ...}
        // maps.
        .add_function("query", -1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "query");
                std::string sql; std::vector<Value> params; bool has_params;
                parse_sql_args(args, "query", sql, params, has_params);
                return query_all(*st, sql, has_params ? &params : nullptr);
            })

        // db.query_one(sql, params?) — first row or null.
        .add_function("query_one", -1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "query_one");
                std::string sql; std::vector<Value> params; bool has_params;
                parse_sql_args(args, "query_one", sql, params, has_params);
                return query_first(*st, sql, has_params ? &params : nullptr);
            })

        // db.changes() — rows affected by the last statement (DML), or row
        // count for SELECT. 0 for DDL / no-result.
        .add_function("changes", 0,
            [st, require_open](Interpreter&, std::vector<Value>) -> Value {
                require_open(st, "changes");
                return Value{static_cast<double>(st->last_changes)};
            })

        // db.close() — release the connection. Idempotent; the deleter
        // would close it anyway when the last reference drops.
        .add_function("close", 0,
            [st](Interpreter&, std::vector<Value>) -> Value {
                close_db(st.get());
                return Value{};
            })

        .build();
}

}  // namespace

// Registered as "_pg" so the public "pg" name belongs to lib/pg.bnl
// (which adds map-form connect + transaction / migrate / insert helpers).
void register_pg(Interpreter& interp) {
    auto m = NativeModule("_pg")

        // _pg.connect(conninfo) — conninfo is either a libpq keyword string
        // ("host=... port=... user=...") or a postgresql:// URI. Map-form
        // (e.g. `pg.connect({host: ..., port: ...})`) is provided by
        // lib/pg.bnl on top of this primitive.
        .add_function("connect", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                const std::string& s =
                    require_string(args[0], "pg.connect: conninfo");
                PGconn* c = PQconnectdb(s.c_str());
                if (!c)
                    throw std::runtime_error("pg.connect: PQconnectdb returned null");
                if (PQstatus(c) != CONNECTION_OK) {
                    std::string msg = strip_trailing_newline(PQerrorMessage(c));
                    PQfinish(c);
                    throw std::runtime_error("pg.connect: " + msg);
                }
                return Value{build_db_module(c)};
            })

        // _pg.version() — libpq client library version, formatted as the
        // standard "Mm.mi.pa" triple. PQlibVersion encodes major*10000 +
        // minor*100 + patch.
        .add_function("version", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                int v     = PQlibVersion();
                int major = v / 10000;
                int minor = (v / 100) % 100;
                int patch = v % 100;
                return Value{fmt::format("{}.{}.{}", major, minor, patch)};
            })

        .build();

    interp.register_native_module("_pg", m);
}

}  // namespace bnl
