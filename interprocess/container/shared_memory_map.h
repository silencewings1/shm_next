#pragma once

#include "../allocator/shared_memory_allocator.h"
#include "detail/shared_memory_rbtree_map.h"
#include <functional>
#include <utility>

namespace interprocess
{

template <typename Key, typename T, typename Compare = std::less<Key>,
          typename Allocator = SharedMemoryAllocator<std::pair<const Key, T>>>
class SharedMemoryMap : public detail::SharedMemoryRbTreeMap<Key, T, Compare, Allocator>
{
    using base_type = detail::SharedMemoryRbTreeMap<Key, T, Compare, Allocator>;

public:
    using base_type::base_type;

    SharedMemoryMap(const SharedMemoryMap&) = default;
    SharedMemoryMap(SharedMemoryMap&&) noexcept = default;

    SharedMemoryMap& operator=(const SharedMemoryMap& other)
    {
        base_type::operator=(other);
        return *this;
    }

    SharedMemoryMap& operator=(SharedMemoryMap&& other) noexcept
    {
        base_type::operator=(std::move(other));
        return *this;
    }

    void swap(SharedMemoryMap& other)
    {
        base_type::swap(other);
    }

    ~SharedMemoryMap() = default;
};

template <typename Key, typename T, typename Compare, typename Allocator>
void swap(SharedMemoryMap<Key, T, Compare, Allocator>& lhs,
          SharedMemoryMap<Key, T, Compare, Allocator>& rhs)
{
    lhs.swap(rhs);
}

} // namespace interprocess
