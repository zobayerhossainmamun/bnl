// bnl plugin contract. Single-file C header. Drop into your project, write a
// shared library that exports `bnl_load`, import it from bnl by path or via
// a `bnl.json` `native` field. No bnl runtime link — all interaction goes
// through the bnl_api function table passed to bnl_load.

#ifndef BNL_PLUGIN_H
#define BNL_PLUGIN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped only on breaking ABI changes. Additive changes append to bnl_api.
#define BNL_PLUGIN_API_VERSION 1

#define BNL_TYPE_NULL    0
#define BNL_TYPE_BOOL    1
#define BNL_TYPE_NUMBER  2
#define BNL_TYPE_STRING  3
#define BNL_TYPE_LIST    4
#define BNL_TYPE_MAP     5
#define BNL_TYPE_OTHER   6   // function / module / instance — read-only

typedef struct bnl_value  bnl_value;
typedef struct bnl_module bnl_module;
typedef struct bnl_api    bnl_api;

// argv entries are valid only during the call. Return NULL after throw_error;
// otherwise return a constructed value (api->make_null() for "nothing").
typedef bnl_value* (*bnl_native_fn)(const bnl_api* api,
                                    int            argc,
                                    bnl_value**    argv,
                                    void*          userdata);

// Values from make_* are owned by bnl and live until the current call returns.
// list_push / map_set deep-copy into the parent, so the original handle can be
// dropped after the attach.
struct bnl_api {
    int version;

    bnl_value*  (*make_null)   (const bnl_api*);
    bnl_value*  (*make_bool)   (const bnl_api*, int b);
    bnl_value*  (*make_number) (const bnl_api*, double n);
    bnl_value*  (*make_string) (const bnl_api*, const char* s, size_t len);
    bnl_value*  (*make_list)   (const bnl_api*);
    bnl_value*  (*make_map)    (const bnl_api*);

    int         (*get_type)    (const bnl_value*);
    int         (*get_bool)    (const bnl_value*);
    double      (*get_number)  (const bnl_value*);
    // Byte pointer, NOT NUL-terminated. *out_len gets the byte length.
    const char* (*get_string)  (const bnl_value*, size_t* out_len);

    size_t      (*list_length) (const bnl_value* list);
    bnl_value*  (*list_get)    (const bnl_api*, bnl_value* list, size_t index);
    void        (*list_push)   (bnl_value* list, bnl_value* item);

    size_t      (*map_size)    (const bnl_value* map);
    int         (*map_has)     (const bnl_value* map, const char* key);
    bnl_value*  (*map_get)     (const bnl_api*, bnl_value* map, const char* key);
    void        (*map_set)     (bnl_value* map, const char* key, bnl_value* val);
    int         (*map_key_at)  (const bnl_value* map, size_t index,
                                const char** out_key, size_t* out_len);

    // bnl takes ownership of the module returned from bnl_load.
    bnl_module* (*module_new)           (const bnl_api*, const char* name);
    // arity is the fixed arg count, or -1 for variadic.
    void        (*module_add_function)  (bnl_module* m, const char* name,
                                         int arity, bnl_native_fn fn,
                                         void* userdata);
    void        (*module_add_value)     (bnl_module* m, const char* name,
                                         bnl_value* value);

    // Caller should return NULL from the native_fn after invoking this.
    void        (*throw_error) (const bnl_api*, const char* msg);
};

// Required export. Called once per process when the plugin is first imported.
typedef bnl_module* (*bnl_load_fn)(const bnl_api*);

#if defined(_WIN32) || defined(__CYGWIN__)
#  define BNL_EXPORT __declspec(dllexport)
#else
#  define BNL_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
}
#endif

#endif
