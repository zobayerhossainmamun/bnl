#include "stdlib/registry.h"

#include <fmt/core.h>
#include <uv.h>

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/native_module.h"
#include "bnl/value.h"

namespace bnl {

namespace {

// Per-handle state, allocated on the heap and pointed to by uv_timer_t::data.
struct TimerData {
    CallablePtr             callback;
    Interpreter*            interp;
    std::shared_ptr<bool>   alive;  // false once the handle has been closed
};

void on_timer_close(uv_handle_t* h) {
    auto* data  = static_cast<TimerData*>(h->data);
    auto* timer = reinterpret_cast<uv_timer_t*>(h);
    if (data && data->alive) *data->alive = false;
    delete data;
    delete timer;
}

void close_timer_once(uv_timer_t* timer) {
    auto* h = reinterpret_cast<uv_handle_t*>(timer);
    if (!uv_is_closing(h)) {
        uv_timer_stop(timer);
        uv_close(h, on_timer_close);
    }
}

void on_timer_fire(uv_timer_t* timer) {
    auto* data = static_cast<TimerData*>(timer->data);
    if (!data || !data->callback) return;

    try {
        data->callback->call(*data->interp, {});
    } catch (ThrowSignal& sig) {
        fmt::print(stderr, "uncaught throw in timer callback: {}\n",
                   sig.value.to_display());
        data->interp->mark_loop_failed();
        close_timer_once(timer);
        return;
    } catch (const RuntimeError& e) {
        fmt::print(stderr, "uncaught error in timer callback at {}:{} (near '{}'): {}\n",
                   e.token.line, e.token.column, e.token.lexeme, e.what());
        data->interp->mark_loop_failed();
        close_timer_once(timer);
        return;
    } catch (const std::exception& e) {
        fmt::print(stderr, "uncaught error in timer callback: {}\n", e.what());
        data->interp->mark_loop_failed();
        close_timer_once(timer);
        return;
    }

    // One-shot timers (repeat == 0) auto-close after firing.
    if (uv_timer_get_repeat(timer) == 0) {
        close_timer_once(timer);
    }
}

// Builds the cancel function returned to bnl. Calling it stops the timer
// (no-op if already closed). Captures `alive` by value so a stale cancel
// after auto-close doesn't touch a freed timer.
Value make_cancel(uv_timer_t* timer, std::shared_ptr<bool> alive) {
    auto fn = std::make_shared<NativeFunction>(
        "cancel", 0,
        [timer, alive](Interpreter&, std::vector<Value>) -> Value {
            if (!*alive) return Value{};
            close_timer_once(timer);
            // on_timer_close will set *alive = false eventually; flip now too
            // so any second cancel() in the same tick is a no-op.
            *alive = false;
            return Value{};
        });
    return Value{std::static_pointer_cast<Callable>(fn)};
}

uint64_t to_ms(const Value& v) {
    if (!v.is_number())
        throw std::runtime_error(fmt::format("timers: ms must be a number, got {}", v.type_name()));
    double n = v.as_number();
    if (n < 0) n = 0;
    return static_cast<uint64_t>(n);
}

CallablePtr to_callable(const Value& v, const char* where) {
    if (!v.is_callable())
        throw std::runtime_error(fmt::format("{}: callback must be a function, got {}",
                                             where, v.type_name()));
    return v.as_callable();
}

}  // namespace

void register_timers(Interpreter& interp) {
    auto m = NativeModule("timers")
        // timers.set(ms, fn) -> cancel callable. One-shot timer.
        .add_function("set", 2,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                uint64_t    ms = to_ms(args[0]);
                CallablePtr cb = to_callable(args[1], "timers.set");

                auto* timer = new uv_timer_t{};
                uv_timer_init(interp.loop(), timer);

                auto alive = std::make_shared<bool>(true);
                timer->data = new TimerData{cb, &interp, alive};

                uv_timer_start(timer, on_timer_fire, ms, /*repeat=*/0);
                return make_cancel(timer, alive);
            })

        // timers.interval(ms, fn) -> cancel callable. Repeats every ms.
        .add_function("interval", 2,
            [&interp](Interpreter&, std::vector<Value> args) -> Value {
                uint64_t    ms = to_ms(args[0]);
                if (ms == 0) ms = 1;  // libuv treats 0 as one-shot for repeat
                CallablePtr cb = to_callable(args[1], "timers.interval");

                auto* timer = new uv_timer_t{};
                uv_timer_init(interp.loop(), timer);

                auto alive = std::make_shared<bool>(true);
                timer->data = new TimerData{cb, &interp, alive};

                uv_timer_start(timer, on_timer_fire, ms, /*repeat=*/ms);
                return make_cancel(timer, alive);
            })

        .build();

    interp.register_native_module("timers", m);
}

}  // namespace bnl
