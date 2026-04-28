#pragma once

#include "../allocator/offset_ptr.h"
#include "../allocator/shared_memory_allocator.h"
#include <cstddef>
#include <ostream>
#include <string>

namespace interprocess
{

// A shared-memory-compatible string that uses OffsetPtr internally
template <typename CharT, typename Traits = std::char_traits<CharT>,
          typename Allocator = SharedMemoryAllocator<CharT>>
class alignas(16) BasicSharedMemoryString
{
public:
    using value_type = CharT;
    using traits_type = Traits;
    using allocator_type = Allocator;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = CharT&;
    using const_reference = const CharT&;
    using pointer = OffsetPtr<CharT>;
    using const_pointer = OffsetPtr<const CharT>;
    using iterator = pointer;
    using const_iterator = const_pointer;

    static const size_type npos = static_cast<size_type>(-1);

    explicit BasicSharedMemoryString(const Allocator& alloc)
        : allocator(alloc), start(nullptr), finish(nullptr), end_of_storage(nullptr)
    {
        init_empty();
    }

    BasicSharedMemoryString(const CharT* s, const Allocator& alloc)
        : allocator(alloc), start(nullptr), finish(nullptr), end_of_storage(nullptr)
    {
        size_type len = Traits::length(s);
        reserve(len);
        Traits::copy(start.get(), s, len);
        finish = start + len;
        *finish = CharT(0);
    }

    BasicSharedMemoryString(const std::basic_string<CharT, Traits>& s, const Allocator& alloc)
        : allocator(alloc), start(nullptr), finish(nullptr), end_of_storage(nullptr)
    {
        size_type len = s.length();
        reserve(len);
        Traits::copy(start.get(), s.data(), len);
        finish = start + len;
        *finish = CharT(0);
    }

    ~BasicSharedMemoryString()
    {
        if (start)
        {
            allocator.deallocate(start.get(), capacity() + 1);
        }
    }

    BasicSharedMemoryString(const BasicSharedMemoryString& other)
        : allocator(other.allocator), start(nullptr), finish(nullptr), end_of_storage(nullptr)
    {
        const size_type len = other.size();
        reserve(len);
        if (len != 0)
        {
            Traits::copy(start.get(), other.data(), len);
        }
        finish = start + len;
        *finish = CharT(0);
    }

    BasicSharedMemoryString(BasicSharedMemoryString&& other) noexcept
        : allocator(other.allocator), start(other.start), finish(other.finish),
          end_of_storage(other.end_of_storage)
    {
        other.start = nullptr;
        other.finish = nullptr;
        other.end_of_storage = nullptr;
    }

    BasicSharedMemoryString& operator=(const BasicSharedMemoryString& other)
    {
        if (this == &other)
            return *this;
        assign(other.data(), other.size());
        return *this;
    }

    BasicSharedMemoryString& operator=(BasicSharedMemoryString&& other)
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

    BasicSharedMemoryString& operator=(const CharT* s)
    {
        assign(s, Traits::length(s));
        return *this;
    }

    BasicSharedMemoryString& operator=(const std::basic_string<CharT, Traits>& s)
    {
        assign(s.data(), s.length());
        return *this;
    }

    size_type size() const noexcept
    {
        return start ? static_cast<size_type>(finish - start) : 0;
    }
    size_type length() const noexcept
    {
        return size();
    }
    size_type capacity() const noexcept
    {
        return start ? static_cast<size_type>(end_of_storage - start) : 0;
    }
    bool empty() const noexcept
    {
        return size() == 0;
    }

    const CharT* c_str() const noexcept
    {
        return start.get();
    }
    const CharT* data() const noexcept
    {
        return start.get();
    }

    reference operator[](size_type n)
    {
        return start[n];
    }
    const_reference operator[](size_type n) const
    {
        return start[n];
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

    void reserve(size_type n)
    {
        if (!start)
        {
            reallocate(n);
        }
        else if (n > capacity())
        {
            reallocate(n);
        }
    }

    void clear() noexcept
    {
        if (start)
        {
            finish = start;
            *finish = CharT(0);
        }
    }

    void assign(const CharT* s, size_type n)
    {
        if (!start)
        {
            init_empty();
        }
        if (n > capacity())
        {
            reallocate(n);
        }
        if (n != 0)
        {
            Traits::copy(start.get(), s, n);
        }
        finish = start + n;
        *finish = CharT(0);
    }

    BasicSharedMemoryString& append(const CharT* s, size_type n)
    {
        size_type old_size = size();
        if (old_size + n > capacity())
        {
            reserve(old_size + n);
        }
        Traits::copy(finish.get(), s, n);
        finish += n;
        *finish = CharT(0);
        return *this;
    }

    BasicSharedMemoryString& append(const CharT* s)
    {
        return append(s, Traits::length(s));
    }

    BasicSharedMemoryString& operator+=(const CharT* s)
    {
        return append(s);
    }

    BasicSharedMemoryString& operator+=(const std::basic_string<CharT, Traits>& s)
    {
        return append(s.data(), s.length());
    }

    bool operator==(const CharT* s) const
    {
        const size_type len = Traits::length(s);
        return size() == len && Traits::compare(c_str(), s, len) == 0;
    }

    bool operator==(const std::basic_string<CharT, Traits>& s) const
    {
        return size() == s.size() && Traits::compare(c_str(), s.data(), s.size()) == 0;
    }

    bool operator<(const BasicSharedMemoryString& other) const
    {
        const size_type lhs_size = size();
        const size_type rhs_size = other.size();
        const size_type compare_size = lhs_size < rhs_size ? lhs_size : rhs_size;
        const int compare_result = Traits::compare(data(), other.data(), compare_size);
        if (compare_result < 0)
        {
            return true;
        }
        if (compare_result > 0)
        {
            return false;
        }
        return lhs_size < rhs_size;
    }

    friend std::ostream& operator<<(std::ostream& os, const BasicSharedMemoryString& s)
    {
        if (s.start)
        {
            os << s.c_str();
        }
        return os;
    }

private:
    void init_empty()
    {
        reallocate(0);
        *start = CharT(0);
        finish = start;
    }

    void release_storage()
    {
        if (start)
        {
            allocator.deallocate(start.get(), capacity() + 1);
            start = nullptr;
            finish = nullptr;
            end_of_storage = nullptr;
        }
    }

    void reallocate(size_type new_capacity)
    {
        size_type old_size = size();
        // We need new_capacity + 1 for the null terminator
        CharT* new_data = allocator.allocate(new_capacity + 1);

        if (start)
        {
            Traits::copy(new_data, start.get(), old_size);
            allocator.deallocate(start.get(), capacity() + 1);
        }

        start.set_pointer(new_data);
        finish.set_pointer(new_data + old_size);
        end_of_storage.set_pointer(new_data + new_capacity);
        *finish = CharT(0);
    }

    Allocator allocator;
    pointer start;
    pointer finish;
    pointer end_of_storage;
};

using SharedMemoryString = BasicSharedMemoryString<char>;

} // namespace interprocess
