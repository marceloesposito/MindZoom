// SOSTITUTO MINIMO di winrt/base.h, SOLO per il controllo di compilazione
// incrociata su un Mac. NON fa parte del progetto e non va copiato nel repo:
// la build vera su Windows usa il winrt/base.h del Windows SDK.
//
// Serve a poter compilare renderer_win32.cpp con mingw-w64, che il C++/WinRT
// non ce l'ha. Riproduce solo la parte di winrt::com_ptr che il renderer usa,
// con le stesse firme, cosi' gli errori che escono sono errori veri del
// codice e non della finzione.
#pragma once

#include <unknwn.h>
#include <utility>

namespace winrt {

struct take_ownership_from_abi_t {};
inline constexpr take_ownership_from_abi_t take_ownership_from_abi{};

template <typename T>
struct com_ptr {
    using type = T;

    com_ptr() noexcept = default;
    com_ptr(std::nullptr_t) noexcept {}

    com_ptr(T* p, take_ownership_from_abi_t) noexcept : ptr_(p) {}

    com_ptr(const com_ptr& other) noexcept : ptr_(other.ptr_) { add_ref(); }
    com_ptr(com_ptr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}

    ~com_ptr() noexcept { release(); }

    com_ptr& operator=(const com_ptr& other) noexcept {
        if (this != &other) { release(); ptr_ = other.ptr_; add_ref(); }
        return *this;
    }
    com_ptr& operator=(com_ptr&& other) noexcept {
        if (this != &other) { release(); ptr_ = std::exchange(other.ptr_, nullptr); }
        return *this;
    }
    com_ptr& operator=(std::nullptr_t) noexcept { release(); return *this; }

    T*  get() const noexcept { return ptr_; }
    T** put() noexcept { return &ptr_; }
    void** put_void() noexcept { return reinterpret_cast<void**>(&ptr_); }
    T*  operator->() const noexcept { return ptr_; }
    T&  operator*() const noexcept { return *ptr_; }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    T* detach() noexcept { return std::exchange(ptr_, nullptr); }

    template <typename To>
    com_ptr<To> try_as() const noexcept {
        com_ptr<To> out;
        if (ptr_) ptr_->QueryInterface(__uuidof(To), out.put_void());
        return out;
    }

    template <typename To>
    com_ptr<To> as() const { return try_as<To>(); }

private:
    void add_ref() const noexcept { if (ptr_) ptr_->AddRef(); }
    void release() noexcept { if (ptr_) { ptr_->Release(); ptr_ = nullptr; } }

    T* ptr_ = nullptr;
};

} // namespace winrt
