#include "stdlib/registry.h"

#include <bson/bson.h>
#include <fmt/core.h>
#include <mongoc/mongoc.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
// MongoDB via mongo-c-driver (Apache-2.0). Collection-shaped API rather than
// SQL: connect → client → db → collection → {insert_one / find / ...}.
//
// Lifetime model:
//   ClientState  — shared_ptr; holds mongoc_client_t*.
//   DbState      — owns mongoc_database_t*, holds ClientState shared_ptr
//                  so the client outlives the db handle.
//   ColState     — owns mongoc_collection_t*, holds ClientState shared_ptr.
//
// BSON ↔ Value:
//   - null     ↔ BSON_TYPE_NULL
//   - bool     ↔ BSON_TYPE_BOOL
//   - number   ↔ INT32 (int-valued, fits 32-bit) / INT64 (int-valued, wider) /
//               DOUBLE (otherwise)
//   - string   ↔ UTF8
//   - map      ↔ DOCUMENT
//   - list     ↔ ARRAY (BSON requires keys to be "0", "1", "2", ...)
//   - read-only types we decode for the user but never produce on writes:
//       OID         → 24-char hex string
//       DATE_TIME   → number (ms since epoch)
//       BINARY      → raw bytes as a string
//       DECIMAL128  → decimal-text string
//       TIMESTAMP   → number (seconds | (increment << 32))
//   - other types (Regex, Code, Symbol, MinKey, MaxKey) decode to a marker
//     string so the user at least sees they exist; v1 doesn't allow writing
//     them.
// ===========================================================================

// One-time process init. mongoc_init/cleanup must be called once each per
// process; the static singleton handles that without coupling to any one
// Interpreter instance (so multi-host embedding still works).
struct MongoLifecycle {
    MongoLifecycle()  { mongoc_init(); }
    ~MongoLifecycle() { mongoc_cleanup(); }
};
void ensure_mongoc_init() {
    static MongoLifecycle inst;
    (void)inst;
}

// ---------- state structs --------------------------------------------------

struct ClientState {
    mongoc_client_t* client = nullptr;
    bool             closed = false;
};

void close_client(ClientState* s) {
    if (s->closed) return;
    s->closed = true;
    if (s->client) {
        mongoc_client_destroy(s->client);
        s->client = nullptr;
    }
}

[[noreturn]] void throw_bson_err(const bson_error_t& err, const char* op) {
    throw std::runtime_error(std::string("mongo ") + op + ": " + err.message);
}

// ---------- Value -> BSON --------------------------------------------------

void value_to_bson_field(bson_t* out, const char* key, int key_len, const Value& v);

void map_to_bson_doc(bson_t* out, const std::unordered_map<std::string, Value>& m) {
    for (const auto& [k, v] : m) {
        value_to_bson_field(out, k.c_str(), static_cast<int>(k.size()), v);
    }
}

void list_to_bson_array(bson_t* out, const std::vector<Value>& xs) {
    // BSON arrays are documents with stringified-integer keys ("0", "1", ...).
    for (std::size_t i = 0; i < xs.size(); ++i) {
        std::string k = std::to_string(i);
        value_to_bson_field(out, k.c_str(), static_cast<int>(k.size()), xs[i]);
    }
}

void value_to_bson_field(bson_t* out, const char* key, int key_len, const Value& v) {
    bool ok = true;
    if (v.is_null()) {
        ok = bson_append_null(out, key, key_len);
    } else if (v.is_bool()) {
        ok = bson_append_bool(out, key, key_len, v.as_bool());
    } else if (v.is_number()) {
        double n = v.as_number();
        double i_part;
        if (std::isfinite(n) && std::modf(n, &i_part) == 0.0
            && std::abs(n) < 9.0e15) {
            std::int64_t i64 = static_cast<std::int64_t>(n);
            if (i64 >= INT32_MIN && i64 <= INT32_MAX) {
                ok = bson_append_int32(out, key, key_len, static_cast<std::int32_t>(i64));
            } else {
                ok = bson_append_int64(out, key, key_len, i64);
            }
        } else {
            ok = bson_append_double(out, key, key_len, n);
        }
    } else if (v.is_string()) {
        const auto& s = v.as_string();
        ok = bson_append_utf8(out, key, key_len,
                              s.data(), static_cast<int>(s.size()));
    } else if (v.is_map()) {
        bson_t child;
        bson_append_document_begin(out, key, key_len, &child);
        map_to_bson_doc(&child, *v.as_map());
        bson_append_document_end(out, &child);
    } else if (v.is_list()) {
        // bson_append_array(parent, ..., child) attaches a fully-built array
        // — sidesteps the _begin/_end pair (now deprecated in libbson 2.x in
        // favour of the array_builder API, which would require a parallel
        // dispatch path we don't need at this size).
        bson_t arr;
        bson_init(&arr);
        list_to_bson_array(&arr, *v.as_list());
        ok = bson_append_array(out, key, key_len, &arr);
        bson_destroy(&arr);
    } else {
        throw std::runtime_error(
            std::string("mongo: cannot serialize value of type ") + v.type_name());
    }
    if (!ok) {
        throw std::runtime_error("mongo: bson_append failed (document too large?)");
    }
}

// Build a fresh bson_t* from a bnl map (the common case for filters / docs).
// Caller owns and must bson_destroy.
bson_t* map_to_new_bson(const Value& v) {
    if (!v.is_map())
        throw std::runtime_error("mongo: expected a map (BSON document)");
    bson_t* b = bson_new();
    try {
        map_to_bson_doc(b, *v.as_map());
    } catch (...) {
        bson_destroy(b);
        throw;
    }
    return b;
}

// ---------- BSON -> Value --------------------------------------------------

Value bson_iter_to_value(const bson_iter_t* it);

Value bson_doc_to_map(const bson_t* doc) {
    auto m = std::make_shared<std::unordered_map<std::string, Value>>();
    bson_iter_t it;
    if (!bson_iter_init(&it, doc)) return Value{m};
    while (bson_iter_next(&it)) {
        const char* key = bson_iter_key(&it);
        (*m)[key ? key : ""] = bson_iter_to_value(&it);
    }
    return Value{m};
}

Value bson_doc_to_array(const bson_t* doc) {
    auto out = std::make_shared<std::vector<Value>>();
    bson_iter_t it;
    if (!bson_iter_init(&it, doc)) return Value{out};
    while (bson_iter_next(&it)) {
        out->push_back(bson_iter_to_value(&it));
    }
    return Value{out};
}

Value bson_iter_to_value(const bson_iter_t* it) {
    switch (bson_iter_type(it)) {
        case BSON_TYPE_NULL:
        case BSON_TYPE_UNDEFINED:
            return Value{};
        case BSON_TYPE_BOOL:
            return Value{bson_iter_bool(it)};
        case BSON_TYPE_INT32:
            return Value{static_cast<double>(bson_iter_int32(it))};
        case BSON_TYPE_INT64:
            return Value{static_cast<double>(bson_iter_int64(it))};
        case BSON_TYPE_DOUBLE:
            return Value{bson_iter_double(it)};
        case BSON_TYPE_UTF8: {
            std::uint32_t len = 0;
            const char*   s   = bson_iter_utf8(it, &len);
            return Value{std::string(s ? s : "", len)};
        }
        case BSON_TYPE_OID: {
            char buf[25] = {0};
            bson_oid_to_string(bson_iter_oid(it), buf);
            return Value{std::string(buf, 24)};
        }
        case BSON_TYPE_DATE_TIME:
            // Milliseconds since Unix epoch as a bnl number. Roundtrips
            // through int64 → double; safe through ~285 millennia.
            return Value{static_cast<double>(bson_iter_date_time(it))};
        case BSON_TYPE_BINARY: {
            const std::uint8_t* data = nullptr;
            std::uint32_t       len  = 0;
            bson_subtype_t      sub;
            bson_iter_binary(it, &sub, &len, &data);
            return Value{std::string(reinterpret_cast<const char*>(data), len)};
        }
        case BSON_TYPE_DECIMAL128: {
            bson_decimal128_t d;
            char              buf[BSON_DECIMAL128_STRING] = {0};
            if (bson_iter_decimal128(it, &d)) {
                bson_decimal128_to_string(&d, buf);
            }
            return Value{std::string(buf)};
        }
        case BSON_TYPE_DOCUMENT: {
            std::uint32_t       len  = 0;
            const std::uint8_t* data = nullptr;
            bson_iter_document(it, &len, &data);
            bson_t child;
            if (!bson_init_static(&child, data, len))
                return Value{};
            return bson_doc_to_map(&child);
        }
        case BSON_TYPE_ARRAY: {
            std::uint32_t       len  = 0;
            const std::uint8_t* data = nullptr;
            bson_iter_array(it, &len, &data);
            bson_t child;
            if (!bson_init_static(&child, data, len))
                return Value{};
            return bson_doc_to_array(&child);
        }
        case BSON_TYPE_TIMESTAMP: {
            std::uint32_t ts  = 0;
            std::uint32_t inc = 0;
            bson_iter_timestamp(it, &ts, &inc);
            // Pack as a double for v1; loses precision above 2^53 but fine
            // for the few decades' worth of seconds we'll see.
            return Value{static_cast<double>(
                (static_cast<std::int64_t>(ts) << 32) | inc)};
        }
        default:
            // Regex, Code, Symbol, MinKey, MaxKey, DBPointer, etc. — return a
            // tagged string so the user sees them. Not writable in v1.
            return Value{std::string("<bson:unsupported>")};
    }
}

// ---------- collection module ---------------------------------------------

ModulePtr build_collection_module(std::shared_ptr<ClientState> client,
                                  mongoc_collection_t*         raw_col,
                                  const std::string&           db_name,
                                  const std::string&           col_name) {
    struct ColState {
        std::shared_ptr<ClientState> client;
        mongoc_collection_t*         col;
        std::string                  db_name;
        std::string                  col_name;
    };
    std::shared_ptr<ColState> st{
        new ColState{std::move(client), raw_col, db_name, col_name},
        [](ColState* s) {
            if (s->col) mongoc_collection_destroy(s->col);
            delete s;
        }};

    auto require_open = [](const std::shared_ptr<ColState>& s, const char* fn) {
        if (s->client->closed)
            throw std::runtime_error(
                std::string("collection.") + fn + ": client is closed");
    };

    return NativeModule("collection")

        // col.insert_one(doc) — inserts a single document. If `_id` is
        // absent, mongoc adds an ObjectId (we read it back and return it).
        // Returns the inserted document with the final `_id` populated.
        .add_function("insert_one", 1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "insert_one");
                bson_t* doc = map_to_new_bson(args[0]);

                // Pre-generate _id if missing so caller can read it back.
                if (!bson_has_field(doc, "_id")) {
                    bson_oid_t oid;
                    bson_oid_init(&oid, nullptr);
                    // Need _id at the START — easiest: rebuild with _id first.
                    bson_t* doc2 = bson_new();
                    bson_append_oid(doc2, "_id", 3, &oid);
                    bson_concat(doc2, doc);
                    bson_destroy(doc);
                    doc = doc2;
                }

                bson_error_t err;
                bool ok = mongoc_collection_insert_one(
                    st->col, doc, nullptr, nullptr, &err);
                if (!ok) {
                    bson_destroy(doc);
                    throw_bson_err(err, "insert_one");
                }
                Value out = bson_doc_to_map(doc);
                bson_destroy(doc);
                return out;
            })

        // col.find(filter, opts?) — returns list of matching documents.
        // opts may set limit, skip, sort (map), projection (map).
        .add_function("find", -1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "find");
                if (args.empty() || args.size() > 2)
                    throw std::runtime_error("collection.find(filter, opts?): wrong arity");

                bson_t* filter = map_to_new_bson(args[0]);
                bson_t* opts   = nullptr;
                if (args.size() == 2 && !args[1].is_null()) {
                    opts = map_to_new_bson(args[1]);
                }

                mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(
                    st->col, filter, opts, nullptr);

                auto out = std::make_shared<std::vector<Value>>();
                const bson_t* doc = nullptr;
                while (mongoc_cursor_next(cursor, &doc)) {
                    out->push_back(bson_doc_to_map(doc));
                }

                bson_error_t err;
                if (mongoc_cursor_error(cursor, &err)) {
                    mongoc_cursor_destroy(cursor);
                    if (opts)   bson_destroy(opts);
                    bson_destroy(filter);
                    throw_bson_err(err, "find");
                }

                mongoc_cursor_destroy(cursor);
                if (opts)   bson_destroy(opts);
                bson_destroy(filter);
                return Value{out};
            })

        // col.find_one(filter) — first match or null.
        .add_function("find_one", 1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "find_one");
                bson_t* filter = map_to_new_bson(args[0]);
                bson_t  opts   = BSON_INITIALIZER;
                BSON_APPEND_INT32(&opts, "limit", 1);

                mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(
                    st->col, filter, &opts, nullptr);

                Value         result = Value{};
                const bson_t* doc    = nullptr;
                if (mongoc_cursor_next(cursor, &doc)) {
                    result = bson_doc_to_map(doc);
                }

                bson_error_t err;
                if (mongoc_cursor_error(cursor, &err)) {
                    mongoc_cursor_destroy(cursor);
                    bson_destroy(&opts);
                    bson_destroy(filter);
                    throw_bson_err(err, "find_one");
                }

                mongoc_cursor_destroy(cursor);
                bson_destroy(&opts);
                bson_destroy(filter);
                return result;
            })

        // col.update_one(filter, update) — returns
        // {matched, modified, upserted_id?}.
        .add_function("update_one", 2,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "update_one");
                bson_t* filter = map_to_new_bson(args[0]);
                bson_t* update = map_to_new_bson(args[1]);

                bson_t       reply = BSON_INITIALIZER;
                bson_error_t err;
                bool ok = mongoc_collection_update_one(
                    st->col, filter, update, nullptr, &reply, &err);
                bson_destroy(filter);
                bson_destroy(update);
                if (!ok) {
                    bson_destroy(&reply);
                    throw_bson_err(err, "update_one");
                }
                Value out = bson_doc_to_map(&reply);
                bson_destroy(&reply);
                return out;
            })

        // col.delete_one(filter) — returns {deletedCount} (mongoc's
        // standard reply shape).
        .add_function("delete_one", 1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "delete_one");
                bson_t* filter = map_to_new_bson(args[0]);

                bson_t       reply = BSON_INITIALIZER;
                bson_error_t err;
                bool ok = mongoc_collection_delete_one(
                    st->col, filter, nullptr, &reply, &err);
                bson_destroy(filter);
                if (!ok) {
                    bson_destroy(&reply);
                    throw_bson_err(err, "delete_one");
                }
                Value out = bson_doc_to_map(&reply);
                bson_destroy(&reply);
                return out;
            })

        // col.count(filter?) — server-side count_documents. Filter defaults
        // to {} (count all).
        .add_function("count", -1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "count");
                if (args.size() > 1)
                    throw std::runtime_error("collection.count(filter?): wrong arity");

                bson_t  empty  = BSON_INITIALIZER;
                bson_t* filter = nullptr;
                if (!args.empty() && !args[0].is_null()) {
                    filter = map_to_new_bson(args[0]);
                }

                bson_error_t err;
                std::int64_t n = mongoc_collection_count_documents(
                    st->col, filter ? filter : &empty, nullptr, nullptr, nullptr, &err);
                if (filter) bson_destroy(filter);
                bson_destroy(&empty);

                if (n < 0) throw_bson_err(err, "count");
                return Value{static_cast<double>(n)};
            })

        // col.drop() — drop the collection.
        .add_function("drop", 0,
            [st, require_open](Interpreter&, std::vector<Value>) -> Value {
                require_open(st, "drop");
                bson_error_t err;
                if (!mongoc_collection_drop(st->col, &err))
                    throw_bson_err(err, "drop");
                return Value{};
            })

        .add_value("name",     Value{col_name})
        .add_value("db_name",  Value{db_name})
        .build();
}

// ---------- db module ------------------------------------------------------

ModulePtr build_db_module(std::shared_ptr<ClientState> client,
                          mongoc_database_t*           raw_db,
                          const std::string&           db_name) {
    struct DbState {
        std::shared_ptr<ClientState> client;
        mongoc_database_t*           db;
        std::string                  name;
    };
    std::shared_ptr<DbState> st{
        new DbState{std::move(client), raw_db, db_name},
        [](DbState* s) {
            if (s->db) mongoc_database_destroy(s->db);
            delete s;
        }};

    auto require_open = [](const std::shared_ptr<DbState>& s, const char* fn) {
        if (s->client->closed)
            throw std::runtime_error(
                std::string("db.") + fn + ": client is closed");
    };

    return NativeModule("db")

        .add_function("collection", 1,
            [st, require_open](Interpreter&, std::vector<Value> args) -> Value {
                require_open(st, "collection");
                if (!args[0].is_string())
                    throw std::runtime_error("db.collection: expected string name");
                const std::string& name = args[0].as_string();
                mongoc_collection_t* col = mongoc_client_get_collection(
                    st->client->client, st->name.c_str(), name.c_str());
                return Value{build_collection_module(st->client, col, st->name, name)};
            })

        .add_function("drop", 0,
            [st, require_open](Interpreter&, std::vector<Value>) -> Value {
                require_open(st, "drop");
                bson_error_t err;
                if (!mongoc_database_drop(st->db, &err))
                    throw_bson_err(err, "db.drop");
                return Value{};
            })

        .add_value("name", Value{db_name})
        .build();
}

// ---------- client module --------------------------------------------------

ModulePtr build_client_module(mongoc_client_t* raw_client) {
    std::shared_ptr<ClientState> st{new ClientState{raw_client, false},
        [](ClientState* s) { close_client(s); delete s; }};

    return NativeModule("client")

        .add_function("db", 1,
            [st](Interpreter&, std::vector<Value> args) -> Value {
                if (st->closed)
                    throw std::runtime_error("client.db: client is closed");
                if (!args[0].is_string())
                    throw std::runtime_error("client.db: expected string name");
                const std::string& name = args[0].as_string();
                mongoc_database_t* db = mongoc_client_get_database(
                    st->client, name.c_str());
                return Value{build_db_module(st, db, name)};
            })

        // Quick path: client.collection(db_name, col_name) without staging
        // the intermediate db module. Common enough to justify.
        .add_function("collection", 2,
            [st](Interpreter&, std::vector<Value> args) -> Value {
                if (st->closed)
                    throw std::runtime_error("client.collection: client is closed");
                if (!args[0].is_string() || !args[1].is_string())
                    throw std::runtime_error(
                        "client.collection(db_name, col_name): expected two strings");
                const std::string& dn = args[0].as_string();
                const std::string& cn = args[1].as_string();
                mongoc_collection_t* col = mongoc_client_get_collection(
                    st->client, dn.c_str(), cn.c_str());
                return Value{build_collection_module(st, col, dn, cn)};
            })

        // client.ping(db_name?) — issues `{ping: 1}` against the admin db
        // (or the named one). Quick "am I actually connected?" check.
        .add_function("ping", -1,
            [st](Interpreter&, std::vector<Value> args) -> Value {
                if (st->closed)
                    throw std::runtime_error("client.ping: client is closed");
                if (args.size() > 1)
                    throw std::runtime_error("client.ping(db_name?): wrong arity");

                std::string db_name = "admin";
                if (!args.empty() && args[0].is_string())
                    db_name = args[0].as_string();

                bson_t cmd = BSON_INITIALIZER;
                BSON_APPEND_INT32(&cmd, "ping", 1);
                bson_t       reply = BSON_INITIALIZER;
                bson_error_t err;
                bool ok = mongoc_client_command_simple(
                    st->client, db_name.c_str(), &cmd, nullptr, &reply, &err);
                bson_destroy(&cmd);
                bson_destroy(&reply);
                if (!ok) throw_bson_err(err, "ping");
                return Value{};
            })

        .add_function("close", 0,
            [st](Interpreter&, std::vector<Value>) -> Value {
                close_client(st.get());
                return Value{};
            })

        .build();
}

}  // namespace

void register_mongo(Interpreter& interp) {
    ensure_mongoc_init();

    auto m = NativeModule("_mongo")

        // _mongo.connect(uri) — uri is a mongodb:// or mongodb+srv:// URI.
        // Synchronous: returns a client module. Note that mongoc does NOT
        // probe the server here — actual TCP/auth happens lazily on the
        // first command. Use client.ping() for an eager handshake.
        .add_function("connect", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_string())
                    throw std::runtime_error("mongo.connect: expected URI string");
                const std::string& uri = args[0].as_string();

                bson_error_t err;
                mongoc_uri_t* parsed = mongoc_uri_new_with_error(uri.c_str(), &err);
                if (!parsed)
                    throw std::runtime_error(
                        std::string("mongo.connect: ") + err.message);

                mongoc_client_t* c = mongoc_client_new_from_uri(parsed);
                mongoc_uri_destroy(parsed);
                if (!c)
                    throw std::runtime_error("mongo.connect: client_new failed");

                // Identify ourselves so the server logs can attribute
                // connections — purely cosmetic.
                mongoc_client_set_appname(c, "bnl");
                return Value{build_client_module(c)};
            })

        // _mongo.version() — mongo-c-driver version string.
        .add_function("version", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                return Value{std::string(mongoc_get_version())};
            })

        .build();

    interp.register_native_module("_mongo", m);
}

}  // namespace bnl
