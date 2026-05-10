#include "stdlib/registry.h"

#include <fmt/core.h>
#include <sqlite3.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
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
// SQLite is fully synchronous so the wiring is simpler than the async
// modules: each call prepares + binds + steps + finalizes inline. The db
// handle lives in a shared_ptr<DbState> with a custom deleter — every method
// lambda captures a copy, so the connection stays open as long as ANY method
// reference is alive (the user's `db` Value, plus any captured method refs).
// Calling db.close() flips a flag + closes early; the deleter is then a
// no-op when the last reference drops.
// ===========================================================================

struct DbState {
    sqlite3* db     = nullptr;
    bool     closed = false;
};

void close_db(DbState* state) {
    if (state->closed) return;
    state->closed = true;
    if (state->db) {
        sqlite3_close_v2(state->db);
        state->db = nullptr;
    }
}

[[noreturn]] void throw_sqlite(sqlite3* db, const char* op) {
    const char* msg = db ? sqlite3_errmsg(db) : "(no db)";
    throw std::runtime_error(std::string("sqlite ") + op + ": " + msg);
}

// ---------- value <-> sqlite type marshalling -----------------------------

// Bind one positional `?` placeholder. Index is 1-based per sqlite convention.
void bind_one(sqlite3_stmt* stmt, int idx, const Value& v, sqlite3* db) {
    int rc = SQLITE_OK;
    if (v.is_null()) {
        rc = sqlite3_bind_null(stmt, idx);
    } else if (v.is_bool()) {
        rc = sqlite3_bind_int64(stmt, idx, v.as_bool() ? 1 : 0);
    } else if (v.is_number()) {
        // Integer-valued doubles bind as INTEGER (matches the json.cpp +
        // print formatting choice — keeps round-trips clean).
        double n = v.as_number();
        double i_part;
        if (std::isfinite(n) && std::modf(n, &i_part) == 0.0
            && std::abs(n) < 9.0e15) {
            rc = sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(n));
        } else {
            rc = sqlite3_bind_double(stmt, idx, n);
        }
    } else if (v.is_string()) {
        const auto& s = v.as_string();
        // SQLITE_TRANSIENT makes sqlite copy the bytes — safe because the
        // bnl string may move/destroy before step() runs.
        rc = sqlite3_bind_text(stmt, idx, s.data(),
                               static_cast<int>(s.size()), SQLITE_TRANSIENT);
    } else {
        throw std::runtime_error(
            std::string("sqlite bind: cannot bind value of type ") + v.type_name());
    }
    if (rc != SQLITE_OK) throw_sqlite(db, "bind");
}

void bind_params(sqlite3_stmt* stmt, const std::vector<Value>& params, sqlite3* db) {
    int expected = sqlite3_bind_parameter_count(stmt);
    if (static_cast<int>(params.size()) != expected) {
        throw std::runtime_error(fmt::format(
            "sqlite: param count mismatch — SQL has {} placeholder(s), got {}",
            expected, params.size()));
    }
    for (int i = 0; i < expected; ++i) bind_one(stmt, i + 1, params[i], db);
}

Value column_to_value(sqlite3_stmt* stmt, int i) {
    switch (sqlite3_column_type(stmt, i)) {
        case SQLITE_NULL:    return Value{};
        case SQLITE_INTEGER: return Value{static_cast<double>(sqlite3_column_int64(stmt, i))};
        case SQLITE_FLOAT:   return Value{sqlite3_column_double(stmt, i)};
        case SQLITE_TEXT: {
            const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
            int         n = sqlite3_column_bytes(stmt, i);
            return Value{std::string(p, static_cast<std::size_t>(n))};
        }
        case SQLITE_BLOB: {
            const char* p = static_cast<const char*>(sqlite3_column_blob(stmt, i));
            int         n = sqlite3_column_bytes(stmt, i);
            return Value{std::string(p, static_cast<std::size_t>(n))};
        }
        default: return Value{};
    }
}

Value row_to_map(sqlite3_stmt* stmt) {
    int  n = sqlite3_column_count(stmt);
    auto m = std::make_shared<std::unordered_map<std::string, Value>>();
    for (int i = 0; i < n; ++i) {
        const char* name = sqlite3_column_name(stmt, i);
        (*m)[name ? name : ""] = column_to_value(stmt, i);
    }
    return Value{m};
}

// RAII finalize so step() exceptions don't leak prepared statements.
struct StmtFinalize {
    sqlite3_stmt* s;
    ~StmtFinalize() { if (s) sqlite3_finalize(s); }
};

// ---------- prepared-statement runners ------------------------------------

void exec_one(sqlite3* db, const std::string& sql, const std::vector<Value>* params) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        throw_sqlite(db, "prepare");
    StmtFinalize fin{stmt};

    if (params) bind_params(stmt, *params, db);

    while (true) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) return;
        if (rc == SQLITE_ROW)  continue;  // exec discards rows
        throw_sqlite(db, "step");
    }
}

Value query_all(sqlite3* db, const std::string& sql, const std::vector<Value>* params) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        throw_sqlite(db, "prepare");
    StmtFinalize fin{stmt};

    if (params) bind_params(stmt, *params, db);

    auto out = std::make_shared<std::vector<Value>>();
    while (true) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc == SQLITE_ROW)  { out->push_back(row_to_map(stmt)); continue; }
        throw_sqlite(db, "step");
    }
    return Value{out};
}

Value query_first(sqlite3* db, const std::string& sql, const std::vector<Value>* params) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        throw_sqlite(db, "prepare");
    StmtFinalize fin{stmt};

    if (params) bind_params(stmt, *params, db);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) return Value{};
    if (rc == SQLITE_ROW)  return row_to_map(stmt);
    throw_sqlite(db, "step");
}

// ---------- argument helpers -----------------------------------------------

const std::string& require_string(const Value& v, const char* where) {
    if (!v.is_string()) throw std::runtime_error(std::string(where) + ": expected string");
    return v.as_string();
}

// (sql) or (sql, params) — params null treated as missing.
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

// ---------- db module exposed to bnl ---------------------------------------

ModulePtr build_db_module(sqlite3* raw_db) {
    // shared_ptr with a custom deleter that closes the connection (idempotent
    // with explicit close()). Each lambda captures a copy, so the db stays
    // open as long as anyone holds a callable from it.
    std::shared_ptr<DbState> st{new DbState{raw_db, false},
        [](DbState* s) { close_db(s); delete s; }};

    auto require_open = [](const std::shared_ptr<DbState>& s, const char* fn) {
        if (s->closed)
            throw std::runtime_error(std::string("db.") + fn + ": db is closed");
    };

    return NativeModule("db")

        // db.exec(sql, params?) — run a non-query statement (CREATE, INSERT,
        // UPDATE, DELETE, BEGIN, COMMIT, …). Throws on error. Returns null.
        .add_function("exec", -1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "exec");
                std::string sql; std::vector<Value> params; bool has_params;
                parse_sql_args(args, "exec", sql, params, has_params);
                exec_one(st->db, sql, has_params ? &params : nullptr);
                return Value{};
            })

        // db.query(sql, params?) — SELECT, returns list of {col_name: value, …}
        // maps (one per row). Empty list when no rows match.
        .add_function("query", -1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "query");
                std::string sql; std::vector<Value> params; bool has_params;
                parse_sql_args(args, "query", sql, params, has_params);
                return query_all(st->db, sql, has_params ? &params : nullptr);
            })

        // db.query_one(sql, params?) — first matching row, or null when none.
        // Add LIMIT 1 yourself if you don't need sqlite to scan the rest.
        .add_function("query_one", -1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "query_one");
                std::string sql; std::vector<Value> params; bool has_params;
                parse_sql_args(args, "query_one", sql, params, has_params);
                return query_first(st->db, sql, has_params ? &params : nullptr);
            })

        // db.last_insert_id() — sqlite3_last_insert_rowid as a number.
        .add_function("last_insert_id", 0,
            [st, require_open](Interpreter&, std::vector<Value>) -> Value {
                require_open(st, "last_insert_id");
                return Value{static_cast<double>(sqlite3_last_insert_rowid(st->db))};
            })

        // db.changes() — rows touched by the last INSERT/UPDATE/DELETE.
        .add_function("changes", 0,
            [st, require_open](Interpreter&, std::vector<Value>) -> Value {
                require_open(st, "changes");
                return Value{static_cast<double>(sqlite3_changes(st->db))};
            })

        // db.close() — release the connection. Subsequent calls throw.
        // Idempotent. Auto-called when the last reference to the db drops.
        .add_function("close", 0,
            [st](Interpreter&, std::vector<Value>) -> Value {
                close_db(st.get());
                return Value{};
            })

        .build();
}

}  // namespace

// Registered as "_sqlite" so the public "sqlite" name belongs to
// lib/sqlite.bnl (which adds transaction / migrate / insert helpers).
void register_sqlite(Interpreter& interp) {
    auto m = NativeModule("_sqlite")

        // sqlite.open(path) — open or create a database file. ":memory:"
        // gives a private in-memory db (great for tests).
        .add_function("open", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                const std::string& path = require_string(args[0], "sqlite.open: path");
                sqlite3* raw = nullptr;
                int rc = sqlite3_open(path.c_str(), &raw);
                if (rc != SQLITE_OK) {
                    std::string msg = raw ? sqlite3_errmsg(raw) : sqlite3_errstr(rc);
                    if (raw) sqlite3_close_v2(raw);
                    throw std::runtime_error("sqlite.open: " + msg);
                }
                return Value{build_db_module(raw)};
            })

        // sqlite.version() — sqlite library version string ("3.47.2" etc).
        .add_function("version", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                return Value{std::string(sqlite3_libversion())};
            })

        .build();

    interp.register_native_module("_sqlite", m);
}

}  // namespace bnl
