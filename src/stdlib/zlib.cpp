#include "stdlib/registry.h"

#include <zlib.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/native_module.h"

namespace bnl {

namespace {

// ===========================================================================
// gzip + raw deflate/inflate via zlib. Streaming pump using one z_stream per
// call, growing the output buffer when the deflater/inflater runs out of
// room. Both compress and decompress are sync — for very large payloads
// async wrapping via uv_queue_work would be a follow-up.
//
// windowBits values:
//   15 + 16 → gzip (RFC 1952)
//   15 + 32 → auto-detect zlib/gzip on inflate (decompress)
//   -15     → raw deflate (no header, no checksum) — for WebSocket extensions
// ===========================================================================

const std::string& require_string(const Value& v, const char* where) {
    if (!v.is_string())
        throw std::runtime_error(std::string(where) + ": expected string");
    return v.as_string();
}

int parse_level(const std::vector<Value>& args, std::size_t idx) {
    if (args.size() <= idx || args[idx].is_null()) return Z_DEFAULT_COMPRESSION;
    if (!args[idx].is_number())
        throw std::runtime_error("zlib: level must be a number (-1 to 9)");
    int n = static_cast<int>(args[idx].as_number());
    if (n < -1 || n > 9)
        throw std::runtime_error("zlib: level out of range — must be -1 (default) or 0..9");
    return n;
}

std::string do_compress(const std::string& in, int window_bits, int level) {
    z_stream s{};
    int rc = deflateInit2(&s, level, Z_DEFLATED, window_bits,
                          /*memLevel=*/8, Z_DEFAULT_STRATEGY);
    if (rc != Z_OK)
        throw std::runtime_error("zlib deflateInit2: rc=" + std::to_string(rc));

    s.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
    s.avail_in = static_cast<uInt>(in.size());

    // Start with deflateBound + slack so most payloads finish in one pass.
    std::size_t cap = static_cast<std::size_t>(deflateBound(&s, static_cast<uLong>(in.size())));
    if (cap < 64) cap = 64;
    std::string out;
    out.resize(cap);
    s.next_out  = reinterpret_cast<Bytef*>(out.data());
    s.avail_out = static_cast<uInt>(out.size());

    while (true) {
        rc = deflate(&s, Z_FINISH);
        if (rc == Z_STREAM_END) break;
        if (rc == Z_OK || rc == Z_BUF_ERROR) {
            // Need more output space. Double + retry.
            std::size_t old = out.size();
            out.resize(old * 2);
            s.next_out  = reinterpret_cast<Bytef*>(out.data()) + old;
            s.avail_out = static_cast<uInt>(old);
            continue;
        }
        deflateEnd(&s);
        throw std::runtime_error("zlib deflate: rc=" + std::to_string(rc));
    }
    out.resize(out.size() - s.avail_out);
    deflateEnd(&s);
    return out;
}

std::string do_decompress(const std::string& in, int window_bits) {
    z_stream s{};
    int rc = inflateInit2(&s, window_bits);
    if (rc != Z_OK)
        throw std::runtime_error("zlib inflateInit2: rc=" + std::to_string(rc));

    s.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
    s.avail_in = static_cast<uInt>(in.size());

    // Compressed → uncompressed is unbounded ratio. Start with 4x input + slack.
    std::size_t cap = in.size() * 4 + 256;
    std::string out;
    out.resize(cap);
    s.next_out  = reinterpret_cast<Bytef*>(out.data());
    s.avail_out = static_cast<uInt>(out.size());

    while (true) {
        rc = inflate(&s, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc == Z_OK) {
            if (s.avail_out == 0) {
                std::size_t old = out.size();
                out.resize(old * 2);
                s.next_out  = reinterpret_cast<Bytef*>(out.data()) + old;
                s.avail_out = static_cast<uInt>(old);
            }
            continue;
        }
        std::string msg = s.msg ? s.msg : ("rc=" + std::to_string(rc));
        inflateEnd(&s);
        throw std::runtime_error("zlib inflate: " + msg);
    }
    out.resize(out.size() - s.avail_out);
    inflateEnd(&s);
    return out;
}

}  // namespace

void register_zlib(Interpreter& interp) {
    auto m = NativeModule("_zlib")

        // gzip_compress(data [, level]) — RFC 1952 gzip-framed output.
        // level: -1 default, 0 = no compression, 1 = fast, 9 = best.
        .add_function("gzip_compress", -1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (args.empty() || args.size() > 2)
                    throw std::runtime_error("zlib.gzip_compress(data, level?): wrong arity");
                return Value{do_compress(
                    require_string(args[0], "zlib.gzip_compress: data"),
                    /*windowBits=*/15 + 16, parse_level(args, 1))};
            })

        // gzip_decompress(data) — accepts either gzip OR zlib-framed input
        // (windowBits = 15 + 32 enables auto-detect).
        .add_function("gzip_decompress", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{do_decompress(
                    require_string(args[0], "zlib.gzip_decompress: data"),
                    /*windowBits=*/15 + 32)};
            })

        // deflate_raw(data [, level]) — raw deflate (no zlib/gzip header
        // or checksum). For WebSocket permessage-deflate and similar
        // protocols that frame compression themselves.
        .add_function("deflate_raw", -1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (args.empty() || args.size() > 2)
                    throw std::runtime_error("zlib.deflate_raw(data, level?): wrong arity");
                return Value{do_compress(
                    require_string(args[0], "zlib.deflate_raw: data"),
                    /*windowBits=*/-15, parse_level(args, 1))};
            })

        // inflate_raw(data) — raw inflate (matches deflate_raw above).
        .add_function("inflate_raw", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{do_decompress(
                    require_string(args[0], "zlib.inflate_raw: data"),
                    /*windowBits=*/-15)};
            })

        // version() — zlib library version, e.g. "1.3.1".
        .add_function("version", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                return Value{std::string(zlibVersion())};
            })

        .build();

    interp.register_native_module("_zlib", m);
}

}  // namespace bnl
