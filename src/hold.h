#pragma once

#include <functional>
#include <memory>

/* A template class to hold and release a pointer context. Like
 * std::unique_ptr but only needs a single type argument. */
template<typename T>
class Hold {
public:
    Hold(T *ptr, std::function<void(T *)> release) : ptr_(ptr, release) {}
    Hold(Hold &&other) : ptr_(other.ptr_) {}
    T *get() const { return ptr_.get(); }
    operator bool() const { return bool(ptr_); }
private:
    std::unique_ptr<T, std::function<void(T *)>> ptr_;
};
