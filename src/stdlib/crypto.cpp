#include "stdlib/registry.h"

#include "bnl/interpreter.h"
#include "bnl/native_module.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bnl {

namespace {

// ---------- argument helpers ------------------------------------------------

const std::string& require_string(const Value& v, const char* where) {
    if (!v.is_string()) throw std::runtime_error(std::string(where) + ": expected string");
    return v.as_string();
}

// Random / encode sizes go through here. Reject negatives, NaN, and absurd
// allocations early — the upper bound is generous (1 GiB) but catches typos
// like passing a millisecond timestamp by mistake.
std::size_t require_count(const Value& v, const char* where) {
    if (!v.is_number()) throw std::runtime_error(std::string(where) + ": expected number");
    double d = v.as_number();
    if (!(d >= 0.0 && d <= 1073741824.0))
        throw std::runtime_error(std::string(where) + ": out of range");
    return static_cast<std::size_t>(d);
}

// ---------- hex codec -------------------------------------------------------

std::string to_hex(const unsigned char* data, std::size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string out(n * 2, '\0');
    for (std::size_t i = 0; i < n; ++i) {
        out[2 * i]     = kHex[data[i] >> 4];
        out[2 * i + 1] = kHex[data[i] & 0x0f];
    }
    return out;
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

std::string from_hex(const std::string& s) {
    if (s.size() % 2 != 0)
        throw std::runtime_error("crypto.hex_decode: odd-length input");
    std::string out(s.size() / 2, '\0');
    for (std::size_t i = 0; i < out.size(); ++i) {
        int hi = hex_nibble(s[2 * i]);
        int lo = hex_nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0)
            throw std::runtime_error("crypto.hex_decode: non-hex character");
        out[i] = static_cast<char>((hi << 4) | lo);
    }
    return out;
}

// ---------- digest / hmac ---------------------------------------------------

const EVP_MD* lookup_md(const std::string& name) {
    const EVP_MD* md = EVP_get_digestbyname(name.c_str());
    if (!md) throw std::runtime_error("crypto: unknown hash algorithm '" + name + "'");
    return md;
}

std::string compute_hash(const std::string& algo, const std::string& data) {
    const EVP_MD* md = lookup_md(algo);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), digest, &len, md, nullptr) != 1)
        throw std::runtime_error("crypto.hash: EVP_Digest failed");
    return to_hex(digest, len);
}

std::string compute_hmac(const std::string& algo, const std::string& key,
                         const std::string& data) {
    const EVP_MD* md = lookup_md(algo);
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (HMAC(md,
             key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(),
             out, &len) == nullptr)
        throw std::runtime_error("crypto.hmac: HMAC failed");
    return to_hex(out, len);
}

// ---------- base64 ----------------------------------------------------------

// EVP_EncodeBlock / EVP_DecodeBlock are the one-shot, no-newline variants —
// exactly what we want for cookie / token / JWT use. The streaming
// EVP_EncodeUpdate path inserts line breaks every 64 chars, which we don't.
std::string b64_encode_std(const std::string& data) {
    if (data.empty()) return {};
    std::string out(4 * ((data.size() + 2) / 3), '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                            reinterpret_cast<const unsigned char*>(data.data()),
                            static_cast<int>(data.size()));
    if (n < 0) throw std::runtime_error("crypto.b64_encode: EVP_EncodeBlock failed");
    out.resize(static_cast<std::size_t>(n));
    return out;
}

std::string b64_decode_std(const std::string& s) {
    if (s.empty()) return {};
    if (s.size() % 4 != 0)
        throw std::runtime_error("crypto.b64_decode: input length not a multiple of 4");
    std::vector<unsigned char> buf(s.size());
    int n = EVP_DecodeBlock(buf.data(),
                            reinterpret_cast<const unsigned char*>(s.data()),
                            static_cast<int>(s.size()));
    if (n < 0) throw std::runtime_error("crypto.b64_decode: invalid base64");
    // EVP_DecodeBlock counts the '=' padding as decoded bytes (zeros) — strip
    // them so the caller gets the original byte count back.
    std::size_t pad = 0;
    for (std::size_t i = s.size(); i > 0 && s[i - 1] == '='; --i) ++pad;
    return std::string(reinterpret_cast<char*>(buf.data()),
                       static_cast<std::size_t>(n) - pad);
}

}  // namespace

void register_crypto(Interpreter& interp) {
    auto m = NativeModule("crypto")

        // crypto.random_bytes(n) — n cryptographic-random bytes as a raw
        // byte string. Inspect via .byte_length / .byte_at / .byte_slice.
        .add_function("random_bytes", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                std::size_t n = require_count(args[0], "crypto.random_bytes(n)");
                std::string out(n, '\0');
                if (n > 0 &&
                    RAND_bytes(reinterpret_cast<unsigned char*>(out.data()),
                               static_cast<int>(n)) != 1)
                    throw std::runtime_error("crypto.random_bytes: RAND_bytes failed");
                return Value{std::move(out)};
            })

        // crypto.random_hex(n) — n random bytes encoded as 2n lowercase hex.
        // The convenience form for tokens / IDs / multipart boundaries.
        .add_function("random_hex", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                std::size_t n = require_count(args[0], "crypto.random_hex(n)");
                std::vector<unsigned char> buf(n);
                if (n > 0 && RAND_bytes(buf.data(), static_cast<int>(n)) != 1)
                    throw std::runtime_error("crypto.random_hex: RAND_bytes failed");
                return Value{to_hex(buf.data(), n)};
            })

        // crypto.hash(algo, data) — hex digest. Algos: md5, sha1, sha224,
        // sha256, sha384, sha512 (anything OpenSSL recognises by name).
        .add_function("hash", 2,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{compute_hash(
                    require_string(args[0], "crypto.hash: algo"),
                    require_string(args[1], "crypto.hash: data"))};
            })

        // crypto.hmac(algo, key, data) — hex HMAC tag.
        .add_function("hmac", 3,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{compute_hmac(
                    require_string(args[0], "crypto.hmac: algo"),
                    require_string(args[1], "crypto.hmac: key"),
                    require_string(args[2], "crypto.hmac: data"))};
            })

        // crypto.b64_encode(data) — standard base64 with `=` padding,
        // single line (no newlines inserted).
        .add_function("b64_encode", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{b64_encode_std(
                    require_string(args[0], "crypto.b64_encode: data"))};
            })

        // crypto.b64_decode(data) — raw bytes from standard base64.
        .add_function("b64_decode", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{b64_decode_std(
                    require_string(args[0], "crypto.b64_decode: data"))};
            })

        // crypto.b64url_encode(data) — URL-safe base64 (`-` `_` instead of
        // `+` `/`), no padding. The encoding JWTs use.
        .add_function("b64url_encode", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                std::string s = b64_encode_std(
                    require_string(args[0], "crypto.b64url_encode: data"));
                for (auto& c : s) {
                    if (c == '+') c = '-';
                    else if (c == '/') c = '_';
                }
                while (!s.empty() && s.back() == '=') s.pop_back();
                return Value{std::move(s)};
            })

        // crypto.b64url_decode(data) — accepts URL-safe alphabet, padding
        // optional. Mirror of b64url_encode.
        .add_function("b64url_decode", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                std::string s = require_string(args[0], "crypto.b64url_decode: data");
                for (auto& c : s) {
                    if (c == '-') c = '+';
                    else if (c == '_') c = '/';
                }
                while (s.size() % 4 != 0) s.push_back('=');
                return Value{b64_decode_std(s)};
            })

        // crypto.hex_encode(data) — lowercase hex of the raw bytes in `data`.
        .add_function("hex_encode", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                const auto& s = require_string(args[0], "crypto.hex_encode: data");
                return Value{to_hex(
                    reinterpret_cast<const unsigned char*>(s.data()), s.size())};
            })

        // crypto.hex_decode(data) — raw bytes from hex (case-insensitive).
        .add_function("hex_decode", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{from_hex(
                    require_string(args[0], "crypto.hex_decode: data"))};
            })

        // crypto.equals(a, b) — constant-time string compare. Use this,
        // not `==`, when checking HMAC tags or session tokens to defeat
        // timing side channels.
        .add_function("equals", 2,
            [](Interpreter&, std::vector<Value> args) -> Value {
                const auto& a = require_string(args[0], "crypto.equals: a");
                const auto& b = require_string(args[1], "crypto.equals: b");
                if (a.size() != b.size()) return Value{false};
                if (a.empty()) return Value{true};
                return Value{CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0};
            })

        .build();

    interp.register_native_module("crypto", m);
}

}  // namespace bnl
