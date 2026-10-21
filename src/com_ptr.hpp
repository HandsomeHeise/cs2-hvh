#pragma once
#include <utility>

template<class T> class ComPtr {
    T* pointer_ = nullptr;
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) { reset(); pointer_ = std::exchange(other.pointer_, nullptr); }
        return *this;
    }
    T* get() const { return pointer_; }
    T* operator->() const { return pointer_; }
    explicit operator bool() const { return pointer_ != nullptr; }
    T** put() { reset(); return &pointer_; }
    void reset() { if (auto* value = std::exchange(pointer_, nullptr)) value->Release(); }
};
