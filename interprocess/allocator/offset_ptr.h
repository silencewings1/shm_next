#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <iostream>

namespace interprocess
{

// A self-relative pointer that can be used in shared memory.
// It stores the offset between its own address and the target address.
template <typename T>
class OffsetPtr
{
public:
    using pointer = T*;
    using reference = T&;
    using difference_type = std::ptrdiff_t;
    using value_type = T;
    using iterator_category = std::random_access_iterator_tag;

    OffsetPtr() noexcept : offset(1)
    {
    } // 1 is used as internal representation for nullptr

    OffsetPtr(std::nullptr_t) noexcept : offset(1)
    {
    }

    OffsetPtr(T* ptr) noexcept
    {
        set_pointer(ptr);
    }

    OffsetPtr(const OffsetPtr& other) noexcept
    {
        set_pointer(other.get());
    }

    template <typename U>
    OffsetPtr(const OffsetPtr<U>& other) noexcept
    {
        set_pointer(other.get());
    }

    OffsetPtr& operator=(const OffsetPtr& other) noexcept
    {
        set_pointer(other.get());
        return *this;
    }

    OffsetPtr& operator=(T* ptr) noexcept
    {
        set_pointer(ptr);
        return *this;
    }

    T* get() const noexcept
    {
        if (offset == 1)
            return nullptr;
        return reinterpret_cast<T*>(reinterpret_cast<char*>(const_cast<OffsetPtr*>(this)) + offset);
    }

    void set_pointer(T* ptr) noexcept
    {
        if (!ptr)
        {
            offset = 1;
        }
        else
        {
            offset = reinterpret_cast<char*>(ptr) - reinterpret_cast<char*>(this);
        }
    }

    T& operator*() const noexcept
    {
        return *get();
    }
    T* operator->() const noexcept
    {
        return get();
    }
    T& operator[](difference_type n) const noexcept
    {
        return get()[n];
    }

    OffsetPtr& operator++() noexcept
    {
        set_pointer(get() + 1);
        return *this;
    }

    OffsetPtr operator++(int) noexcept
    {
        OffsetPtr tmp(*this);
        ++(*this);
        return tmp;
    }

    OffsetPtr& operator--() noexcept
    {
        set_pointer(get() - 1);
        return *this;
    }

    OffsetPtr operator--(int) noexcept
    {
        OffsetPtr tmp(*this);
        --(*this);
        return tmp;
    }

    OffsetPtr& operator+=(difference_type n) noexcept
    {
        set_pointer(get() + n);
        return *this;
    }

    OffsetPtr& operator-=(difference_type n) noexcept
    {
        set_pointer(get() - n);
        return *this;
    }

    friend OffsetPtr operator+(OffsetPtr lhs, difference_type rhs) noexcept
    {
        lhs += rhs;
        return lhs;
    }

    friend OffsetPtr operator+(difference_type lhs, OffsetPtr rhs) noexcept
    {
        rhs += lhs;
        return rhs;
    }

    friend OffsetPtr operator-(OffsetPtr lhs, difference_type rhs) noexcept
    {
        lhs -= rhs;
        return lhs;
    }

    friend difference_type operator-(const OffsetPtr& lhs, const OffsetPtr& rhs) noexcept
    {
        return lhs.get() - rhs.get();
    }

    bool operator==(const OffsetPtr& other) const noexcept
    {
        return get() == other.get();
    }
    bool operator!=(const OffsetPtr& other) const noexcept
    {
        return get() != other.get();
    }
    bool operator<(const OffsetPtr& other) const noexcept
    {
        return get() < other.get();
    }
    bool operator>(const OffsetPtr& other) const noexcept
    {
        return get() > other.get();
    }
    bool operator<=(const OffsetPtr& other) const noexcept
    {
        return get() <= other.get();
    }
    bool operator>=(const OffsetPtr& other) const noexcept
    {
        return get() >= other.get();
    }

    explicit operator bool() const noexcept
    {
        return get() != nullptr;
    }

private:
    int64_t offset;
};

// Specialization for void
template <>
class OffsetPtr<void>
{
public:
    using pointer = void*;
    using difference_type = std::ptrdiff_t;
    using value_type = void;

    OffsetPtr() noexcept : offset(1)
    {
    }
    OffsetPtr(std::nullptr_t) noexcept : offset(1)
    {
    }
    OffsetPtr(void* ptr) noexcept
    {
        set_pointer(ptr);
    }

    template <typename U>
    OffsetPtr(U* ptr) noexcept
    {
        set_pointer(ptr);
    }

    template <typename U>
    OffsetPtr(const OffsetPtr<U>& other) noexcept
    {
        set_pointer(other.get());
    }

    OffsetPtr& operator=(void* ptr) noexcept
    {
        set_pointer(ptr);
        return *this;
    }

    OffsetPtr& operator=(const OffsetPtr& other) noexcept
    {
        set_pointer(other.get());
        return *this;
    }

    void* get() const noexcept
    {
        if (offset == 1)
            return nullptr;
        return reinterpret_cast<char*>(const_cast<OffsetPtr*>(this)) + offset;
    }

    void set_pointer(void* ptr) noexcept
    {
        if (!ptr)
        {
            offset = 1;
        }
        else
        {
            offset = reinterpret_cast<char*>(ptr) - reinterpret_cast<char*>(this);
        }
    }

    explicit operator bool() const noexcept
    {
        return get() != nullptr;
    }
    bool operator==(const OffsetPtr& other) const noexcept
    {
        return get() == other.get();
    }
    bool operator!=(const OffsetPtr& other) const noexcept
    {
        return get() != other.get();
    }

private:
    int64_t offset;
};

static_assert(sizeof(OffsetPtr<void>) == 8, "OffsetPtr size must be 8 bytes");
static_assert(sizeof(OffsetPtr<int>) == 8, "OffsetPtr size must be 8 bytes");

// Specialization for const void
template <>
class OffsetPtr<const void>
{
public:
    using pointer = const void*;
    using difference_type = std::ptrdiff_t;
    using value_type = const void;

    OffsetPtr() noexcept : offset(1)
    {
    }
    OffsetPtr(std::nullptr_t) noexcept : offset(1)
    {
    }
    OffsetPtr(const void* ptr) noexcept
    {
        set_pointer(ptr);
    }

    template <typename U>
    OffsetPtr(const U* ptr) noexcept
    {
        set_pointer(ptr);
    }

    template <typename U>
    OffsetPtr(const OffsetPtr<U>& other) noexcept
    {
        set_pointer(other.get());
    }

    OffsetPtr& operator=(const void* ptr) noexcept
    {
        set_pointer(ptr);
        return *this;
    }

    OffsetPtr& operator=(const OffsetPtr& other) noexcept
    {
        set_pointer(other.get());
        return *this;
    }

    const void* get() const noexcept
    {
        if (offset == 1)
            return nullptr;
        return reinterpret_cast<const char*>(this) + offset;
    }

    void set_pointer(const void* ptr) noexcept
    {
        if (!ptr)
        {
            offset = 1;
        }
        else
        {
            offset = reinterpret_cast<const char*>(ptr) - reinterpret_cast<const char*>(this);
        }
    }

    explicit operator bool() const noexcept
    {
        return get() != nullptr;
    }
    bool operator==(const OffsetPtr& other) const noexcept
    {
        return get() == other.get();
    }
    bool operator!=(const OffsetPtr& other) const noexcept
    {
        return get() != other.get();
    }

private:
    int64_t offset;
};

} // namespace interprocess
