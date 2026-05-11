#include "stdlib/registry.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
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
// OS clock primitives + tm-struct conversion. Higher-level formatting
// (ISO 8601), parsing, and arithmetic (add_seconds / add_days / ...) live
// in lib/time.bnl.
//
// Two clocks exposed:
//   now_ms()  — wall-clock ms since Unix epoch. Subject to NTP jumps.
//   now_ns()  — monotonic ns counter (from std::steady_clock). For elapsed-
//               time measurements that need to be immune to clock changes.
// ===========================================================================

double require_number(const Value& v, const char* where) {
    if (!v.is_number())
        throw std::runtime_error(std::string(where) + ": expected number");
    return v.as_number();
}

// std::tm <-> time_t portability shims. POSIX has reentrant *_r variants
// and timegm; MSVC has *_s variants and _mkgmtime.
void to_tm_utc(std::time_t s, std::tm& out) {
#ifdef _WIN32
    gmtime_s(&out, &s);
#else
    gmtime_r(&s, &out);
#endif
}

void to_tm_local(std::time_t s, std::tm& out) {
#ifdef _WIN32
    localtime_s(&out, &s);
#else
    localtime_r(&s, &out);
#endif
}

std::time_t tm_to_utc(std::tm& tm) {
#ifdef _WIN32
    return _mkgmtime(&tm);
#else
    return timegm(&tm);
#endif
}

Value make_components(const std::tm& tm, int ms) {
    auto m = std::make_shared<std::unordered_map<std::string, Value>>();
    (*m)["year"]    = Value{static_cast<double>(tm.tm_year + 1900)};
    (*m)["month"]   = Value{static_cast<double>(tm.tm_mon + 1)};
    (*m)["day"]     = Value{static_cast<double>(tm.tm_mday)};
    (*m)["hour"]    = Value{static_cast<double>(tm.tm_hour)};
    (*m)["minute"]  = Value{static_cast<double>(tm.tm_min)};
    (*m)["second"]  = Value{static_cast<double>(tm.tm_sec)};
    (*m)["ms"]      = Value{static_cast<double>(ms)};
    (*m)["weekday"] = Value{static_cast<double>(tm.tm_wday)};   // 0=Sun..6=Sat
    (*m)["yearday"] = Value{static_cast<double>(tm.tm_yday + 1)};
    return Value{m};
}

}  // namespace

void register_time(Interpreter& interp) {
    auto m = NativeModule("_time")

        // _time.now_ms() — wall-clock ms since Unix epoch (1970-01-01 UTC).
        .add_function("now_ms", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                auto now = std::chrono::system_clock::now();
                auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();
                return Value{static_cast<double>(ms)};
            })

        // _time.now_ns() — monotonic ns counter. Use for elapsed-time
        // measurements; differences are meaningful, absolute values are not.
        .add_function("now_ns", 0,
            [](Interpreter&, std::vector<Value>) -> Value {
                auto now = std::chrono::steady_clock::now();
                auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now.time_since_epoch()).count();
                return Value{static_cast<double>(ns)};
            })

        // _time.to_components(ms, local?) — break a ms-since-epoch into
        // {year, month, day, hour, minute, second, ms, weekday, yearday}.
        // weekday is 0=Sunday..6=Saturday; yearday is 1..366.
        .add_function("to_components", 2,
            [](Interpreter&, std::vector<Value> args) -> Value {
                double ms_d = require_number(args[0], "time.to_components");
                if (!args[1].is_bool())
                    throw std::runtime_error(
                        "time.to_components: 2nd arg must be bool (local?)");
                bool local = args[1].as_bool();

                // Split into seconds + sub-second ms, carrying negative
                // remainders back into the seconds part so the components
                // are always in [0, 999] for ms.
                double secs_d = std::floor(ms_d / 1000.0);
                int    ms     = static_cast<int>(ms_d - secs_d * 1000.0);
                if (ms < 0) { ms += 1000; secs_d -= 1.0; }

                std::time_t s = static_cast<std::time_t>(secs_d);
                std::tm     tm{};
                if (local) to_tm_local(s, tm); else to_tm_utc(s, tm);
                return make_components(tm, ms);
            })

        // _time.from_components({year,month,day,hour?,minute?,second?,ms?},
        // local?) — invert to_components. Missing fields default to the
        // start of the period (hour/min/sec/ms = 0). Year + month + day are
        // expected, everything else is optional.
        .add_function("from_components", 2,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (!args[0].is_map())
                    throw std::runtime_error(
                        "time.from_components: 1st arg must be map");
                if (!args[1].is_bool())
                    throw std::runtime_error(
                        "time.from_components: 2nd arg must be bool (local?)");
                bool        local = args[1].as_bool();
                const auto& m     = *args[0].as_map();

                auto get_int = [&](const char* k, int def) -> int {
                    auto it = m.find(k);
                    if (it == m.end() || !it->second.is_number()) return def;
                    return static_cast<int>(it->second.as_number());
                };

                std::tm tm{};
                tm.tm_year  = get_int("year",   1970) - 1900;
                tm.tm_mon   = get_int("month",  1)    - 1;
                tm.tm_mday  = get_int("day",    1);
                tm.tm_hour  = get_int("hour",   0);
                tm.tm_min   = get_int("minute", 0);
                tm.tm_sec   = get_int("second", 0);
                tm.tm_isdst = -1;  // let mktime figure out DST for local
                int ms      = get_int("ms",     0);

                std::time_t s = local ? std::mktime(&tm) : tm_to_utc(tm);
                if (s == static_cast<std::time_t>(-1))
                    throw std::runtime_error(
                        "time.from_components: invalid date components");
                return Value{static_cast<double>(s) * 1000.0
                             + static_cast<double>(ms)};
            })

        // _time.local_offset_ms(ms?) — local timezone offset from UTC at
        // the given instant, in milliseconds (positive = east of UTC).
        // Defaults to "right now" if no arg.
        .add_function("local_offset_ms", -1,
            [](Interpreter&, std::vector<Value> args) -> Value {
                if (args.size() > 1)
                    throw std::runtime_error(
                        "time.local_offset_ms(ms?): wrong arity");
                std::time_t s;
                if (args.empty() || args[0].is_null()) {
                    s = std::time(nullptr);
                } else {
                    if (!args[0].is_number())
                        throw std::runtime_error(
                            "time.local_offset_ms: ms must be a number");
                    s = static_cast<std::time_t>(args[0].as_number() / 1000.0);
                }
                // Trick: treat the local broken-down time AS IF it were UTC,
                // convert to time_t via timegm — diff from real UTC time_t
                // gives the offset.
                std::tm utc{}, local{};
                to_tm_utc(s, utc);
                to_tm_local(s, local);
                local.tm_isdst = 0;  // we don't want mktime to "correct" it
                std::time_t s_local_as_utc = tm_to_utc(local);
                std::time_t s_utc          = tm_to_utc(utc);
                double diff_sec = static_cast<double>(
                    static_cast<std::int64_t>(s_local_as_utc)
                    - static_cast<std::int64_t>(s_utc));
                return Value{diff_sec * 1000.0};
            })

        .build();

    interp.register_native_module("_time", m);
}

}  // namespace bnl
