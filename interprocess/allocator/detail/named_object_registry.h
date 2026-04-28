#pragma once

#include "../offset_ptr.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace interprocess::detail
{

enum class NamedObjectState : uint32_t
{
    constructing = 1,
    ready = 2,
    destroying = 3
};

struct alignas(16) NamedObjectHeader
{
    OffsetPtr<void> ptr;
    OffsetPtr<char> name;
    OffsetPtr<NamedObjectHeader> next;
    std::size_t name_length;
    std::size_t instance_count;
    std::size_t object_size;
    uint64_t name_hash;
    uint64_t owner_pid;
    NamedObjectState state;
    uint32_t reserved;
};

class NamedObjectRegistry
{
public:
    static constexpr std::size_t bucket_count = 64;

    NamedObjectRegistry() noexcept : buckets(), ready_count(0), total_count(0)
    {
    }

    void initialize() noexcept
    {
        for (std::size_t i = 0; i < bucket_count; ++i)
        {
            buckets[i] = nullptr;
        }
        ready_count = 0;
        total_count = 0;
    }

    static uint64_t hash_name(const char* name, std::size_t length) noexcept
    {
        uint64_t hash = 1469598103934665603ull;
        for (std::size_t i = 0; i < length; ++i)
        {
            hash ^= static_cast<unsigned char>(name[i]);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    NamedObjectHeader* find_any(const char* name, std::size_t length) const noexcept
    {
        uint64_t hash = hash_name(name, length);
        NamedObjectHeader* curr = buckets[bucket_index(hash)].get();
        while (curr)
        {
            if (curr->name_hash == hash && curr->name_length == length &&
                std::memcmp(curr->name.get(), name, length) == 0)
            {
                return curr;
            }
            curr = curr->next.get();
        }
        return nullptr;
    }

    NamedObjectHeader* find_ready(const char* name, std::size_t length) const noexcept
    {
        NamedObjectHeader* header = find_any(name, length);
        if (header == nullptr || header->state != NamedObjectState::ready)
        {
            return nullptr;
        }
        return header;
    }

    NamedObjectHeader* find_ready_by_ptr(const void* ptr) const noexcept
    {
        for (std::size_t i = 0; i < bucket_count; ++i)
        {
            NamedObjectHeader* curr = buckets[i].get();
            while (curr)
            {
                if (curr->state == NamedObjectState::ready && curr->ptr.get() == ptr)
                {
                    return curr;
                }
                curr = curr->next.get();
            }
        }
        return nullptr;
    }

    void insert(NamedObjectHeader* header) noexcept
    {
        std::size_t index = bucket_index(header->name_hash);
        header->next = buckets[index];
        buckets[index] = header;
        ++total_count;
        if (header->state == NamedObjectState::ready)
        {
            ++ready_count;
        }
    }

    void mark_ready(NamedObjectHeader* header) noexcept
    {
        if (header->state != NamedObjectState::ready)
        {
            header->state = NamedObjectState::ready;
            ++ready_count;
        }
    }

    void mark_not_ready(NamedObjectHeader* header, NamedObjectState state) noexcept
    {
        if (header->state == NamedObjectState::ready && ready_count > 0)
        {
            --ready_count;
        }
        header->state = state;
    }

    void unlink(NamedObjectHeader* target) noexcept
    {
        if (!target)
        {
            return;
        }

        std::size_t index = bucket_index(target->name_hash);
        NamedObjectHeader* prev = nullptr;
        NamedObjectHeader* curr = buckets[index].get();
        while (curr)
        {
            if (curr == target)
            {
                if (prev)
                {
                    prev->next = curr->next;
                }
                else
                {
                    buckets[index] = curr->next;
                }
                if (curr->state == NamedObjectState::ready && ready_count > 0)
                {
                    --ready_count;
                }
                if (total_count > 0)
                {
                    --total_count;
                }
                curr->next = nullptr;
                return;
            }
            prev = curr;
            curr = curr->next.get();
        }
    }

    std::size_t ready_size() const noexcept
    {
        return ready_count;
    }

    std::size_t total_size() const noexcept
    {
        return total_count;
    }

    template <typename Func>
    void for_each(Func&& func) const
    {
        for (std::size_t i = 0; i < bucket_count; ++i)
        {
            NamedObjectHeader* curr = buckets[i].get();
            while (curr)
            {
                func(*curr);
                curr = curr->next.get();
            }
        }
    }

    template <typename Predicate>
    NamedObjectHeader* find_if(Predicate&& predicate) const
    {
        for (std::size_t i = 0; i < bucket_count; ++i)
        {
            NamedObjectHeader* curr = buckets[i].get();
            while (curr)
            {
                if (predicate(*curr))
                {
                    return curr;
                }
                curr = curr->next.get();
            }
        }
        return nullptr;
    }

private:
    static std::size_t bucket_index(uint64_t hash) noexcept
    {
        return static_cast<std::size_t>(hash % bucket_count);
    }

    OffsetPtr<NamedObjectHeader> buckets[bucket_count];
    std::size_t ready_count;
    std::size_t total_count;
};

} // namespace interprocess::detail
