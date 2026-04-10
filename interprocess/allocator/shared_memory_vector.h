#pragma once

#include "offset_ptr.h"
#include "shared_memory_allocator.h"
#include <cstddef>
#include <utility>

namespace interprocess
{

// A shared-memory-compatible vector that uses OffsetPtr internally
template <typename T, typename Allocator = SharedMemoryAllocator<T>>
class alignas(16) SharedMemoryVector
{
public:
    using value_type = T;
    using allocator_type = Allocator;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = OffsetPtr<T>;
    using const_pointer = OffsetPtr<const T>;
    using iterator = pointer;
    using const_iterator = const_pointer;

    explicit SharedMemoryVector(const Allocator& alloc) noexcept
        : allocator(alloc), start(nullptr), finish(nullptr), end_of_storage(nullptr)
    {
    }

    ~SharedMemoryVector()
    {
        clear();
        if (start)
        {
            allocator.deallocate(start.get(), capacity());
        }
    }

    SharedMemoryVector(const SharedMemoryVector& other)
        : allocator(other.allocator), start(nullptr), finish(nullptr), end_of_storage(nullptr)
    {
        const size_type n = other.size();
        if (n != 0)
        {
            reallocate_exact(n);
            for (size_type i = 0; i < n; ++i)
            {
                allocator.construct(start.get() + i, other[i]);
            }
            finish = start + n;
        }
    }

    SharedMemoryVector(SharedMemoryVector&& other) noexcept
        : allocator(other.allocator), start(other.start), finish(other.finish),
          end_of_storage(other.end_of_storage)
    {
        other.start = nullptr;
        other.finish = nullptr;
        other.end_of_storage = nullptr;
    }

    SharedMemoryVector& operator=(const SharedMemoryVector& other)
    {
        if (this == &other)
            return *this;
        assign_from(other);
        return *this;
    }

    SharedMemoryVector& operator=(SharedMemoryVector&& other) noexcept
    {
        if (this == &other)
            return *this;
        release_storage();
        allocator = other.allocator;
        start = other.start;
        finish = other.finish;
        end_of_storage = other.end_of_storage;
        other.start = nullptr;
        other.finish = nullptr;
        other.end_of_storage = nullptr;
        return *this;
    }

    size_type size() const noexcept
    {
        return start ? static_cast<size_type>(finish - start) : 0;
    }
    size_type capacity() const noexcept
    {
        return start ? static_cast<size_type>(end_of_storage - start) : 0;
    }
    bool empty() const noexcept
    {
        return size() == 0;
    }

    reference operator[](size_type n)
    {
        return start[n];
    }
    const_reference operator[](size_type n) const
    {
        return start[n];
    }

    reference back()
    {
        return *(finish - 1);
    }
    const_reference back() const
    {
        return *(finish - 1);
    }

    iterator begin() noexcept
    {
        return start;
    }
    const_iterator begin() const noexcept
    {
        return start;
    }
    iterator end() noexcept
    {
        return finish;
    }
    const_iterator end() const noexcept
    {
        return finish;
    }

    void reserve(size_type new_capacity)
    {
        if (new_capacity > capacity())
        {
            reallocate(new_capacity);
        }
    }

    void push_back(const T& value)
    {
        if (finish == end_of_storage)
        {
            reallocate(capacity() == 0 ? 1 : capacity() * 2);
        }
        allocator.construct(finish.get(), value);
        finish += 1;
    }

    void push_back(T&& value)
    {
        if (finish == end_of_storage)
        {
            reallocate(capacity() == 0 ? 1 : capacity() * 2);
        }
        allocator.construct(finish.get(), std::move(value));
        finish += 1;
    }

    template <typename... Args>
    reference emplace_back(Args&&... args)
    {
        if (finish == end_of_storage)
        {
            reallocate(capacity() == 0 ? 1 : capacity() * 2);
        }
        allocator.construct(finish.get(), std::forward<Args>(args)...);
        finish += 1;
        return back();
    }

    void pop_back()
    {
        if (!empty())
        {
            finish -= 1;
            allocator.destroy(finish.get());
        }
    }

    void clear()
    {
        while (!empty())
        {
            pop_back();
        }
    }

    void erase(iterator pos)
    {
        if (pos == end())
            return;

        // Destroy the element at pos
        allocator.destroy(pos.get());

        // Move subsequent elements forward
        for (iterator next = pos + 1; next != end(); ++pos, ++next)
        {
            allocator.construct(pos.get(), std::move(*next));
            allocator.destroy(next.get());
        }

        finish -= 1;
    }

private:
    void release_storage() noexcept
    {
        if (start)
        {
            clear();
            allocator.deallocate(start.get(), capacity());
            start = nullptr;
            finish = nullptr;
            end_of_storage = nullptr;
        }
    }

    void assign_from(const SharedMemoryVector& other)
    {
        release_storage();
        const size_type n = other.size();
        if (n == 0)
            return;
        reallocate_exact(n);
        for (size_type i = 0; i < n; ++i)
        {
            allocator.construct(start.get() + i, other[i]);
        }
        finish = start + n;
    }

    void reallocate(size_type new_capacity)
    {
        size_type old_size = size();
        T* new_data = allocator.allocate(new_capacity);

        for (size_type i = 0; i < old_size; ++i)
        {
            allocator.construct(new_data + i, std::move(start[i]));
            allocator.destroy(start.get() + i);
        }

        if (start)
        {
            allocator.deallocate(start.get(), capacity());
        }

        start.set_pointer(new_data);
        finish.set_pointer(new_data + old_size);
        end_of_storage.set_pointer(new_data + new_capacity);
    }

    void reallocate_exact(size_type new_capacity)
    {
        if (start)
        {
            allocator.deallocate(start.get(), capacity());
            start = nullptr;
            finish = nullptr;
            end_of_storage = nullptr;
        }
        T* new_data = allocator.allocate(new_capacity);
        start.set_pointer(new_data);
        finish.set_pointer(new_data);
        end_of_storage.set_pointer(new_data + new_capacity);
    }

    Allocator allocator;
    pointer start;
    pointer finish;
    pointer end_of_storage;
};

} // namespace interprocess
