#pragma once

#include "../allocator/offset_ptr.h"
#include "../allocator/shared_memory_allocator.h"
#include <cstddef>
#include <iterator>
#include <stdexcept>
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
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

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
            size_type constructed = 0;
            try
            {
                for (; constructed < n; ++constructed)
                {
                    allocator.construct(start.get() + constructed, other[constructed]);
                }
            }
            catch (...)
            {
                destroy_range(start.get(), constructed);
                allocator.deallocate(start.get(), capacity());
                start = nullptr;
                finish = nullptr;
                end_of_storage = nullptr;
                throw;
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
    size_type max_size() const noexcept
    {
        return allocator.max_size();
    }

    reference operator[](size_type n)
    {
        return start[n];
    }
    const_reference operator[](size_type n) const
    {
        return start[n];
    }

    reference at(size_type n)
    {
        if (n >= size())
        {
            throw std::out_of_range("SharedMemoryVector::at index out of range");
        }
        return (*this)[n];
    }
    const_reference at(size_type n) const
    {
        if (n >= size())
        {
            throw std::out_of_range("SharedMemoryVector::at index out of range");
        }
        return (*this)[n];
    }

    reference front()
    {
        return *start;
    }
    const_reference front() const
    {
        return *start;
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
    const_iterator cbegin() const noexcept
    {
        return begin();
    }
    iterator end() noexcept
    {
        return finish;
    }
    const_iterator end() const noexcept
    {
        return finish;
    }
    const_iterator cend() const noexcept
    {
        return end();
    }
    reverse_iterator rbegin() noexcept
    {
        return reverse_iterator(end());
    }
    const_reverse_iterator rbegin() const noexcept
    {
        return const_reverse_iterator(end());
    }
    const_reverse_iterator crbegin() const noexcept
    {
        return const_reverse_iterator(cend());
    }
    reverse_iterator rend() noexcept
    {
        return reverse_iterator(begin());
    }
    const_reverse_iterator rend() const noexcept
    {
        return const_reverse_iterator(begin());
    }
    const_reverse_iterator crend() const noexcept
    {
        return const_reverse_iterator(cbegin());
    }

    T* data() noexcept
    {
        return start.get();
    }
    const T* data() const noexcept
    {
        return start.get();
    }

    void reserve(size_type new_capacity)
    {
        if (new_capacity > capacity())
        {
            reallocate(new_capacity);
        }
    }

    void resize(size_type count)
    {
        resize_default(count);
    }

    void resize(size_type count, const T& value)
    {
        resize_fill(count, value);
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

    iterator erase(iterator pos)
    {
        if (pos == end())
            return pos;

        iterator result = pos;
        for (iterator next = pos + 1; next != end(); ++pos, ++next)
        {
            *pos = std::move(*next);
        }

        finish -= 1;
        allocator.destroy(finish.get());
        return result;
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
        const size_type n = other.size();
        if (n == 0)
        {
            release_storage();
            return;
        }

        T* new_data = allocator.allocate(n);
        size_type constructed = 0;
        try
        {
            for (; constructed < n; ++constructed)
            {
                allocator.construct(new_data + constructed, other[constructed]);
            }
        }
        catch (...)
        {
            destroy_range(new_data, constructed);
            allocator.deallocate(new_data, n);
            throw;
        }

        release_storage();
        start.set_pointer(new_data);
        finish.set_pointer(new_data + n);
        end_of_storage.set_pointer(new_data + n);
    }

    void reallocate(size_type new_capacity)
    {
        size_type old_size = size();
        size_type old_capacity = capacity();
        T* old_data = start.get();

        if (old_data && new_capacity > old_capacity &&
            allocator.try_expand(old_data, old_capacity, new_capacity))
        {
            finish.set_pointer(old_data + old_size);
            end_of_storage.set_pointer(old_data + new_capacity);
            return;
        }

        T* new_data = allocator.allocate(new_capacity);

        size_type constructed = 0;
        try
        {
            for (; constructed < old_size; ++constructed)
            {
                allocator.construct(new_data + constructed,
                                    std::move_if_noexcept(old_data[constructed]));
            }
        }
        catch (...)
        {
            destroy_range(new_data, constructed);
            allocator.deallocate(new_data, new_capacity);
            throw;
        }

        destroy_range(old_data, old_size);
        if (old_data)
        {
            allocator.deallocate(old_data, old_capacity);
        }

        start.set_pointer(new_data);
        finish.set_pointer(new_data + old_size);
        end_of_storage.set_pointer(new_data + new_capacity);
    }

    void resize_default(size_type count)
    {
        const size_type old_size = size();
        if (count < old_size)
        {
            destroy_tail(count);
            return;
        }
        if (count == old_size)
        {
            return;
        }

        if (count > capacity())
        {
            reallocate(count);
        }
        construct_default_tail(old_size, count);
    }

    void resize_fill(size_type count, const T& value)
    {
        const size_type old_size = size();
        if (count < old_size)
        {
            destroy_tail(count);
            return;
        }
        if (count == old_size)
        {
            return;
        }

        if (count > capacity())
        {
            reallocate(count);
        }
        construct_fill_tail(old_size, count, value);
    }

    void construct_default_tail(size_type from, size_type to)
    {
        size_type constructed = from;
        try
        {
            for (; constructed < to; ++constructed)
            {
                allocator.construct(start.get() + constructed);
            }
        }
        catch (...)
        {
            destroy_range(start.get() + from, constructed - from);
            finish.set_pointer(start.get() + from);
            throw;
        }
        finish.set_pointer(start.get() + to);
    }

    void construct_fill_tail(size_type from, size_type to, const T& value)
    {
        size_type constructed = from;
        try
        {
            for (; constructed < to; ++constructed)
            {
                allocator.construct(start.get() + constructed, value);
            }
        }
        catch (...)
        {
            destroy_range(start.get() + from, constructed - from);
            finish.set_pointer(start.get() + from);
            throw;
        }
        finish.set_pointer(start.get() + to);
    }

    void destroy_tail(size_type new_size) noexcept
    {
        while (size() > new_size)
        {
            pop_back();
        }
    }

    void reallocate_exact(size_type new_capacity)
    {
        if (start)
        {
            release_storage();
        }
        T* new_data = allocator.allocate(new_capacity);
        start.set_pointer(new_data);
        finish.set_pointer(new_data);
        end_of_storage.set_pointer(new_data + new_capacity);
    }

    void destroy_range(T* data, size_type count)
    {
        for (size_type i = 0; i < count; ++i)
        {
            allocator.destroy(data + i);
        }
    }

    Allocator allocator;
    pointer start;
    pointer finish;
    pointer end_of_storage;
};

} // namespace interprocess
