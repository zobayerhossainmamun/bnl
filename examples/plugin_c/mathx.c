// Sample bnl plugin written in pure C. Exposes three things to bnl:
//
//     m.cube(n)      → n * n * n
//     m.hypot(a, b)  → sqrt(a*a + b*b)
//     m.greeting     → "hi from C plugin"
//
// Compile as a shared library:
//
//     Windows:  cl /LD mathx.c
//     Linux:    gcc -shared -fPIC -o libmathx.so mathx.c -lm
//     macOS:    clang -shared -o libmathx.dylib mathx.c -lm
//
// Then from bnl:
//
//     import "./mathx.dll" as m;
//     print(m.cube(4));        // 64
//     print(m.hypot(3, 4));    // 5
//     print(m.greeting);       // hi from C plugin

#include <math.h>

#include "bnl/plugin.h"

static bnl_value* cube(const bnl_api* api,
                       int argc, bnl_value** argv, void* ud) {
    (void)argc; (void)ud;
    if (api->get_type(argv[0]) != BNL_TYPE_NUMBER) {
        api->throw_error(api, "cube: expected a number");
        return 0;
    }
    double x = api->get_number(argv[0]);
    return api->make_number(api, x * x * x);
}

static bnl_value* hypot_fn(const bnl_api* api,
                            int argc, bnl_value** argv, void* ud) {
    (void)argc; (void)ud;
    double a = api->get_number(argv[0]);
    double b = api->get_number(argv[1]);
    return api->make_number(api, sqrt(a * a + b * b));
}

BNL_EXPORT bnl_module* bnl_load(const bnl_api* api) {
    bnl_module* m = api->module_new(api, "mathx");
    api->module_add_function(m, "cube",  1, cube,      0);
    api->module_add_function(m, "hypot", 2, hypot_fn,  0);
    api->module_add_value   (m, "greeting",
                              api->make_string(api, "hi from C plugin", 16));
    return m;
}
