#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace interprocess
{

namespace detail
{

constexpr int64_t offset_ptr_null = 1;

template <typename To, typename From>
using enable_offset_ptr_conversion_t =
    std::enable_if_t<std::is_convertible<From*, To*>::value, int>;

inline bool try_make_offset(const void* self, const void* ptr, int64_t& result) noexcept
{
    const auto self_addr = reinterpret_cast<uintptr_t>(self);
    const auto ptr_addr = reinterpret_cast<uintptr_t>(ptr);
    constexpr auto max_offset = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

    if (ptr_addr >= self_addr)
    {
        const uint64_t diff = ptr_addr - self_addr;
        if (diff > max_offset || diff == static_cast<uint64_t>(offset_ptr_null))
        {
            return false;
        }
        result = static_cast<int64_t>(diff);
        return true;
    }

    const uint64_t diff = self_addr - ptr_addr;
    if (diff > max_offset)
    {
        return false;
    }
    result = -static_cast<int64_t>(diff);
    return true;
}

inline int64_t make_offset(const void* self, const void* ptr)
{
    int64_t result = offset_ptr_null;
    if (!try_make_offset(self, ptr, result))
    {
        throw std::out_of_range(
            "OffsetPtr target address is outside representable range or conflicts with null");
    }
    return result;
}

} // namespace detail

// A self-relative pointer that can be used in shared memory.
// It stores the offset between its own address and the target address.
template <typename T>
class OffsetPtr
{
public:
    using pointer = T*;
    using reference = T&;
    using element_type = T;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::random_access_iterator_tag;

    template <typename U>
    using rebind = OffsetPtr<U>;

    OffsetPtr() noexcept : offset(detail::offset_ptr_null)
    {
    } // 1 is used as internal representation for nullptr

    OffsetPtr(std::nullptr_t) noexcept : offset(detail::offset_ptr_null)
    {
    }

    OffsetPtr(T* ptr)
    {
        set_pointer(ptr);
    }

    OffsetPtr(const OffsetPtr& other)
    {
        set_pointer(other.get());
    }

    template <typename U, detail::enable_offset_ptr_conversion_t<T, U> = 0>
    OffsetPtr(const OffsetPtr<U>& other)
    {
        set_pointer(other.get());
    }

    OffsetPtr& operator=(std::nullptr_t) noexcept
    {
        offset = detail::offset_ptr_null;
        return *this;
    }

    OffsetPtr& operator=(const OffsetPtr& other)
    {
        set_pointer(other.get());
        return *this;
    }

    OffsetPtr& operator=(T* ptr)
    {
        set_pointer(ptr);
        return *this;
    }

    T* get() const noexcept
    {
        if (offset == detail::offset_ptr_null)
            return nullptr;
        return reinterpret_cast<T*>(reinterpret_cast<char*>(const_cast<OffsetPtr*>(this)) + offset);
    }

    void set_pointer(T* ptr)
    {
        if (!ptr)
        {
            offset = detail::offset_ptr_null;
        }
        else
        {
            offset = detail::make_offset(this, ptr);
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

    OffsetPtr& operator++()
    {
        set_pointer(get() + 1);
        return *this;
    }

    OffsetPtr operator++(int)
    {
        OffsetPtr tmp(*this);
        ++(*this);
        return tmp;
    }

    OffsetPtr& operator--()
    {
        set_pointer(get() - 1);
        return *this;
    }

    OffsetPtr operator--(int)
    {
        OffsetPtr tmp(*this);
        --(*this);
        return tmp;
    }

    OffsetPtr& operator+=(difference_type n)
    {
        set_pointer(get() + n);
        return *this;
    }

    OffsetPtr& operator-=(difference_type n)
    {
        set_pointer(get() - n);
        return *this;
    }

    friend OffsetPtr operator+(OffsetPtr lhs, difference_type rhs)
    {
        lhs += rhs;
        return lhs;
    }

    friend OffsetPtr operator+(difference_type lhs, OffsetPtr rhs)
    {
        rhs += lhs;
        return rhs;
    }

    friend OffsetPtr operator-(OffsetPtr lhs, difference_type rhs)
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
    using element_type = void;
    using value_type = void;
    using difference_type = std::ptrdiff_t;

    template <typename U>
    using rebind = OffsetPtr<U>;

    OffsetPtr() noexcept : offset(detail::offset_ptr_null)
    {
    }
    OffsetPtr(std::nullptr_t) noexcept : offset(detail::offset_ptr_null)
    {
    }
    OffsetPtr(void* ptr)
    {
        set_pointer(ptr);
    }

    OffsetPtr(const OffsetPtr& other)
    {
        set_pointer(other.get());
    }

    template <typename U>
    OffsetPtr(U* ptr)
    {
        set_pointer(ptr);
    }

    template <typename U>
    OffsetPtr(const OffsetPtr<U>& other)
    {
        set_pointer(other.get());
    }

    OffsetPtr& operator=(std::nullptr_t) noexcept
    {
        offset = detail::offset_ptr_null;
        return *this;
    }

    OffsetPtr& operator=(void* ptr)
    {
        set_pointer(ptr);
        return *this;
    }

    OffsetPtr& operator=(const OffsetPtr& other)
    {
        set_pointer(other.get());
        return *this;
    }

    void* get() const noexcept
    {
        if (offset == detail::offset_ptr_null)
            return nullptr;
        return reinterpret_cast<char*>(const_cast<OffsetPtr*>(this)) + offset;
    }

    void set_pointer(void* ptr)
    {
        if (!ptr)
        {
            offset = detail::offset_ptr_null;
        }
        else
        {
            offset = detail::make_offset(this, ptr);
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
    using element_type = const void;
    using value_type = const void;
    using difference_type = std::ptrdiff_t;

    template <typename U>
    using rebind = OffsetPtr<U>;

    OffsetPtr() noexcept : offset(detail::offset_ptr_null)
    {
    }
    OffsetPtr(std::nullptr_t) noexcept : offset(detail::offset_ptr_null)
    {
    }
    OffsetPtr(const void* ptr)
    {
        set_pointer(ptr);
    }

    OffsetPtr(const OffsetPtr& other)
    {
        set_pointer(other.get());
    }

    template <typename U>
    OffsetPtr(const U* ptr)
    {
        set_pointer(ptr);
    }

    template <typename U, detail::enable_offset_ptr_conversion_t<const void, U> = 0>
    OffsetPtr(const OffsetPtr<U>& other)
    {
        set_pointer(other.get());
    }

    OffsetPtr& operator=(std::nullptr_t) noexcept
    {
        offset = detail::offset_ptr_null;
        return *this;
    }

    OffsetPtr& operator=(const void* ptr)
    {
        set_pointer(ptr);
        return *this;
    }

    OffsetPtr& operator=(const OffsetPtr& other)
    {
        set_pointer(other.get());
        return *this;
    }

    const void* get() const noexcept
    {
        if (offset == detail::offset_ptr_null)
            return nullptr;
        return reinterpret_cast<const char*>(this) + offset;
    }

    void set_pointer(const void* ptr)
    {
        if (!ptr)
        {
            offset = detail::offset_ptr_null;
        }
        else
        {
            offset = detail::make_offset(this, ptr);
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
