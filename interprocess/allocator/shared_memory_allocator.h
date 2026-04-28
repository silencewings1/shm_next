#pragma once

#include "offset_ptr.h"
#include "shared_memory_manager.h"
#include <cstddef>
#include <new>
#include <utility>

namespace interprocess
{

// A standard C++ allocator that uses SharedMemoryManager
template <typename T>
class SharedMemoryAllocator
{
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template <typename U>
    struct rebind
    {
        using other = SharedMemoryAllocator<U>;
    };

    // The allocator needs to know which manager to use.
    // Using OffsetPtr ensures it's valid even when the allocator is stored in SHM.
    explicit SharedMemoryAllocator(SharedMemoryManager* manager) noexcept : manager(manager)
    {
    }

    template <typename U>
    SharedMemoryAllocator(const SharedMemoryAllocator<U>& other) noexcept
        : manager(other.get_manager())
    {
    }

    pointer allocate(size_type n, const void* hint = nullptr)
    {
        if (n > max_size())
        {
            throw std::bad_alloc();
        }

        (void)hint;

        void* ptr = manager->allocate(n * sizeof(T), alignof(T));
        if (!ptr)
        {
            throw std::bad_alloc();
        }
        return static_cast<pointer>(ptr);
    }

    void allocate_many(size_type n, size_type count, pointer* out)
    {
        if (n > max_size())
        {
            throw std::bad_alloc();
        }
        manager->allocate_many(n * sizeof(T), count, alignof(T), reinterpret_cast<void**>(out));
    }

    void deallocate(pointer p, size_type n)
    {
        (void)n;
        if (p)
        {
            manager->deallocate(p);
        }
    }

    void deallocate_many(pointer const* ptrs, size_type count)
    {
        manager->deallocate_many(reinterpret_cast<void* const*>(ptrs), count);
    }

    bool try_expand(pointer p, size_type old_count, size_type new_count)
    {
        (void)old_count;
        if (new_count > max_size())
        {
            return false;
        }
        return manager->try_expand(p, new_count * sizeof(T), alignof(T));
    }

    size_type max_size() const noexcept
    {
        return std::size_t(-1) / sizeof(T);
    }

    template <typename U, typename... Args>
    void construct(U* p, Args&&... args)
    {
        ::new ((void*)p) U(std::forward<Args>(args)...);
    }

    template <typename U>
    void destroy(U* p)
    {
        p->~U();
    }

    SharedMemoryManager* get_manager() const noexcept
    {
        return manager.get();
    }

private:
    OffsetPtr<SharedMemoryManager> manager;
};

// Allocators are equal if they allocate from the same manager
template <typename T, typename U>
inline bool operator==(const SharedMemoryAllocator<T>& a, const SharedMemoryAllocator<U>& b)
{
    return a.get_manager() == b.get_manager();
}

template <typename T, typename U>
inline bool operator!=(const SharedMemoryAllocator<T>& a, const SharedMemoryAllocator<U>& b)
{
    return !(a == b);
}

} // namespace interprocess
