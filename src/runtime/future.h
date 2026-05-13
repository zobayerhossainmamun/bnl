#pragma once

// Internal — the Future runtime type.
//
// A Future is bnl's async value: it carries a *pending* state that eventually
// becomes either *fulfilled* with a value or *rejected* with an error, and
// runs callbacks queued via .then / .fail / .always when it settles.
//
// State machine:
//
//   pending  --resolve(v)--> fulfilled (value held in value_)
//   pending  --reject(e)-->  rejected  (error held in error_)
//   fulfilled, rejected      terminal — further resolve/reject calls no-op
//
// Continuations queued on a pending Future accumulate in waiters_. When the
// Future settles, each waiter is enqueued as a microtask on the Interpreter,
// to be drained at the next loop iteration boundary. This prevents stack
// blowup on long .then chains and matches the JS Promise specification.

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bnl/value.h"

namespace bnl {

class Interpreter;

class Future : public std::enable_shared_from_this<Future> {
public:
    enum class State { Pending, Fulfilled, Rejected };

    Future() = default;

    State        state()        const { return state_; }
    bool         is_pending()   const { return state_ == State::Pending; }
    bool         is_fulfilled() const { return state_ == State::Fulfilled; }
    bool         is_rejected()  const { return state_ == State::Rejected; }
    const Value& value()        const { return value_; }
    const Value& error()        const { return error_; }

    // Settle the future. Safe to call from anywhere; if already settled, the
    // call is a no-op (first writer wins, matching Promise semantics). When
    // resolving with another Future, the receiver adopts that Future's
    // eventual state instead of treating it as a plain value.
    void resolve(Interpreter& interp, Value v);
    void reject (Interpreter& interp, Value e);

    // Attach continuations. on_ok / on_err may be null callables: if so, the
    // corresponding state propagates through to the returned downstream Future
    // unchanged. Any callback that throws causes the downstream to reject
    // with the thrown value.
    FuturePtr add_next  (Interpreter& interp, CallablePtr on_ok, CallablePtr on_err);
    FuturePtr add_fail  (Interpreter& interp, CallablePtr on_err);
    FuturePtr add_always(Interpreter& interp, CallablePtr on_either);

private:
    struct Waiter {
        CallablePtr on_ok;     // may be null
        CallablePtr on_err;    // may be null
        FuturePtr   downstream;
    };

    void fire_waiter(Interpreter& interp, Waiter w);

    State              state_ = State::Pending;
    Value              value_;
    Value              error_;
    std::vector<Waiter> waiters_;
};

// Tag class so MemberExpr can recognize the Future global and dispatch
// Future.of / Future.err / Future.all / etc. Calling the global itself
// runs an executor function: `Future(function (resolve, reject) {...})`.
// (The executor's params are user-named — only the *static methods* go
// through find_static.)
//
// Statics live on the instance so MemberExpr can look them up without the
// Interpreter holding a separate Future-specific registry.
class FutureBuiltin : public Callable {
public:
    FutureBuiltin();
    int         arity() const override { return 1; }
    std::string name()  const override { return "Future"; }
    Value       call(Interpreter& interp, std::vector<Value> args) override;

    CallablePtr find_static(const std::string& name) const {
        auto it = statics_.find(name);
        return it == statics_.end() ? nullptr : it->second;
    }

private:
    std::unordered_map<std::string, CallablePtr> statics_;
};

// Register the `Future` global and the `futurify` helper.
void register_future(Interpreter& interp);

}  // namespace bnl
