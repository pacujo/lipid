#pragma once

#include <coroutine>
#include <optional>

namespace pacujo::cordial {

using Thunk = std::function<void()>;

void throw_errno [[noreturn]] (const std::string &what_arg) {
    throw std::system_error(errno, std::generic_category(), what_arg);
}
void throw_errno [[noreturn]] () {
    throw std::system_error(errno, std::generic_category());
}

struct Nothing {};

class Framework {
public:
    virtual ~Framework() {}

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

    template<typename Left, typename Right>
    class Multiplex {
    public:
        Multiplex(const Thunk *wakeup) :
            wakeup_left_ {
                [this, wakeup]() {
                    /* Allow awaiting the left leg separately before
                     * the right leg is available for tying. */
                    if (left_state_ == PENDING)
                        left_state_ = DONE;
                    (*wakeup)();
                }
            },
            wakeup_right_ {
                [this, wakeup]() {
                    assert(right_state_ == PENDING);
                    right_state_ = DONE;
                    (*wakeup)();
                }
            }
        {}
        const Thunk *wakeup_left() const { return &wakeup_left_; }
        const Thunk *wakeup_right() const { return &wakeup_right_; }
        Multiplex &tie(Left *left_coro, Right *right_coro) {
            left_coro_ = left_coro;
            right_coro_ = right_coro;
            return *this;
        }
        bool await_ready() {
            return left_state_ == DONE || right_state_ == DONE;
        }
        void await_suspend(std::coroutine_handle<>) {
            if (left_state_ == IDLE) {
                left_state_ = PENDING;
                left_coro_->await_suspend();
            }
            if (right_state_ == IDLE) {
                right_state_ = PENDING;
                right_coro_->await_suspend();
            }
        }
        decltype(auto) await_resume() {
            using LeftVal = decltype(left_coro_->await_resume());
            using RightVal = decltype(right_coro_->await_resume());

            if (left_state_ == DONE) {
                left_state_ = IDLE;
                std::variant<LeftVal, RightVal> value
                    { std::in_place_index<LEFT>, left_coro_->await_resume() };
                return Either<LeftVal, RightVal> { std::move(value) };
            }
            assert(right_state_ == DONE);
            right_state_ = IDLE;
            std::variant<LeftVal, RightVal> value
                { std::in_place_index<RIGHT>, right_coro_->await_resume() };
            return Either<LeftVal, RightVal> { std::move(value) };
        }

    private:
        enum Choice { LEFT, RIGHT };
        
        template<typename LeftVal, typename RightVal>
        class Either {
        public:
            Either(std::variant<LeftVal, RightVal> value) :
                value_ { std::move(value) } {}
            bool got_left() const { return value_.index() == LEFT; }
            bool got_right() const { return value_.index() == RIGHT; }
            const LeftVal &get_left() const { return std::get<LEFT>(value_); }
            const RightVal &get_right() const {
                return std::get<RIGHT>(value_);
            }
            
        private:
            std::variant<LeftVal, RightVal> value_;
        };

        enum HalfState { IDLE, PENDING, DONE };

        HalfState left_state_ { IDLE };
        HalfState right_state_ { IDLE };
        Thunk wakeup_left_;
        Thunk wakeup_right_;
        Left *left_coro_;
        Right *right_coro_;
    };

    virtual Thunk executor(const Thunk *function) = 0;
    virtual void dispose(Disposable *disposable) = 0;

    template<typename Introspect>
    Introspect intro(const Thunk *notify = nullptr) {
        return Introspect { this, notify ?: &no_op};
    }

    template<typename Duration>
    Task delay(const Thunk *notify, Duration duration) {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>
            (duration).count();
        return delay_ns(notify, ns);
    }

    virtual Task reschedule(const Thunk *notify) = 0;

private:
    virtual Task delay_ns(const Thunk *notify, uint64_t ns) = 0;

    Thunk no_op = [](){};
};

}; // namespace pacujo::cordial
