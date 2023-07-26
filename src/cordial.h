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

    /**
     * A reference-counting structure to support callbacks from the
     * "background" after a coroutine has been deallocated.
     *
     * Its function is analogous to the standard smart pointers, but
     * it has technical advantages. In particular, a simple pointer
     * can be passed on to the concrete (C?) framework for deletion in
     * the background.
     */
    class Disposable {
    public:
        virtual ~Disposable() {}
        void take() { use_count_++; }
        bool release() { return --use_count_ == 0; }
        bool disown() {
            owned_ = false;
            return release();
        }
        bool owned() const { return owned_; }

    private:
        bool owned_ { true };
        unsigned use_count_ { 1 };
    };

    /**
     * Commonalities between `Task`, `Future` and `Flow` coroutines.
     */
    class BasePromiseType {
    public:
        std::suspend_always final_suspend() noexcept {
            notify();
            return {};
        }
        void unhandled_exception() {
            exception_ = std::current_exception();
        }
        void arm(const Thunk *notify, Disposable *companion) {
            notify_ = notify;
            companion_ = companion;
        }
        void check_exception() {
            if (exception_)
                std::rethrow_exception(exception_);
        }

    protected:
        ~BasePromiseType() {
            if (companion_->disown()) {
                // No need to go through framework->dispose(). Thus,
                // BasePromiseType doesn't even need to carry a
                // framework reference around.
                delete companion_;
            }
        }
        void notify() { (*notify_)(); }

    private:
        std::exception_ptr exception_;
        const Thunk *notify_;
        Disposable *companion_;
    };

    /**
     * A "zombie" structure left behind after a coroutine has been
     * deallocated but callbacks are still pending.
     */
    template<typename PromiseType>
    class Companion : public Disposable {
    public:
        Companion(PromiseType *promise, Framework *framework) :
            electric_wakeup_ {
                [this, promise, framework]() {
                    if (owned())
                        promise->get_handle().resume();
                    if (release())
                        framework->dispose(this);
                }
            },
            lazy_wakeup_ {
                [this, framework]() {
                    take();
                    framework->executor(&electric_wakeup_)();
                }
            }
        {}
        const Thunk *wakeup() const { return &lazy_wakeup_; }
        
    private:
        Thunk electric_wakeup_;
        Thunk lazy_wakeup_;
    };

    /**
     * A class used by the `intro` boilerplate.
     *
     * @see intro(const Thunk *)
     */
    template<typename PromiseType>
    class Introspect {
    public:
        Introspect(Framework *framework, const Thunk *notify) :
            framework_ { framework }, notify_ { notify } {}
        bool await_ready() { return false; }
        bool await_suspend(std::coroutine_handle<PromiseType> h) {
            promise_ = &h.promise();
            return false;
        }
        const Thunk *await_resume() const noexcept {
            auto companion { new Companion(promise_, framework_) };
            promise_->arm(notify_, companion);
            return companion->wakeup();
        }

    private:
        Framework *framework_;
        PromiseType *promise_;
        const Thunk *notify_;
    };

    /**
     * A coroutine returning `Task` performs an operation and
     * (possibly) completes without a return value. The technical
     * return type `Nothing` is equivalent with `void`.
     */
    class Task {
    public:
        struct promise_type : public BasePromiseType {
            Task get_return_object() { return Task { get_handle() }; }
            std::suspend_never initial_suspend() { return {}; }
            void return_void() {}
            std::coroutine_handle<promise_type> get_handle() {
                return std::coroutine_handle<promise_type>::from_promise(*this);
            }
        };

        using introspect = Introspect<promise_type>;

        bool await_ready() { return false; }
        void await_suspend() {}
        void await_suspend(std::coroutine_handle<>) {}
        Nothing await_resume() {
            handle_.promise().check_exception();
            return {};
        }
        Task(Task &&other) : handle_ { other.handle_ } {
            other.handle_ = nullptr;
        }
        Task &operator=(Task &&other) {
            handle_ = other.handle_;
            other.handle_ = nullptr;
            return *this;
        }
        ~Task() { if (handle_) handle_.destroy(); }

    private:
        Task(std::coroutine_handle<promise_type> handle) : handle_ { handle } {}
        std::coroutine_handle<promise_type> handle_ {};
    };

    /**
     * A coroutine returning `Future` produces a single result via
     * `co_return EXPR`.
     */
    template<typename Result>
    class Future {
    public:
        struct promise_type : public BasePromiseType {
            Future get_return_object() { return Future { get_handle() }; }
            std::suspend_never initial_suspend() { return {}; }
            std::suspend_always return_value(Result &&value) {
                result = std::forward<Result>(value);
                return {};
            }
            std::coroutine_handle<promise_type> get_handle() {
                return std::coroutine_handle<promise_type>::from_promise(*this);
            }
            Result get_result() {
                check_exception();
                return std::move(result);
            }

            Result result {};
        };

        using introspect = Introspect<promise_type>;

        bool await_ready() { return false; }
        void await_suspend() {}
        void await_suspend(std::coroutine_handle<>) {}
        Result await_resume() { return handle_.promise().get_result(); }
        Future(Future &&other) : handle_ { other.handle_ } {
            other.handle_ = nullptr;
        }
        Future &operator=(Future &&other) {
            handle_ = other.handle_;
            other.handle_ = nullptr;
            return *this;
        }
        ~Future() { if (handle_) handle_.destroy(); }

    private:
        Future(std::coroutine_handle<promise_type> handle) :
            handle_ { handle } {}
        std::coroutine_handle<promise_type> handle_ {};
    };

    /**
     * A coroutine returning `Flow` produces a number results with
     * `co_yield` and (optionally) finishes.
     *
     * The caller receives the results wrapped in `std::optional`.
     * When the flow finishes, `nullopt` is produced. The flow should
     * not be awaited again afterward.
     */
    template<typename Result>
    class Flow {
    public:
        struct promise_type : public BasePromiseType {
            Flow get_return_object() { return Flow { get_handle() }; }
            std::suspend_always initial_suspend() { return {}; }
            std::suspend_always yield_value(Result &&value) {
                result = std::forward<Result>(value);
                notify();
                return {};
            }
            void return_void() { result = std::nullopt; }
            std::coroutine_handle<promise_type> get_handle() {
                return std::coroutine_handle<promise_type>::from_promise(*this);
            }

            std::optional<Result> get_result() {
                check_exception();
                return std::move(result);
            }

            std::optional<Result> result {};
        };

        using introspect = Introspect<promise_type>;

        bool await_ready() { return false; }
        void await_suspend() { handle_.resume(); }
        void await_suspend(std::coroutine_handle<> h) {
            await_suspend();
        }
        std::optional<Result> await_resume() {
            return handle_.promise().get_result();
        }
        Flow(Flow &&other) : handle_ { other.handle_ } {
            other.handle_ = nullptr;
        }
        Flow &operator=(Flow &&other) {
            handle_ = other.handle_;
            other.handle_ = nullptr;
            return *this;
        }
        ~Flow() { if (handle_) handle_.destroy(); }

    private:
        Flow(std::coroutine_handle<promise_type> handle) : handle_ { handle } {}
        std::coroutine_handle<promise_type> handle_ {};
    };

    /**
     * Multiplex a number of other, "participating" coroutines. The
     * Multiplex object can be awaited (with `co_await` or by calling
     * the standard await routines directly), and it resumes whenever
     * any of the participating coroutines yields or returns.
     *
     * The setup is a multistep process:
     *
     *     Multiplex<Task, Future<string>, Flow<int>> mx(wakeup);
     *     auto task { do_it(mx.wakeup(0) };
     *     auto future { find_out(mx.wakeup(1) };
     *     auto flow { pour_it(mx.wakeup(2) };
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
         * The constructor takes the wakeup thunk of the calling
         * coroutine.
         *
         * Participating coroutines can then be instantiated and tied.
         */
        Multiplex(const Thunk *wakeup) {
            for (auto i = 0; i < legs_.size(); i++)
                legs_[i].wakeup = [this, wakeup, i]() {
                    if (legs_[i].state == PENDING)
                        legs_[i].state = DONE;
                    (*wakeup)();
                };
        }

        /**
         * Participating coroutines must be instantiated (called) with
         * a wakeup thunk returned by this method. The coroutine can
         * still be used independently until it is tied for the first
         * time.
         *
         * @see tie(Coros *...)
         */
        const Thunk *wakeup(std::size_t i) const { return &legs_[i].wakeup; }

        /**
         * Assign the participating coroutines. The coroutines can be
         * used independently before they are tied. However, once a
         * coroutine has been tied, it can no longer be used outside
         * the Multiplex object's context.
         *
         * `nullptr` can take the place of a coroutine until it is
         * tied for the first time. Also, once a coroutine has
         * finished, `nullptr` should be used in its staid
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
        void await_suspend() {
            await_suspend_tpl();
        }
        void await_suspend(std::coroutine_handle<>) {
            await_suspend();
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

        Result await_resume() {
            return await_resume_tpl();
        }

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
            Thunk wakeup;
        };

        std::array<Leg, sizeof...(Coros)> legs_;
        std::tuple<Coros *...> coros_;

        template<std::size_t I = 0>
        inline typename std::enable_if<I < sizeof...(Coros), void>::type
        await_suspend_tpl() {
            if (legs_[I].state == IDLE) {
                auto coro { std::get<I>(coros_) };
                if (coro != nullptr) {
                    legs_[I].state = PENDING;
                    coro->await_suspend();
                }
            }
            await_suspend_tpl<I + 1>();
        }

        template<std::size_t I = 0>
        inline typename std::enable_if<I == sizeof...(Coros), void>::type
        await_suspend_tpl() {}

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
        Future<T> receive(const Thunk *notify) {
            auto wakeup {
                co_await
                framework_->intro<typename Future<T>::introspect>(notify)
            };
            if (status_ == CONGESTED)
                while (!peers_.empty()) {
                    auto peer { peers_.front().lock() };
                    peers_.pop();
                    if (peer) {
                        auto value { *peer->value };
                        (*peer->wakeup)();
                        co_return value;
                    }
                }
            status_ = STARVED;
            T value;
            auto peer { std::make_shared<Peer>(&value, wakeup) };
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
        Task send(const Thunk *notify, T &&value) {
            auto wakeup
                { co_await framework_->intro<Task::introspect>(notify) };
            if (status_ == STARVED)
                while (!peers_.empty()) {
                    auto peer { peers_.front().lock() };
                    peers_.pop();
                    if (peer) {
                        *peer->value = std::move(value);
                        (*peer->wakeup)();
                        co_return;
                    }
                }
            status_ = CONGESTED;
            auto peer { std::make_shared<Peer>(&value, wakeup) };
            peers_.emplace(peer);
            co_await std::suspend_always {};
        }

    private:
        Framework *framework_;
        struct Peer {
            T *value;
            const Thunk *wakeup;
        };

        // The value of status_ is a don't-care if peers_ is empty.
        enum { STARVED, CONGESTED } status_ { STARVED };

        // Either all congested senders or all starved receivers.
        std::queue<std::weak_ptr<Peer>> peers_;
    };

    /**
     * Produce a thunk that, when called, "backgrounds" the execution
     * of another thunk (`function`). The returned thunk can be called
     * any number of times as long as `function` exists.
     */
    virtual Thunk executor(const Thunk *function) = 0;

    /**
     * "Background" the deletion of the given disposable object to
     * avoid the questionable `delete this` statement.
     */
    virtual void dispose(Disposable *disposable) = 0;

    /**
     * The mandatory introductory command in any Task, Future or Flow
     * in this framework. It is used to form the necessary
     * interconnects between coroutines.
     *
     * The typical coroutine is called with a notification thunk
     * ("notify") that is used to handshake with the caller/owner of
     * the coroutine. The intro call produces another thunk ("wakeup")
     * that is used between the subsidiary coroutines of the current
     * coroutine.
     *
     * Thus, a coroutine could begin like this:
     *
     *     Task my_task(const Thunk *notify)
     *     {
     *         auto wakeup { co_await intro<Task::introspect>(notify) };
     *         co_await other_task(wakeup);
     *         auto result { co_await random(wakeup) };
     *              :     :     :
     *     }
     */
    template<typename Introspect>
    Introspect intro(const Thunk *notify = nullptr) {
        return Introspect { this, notify ?: &no_op};
    }

    /**
     * Suspend the current coroutine for the given duration, which
     * must support std::chrono::duration_cast.
     */
    template<typename Duration>
    Task delay(const Thunk *notify, Duration duration) {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>
            (duration).count();
        return delay_ns(notify, ns);
    }

    /**
     * Cede the CPU to other coroutines and resume without delay. Used
     * to prevent CPU starvation.
     */
    virtual Task reschedule(const Thunk *notify) = 0;

private:

    /**
     * An auxiliary task that blocks for the given number of
     * nanoseconds.
     *
     * @see delay(const Thunk *, Duration)
     */
    virtual Task delay_ns(const Thunk *notify, uint64_t ns) = 0;

    /**
     * A dummy thunk. It is not static so a separate source code file
     * is not needed.
     */
    Thunk no_op = [](){};
};

}; // namespace pacujo::cordial
