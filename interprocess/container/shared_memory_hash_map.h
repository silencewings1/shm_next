#pragma once

#include "../allocator/shared_memory_allocator.h"
#include "detail/shared_memory_hash_table.h"
#include <functional>
#include <utility>

namespace interprocess
{

template <typename Key, typename T, typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Allocator = SharedMemoryAllocator<std::pair<const Key, T>>>
class SharedMemoryHashMap
    : public detail::SharedMemoryHashTable<Key, T, Hash, KeyEqual, Allocator>
{
    using base_type = detail::SharedMemoryHashTable<Key, T, Hash, KeyEqual, Allocator>;

public:
    using base_type::base_type;

    SharedMemoryHashMap(const SharedMemoryHashMap&) = default;
    SharedMemoryHashMap(SharedMemoryHashMap&&) noexcept = default;

    SharedMemoryHashMap& operator=(const SharedMemoryHashMap& other)
    {
        base_type::operator=(other);
        return *this;
    }

    SharedMemoryHashMap& operator=(SharedMemoryHashMap&& other) noexcept
    {
        base_type::operator=(std::move(other));
        return *this;
    }

    void swap(SharedMemoryHashMap& other)
    {
        base_type::swap(other);
    }

    ~SharedMemoryHashMap() = default;
};

template <typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator>
void swap(SharedMemoryHashMap<Key, T, Hash, KeyEqual, Allocator>& lhs,
          SharedMemoryHashMap<Key, T, Hash, KeyEqual, Allocator>& rhs)
{
    lhs.swap(rhs);
}

} // namespace interprocess
