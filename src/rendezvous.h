#pragma once

#include <array>
#include <coroutine>
#include <memory>
#include <optional>
#include <queue>

namespace pacujo::cordial {

/**
 * A RendezVous object is a synchronous messaging channel between
 * any number of sending and receiving coroutines. Sending is
 * blocked until receiving takes place and vice versa.
 */
template<typename T>
class RendezVous {
    template<typename Result>
    using Future = Framework::Future<Result>;

    using Task = Framework::Task;

    template<typename Promise>
    using Introspect = Framework::Introspect<Promise>;

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

}; // namespace pacujo::cordial
