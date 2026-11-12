// coro.hpp — the comparison: a C++20 stackless coroutine.
//
// A fiber has its own stack, so any function in its call chain can yield. A
// C++20 coroutine has no stack of its own: the compiler rewrites one function
// into a state machine whose locals live in a heap-allocated frame, and only
// that function can suspend. Resuming it is a function call through the frame
// plus a jump into the right state; suspending it is a store of the state and
// a return. Nothing is saved or restored register by register.
//
// Generator<T> is the smallest useful coroutine type: `co_yield v` hands a
// value to the consumer and suspends; next() resumes it.
#pragma once

#include <coroutine>
#include <exception>
#include <utility>

namespace fib {

template <class T>
class Generator {
public:
    struct promise_type {
        T                  value{};
        std::exception_ptr error;

        Generator get_return_object() { return Generator{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }  // start lazily
        std::suspend_always final_suspend() noexcept { return {}; }    // keep the frame until destroyed
        std::suspend_always yield_value(T v) noexcept { value = std::move(v); return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };
    using handle = std::coroutine_handle<promise_type>;

    explicit Generator(handle h) : h_(h) {}
    Generator(Generator&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;
    ~Generator() { if (h_) h_.destroy(); }

    // Resume until the next co_yield. Returns false when the coroutine is done.
    bool next() {
        h_.resume();
        if (h_.promise().error) std::rethrow_exception(h_.promise().error);
        return !h_.done();
    }
    const T& value() const noexcept { return h_.promise().value; }

private:
    handle h_;
};

}  // namespace fib
