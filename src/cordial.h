#pragma once

#include <array>
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

    template<typename... Coros>
    class Multiplex {
    public:
        Multiplex(const Thunk *wakeup) {
            for (auto i = 0; i < legs_.size(); i++)
                legs_[i].wakeup = [this, wakeup, i]() {
                    if (legs_[i].state == PENDING)
                        legs_[i].state = DONE;
                    (*wakeup)();
                };
        }
        const Thunk *wakeup(std::size_t i) const { return &legs_[i].wakeup; }
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
        void await_suspend(std::coroutine_handle<>) {
            await_suspend_tpl();
        }

        using Result = std::variant<
            decltype(reinterpret_cast<Coros *>(0)->await_resume())...>;

        Result await_resume() {
            return await_resume_tpl();
        }

    private:
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
