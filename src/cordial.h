#pragma once

#include <array>
#include <coroutine>
#include <memory>
#include <optional>
#include <queue>

namespace pacujo::cordial {

/**
 * A parameterless lambda.
 */
using Thunk = std::function<void()>;

/**
 * A utility function for converting an `errno` value to an exception
 * with a single string argument.
 */
void throw_errno [[noreturn]] (const std::string &what_arg) {
    throw std::system_error(errno, std::generic_category(), what_arg);
}

/**
 * A utility function for converting an `errno` value to an exception
 * without an extra argument.
 */
void throw_errno [[noreturn]] () {
    throw std::system_error(errno, std::generic_category());
}

/**
 * A technical synonym for `void`. Needed in contexts where `void` is
 * not allowed as a placeholder type.
 */
struct Nothing {};

/**
 * A header-only, generic, abstract coroutine framework made from the
 * C++ coroutine building blocks.
 * 
 * A number of virtual functions need to be provided by the concrete
 * framework.
 */
class Framework {
public:
    virtual ~Framework() {}

    struct Disposable {
        virtual ~Disposable() {}
    };

    using StrongPulse = std::shared_ptr<Nothing>;
    using WeakPulse = std::weak_ptr<Nothing>;

    template<typename Promise>
    class Resumer : public Disposable {
    public:
        Resumer(Promise *promise) :
            pulse_ { promise->pulse_ },
            framework_ { promise->framework_ },
            resume_ {
                [this, promise]() {
                    if (pulse_.lock())
                        promise->get_handle().resume();
                    framework_->dispose(this);
                }
            }
        {}
        const Thunk *resumer() const { return &resume_; }
        
    private:
        WeakPulse pulse_;
        Framework *framework_;
        Thunk resume_;
    };

    template<typename Promise>
    class BaseTask;

    /**
     * Commonalities between `TaskPromise`, `FuturePromise` and `FlowPromise`.
     */
    template<typename Result>
    class BasePromise {
        template<typename Promise>
        friend class BaseTask;

        template<typename Promise>
        friend class Resumer;

    public:
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept {
            notify();
            return {};
        }
        void unhandled_exception() {
            exception_ = std::current_exception();
        }
        void arm(const Thunk *notify) { notify_ = notify; }
        Framework *framework() const { return framework_; }
        Result get_result() {
            if (exception_)
                std::rethrow_exception(exception_);
            return std::move(result_);
        }
        const Thunk *resumer() const { return &lazy_resume_; }

    protected:
        void notify() { (*notify_)(); }

        Result result_ {};

    private:
        std::exception_ptr exception_;
        Framework *framework_ {};
        const Thunk *notify_ {};
        Thunk lazy_resume_;
        StrongPulse pulse_ { std::make_shared<Nothing>() };
    };

    /**
     * Discover the handle and resume thunk of the running coroutine:
     *
     *     auto [handle, resume]
     *         { co_await Introspect<MyCoro::promise_type> {} };
     *
     * The introspection coroutine finishes immediately without suspending.
     */
    template<typename Promise>
    class Introspect {
    public:
        bool await_ready() { return false; }
        bool await_suspend(std::coroutine_handle<Promise> h) {
            handle_ = h;
            return false;
        }
        auto await_resume() const noexcept {
            return std::pair(handle_, handle_.promise().resumer());
        }

    private:
        std::coroutine_handle<Promise> handle_;
    };

    /**
     * Commonalities between `Task`, `Future` and `Flow`.
     */
    template<typename Promise>
    class BaseTask {
    public:
        using promise_type = Promise;

        BaseTask(std::coroutine_handle<Promise> handle) : handle_ { handle } {}
        BaseTask(BaseTask &&other) : handle_ { other.handle_ } {
            other.handle_ = nullptr;
        }
        BaseTask &operator=(BaseTask &&other) {
            handle_ = other.handle_;
            other.handle_ = nullptr;
            return *this;
        }
        ~BaseTask() { if (handle_) handle_.destroy(); }
        bool await_ready() { return false; }

        /**
         * Use this version of await_suspend() to start a coroutine
         * from a non-coroutine. The optional `notify` argument is
         * only used internally.
         */
        void await_suspend(Framework *framework, const Thunk *notify = nullptr) {
            auto promise { &handle_.promise() };
            // This is how the framework pointer propagates behind the
            // scenes throughout the mesh of coroutines.
            if (!promise->framework_) {
                promise->framework_ = framework;
                promise->lazy_resume_ = [promise]() {
                    auto resumer { (new Resumer(promise))->resumer() };
                    promise->framework_->execute(resumer);
                };
                promise->notify_ = notify;
            }
            handle_.resume();
        }

        /**
         * Coroutines are typically invoked with `co_await`. However,
         * more complicated use cases (like multiplexing) entail
         * micromanaging the process in user code:
         *
         *     auto [handle, resume]
         *         { co_await Introspect<MyCoro::promise_type> {} };
         *     Thunk resume_special { tweak(resume) };
         *     auto other { other_coro() };
         *     other.await_suspend(handle, &resume_special);
         *     co_await std::suspend_always {};
         *     auto result { other.await_resume() };
         */
        template<typename AwaitingPromise>
        void await_suspend(std::coroutine_handle<AwaitingPromise> awaiter,
                           const Thunk *notify = nullptr) {
            auto awaiter_promise { &awaiter.promise() };
            await_suspend(awaiter_promise->framework_,
                          notify ?: awaiter_promise->resumer());
        }

        auto await_resume() { return handle_.promise().get_result(); }

    private:
        std::coroutine_handle<Promise> handle_ {};
    };

    struct TaskPromise;

    /**
     * A coroutine returning `Task` performs an operation and
     * (possibly) completes without a return value. The technical
     * return type `Nothing` is equivalent with `void`.
     */
    using Task = BaseTask<TaskPromise>;

    struct TaskPromise : BasePromise<Nothing> {
        auto get_return_object() { return Task { get_handle() }; }
        void return_void() {}
        std::coroutine_handle<TaskPromise> get_handle() {
            return std::coroutine_handle<TaskPromise>::from_promise(*this);
        }
    };

    template<typename Result> struct FuturePromise;

    /**
     * A coroutine returning `Future` produces a single result via
     * `co_return EXPR`.
     */
    template<typename Result>
    using Future = BaseTask<FuturePromise<Result>>;

    template<typename Result>
    struct FuturePromise : BasePromise<Result> {
        auto get_return_object() { return Future<Result> { get_handle() }; }
        std::suspend_always return_value(Result &&value) {
            this->result_ = std::forward<Result>(value);
            return {};
        }
        std::coroutine_handle<FuturePromise> get_handle() {
            return std::coroutine_handle<FuturePromise>::from_promise(*this);
        }
    };

    template<typename Result> struct FlowPromise;

    /**
     * A coroutine returning `Flow` produces a number results with
     * `co_yield` and (optionally) finishes.
     *
     * The caller receives the results wrapped in `std::optional`.
     * When the flow finishes, `nullopt` is produced. The flow should
     * not be awaited again afterward.
     */
    template<typename Result>
    using Flow = BaseTask<FlowPromise<Result>>;

    template<typename Result>
    struct FlowPromise : BasePromise<std::optional<Result>> {
        auto get_return_object() { return Flow<Result> { get_handle() }; }
        std::suspend_always yield_value(Result &&value) {
            this->result_ = std::forward<Result>(value);
            this->notify();
            return {};
        }
        void return_void() { this->result_ = {}; }
        std::coroutine_handle<FlowPromise> get_handle() {
            return std::coroutine_handle<FlowPromise>::from_promise(*this);
        }
    };

    /**
     * "Background" the execution of a thunk
     */
    virtual void execute(const Thunk *function) = 0;

    /**
     * "Background" the deletion of the given disposable object to
     * avoid the questionable `delete this` statement.
     */
    virtual void dispose(Disposable *disposable) = 0;

    /**
     * Suspend the current coroutine for the given duration, which
     * must support std::chrono::duration_cast.
     */
    template<typename Duration>
    Task delay(Duration duration) {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>
            (duration).count();
        return delay_ns(ns);
    }

    /**
     * Cede the CPU to other coroutines and resume without delay. Used
     * to prevent CPU starvation.
     */
    virtual Task reschedule() = 0;

private:

    /**
     * An auxiliary task that blocks for the given number of
     * nanoseconds.
     *
     * @see delay(const Thunk *, Duration)
     */
    virtual Task delay_ns(uint64_t ns) = 0;
};

}; // namespace pacujo::cordial
