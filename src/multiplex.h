#pragma once

#include "cordial.h"

namespace pacujo::cordial {

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

}; // namespace pacujo::cordial
