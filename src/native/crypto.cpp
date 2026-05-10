#include "native/builtins.h"

#include <fmt/core.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/native_module.h"

namespace bnl {

namespace {

std::string digest_to_hex(const unsigned char* digest, unsigned int len) {
    std::string out;
    out.reserve(len * 2);
    static const char* hex = "0123456789abcdef";
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(hex[(digest[i] >> 4) & 0xF]);
        out.push_back(hex[digest[i]        & 0xF]);
    }
    return out;
}

std::string hash_with(const EVP_MD* algo, const std::string& input) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");
    if (!EVP_DigestInit_ex(ctx, algo, nullptr) ||
        !EVP_DigestUpdate(ctx, input.data(), input.size())) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP digest init/update failed");
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    if (!EVP_DigestFinal_ex(ctx, digest, &len)) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_DigestFinal_ex failed");
    }
    EVP_MD_CTX_free(ctx);
    return digest_to_hex(digest, len);
}

const std::string& as_string_arg(const Value& v, const char* where) {
    if (!v.is_string())
        throw std::runtime_error(fmt::format("{}: input must be a string, got {}",
                                             where, v.type_name()));
    return v.as_string();
}

}  // namespace

void register_crypto(Interpreter& interp) {
    auto m = NativeModule("crypto")
        .add_function("sha256", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{hash_with(EVP_sha256(), as_string_arg(args[0], "crypto.sha256"))};
            })
        .add_function("sha1", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{hash_with(EVP_sha1(), as_string_arg(args[0], "crypto.sha1"))};
            })
        .add_function("md5", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                return Value{hash_with(EVP_md5(), as_string_arg(args[0], "crypto.md5"))};
            })
        // crypto.random_hex(n) -> 2n-char hex string from n cryptographic random bytes.
        .add_function("random_hex", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_number())
                    throw std::runtime_error("crypto.random_hex(n): n must be a number");
                int n = static_cast<int>(args[0].as_number());
                if (n <= 0 || n > 4096)
                    throw std::runtime_error("crypto.random_hex(n): n must be in 1..4096");
                std::vector<unsigned char> buf(static_cast<std::size_t>(n));
                if (RAND_bytes(buf.data(), n) != 1)
                    throw std::runtime_error("crypto.random_hex: RAND_bytes failed");
                return Value{digest_to_hex(buf.data(), static_cast<unsigned int>(n))};
            })
        .build();

    interp.register_native_module("crypto", m);
}

}  // namespace bnl
