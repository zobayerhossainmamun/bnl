#include "native/builtins.h"

#include <fmt/core.h>
#include <zlib.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/native_module.h"

namespace bnl {

namespace {

const std::string& as_string_arg(const Value& v, const char* where) {
    if (!v.is_string())
        throw std::runtime_error(fmt::format("{}: input must be a string, got {}",
                                             where, v.type_name()));
    return v.as_string();
}

std::string zlib_compress(const std::string& input) {
    uLongf      out_size = compressBound(static_cast<uLong>(input.size()));
    std::string out(out_size, '\0');
    int rc = ::compress(reinterpret_cast<Bytef*>(out.data()), &out_size,
                        reinterpret_cast<const Bytef*>(input.data()),
                        static_cast<uLong>(input.size()));
    if (rc != Z_OK) throw std::runtime_error(fmt::format("zlib.compress failed (rc={})", rc));
    out.resize(out_size);
    return out;
}

std::string zlib_decompress(const std::string& input) {
    // Size unknown — grow buffer until uncompress() stops returning Z_BUF_ERROR.
    uLongf      out_size = static_cast<uLongf>(input.size() < 16 ? 64 : input.size() * 4);
    std::string out;
    while (true) {
        out.resize(out_size);
        uLongf actual = out_size;
        int rc = ::uncompress(reinterpret_cast<Bytef*>(out.data()), &actual,
                              reinterpret_cast<const Bytef*>(input.data()),
                              static_cast<uLong>(input.size()));
        if (rc == Z_OK) {
            out.resize(actual);
            return out;
        }
        if (rc == Z_BUF_ERROR) {
            out_size *= 2;
            if (out_size > (1u << 30))
                throw std::runtime_error("zlib.decompress: output exceeds 1GB");
            continue;
        }
        throw std::runtime_error(fmt::format("zlib.decompress failed (rc={})", rc));
    }
}

}  // namespace

void register_zlib(Interpreter& interp) {
    auto m = NativeModule("zlib")
        .add_function("compress", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{zlib_compress(as_string_arg(args[0], "zlib.compress"))};
            })
        .add_function("decompress", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{zlib_decompress(as_string_arg(args[0], "zlib.decompress"))};
            })
        .build();

    interp.register_native_module("zlib", m);
}

}  // namespace bnl
