#pragma once

#include <mutex>
#include <type_traits>
#include <utility>

namespace interprocess
{

template <typename Mutex, typename Func>
decltype(auto) with_lock(Mutex& mutex, Func&& func)
{
    std::lock_guard<Mutex> lock(mutex);
    return std::forward<Func>(func)();
}

template <typename T, typename Mutex>
class LockedRef
{
public:
    using value_type = T;
    using mutex_type = Mutex;

    LockedRef(T& value, Mutex& mutex) : value_ptr(&value), lock(mutex)
    {
    }

    LockedRef(const LockedRef&) = delete;
    LockedRef& operator=(const LockedRef&) = delete;
    LockedRef(LockedRef&&) = delete;
    LockedRef& operator=(LockedRef&&) = delete;

    T& get() noexcept
    {
        return *value_ptr;
    }

    const T& get() const noexcept
    {
        return *value_ptr;
    }

    T* operator->() noexcept
    {
        return value_ptr;
    }

    const T* operator->() const noexcept
    {
        return value_ptr;
    }

    T& operator*() noexcept
    {
        return *value_ptr;
    }

    const T& operator*() const noexcept
    {
        return *value_ptr;
    }

private:
    T* value_ptr;
    std::unique_lock<Mutex> lock;
};

template <typename T, typename Mutex>
class ConstLockedRef
{
public:
    using value_type = T;
    using mutex_type = Mutex;

    ConstLockedRef(const T& value, Mutex& mutex) : value_ptr(&value), lock(mutex)
    {
    }

    ConstLockedRef(const ConstLockedRef&) = delete;
    ConstLockedRef& operator=(const ConstLockedRef&) = delete;
    ConstLockedRef(ConstLockedRef&&) = delete;
    ConstLockedRef& operator=(ConstLockedRef&&) = delete;

    const T& get() const noexcept
    {
        return *value_ptr;
    }

    const T* operator->() const noexcept
    {
        return value_ptr;
    }

    const T& operator*() const noexcept
    {
        return *value_ptr;
    }

private:
    const T* value_ptr;
    std::unique_lock<Mutex> lock;
};

template <typename T, typename Mutex>
class Synchronized
{
public:
    using value_type = T;
    using mutex_type = Mutex;
    using locked_type = LockedRef<T, Mutex>;
    using const_locked_type = ConstLockedRef<T, Mutex>;

    template <typename... Args,
              typename = std::enable_if_t<std::is_constructible<T, Args&&...>::value>>
    explicit Synchronized(Args&&... args) : value(std::forward<Args>(args)...)
    {
    }

    Synchronized(const Synchronized&) = delete;
    Synchronized& operator=(const Synchronized&) = delete;
    Synchronized(Synchronized&&) = delete;
    Synchronized& operator=(Synchronized&&) = delete;

    locked_type lock()
    {
        return locked_type(value, mutex);
    }

    const_locked_type lock() const
    {
        return const_locked_type(value, mutex);
    }

    template <typename Func>
    decltype(auto) with_lock(Func&& func)
    {
        std::lock_guard<Mutex> guard(mutex);
        return std::forward<Func>(func)(value);
    }

    template <typename Func>
    decltype(auto) with_lock(Func&& func) const
    {
        std::lock_guard<Mutex> guard(mutex);
        return std::forward<Func>(func)(value);
    }

    Mutex& native_mutex() noexcept
    {
        return mutex;
    }

    const Mutex& native_mutex() const noexcept
    {
        return mutex;
    }

private:
    mutable Mutex mutex;
    T value;
};

} // namespace interprocess
