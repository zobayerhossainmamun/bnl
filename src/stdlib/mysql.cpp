#include "stdlib/registry.h"

#include <fmt/core.h>
#include <mysql.h>

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
// MySQL / MariaDB via libmariadb (LGPL, wire-compatible with both MySQL and
// MariaDB servers). Mirrors sqlite.cpp / pg.cpp structure.
//
// Differences from PG:
//   - Placeholders are `?` (same as sqlite).
//   - Parameter and result binding go through prepared statements
//     (`mysql_stmt_*`). Params bind as typed buffers (LONGLONG/DOUBLE/TINY/
//     STRING), results are all bound as STRING and decoded by column type
//     on read — sidesteps per-column binary buffer sizing for the integer
//     and float family.
//   - last_insert_id() IS supported (MySQL has `mysql_stmt_insert_id`).
// ===========================================================================

struct DbState {
    MYSQL*    conn        = nullptr;
    bool      closed      = false;
    long long last_id     = 0;
    long      last_changes = 0;
};

void close_db(DbState* s) {
    if (s->closed) return;
    s->closed = true;
    if (s->conn) {
        mysql_close(s->conn);
        s->conn = nullptr;
    }
}

[[noreturn]] void throw_mysql(MYSQL* m, const char* op) {
    const char* err = m ? mysql_error(m) : "(no conn)";
    throw std::runtime_error(std::string("mysql ") + op + ": " + err);
}

[[noreturn]] void throw_stmt(MYSQL_STMT* s, const char* op) {
    const char* err = s ? mysql_stmt_error(s) : "(no stmt)";
    throw std::runtime_error(std::string("mysql ") + op + ": " + err);
}

// RAII for prepared statement handles so an exception mid-step doesn't leak
// the stmt slot on the connection.
struct StmtGuard {
    MYSQL_STMT* s;
    ~StmtGuard() { if (s) mysql_stmt_close(s); }
};

struct ResultMetaGuard {
    MYSQL_RES* r;
    ~ResultMetaGuard() { if (r) mysql_free_result(r); }
};

// ---------- value -> param marshalling ------------------------------------

// Stable backing for one set of bound parameters. All vectors are sized to
// params.size() up front so MYSQL_BIND::buffer pointers stay valid across
// the entire execute() call. `my_bool` is libmariadb's typedef for the
// is_null / error fields — distinct from C++ `bool`, hence its own vector.
struct ParamBuf {
    std::vector<MYSQL_BIND>    binds;
    std::vector<my_bool>       nulls;
    std::vector<unsigned long> lengths;
    std::vector<std::int64_t>  i64s;
    std::vector<double>        doubles;
    std::vector<std::int8_t>   tinys;
    std::vector<std::string>   strings;
};

ParamBuf build_params(const std::vector<Value>& params) {
    ParamBuf b;
    std::size_t n = params.size();
    b.binds.assign(n, MYSQL_BIND{});
    b.nulls.assign(n, 0);
    b.lengths.assign(n, 0);
    b.i64s.assign(n, 0);
    b.doubles.assign(n, 0.0);
    b.tinys.assign(n, 0);
    b.strings.resize(n);

    for (std::size_t i = 0; i < n; ++i) {
        MYSQL_BIND& bd = b.binds[i];
        bd.is_null = &b.nulls[i];
        bd.length  = &b.lengths[i];

        const Value& v = params[i];
        if (v.is_null()) {
            b.nulls[i]     = 1;
            bd.buffer_type = MYSQL_TYPE_NULL;
        } else if (v.is_bool()) {
            b.tinys[i]       = v.as_bool() ? 1 : 0;
            bd.buffer_type   = MYSQL_TYPE_TINY;
            bd.buffer        = &b.tinys[i];
            bd.buffer_length = sizeof(std::int8_t);
            bd.is_unsigned   = 0;
        } else if (v.is_number()) {
            double n_val = v.as_number();
            double i_part;
            // Match sqlite/pg: integer-valued doubles bind as integers so
            // the server can target an INT column without surprise casts.
            if (std::isfinite(n_val) && std::modf(n_val, &i_part) == 0.0
                && std::abs(n_val) < 9.0e15) {
                b.i64s[i]        = static_cast<std::int64_t>(n_val);
                bd.buffer_type   = MYSQL_TYPE_LONGLONG;
                bd.buffer        = &b.i64s[i];
                bd.buffer_length = sizeof(std::int64_t);
                bd.is_unsigned   = 0;
            } else {
                b.doubles[i]     = n_val;
                bd.buffer_type   = MYSQL_TYPE_DOUBLE;
                bd.buffer        = &b.doubles[i];
                bd.buffer_length = sizeof(double);
            }
        } else if (v.is_string()) {
            b.strings[i]   = v.as_string();
            b.lengths[i]   = static_cast<unsigned long>(b.strings[i].size());
            bd.buffer_type = MYSQL_TYPE_STRING;
            // libmariadb tolerates a null buffer with length 0, but be defensive.
            bd.buffer        = b.strings[i].empty()
                                 ? const_cast<char*>("")
                                 : b.strings[i].data();
            bd.buffer_length = static_cast<unsigned long>(b.strings[i].size());
        } else {
            throw std::runtime_error(
                std::string("mysql bind: cannot bind value of type ") + v.type_name());
        }
    }
    return b;
}

// ---------- result column metadata + decode -------------------------------

struct ResultCol {
    std::string       name;
    enum_field_types  type    = MYSQL_TYPE_STRING;
    std::vector<char> buffer;
    my_bool           is_null = 0;
    my_bool           error_  = 0;
    unsigned long     length  = 0;
};

bool is_int_field(enum_field_types t) {
    return t == MYSQL_TYPE_TINY  || t == MYSQL_TYPE_SHORT
        || t == MYSQL_TYPE_INT24 || t == MYSQL_TYPE_LONG
        || t == MYSQL_TYPE_LONGLONG;
}

bool is_float_field(enum_field_types t) {
    return t == MYSQL_TYPE_FLOAT || t == MYSQL_TYPE_DOUBLE
        || t == MYSQL_TYPE_DECIMAL || t == MYSQL_TYPE_NEWDECIMAL;
}

Value decode_text(const std::string& s, enum_field_types t) {
    if (is_int_field(t) || is_float_field(t)) {
        char*  end = nullptr;
        double d   = std::strtod(s.c_str(), &end);
        if (end != s.c_str()) return Value{d};
    }
    return Value{s};
}

// ---------- statement runners ---------------------------------------------

// Prepare + bind params + execute. Caller decides whether to read results.
// `out_stmt` is filled with the executed statement handle (owned by caller —
// they're expected to wrap it in StmtGuard).
void execute_stmt(DbState& st, const std::string& sql,
                  const std::vector<Value>* params, MYSQL_STMT** out_stmt) {
    MYSQL_STMT* stmt = mysql_stmt_init(st.conn);
    if (!stmt) throw_mysql(st.conn, "stmt_init");

    if (mysql_stmt_prepare(stmt, sql.data(),
                           static_cast<unsigned long>(sql.size())) != 0) {
        std::string err = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        throw std::runtime_error("mysql prepare: " + err);
    }

    unsigned long n_params = mysql_stmt_param_count(stmt);
    std::size_t   got      = params ? params->size() : 0;
    if (n_params != got) {
        mysql_stmt_close(stmt);
        throw std::runtime_error(fmt::format(
            "mysql: param count mismatch — SQL has {} placeholder(s), got {}",
            n_params, got));
    }

    ParamBuf pb;
    if (n_params > 0) {
        pb = build_params(*params);
        if (mysql_stmt_bind_param(stmt, pb.binds.data()) != 0) {
            std::string err = mysql_stmt_error(stmt);
            mysql_stmt_close(stmt);
            throw std::runtime_error("mysql bind_param: " + err);
        }
    }

    // Ask the server to populate field->max_length during store_result so we
    // can size string buffers precisely. Without this max_length stays 0 and
    // every column would need a guess (or a refit via fetch_column).
    my_bool update_max = 1;
    mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max);

    if (mysql_stmt_execute(stmt) != 0) {
        std::string err = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        throw std::runtime_error("mysql execute: " + err);
    }

    st.last_id      = static_cast<long long>(mysql_stmt_insert_id(stmt));
    st.last_changes = static_cast<long>(mysql_stmt_affected_rows(stmt));

    *out_stmt = stmt;
}

void exec_one(DbState& st, const std::string& sql, const std::vector<Value>* params) {
    MYSQL_STMT* stmt = nullptr;
    execute_stmt(st, sql, params, &stmt);
    StmtGuard g{stmt};
    // exec discards result sets (if any) — caller asked for no rows back.
    if (mysql_stmt_result_metadata(stmt) != nullptr) {
        // there IS a result set; consume it so the connection is ready for
        // the next query. store_result + free are enough.
        mysql_stmt_store_result(stmt);
    }
}

// Drive the result fetch loop. `stmt` must already have executed successfully.
Value collect_rows(MYSQL_STMT* stmt, bool first_only) {
    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
    if (!meta) {
        // No result set — return empty list (query) or null (query_one).
        if (first_only) return Value{};
        return Value{std::make_shared<std::vector<Value>>()};
    }
    ResultMetaGuard mg{meta};

    if (mysql_stmt_store_result(stmt) != 0) throw_stmt(stmt, "store_result");

    int          n_cols = static_cast<int>(mysql_num_fields(meta));
    MYSQL_FIELD* fields = mysql_fetch_fields(meta);

    std::vector<MYSQL_BIND> rbinds(static_cast<std::size_t>(n_cols), MYSQL_BIND{});
    std::vector<ResultCol> cols(static_cast<std::size_t>(n_cols));

    for (int i = 0; i < n_cols; ++i) {
        ResultCol& c = cols[static_cast<std::size_t>(i)];
        c.name = std::string(fields[i].name,
                             static_cast<std::size_t>(fields[i].name_length));
        c.type = fields[i].type;

        unsigned long cap = fields[i].max_length + 1;
        if (cap < 16) cap = 16;
        c.buffer.assign(cap, 0);

        MYSQL_BIND& b = rbinds[static_cast<std::size_t>(i)];
        b.buffer_type   = MYSQL_TYPE_STRING;
        b.buffer        = c.buffer.data();
        b.buffer_length = cap;
        b.is_null       = &c.is_null;
        b.length        = &c.length;
        b.error         = &c.error_;
    }

    if (mysql_stmt_bind_result(stmt, rbinds.data()) != 0)
        throw_stmt(stmt, "bind_result");

    auto rows = std::make_shared<std::vector<Value>>();

    while (true) {
        int rc = mysql_stmt_fetch(stmt);
        if (rc == MYSQL_NO_DATA) break;
        if (rc == 1)             throw_stmt(stmt, "fetch");
        // rc == 0 or MYSQL_DATA_TRUNCATED. With update_max + store_result
        // truncation shouldn't happen, but if it does we silently take the
        // truncated value — caller can switch to a different buffer type
        // if they need full fidelity.

        auto row = std::make_shared<std::unordered_map<std::string, Value>>();
        for (int i = 0; i < n_cols; ++i) {
            ResultCol& c = cols[static_cast<std::size_t>(i)];
            if (c.is_null) {
                (*row)[c.name] = Value{};
            } else {
                std::string s(c.buffer.data(),
                              static_cast<std::size_t>(c.length));
                (*row)[c.name] = decode_text(s, c.type);
            }
        }

        if (first_only) return Value{row};
        rows->push_back(Value{row});
    }

    if (first_only) return Value{};  // zero rows
    return Value{rows};
}

Value query_all(DbState& st, const std::string& sql, const std::vector<Value>* params) {
    MYSQL_STMT* stmt = nullptr;
    execute_stmt(st, sql, params, &stmt);
    StmtGuard g{stmt};
    return collect_rows(stmt, /*first_only=*/false);
}

Value query_first(DbState& st, const std::string& sql, const std::vector<Value>* params) {
    MYSQL_STMT* stmt = nullptr;
    execute_stmt(st, sql, params, &stmt);
    StmtGuard g{stmt};
    return collect_rows(stmt, /*first_only=*/true);
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

// ---------- connect -------------------------------------------------------

MYSQL* open_connection(const std::unordered_map<std::string, Value>& opts) {
    MYSQL* m = mysql_init(nullptr);
    if (!m) throw std::runtime_error("mysql.connect: mysql_init failed");

    auto get = [&](const char* k) -> const Value* {
        auto it = opts.find(k);
        return it == opts.end() ? nullptr : &it->second;
    };

    // `ssl: true` switches the connection to TLS with default server cert
    // handling. Fine-grained config (CA/cert/key paths) would route through
    // mysql_ssl_set's other args — left for v2.
    if (auto* v = get("ssl"); v && v->is_bool() && v->as_bool()) {
        mysql_ssl_set(m, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    if (auto* v = get("connect_timeout"); v && v->is_number()) {
        unsigned int t = static_cast<unsigned int>(v->as_number());
        mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &t);
    }

    std::string  host_s = "127.0.0.1";
    if (auto* v = get("host"); v && v->is_string()) host_s = v->as_string();

    unsigned int port = 3306;
    if (auto* v = get("port"); v && v->is_number())
        port = static_cast<unsigned int>(v->as_number());

    std::string user_s;
    if (auto* v = get("user"); v && v->is_string()) user_s = v->as_string();

    std::string pass_s;
    if (auto* v = get("password"); v && v->is_string()) pass_s = v->as_string();

    std::string db_s;
    bool        have_db = false;
    if (auto* v = get("database"); v && v->is_string()) {
        db_s    = v->as_string();
        have_db = true;
    }

    if (!mysql_real_connect(m, host_s.c_str(),
                            user_s.empty() ? nullptr : user_s.c_str(),
                            pass_s.empty() ? nullptr : pass_s.c_str(),
                            have_db        ? db_s.c_str() : nullptr,
                            port, nullptr, 0)) {
        std::string err = mysql_error(m);
        mysql_close(m);
        throw std::runtime_error("mysql.connect: " + err);
    }
    return m;
}

// ---------- db module ------------------------------------------------------

ModulePtr build_db_module(MYSQL* raw) {
    std::shared_ptr<DbState> st{new DbState{raw, false, 0, 0},
        [](DbState* s) { close_db(s); delete s; }};

    auto require_open = [](const std::shared_ptr<DbState>& s, const char* fn) {
        if (s->closed)
            throw std::runtime_error(std::string("db.") + fn + ": db is closed");
    };

    return NativeModule("db")

        .add_function("exec", -1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "exec");
                std::string sql; std::vector<Value> params; bool has_params;
                parse_sql_args(args, "exec", sql, params, has_params);
                exec_one(*st, sql, has_params ? &params : nullptr);
                return Value{};
            })

        .add_function("query", -1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "query");
                std::string sql; std::vector<Value> params; bool has_params;
                parse_sql_args(args, "query", sql, params, has_params);
                return query_all(*st, sql, has_params ? &params : nullptr);
            })

        .add_function("query_one", -1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "query_one");
                std::string sql; std::vector<Value> params; bool has_params;
                parse_sql_args(args, "query_one", sql, params, has_params);
                return query_first(*st, sql, has_params ? &params : nullptr);
            })

        .add_function("last_insert_id", 0,
            [st, require_open](Interpreter&, std::vector<Value>) -> Value {
                require_open(st, "last_insert_id");
                return Value{static_cast<double>(st->last_id)};
            })

        .add_function("changes", 0,
            [st, require_open](Interpreter&, std::vector<Value>) -> Value {
                require_open(st, "changes");
                return Value{static_cast<double>(st->last_changes)};
            })

        .add_function("close", 0,
            [st](Interpreter&, std::vector<Value>) -> Value {
                close_db(st.get());
                return Value{};
            })

        .build();
}

}  // namespace

void register_mysql(Interpreter& interp) {
    auto m = NativeModule("_mysql")

        // _mysql.connect(opts) — opts is a map with at minimum `user`. Other
        // keys: host (default 127.0.0.1), port (3306), password (""),
        // database (none), ssl (false), connect_timeout (libmariadb default).
        .add_function("connect", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_map())
                    throw std::runtime_error("mysql.connect: expected options map");
                MYSQL* c = open_connection(*args[0].as_map());
                return Value{build_db_module(c)};
            })

        // _mysql.version() — libmariadb client version string (e.g. "3.4.8").
        .add_function("version", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                return Value{std::string(mysql_get_client_info())};
            })

        .build();

    interp.register_native_module("_mysql", m);
}

}  // namespace bnl
