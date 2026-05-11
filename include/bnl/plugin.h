// bnl plugin contract — single-file C header.
//
// Plugin authors copy this header into their project (or vendor it from a
// release of bnl), write a single C function, compile as a shared library:
//
//     #include "plugin.h"
//
//     static bnl_value* cube(const bnl_api* api,
//                            int argc, bnl_value** argv, void* ud) {
//         (void)argc; (void)ud;
//         double x = api->get_number(argv[0]);
//         return api->make_number(api, x * x * x);
//     }
//
//     BNL_EXPORT bnl_module* bnl_load(const bnl_api* api) {
//         bnl_module* m = api->module_new(api, "mathx");
//         api->module_add_function(m, "cube", 1, cube, NULL);
//         return m;
//     }
//
//     // bnl side:
//     //   import "./mathx.dll" as m;
//     //   print(m.cube(4));      // 64
//
// Plugins compile and load identically on Windows / Linux / macOS. No bnl
// runtime dependencies at link time — every interaction goes through the
// `bnl_api` function-pointer table bnl hands the plugin at load time.
//
// Languages: C, C++, Rust (`extern "C"`), Go (cgo), Zig, Pascal, anything
// that can produce a C-ABI shared library.

#ifndef BNL_PLUGIN_H
#define BNL_PLUGIN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped on breaking ABI changes. Backward-compatible additions append new
// function pointers to the end of bnl_api without bumping the version.
#define BNL_PLUGIN_API_VERSION 1

// Type tags returned by get_type().
#define BNL_TYPE_NULL    0
#define BNL_TYPE_BOOL    1
#define BNL_TYPE_NUMBER  2
#define BNL_TYPE_STRING  3
#define BNL_TYPE_LIST    4
#define BNL_TYPE_MAP     5
#define BNL_TYPE_OTHER   6   // function / module / class instance — read-only

// Opaque types. The plugin only sees pointers; bnl owns the underlying memory.
typedef struct bnl_value  bnl_value;
typedef struct bnl_module bnl_module;
typedef struct bnl_api    bnl_api;

// Native-function callback. argv entries are valid only for the duration of
// the call; do not stash pointers between calls. Return NULL after calling
// api->throw_error to surface a runtime error to the bnl caller; otherwise
// return a constructed value (use api->make_null() if you have nothing to
// return).
typedef bnl_value* (*bnl_native_fn)(const bnl_api* api,
                                    int            argc,
                                    bnl_value**    argv,
                                    void*          userdata);

struct bnl_api {
    int version;   // == BNL_PLUGIN_API_VERSION at the time bnl was built

    // ---- value constructors -------------------------------------------------
    // Allocated values are owned by bnl and live for the duration of the
    // current native-fn call (or bnl_load call). Attaching one to a list/map
    // via list_push/map_set deep-copies it into the parent, so the original
    // can be ignored after the attach.
    bnl_value*  (*make_null)   (const bnl_api*);
    bnl_value*  (*make_bool)   (const bnl_api*, int b);
    bnl_value*  (*make_number) (const bnl_api*, double n);
    bnl_value*  (*make_string) (const bnl_api*, const char* s, size_t len);
    bnl_value*  (*make_list)   (const bnl_api*);
    bnl_value*  (*make_map)    (const bnl_api*);

    // ---- value queries ------------------------------------------------------
    int         (*get_type)    (const bnl_value*);
    int         (*get_bool)    (const bnl_value*);
    double      (*get_number)  (const bnl_value*);
    // Returns the byte pointer (NOT NUL-terminated). Writes byte length to
    // *out_len if non-NULL. Pointer is valid until the parent value is freed.
    const char* (*get_string)  (const bnl_value*, size_t* out_len);

    // ---- list ops -----------------------------------------------------------
    size_t      (*list_length) (const bnl_value* list);
    // Returns an arena-owned snapshot of element [index]. NULL on out-of-range.
    bnl_value*  (*list_get)    (const bnl_api*, bnl_value* list, size_t index);
    void        (*list_push)   (bnl_value* list, bnl_value* item);

    // ---- map ops ------------------------------------------------------------
    size_t      (*map_size)    (const bnl_value* map);
    int         (*map_has)     (const bnl_value* map, const char* key);
    // Returns an arena-owned snapshot of the value for `key`. NULL if absent.
    bnl_value*  (*map_get)     (const bnl_api*, bnl_value* map, const char* key);
    void        (*map_set)     (bnl_value* map, const char* key, bnl_value* val);
    // Iterate keys by index. Returns 0 when index >= map size, else 1 and
    // writes the key pointer + length. Key is valid until the map is freed.
    int         (*map_key_at)  (const bnl_value* map, size_t index,
                                const char** out_key, size_t* out_len);

    // ---- module construction ------------------------------------------------
    // module_new is called once per plugin in bnl_load. Add functions and
    // values to it, then return the module pointer from bnl_load. bnl takes
    // ownership; you do NOT free the module yourself.
    bnl_module* (*module_new)           (const bnl_api*, const char* name);
    // arity: number of args, or -1 for variadic.
    // The function pointer must stay valid for the lifetime of the loaded
    // plugin (i.e., it lives in the plugin's text segment).
    void        (*module_add_function)  (bnl_module* m, const char* name,
                                         int arity, bnl_native_fn fn,
                                         void* userdata);
    // Attach a value (typically a string / number constant) as a module
    // member. The value is deep-copied into the module.
    void        (*module_add_value)     (bnl_module* m, const char* name,
                                         bnl_value* value);

    // ---- error reporting ----------------------------------------------------
    // Record an error to be thrown when the current native_fn or bnl_load
    // returns. The function should return NULL after calling this.
    void        (*throw_error) (const bnl_api*, const char* msg);
};

// Required entry point: every plugin must export this symbol.
// Called exactly once when the plugin is first imported.
typedef bnl_module* (*bnl_load_fn)(const bnl_api*);

// Cross-platform export macro.
#if defined(_WIN32) || defined(__CYGWIN__)
#  define BNL_EXPORT __declspec(dllexport)
#else
#  define BNL_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BNL_PLUGIN_H
