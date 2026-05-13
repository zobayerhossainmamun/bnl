#include "runtime/future.h"

#include <fmt/core.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "bnl/interpreter.h"
#include "runtime/environment.h"
#include "runtime/internal.h"

namespace bnl {

namespace {

// Safely invoke a user callable from C++ context. Returns true on success
// (out = return value); on failure out_err is set with the thrown value.
// Catches ThrowSignal (user `throw`), RuntimeError, and std::exception.
bool safe_call(Interpreter& interp, const CallablePtr& fn,
               std::vector<Value> args, Value& out, Value& out_err) {
    try {
        out = fn->call(interp, std::move(args));
        return true;
    } catch (ThrowSignal& sig) {
        out_err = std::move(sig.value);
    } catch (const RuntimeError& e) {
        out_err = Value{std::string(e.what())};
    } catch (const std::exception& e) {
        out_err = Value{std::string(e.what())};
    }
    return false;
}

// Settle `down` based on the outcome of running `cb(arg)`. If `cb` is null,
// propagate the originating value / error unchanged through to `down`.
void settle_through(Interpreter& interp, const FuturePtr& down,
                    const CallablePtr& cb, Value arg, bool was_rejected) {
    if (!cb) {
        if (was_rejected) down->reject(interp, std::move(arg));
        else              down->resolve(interp, std::move(arg));
        return;
    }
    Value ret;
    Value err;
    if (safe_call(interp, cb, { std::move(arg) }, ret, err)) {
        // Callback returned a Future → down adopts its state.
        down->resolve(interp, std::move(ret));
    } else {
        down->reject(interp, std::move(err));
    }
}

}  // namespace

void Future::fire_waiter(Interpreter& interp, Waiter w) {
    // Snapshot the values needed inside the microtask. `this` is captured via
    // shared_from_this so the future stays alive until the microtask runs.
    auto self = shared_from_this();
    bool rejected = (state_ == State::Rejected);
    Value carried = rejected ? error_ : value_;

    interp.enqueue_microtask(
        [&interp, self, w = std::move(w), rejected, carried = std::move(carried)]() mutable {
            const CallablePtr& cb = rejected ? w.on_err : w.on_ok;
            settle_through(interp, w.downstream, cb, std::move(carried), rejected);
        });
}

void Future::resolve(Interpreter& interp, Value v) {
    if (state_ != State::Pending) return;

    // If resolving with another Future, adopt its eventual state instead of
    // wrapping it. This is what makes `return some_future;` inside a .then
    // callback "just work" — the chain unwraps it.
    if (v.is_future()) {
        FuturePtr inner = v.as_future();
        if (inner.get() == this) {
            // Self-resolution would create an unresolvable cycle; reject with
            // a type-error string to match JS Promise behavior.
            state_ = State::Rejected;
            error_ = Value{std::string("Future cannot resolve to itself")};
        } else if (inner->is_fulfilled()) {
            state_ = State::Fulfilled;
            value_ = inner->value();
        } else if (inner->is_rejected()) {
            state_ = State::Rejected;
            error_ = inner->error();
        } else {
            // Inner still pending — chain ourselves as a waiter on it so we
            // settle when it does. Use a thunk callable that forwards.
            auto self = shared_from_this();
            inner->add_next(interp,
                std::make_shared<NativeFunction>("__adopt_ok", 1,
                    [self](Interpreter& i, std::vector<Value> args) -> Value {
                        self->resolve(i, std::move(args[0]));
                        return Value{};
                    }),
                std::make_shared<NativeFunction>("__adopt_err", 1,
                    [self](Interpreter& i, std::vector<Value> args) -> Value {
                        self->reject(i, std::move(args[0]));
                        return Value{};
                    }));
            return;
        }
    } else {
        state_ = State::Fulfilled;
        value_ = std::move(v);
    }

    auto waiters = std::move(waiters_);
    waiters_.clear();
    for (auto& w : waiters) fire_waiter(interp, std::move(w));
}

void Future::reject(Interpreter& interp, Value e) {
    if (state_ != State::Pending) return;
    state_ = State::Rejected;
    error_ = std::move(e);

    auto waiters = std::move(waiters_);
    waiters_.clear();
    for (auto& w : waiters) fire_waiter(interp, std::move(w));
}

FuturePtr Future::add_next(Interpreter& interp,
                           CallablePtr on_ok, CallablePtr on_err) {
    auto down = std::make_shared<Future>();
    Waiter w{ std::move(on_ok), std::move(on_err), down };
    if (state_ == State::Pending) {
        waiters_.push_back(std::move(w));
    } else {
        fire_waiter(interp, std::move(w));
    }
    return down;
}

FuturePtr Future::add_fail(Interpreter& interp, CallablePtr on_err) {
    return add_next(interp, nullptr, std::move(on_err));
}

FuturePtr Future::add_always(Interpreter& interp, CallablePtr on_either) {
    // .always runs on either path. The downstream future settles with the
    // ORIGINAL state — the callback's return is ignored — unless the callback
    // throws, in which case the downstream rejects with the thrown value.
    // This matches JS Promise.prototype.finally semantics.
    auto down = std::make_shared<Future>();

    auto make_wrapper = [on_either, down](bool rejected) {
        return std::make_shared<NativeFunction>("__always", 1,
            [on_either, down, rejected](Interpreter& interp,
                                        std::vector<Value> args) -> Value {
                Value carried = std::move(args[0]);
                if (on_either) {
                    Value ret;
                    Value err;
                    if (!safe_call(interp, on_either, {}, ret, err)) {
                        down->reject(interp, std::move(err));
                        return Value{};
                    }
                }
                if (rejected) down->reject(interp, std::move(carried));
                else          down->resolve(interp, std::move(carried));
                return Value{};
            });
    };

    Waiter w{ make_wrapper(false), make_wrapper(true), nullptr };
    // The wrappers handle downstream settlement themselves; we don't want
    // settle_through to also touch `down`. Use a sink future to absorb its
    // return.
    w.downstream = std::make_shared<Future>();
    if (state_ == State::Pending) {
        waiters_.push_back(std::move(w));
    } else {
        fire_waiter(interp, std::move(w));
    }
    return down;
}

// ---- FutureBuiltin: `Future(executor)` constructs a new Future ------------

Value FutureBuiltin::call(Interpreter& interp, std::vector<Value> args) {
    if (args.empty() || !args[0].is_callable()) {
        throw std::runtime_error(
            "Future(executor): executor must be a function "
            "of (resolve, reject)");
    }
    CallablePtr executor = args[0].as_callable();

    auto fut = std::make_shared<Future>();

    auto resolve_fn = std::make_shared<NativeFunction>("resolve", 1,
        [fut](Interpreter& i, std::vector<Value> a) -> Value {
            fut->resolve(i, a.empty() ? Value{} : std::move(a[0]));
            return Value{};
        });
    auto reject_fn = std::make_shared<NativeFunction>("reject", 1,
        [fut](Interpreter& i, std::vector<Value> a) -> Value {
            fut->reject(i, a.empty() ? Value{} : std::move(a[0]));
            return Value{};
        });

    // Run the executor synchronously. If it throws, reject the future with
    // the thrown value — this is how `Future(function (res, rej) { throw "x"; })`
    // produces a rejected future.
    Value ret, err;
    if (!safe_call(interp, executor,
                   { Value{resolve_fn}, Value{reject_fn} }, ret, err)) {
        fut->reject(interp, std::move(err));
    }
    return Value{fut};
}

// ---- Statics: Future.of, Future.err, Future.all, Future.race --------------

namespace {

CallablePtr make_of_static() {
    return std::make_shared<NativeFunction>("Future.of", -1,
        [](Interpreter& interp, std::vector<Value> args) -> Value {
            auto fut = std::make_shared<Future>();
            fut->resolve(interp, args.empty() ? Value{} : std::move(args[0]));
            return Value{fut};
        });
}

CallablePtr make_err_static() {
    return std::make_shared<NativeFunction>("Future.err", -1,
        [](Interpreter& interp, std::vector<Value> args) -> Value {
            auto fut = std::make_shared<Future>();
            fut->reject(interp, args.empty() ? Value{} : std::move(args[0]));
            return Value{fut};
        });
}

// Future.all([f1, f2, ...]) → Future that fulfills with [v1, v2, ...] when
// every input fulfills, OR rejects with the first rejection.
CallablePtr make_all_static() {
    return std::make_shared<NativeFunction>("Future.all", 1,
        [](Interpreter& interp, std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list()) {
                throw std::runtime_error("Future.all: expected a list");
            }
            const auto& items = *args[0].as_list();
            auto result_fut = std::make_shared<Future>();
            auto results = std::make_shared<std::vector<Value>>(items.size());
            auto remaining = std::make_shared<std::size_t>(items.size());
            auto settled = std::make_shared<bool>(false);

            if (items.empty()) {
                result_fut->resolve(interp, Value{std::make_shared<std::vector<Value>>()});
                return Value{result_fut};
            }

            for (std::size_t i = 0; i < items.size(); ++i) {
                FuturePtr f;
                if (items[i].is_future()) {
                    f = items[i].as_future();
                } else {
                    f = std::make_shared<Future>();
                    f->resolve(interp, items[i]);
                }
                auto on_ok = std::make_shared<NativeFunction>("__all_ok", 1,
                    [result_fut, results, remaining, settled, i](
                        Interpreter& ii, std::vector<Value> a) -> Value {
                        if (*settled) return Value{};
                        (*results)[i] = a.empty() ? Value{} : std::move(a[0]);
                        if (--(*remaining) == 0) {
                            *settled = true;
                            result_fut->resolve(ii, Value{results});
                        }
                        return Value{};
                    });
                auto on_err = std::make_shared<NativeFunction>("__all_err", 1,
                    [result_fut, settled](
                        Interpreter& ii, std::vector<Value> a) -> Value {
                        if (*settled) return Value{};
                        *settled = true;
                        result_fut->reject(ii, a.empty() ? Value{} : std::move(a[0]));
                        return Value{};
                    });
                f->add_next(interp, on_ok, on_err);
            }
            return Value{result_fut};
        });
}

// Future.race([f1, f2, ...]) → settles with the first input to settle.
CallablePtr make_race_static() {
    return std::make_shared<NativeFunction>("Future.race", 1,
        [](Interpreter& interp, std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list()) {
                throw std::runtime_error("Future.race: expected a list");
            }
            const auto& items = *args[0].as_list();
            auto result_fut = std::make_shared<Future>();
            auto settled = std::make_shared<bool>(false);

            for (const auto& item : items) {
                FuturePtr f;
                if (item.is_future()) f = item.as_future();
                else { f = std::make_shared<Future>(); f->resolve(interp, item); }

                auto on_ok = std::make_shared<NativeFunction>("__race_ok", 1,
                    [result_fut, settled](Interpreter& ii, std::vector<Value> a) -> Value {
                        if (*settled) return Value{};
                        *settled = true;
                        result_fut->resolve(ii, a.empty() ? Value{} : std::move(a[0]));
                        return Value{};
                    });
                auto on_err = std::make_shared<NativeFunction>("__race_err", 1,
                    [result_fut, settled](Interpreter& ii, std::vector<Value> a) -> Value {
                        if (*settled) return Value{};
                        *settled = true;
                        result_fut->reject(ii, a.empty() ? Value{} : std::move(a[0]));
                        return Value{};
                    });
                f->add_next(interp, on_ok, on_err);
            }
            return Value{result_fut};
        });
}

// Future.all_settled([f1, f2, ...]) → fulfills with a list of
// {ok: bool, value?: v, error?: e} maps describing each input's outcome.
// Never rejects.
CallablePtr make_all_settled_static() {
    return std::make_shared<NativeFunction>("Future.all_settled", 1,
        [](Interpreter& interp, std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list()) {
                throw std::runtime_error("Future.all_settled: expected a list");
            }
            const auto& items = *args[0].as_list();
            auto result_fut = std::make_shared<Future>();
            auto results = std::make_shared<std::vector<Value>>(items.size());
            auto remaining = std::make_shared<std::size_t>(items.size());

            if (items.empty()) {
                result_fut->resolve(interp, Value{std::make_shared<std::vector<Value>>()});
                return Value{result_fut};
            }

            auto record = [results, remaining, result_fut](
                Interpreter& ii, std::size_t i, bool ok, Value carried) {
                auto entry = std::make_shared<std::unordered_map<std::string, Value>>();
                (*entry)["ok"] = Value{ok};
                if (ok) (*entry)["value"] = std::move(carried);
                else    (*entry)["error"] = std::move(carried);
                (*results)[i] = Value{entry};
                if (--(*remaining) == 0) {
                    result_fut->resolve(ii, Value{results});
                }
            };

            for (std::size_t i = 0; i < items.size(); ++i) {
                FuturePtr f;
                if (items[i].is_future()) f = items[i].as_future();
                else { f = std::make_shared<Future>(); f->resolve(interp, items[i]); }

                auto on_ok = std::make_shared<NativeFunction>("__alls_ok", 1,
                    [record, i](Interpreter& ii, std::vector<Value> a) -> Value {
                        record(ii, i, true, a.empty() ? Value{} : std::move(a[0]));
                        return Value{};
                    });
                auto on_err = std::make_shared<NativeFunction>("__alls_err", 1,
                    [record, i](Interpreter& ii, std::vector<Value> a) -> Value {
                        record(ii, i, false, a.empty() ? Value{} : std::move(a[0]));
                        return Value{};
                    });
                f->add_next(interp, on_ok, on_err);
            }
            return Value{result_fut};
        });
}

}  // namespace

FutureBuiltin::FutureBuiltin() {
    statics_["of"]          = make_of_static();
    statics_["err"]         = make_err_static();
    statics_["all"]         = make_all_static();
    statics_["race"]        = make_race_static();
    statics_["all_settled"] = make_all_settled_static();
}

void register_future(Interpreter& interp) {
    auto builtin = std::make_shared<FutureBuiltin>();
    interp.globals()->define("Future", Value{builtin});

    // futurify(fn): wrap a callback-style fn (last arg is (err, result) callback)
    // as a Future-returning function. usage:
    //     var get_async = futurify(r.get);
    //     get_async(url).next(...);
    auto futurify_fn = std::make_shared<NativeFunction>("futurify", 1,
        [](Interpreter& /*i*/, std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_callable()) {
                throw std::runtime_error(
                    "futurify(fn): fn must be a callback-style function");
            }
            CallablePtr inner = args[0].as_callable();
            // Return a variadic function. When called with (a, b, c), invokes
            // inner(a, b, c, function(err, result) { ... resolve/reject }).
            return Value{ std::make_shared<NativeFunction>(
                "futurified", -1,
                [inner](Interpreter& ii, std::vector<Value> call_args) -> Value {
                    auto fut = std::make_shared<Future>();
                    auto cb = std::make_shared<NativeFunction>("__cb", -1,
                        [fut](Interpreter& iii, std::vector<Value> cb_args) -> Value {
                            Value err = cb_args.size() > 0 ? cb_args[0] : Value{};
                            Value res = cb_args.size() > 1 ? cb_args[1] : Value{};
                            if (!err.is_null()) fut->reject(iii, std::move(err));
                            else                fut->resolve(iii, std::move(res));
                            return Value{};
                        });
                    call_args.push_back(Value{cb});
                    inner->call(ii, std::move(call_args));
                    return Value{fut};
                }) };
        });
    interp.globals()->define("futurify", Value{futurify_fn});
}

}  // namespace bnl
