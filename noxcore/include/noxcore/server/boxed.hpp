// An optional kept on the heap: std::optional's
// interface as ServerObject's big rare parts use it (a player's, a monster's, an ankh's...), so
// an object without them stays small. Copies copy what it holds, as an optional's do; what it
// holds stays where it is when its object moves. `Watch` (if any) is told each time one comes to
// hold something or stops holding it (Watch::changed()), a new one's first value aside.
#pragma once

#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace nox::server {

template <class T, class Watch = void>
class Boxed {
public:
    Boxed() = default;
    Boxed(std::nullopt_t) {}
    Boxed(const T& v) : p_(std::make_unique<T>(v)) {}
    Boxed(T&& v) : p_(std::make_unique<T>(std::move(v))) {}
    Boxed(const Boxed& o) : p_(o.p_ ? std::make_unique<T>(*o.p_) : nullptr) {}
    Boxed(Boxed&& o) noexcept : p_(std::move(o.p_))
    {
        if (p_) {
            changed(); // (the one moved from holds nothing now)
        }
    }
    Boxed& operator=(const Boxed& o)
    {
        if (this != &o) {
            const bool had = p_ != nullptr;
            p_ = o.p_ ? std::make_unique<T>(*o.p_) : nullptr;
            if (had != (p_ != nullptr)) {
                changed();
            }
        }
        return *this;
    }
    Boxed& operator=(Boxed&& o) noexcept
    {
        if (this != &o) {
            const bool had = p_ != nullptr;
            const bool took = o.p_ != nullptr;
            p_ = std::move(o.p_);
            if (took || had) {
                changed();
            }
        }
        return *this;
    }
    Boxed& operator=(std::nullopt_t)
    {
        reset();
        return *this;
    }
    Boxed& operator=(const T& v)
    {
        if (p_) {
            *p_ = v;
        } else {
            p_ = std::make_unique<T>(v);
            changed();
        }
        return *this;
    }
    Boxed& operator=(T&& v)
    {
        if (p_) {
            *p_ = std::move(v);
        } else {
            p_ = std::make_unique<T>(std::move(v));
            changed();
        }
        return *this;
    }

    template <class... Args>
    T& emplace(Args&&... args)
    {
        const bool had = p_ != nullptr;
        p_.reset(); // (the old one gone first, as an optional's emplace)
        p_ = std::make_unique<T>(std::forward<Args>(args)...);
        if (!had) {
            changed();
        }
        return *p_;
    }
    void reset() noexcept
    {
        if (p_) {
            p_.reset();
            changed();
        }
    }

    bool has_value() const noexcept { return p_ != nullptr; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    T* operator->() noexcept { return p_.get(); }
    const T* operator->() const noexcept { return p_.get(); }
    T& operator*() noexcept { return *p_; }
    const T& operator*() const noexcept { return *p_; }
    T& value()
    {
        if (!p_) {
            throw std::bad_optional_access();
        }
        return *p_;
    }
    const T& value() const
    {
        if (!p_) {
            throw std::bad_optional_access();
        }
        return *p_;
    }
    template <class U>
    T value_or(U&& fallback) const
    {
        return p_ ? *p_ : static_cast<T>(std::forward<U>(fallback));
    }

    friend bool operator==(const Boxed& a, std::nullopt_t) noexcept { return !a.p_; }

private:
    static void changed() noexcept
    {
        if constexpr (!std::is_void_v<Watch>) {
            Watch::changed();
        }
    }

    std::unique_ptr<T> p_;
};

} // namespace nox::server
