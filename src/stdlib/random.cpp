#include "stdlib/registry.h"

#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/native_module.h"

namespace bnl {

namespace {

// ===========================================================================
// Non-crypto PRNG. For security-sensitive randomness use crypto.random_bytes
// (CSPRNG via OpenSSL). This is a single-threaded Mersenne-Twister state
// that auto-seeds from `std::random_device` on first use.
//
// Native exposes only the raw engine: seed / next [0,1) / next_int(max) /
// bytes(n). The user-facing helpers (int(min,max), choice, shuffle, sample,
// bool) live in lib/random.bnl.
// ===========================================================================

std::mt19937_64& rng() {
    static std::mt19937_64 r{std::random_device{}()};
    return r;
}

double require_number(const Value& v, const char* where) {
    if (!v.is_number())
        throw std::runtime_error(std::string(where) + ": expected number");
    return v.as_number();
}

}  // namespace

void register_random(Interpreter& interp) {
    auto m = NativeModule("_random")

        // _random.seed(n) — deterministic seed (useful for reproducible
        // tests / simulations). Without a seed call, the engine is auto-
        // seeded from random_device on first use.
        .add_function("seed", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                rng().seed(static_cast<std::uint64_t>(
                    require_number(args[0], "random.seed")));
                return Value{};
            })

        // _random.next() — uniform double in [0, 1).
        .add_function("next", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                std::uniform_real_distribution<double> d(0.0, 1.0);
                return Value{d(rng())};
            })

        // _random.next_int(max) — uniform integer in [0, max). Uses
        // uniform_int_distribution so there's no modulo bias across the
        // generator's 64-bit output. max must be >= 1.
        .add_function("next_int", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                double n = require_number(args[0], "random.next_int");
                if (!(n >= 1.0))
                    throw std::runtime_error("random.next_int: max must be >= 1");
                std::uniform_int_distribution<std::int64_t> d(
                    0, static_cast<std::int64_t>(n) - 1);
                return Value{static_cast<double>(d(rng()))};
            })

        // _random.bytes(n) — n random bytes as a binary string. NOT
        // cryptographically secure; use crypto.random_bytes for that.
        .add_function("bytes", 1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                double dn = require_number(args[0], "random.bytes");
                if (dn < 0)
                    throw std::runtime_error("random.bytes: negative count");
                std::size_t n = static_cast<std::size_t>(dn);
                std::string out(n, '\0');
                auto&       r = rng();
                // Pull 8 bytes at a time from the 64-bit engine.
                std::size_t i = 0;
                while (i + 8 <= n) {
                    std::uint64_t v = r();
                    for (int b = 0; b < 8; ++b) {
                        out[i + static_cast<std::size_t>(b)] =
                            static_cast<char>((v >> (b * 8)) & 0xFF);
                    }
                    i += 8;
                }
                if (i < n) {
                    std::uint64_t v = r();
                    for (; i < n; ++i) {
                        out[i] = static_cast<char>(v & 0xFF);
                        v >>= 8;
                    }
                }
                return Value{std::move(out)};
            })

        .build();

    interp.register_native_module("_random", m);
}

}  // namespace bnl
