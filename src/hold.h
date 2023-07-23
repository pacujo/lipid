#pragma once

#include <functional>
#include <memory>

namespace pacujo::etc {

/* A template class to hold and release a pointer context. Like
 * std::unique_ptr but only needs a single type argument. */
template<typename T>
class Hold {
public:
    Hold() {}
    Hold(T *ptr, std::function<void(T *)> release) : ptr_ { ptr, release } {}
    Hold(Hold &&other) = default;
    Hold &operator=(Hold &&other) = default;
    T *get() const { return ptr_.get(); }
    operator bool() const { return bool(ptr_); }
private:
    std::unique_ptr<T, std::function<void(T *)>> ptr_;
};

/* Like Hold but for a non-pointer handle. */
template<typename T>
class Keep {
public:
    Keep(T handle, std::function<void(T)> release) :
        handle_ { handle }, release_ { release } {}
    Keep(Keep &&other) = default;
    Keep &operator=(Keep &&other) = default;
    ~Keep() { release_(handle_); }
    T get() const { return handle_; }
private:
    T handle_;
    std::function<void(T)> release_;
};

};
