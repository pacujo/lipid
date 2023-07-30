#pragma once

#include <chrono>
#include <async/async.h>
#include "cordial.h"

namespace pacujo::coasync {

class Framework : public pacujo::cordial::Framework {
public:
    ~Framework() { destroy_async(async_); }

    async_t *get_async() const { return async_; }

    static action_1 thunk_to_action(const pacujo::cordial::Thunk *resume) {
        void *obj = (void *) resume;
        return { obj, execute };
    }

private:
    class Timer {
    public:
        Timer(async_t *async, uint64_t expires,
              const pacujo::cordial::Thunk *timeout) :
            async_{ async }, timeout_ { timeout } {
            action_1 action { this, timed_out };
            timer_ = async_timer_start(async, expires, action);
        }
        Timer(async_t *async, const pacujo::cordial::Thunk *timeout) :
            async_ { async }, timeout_ { timeout } {
            action_1 action { this, timed_out };
            timer_ = async_execute(async, action);
        }
        ~Timer() {
            if (timer_)
                async_timer_cancel(async_, timer_);
        }
    private:
        async_t *async_;
        async_timer_t *timer_;
        const pacujo::cordial::Thunk *timeout_;
        static void timed_out(void *obj) {
            reinterpret_cast<Timer *>(obj)->timed_out();
        }
        void timed_out() {
            timer_ = nullptr;
            (*timeout_)();
        }
    };

public:
    Task reschedule() override {
        auto [_, resume] { co_await Introspect<Task::promise_type> {} };
        Timer t { get_async(), resume };
        co_await std::suspend_always {};
    }

    void execute(const pacujo::cordial::Thunk *function) override {
        async_execute(get_async(), thunk_to_action(function));
    }

    void dispose(Disposable *disposable) override {
        async_execute(get_async(), action_1 { disposable, delete_it });
    }

private:
    async_t *async_ { make_async() };

    static void execute(void *obj) {
        (*reinterpret_cast<pacujo::cordial::Thunk *>(obj))();
    }

    static void delete_it(void *obj) {
        delete reinterpret_cast<Disposable *>(obj);
    }

    Task delay_ns(uint64_t ns) override {
        auto [_, resume] { co_await Introspect<Task::promise_type> {} };
        Timer t { get_async(), async_now(get_async()) + ns * ASYNC_NS, resume };
        co_await std::suspend_always {};
    }
};

}; // namespace pacujo::coasync
