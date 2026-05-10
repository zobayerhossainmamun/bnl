#include "stdlib/registry.h"

#include <fmt/core.h>
#include <sqlite3.h>

#include <cmath>
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

// ---------- handle ---------------------------------------------------------

struct DbHandle {
    sqlite3*              conn  = nullptr;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

void close_db(DbHandle* db) {
    if (db->conn) {
        sqlite3_close(db->conn);
        db->conn = nullptr;
    }
    if (db->alive) *db->alive = false;
}

// RAII finalize for prepared statements — keeps every error path leak-free.
struct StmtGuard {
    sqlite3_stmt* s = nullptr;
    explicit StmtGuard(sqlite3_stmt* p) : s(p) {}
    ~StmtGuard() { if (s) sqlite3_finalize(s); }
    StmtGuard(const StmtGuard&) = delete;
    StmtGuard& operator=(const StmtGuard&) = delete;
};

// ---------- value bridges --------------------------------------------------

void bind_value(sqlite3* conn, sqlite3_stmt* stmt, int idx, const Value& v) {
    int rc = SQLITE_OK;
    if (v.is_null()) {
        rc = sqlite3_bind_null(stmt, idx);
    } else if (v.is_bool()) {
        rc = sqlite3_bind_int(stmt, idx, v.as_bool() ? 1 : 0);
    } else if (v.is_number()) {
        // Bind whole-numbered values as int64 to preserve precision past 2^53.
        double d = v.as_number();
        if (std::trunc(d) == d
            && d >= -9.2233720368547758e18 && d <= 9.2233720368547758e18) {
            rc = sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(d));
        } else {
            rc = sqlite3_bind_double(stmt, idx, d);
        }
    } else if (v.is_string()) {
        const auto& s = v.as_string();
        rc = sqlite3_bind_text(stmt, idx, s.data(),
                               static_cast<int>(s.size()), SQLITE_TRANSIENT);
    } else {
        throw std::runtime_error(fmt::format(
            "sqlite: cannot bind value of type {}", v.type_name()));
    }
    if (rc != SQLITE_OK)
        throw std::runtime_error(fmt::format("sqlite bind: {}", sqlite3_errmsg(conn)));
}

void bind_params(sqlite3* conn, sqlite3_stmt* stmt, const Value& params) {
    if (params.is_null()) {
        if (sqlite3_bind_parameter_count(stmt) != 0)
            throw std::runtime_error(fmt::format(
                "sqlite: query expects {} params, got 0",
                sqlite3_bind_parameter_count(stmt)));
        return;
    }
    if (!params.is_list())
        throw std::runtime_error("sqlite: params must be a list (or null)");
    const auto& list = *params.as_list();
    int expected = sqlite3_bind_parameter_count(stmt);
    if (static_cast<int>(list.size()) != expected) {
        throw std::runtime_error(fmt::format(
            "sqlite: query expects {} params, got {}", expected, list.size()));
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        bind_value(conn, stmt, static_cast<int>(i) + 1, list[i]);
    }
}

Value column_value(sqlite3_stmt* stmt, int col) {
    switch (sqlite3_column_type(stmt, col)) {
        case SQLITE_INTEGER:
            return Value{static_cast<double>(sqlite3_column_int64(stmt, col))};
        case SQLITE_FLOAT:
            return Value{sqlite3_column_double(stmt, col)};
        case SQLITE_TEXT: {
            const char* s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            int n = sqlite3_column_bytes(stmt, col);
            return Value{std::string(s, static_cast<std::size_t>(n))};
        }
        case SQLITE_BLOB: {
            const void* p = sqlite3_column_blob(stmt, col);
            int n = sqlite3_column_bytes(stmt, col);
            return Value{std::string(reinterpret_cast<const char*>(p),
                                     static_cast<std::size_t>(n))};
        }
        case SQLITE_NULL:
        default:
            return Value{};
    }
}

CallablePtr to_optional_params(std::vector<Value>& args) {
    // Helper lifts the optional-second-arg pattern out of every fn body.
    (void)args;
    return nullptr;  // unused — kept the signature simple; see fn bodies
}

// ---------- per-db module --------------------------------------------------

ModulePtr build_db_module(DbHandle* db) {
    auto alive = db->alive;

    // db.exec(sql [, params]) — runs a non-query statement. Returns null.
    auto exec_fn = [db, alive](Interpreter&, std::vector<Value> args) -> Value {
        if (!*alive) throw std::runtime_error("sqlite.exec: database is closed");
        if (args.empty() || args.size() > 2)
            throw std::runtime_error("sqlite.exec(sql [, params]): expects 1 or 2 args");
        if (!args[0].is_string())
            throw std::runtime_error("sqlite.exec: sql must be a string");
        sqlite3_stmt* raw = nullptr;
        const auto& sql = args[0].as_string();
        int rc = sqlite3_prepare_v2(db->conn, sql.c_str(), -1, &raw, nullptr);
        if (rc != SQLITE_OK)
            throw std::runtime_error(fmt::format(
                "sqlite.exec prepare: {}", sqlite3_errmsg(db->conn)));
        StmtGuard stmt(raw);
        bind_params(db->conn, stmt.s, args.size() == 2 ? args[1] : Value{});
        rc = sqlite3_step(stmt.s);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW)
            throw std::runtime_error(fmt::format(
                "sqlite.exec step: {}", sqlite3_errmsg(db->conn)));
        return Value{};
    };

    // db.query(sql [, params]) — runs a SELECT and returns a list of row maps.
    // Each row is a map keyed by column name (alias if you used "AS").
    auto query_fn = [db, alive](Interpreter&, std::vector<Value> args) -> Value {
        if (!*alive) throw std::runtime_error("sqlite.query: database is closed");
        if (args.empty() || args.size() > 2)
            throw std::runtime_error("sqlite.query(sql [, params]): expects 1 or 2 args");
        if (!args[0].is_string())
            throw std::runtime_error("sqlite.query: sql must be a string");
        sqlite3_stmt* raw = nullptr;
        const auto& sql = args[0].as_string();
        int rc = sqlite3_prepare_v2(db->conn, sql.c_str(), -1, &raw, nullptr);
        if (rc != SQLITE_OK)
            throw std::runtime_error(fmt::format(
                "sqlite.query prepare: {}", sqlite3_errmsg(db->conn)));
        StmtGuard stmt(raw);
        bind_params(db->conn, stmt.s, args.size() == 2 ? args[1] : Value{});

        int n_cols = sqlite3_column_count(stmt.s);
        std::vector<std::string> col_names(static_cast<std::size_t>(n_cols));
        for (int i = 0; i < n_cols; ++i)
            col_names[static_cast<std::size_t>(i)] = sqlite3_column_name(stmt.s, i);

        auto out = std::make_shared<std::vector<Value>>();
        while (true) {
            rc = sqlite3_step(stmt.s);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW)
                throw std::runtime_error(fmt::format(
                    "sqlite.query step: {}", sqlite3_errmsg(db->conn)));
            auto row = std::make_shared<std::unordered_map<std::string, Value>>();
            for (int i = 0; i < n_cols; ++i) {
                (*row)[col_names[static_cast<std::size_t>(i)]] = column_value(stmt.s, i);
            }
            out->emplace_back(Value{row});
        }
        return Value{out};
    };

    // db.query_one(sql [, params]) — first row as a map, or null if none.
    auto query_one_fn = [db, alive, query_fn](Interpreter& interp, std::vector<Value> args) -> Value {
        Value rows = query_fn(interp, std::move(args));
        const auto& list = *rows.as_list();
        if (list.empty()) return Value{};
        return list.front();
    };

    auto last_id_fn = [db, alive](Interpreter&, std::vector<Value>) -> Value {
        if (!*alive) throw std::runtime_error("sqlite.last_insert_id: database is closed");
        return Value{static_cast<double>(sqlite3_last_insert_rowid(db->conn))};
    };

    auto changes_fn = [db, alive](Interpreter&, std::vector<Value>) -> Value {
        if (!*alive) throw std::runtime_error("sqlite.changes: database is closed");
        return Value{static_cast<double>(sqlite3_changes(db->conn))};
    };

    auto close_fn = [db, alive](Interpreter&, std::vector<Value>) -> Value {
        if (!*alive) return Value{};
        close_db(db);
        delete db;
        return Value{};
    };

    return NativeModule("sqlite_db")
        .add_function("exec",          -1, exec_fn)
        .add_function("query",         -1, query_fn)
        .add_function("query_one",     -1, query_one_fn)
        .add_function("last_insert_id", 0, last_id_fn)
        .add_function("changes",        0, changes_fn)
        .add_function("close",          0, close_fn)
        .build();
}

}  // namespace

void register_sqlite(Interpreter& interp) {
    auto m = NativeModule("sqlite")
        // sqlite.open(path) -> db. ":memory:" for an in-memory database.
        .add_function("open", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("sqlite.open: path must be a string");
                const auto& path = args[0].as_string();
                sqlite3* conn = nullptr;
                int rc = sqlite3_open(path.c_str(), &conn);
                if (rc != SQLITE_OK) {
                    std::string err = conn ? sqlite3_errmsg(conn) : sqlite3_errstr(rc);
                    if (conn) sqlite3_close(conn);
                    throw std::runtime_error(fmt::format(
                        "sqlite.open '{}': {}", path, err));
                }
                auto* db = new DbHandle{};
                db->conn = conn;
                return Value{build_db_module(db)};
            })

        // sqlite.version() -> the linked libsqlite3 version string.
        .add_function("version", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                return Value{std::string(sqlite3_libversion())};
            })

        .build();

    interp.register_native_module("sqlite", m);
}

}  // namespace bnl
