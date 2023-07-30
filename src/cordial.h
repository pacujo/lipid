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
    class BaseTask;

    template<typename Promise>
    class Resumer;

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

    template<typename Promise>
    class Resumer : public Disposable {
    public:
        Resumer(Promise *promise) :
            pulse_ { promise->pulse_ },
            resume_ {
                [this, promise]() {
                    auto pulse { pulse_.lock() };
                    if (pulse)
                        promise->get_handle().resume();
                    promise->framework_->dispose(this);
                }
            }
        {}
        const Thunk *resumer() const { return &resume_; }
        
    private:
        WeakPulse pulse_;
        Thunk resume_;
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
     * Multiplex a number of other, "participating" coroutines. The
     * Multiplex object can be awaited (with `co_await` or by calling
     * the standard await routines directly), and it resumes whenever
     * any of the participating coroutines yields or returns.
     *
     * The setup is a multistep process:
     *     
     *     auto [_, resume]
     *         { co_await Introspect<MyCoro::promise_type> {} };
     *     Multiplex<Task, Future<string>, Flow<int>> mx { resume };
     *     auto task { do_it() };
     *     auto future { find_out() };
     *     auto flow { pour_it() };
     *     for (;;) {
     *          auto result { co_await mx.tie(&task, &future, &flow) };
     *          if (result.index() == 0) {
     *              auto &value { std::get<0>(result) };
     *                 :       :       :
     */
    template<typename... Coros>
    class Multiplex {
    public:

        /**
         * The constructor takes the resume thunk of the calling
         * coroutine.
         *
         * Participating coroutines can then be instantiated and tied.
         */
        Multiplex(const Thunk *resume) {
            for (auto i = 0; i < legs_.size(); i++)
                legs_[i].resume = [this, resume, i]() {
                    if (legs_[i].state == PENDING)
                        legs_[i].state = DONE;
                    (*resume)();
                };
        }

        /**
         * Assign the participating coroutines. The participating
         * coroutines cannot be awaited independently but the
         * `Multiplex` object must be awaited instead.
         *
         * `nullptr` can take the place of a coroutine until it is
         * tied for the first time. Also, once a coroutine has
         * finished, `nullptr` should be used in its stead
         * subsequently.
         *
         * You can tie the participating coroutines once and await the
         * Multiplex object itself, or you can tie repeatedly and
         * await the return value. However, you must always specify
         * the same coroutine in the same position (or replace it with
         * `nullptr` as described above).
         */
        Multiplex &tie(Coros *... coros) {
            coros_ = std::tie(coros...);
            return *this;
        }

        bool await_ready() {
            for (const auto &leg : legs_)
                if (leg.state == DONE)
                    return true;
            return false;
        }

        template<typename AwaitingPromise>
        void await_suspend(std::coroutine_handle<AwaitingPromise> awaiter) {
            await_suspend_tpl(awaiter);
        }

        /**
         * The coroutine results are returned one at a time using this
         * variant type. The variant index corresponds to the index of
         * the participating coroutine. And the type matches that of
         * the coroutine.
         *
         * No fairness is guaranteed. A flow that keeps producing
         * results may prevent other coroutines from communicating
         * their results.
         */
        using Result = std::variant<
            decltype(reinterpret_cast<Coros *>(0)->await_resume())...>;

        Result await_resume() { return await_resume_tpl(); }

    private:

        /**
         * Every participating coroutine ("leg") rotates through these
         * three states. When the Multiplex object is suspended (via
         * `co_await`), the legs move from `IDLE` to `PENDING`. As
         * legs finish, they move from `PENDING` to `DONE`. Finally,
         * as the result of the leg is relayed to the caller, it is
         * moved back from `DONE` to `IDLE`.
         */
        enum State { IDLE, PENDING, DONE };

        struct Leg {
            State state { IDLE };
            Thunk resume;
        };

        std::array<Leg, sizeof...(Coros)> legs_;
        std::tuple<Coros *...> coros_;

        template<std::size_t I = 0, typename AwaitingPromise>
        inline typename std::enable_if<I < sizeof...(Coros), void>::type
        await_suspend_tpl(std::coroutine_handle<AwaitingPromise> awaiter) {
            if (legs_[I].state == IDLE) {
                auto coro { std::get<I>(coros_) };
                if (coro != nullptr) {
                    legs_[I].state = PENDING;
                    coro->await_suspend(awaiter, &legs_[I].resume);
                }
            }
            await_suspend_tpl<I + 1>(awaiter);
        }

        template<std::size_t I = 0, typename AwaitingPromise>
        inline typename std::enable_if<I == sizeof...(Coros), void>::type
        await_suspend_tpl(std::coroutine_handle<AwaitingPromise>) {}

        template<std::size_t I = 0>
        inline typename std::enable_if<I < sizeof...(Coros), Result>::type
        await_resume_tpl() {
            if (legs_[I].state == DONE) {
                legs_[I].state = IDLE;
                return Result {
                    std::in_place_index<I>,
                    std::get<I>(coros_)->await_resume()
                };
            }
            return await_resume_tpl<I + 1>();
        }

        template<std::size_t I = 0>
        inline typename std::enable_if<I == sizeof...(Coros), Result>::type
        await_resume_tpl() {
            assert(false);
        }
    };

    /**
     * A RendezVous object is a synchronous messaging channel between
     * any number of sending and receiving coroutines. Sending is
     * blocked until receiving takes place and vice versa.
     */
    template<typename T>
    class RendezVous {
    public:

        /**
         * The RendezVous constructor takes the framework as an
         * argument.
         */
        RendezVous(Framework *framework) : framework_ { framework } {}
        RendezVous(const RendezVous &) = delete;

        /**
         * A receiving `Future` coroutine. Wait until at least one
         * sending coroutine is offering a value and return it. In
         * case of contention between multiple receivers, the receiver
         * that has waited longest gets the value.
         */
        Future<T> receive() {
            if (status_ == CONGESTED)
                while (!peers_.empty()) {
                    auto peer { peers_.front().lock() };
                    peers_.pop();
                    if (peer) {
                        auto value { *peer->value };
                        (*peer->resume)();
                        co_return value;
                    }
                }
            status_ = STARVED;
            T value;
            auto [handle, resume]
                { co_await Introspect<typename Future<T>::promise_type> {} };
            auto peer { std::make_shared<Peer>(&value, resume) };
            peers_.emplace(peer);
            co_await std::suspend_always {};
            co_return std::move(value);
        }

        /**
         * A sending `Task` coroutine. Wait until at least one
         * receiving coroutine is awaiting a value and relay it. In
         * case of contention between multiple senders, the sender
         * that has waited longest sends the value.
         */
        Task send(T &&value) {
            if (status_ == STARVED)
                while (!peers_.empty()) {
                    auto peer { peers_.front().lock() };
                    peers_.pop();
                    if (peer) {
                        *peer->value = std::move(value);
                        (*peer->resume)();
                        co_return;
                    }
                }
            status_ = CONGESTED;
            auto [handle, resume]
                { co_await Introspect<typename Future<T>::promise_type> {} };
            auto peer { std::make_shared<Peer>(&value, resume) };
            peers_.emplace(peer);
            co_await std::suspend_always {};
        }

    private:
        Framework *framework_;
        struct Peer {
            T *value;
            const Thunk *resume;
        };

        // The value of status_ is a don't-care if peers_ is empty.
        enum { STARVED, CONGESTED } status_ { STARVED };

        // Either all congested senders or all starved receivers.
        std::queue<std::weak_ptr<Peer>> peers_;
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

    /**
     * A dummy thunk. It is not static so a separate source code file
     * is not needed.
     */
    Thunk no_op = [](){};
};

}; // namespace pacujo::cordial
