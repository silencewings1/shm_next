#pragma once

#include "../offset_ptr.h"
#include <cstdint>
#include <cstring>

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
    char name[64];
    OffsetPtr<void> ptr;
    OffsetPtr<NamedObjectHeader> next;
    NamedObjectState state;
    uint32_t reserved;
};

class NamedObjectRegistry
{
public:
    NamedObjectRegistry() noexcept : head(nullptr)
    {
    }

    void initialize() noexcept
    {
        head = nullptr;
    }

    NamedObjectHeader* find_any(const char* name) const noexcept
    {
        NamedObjectHeader* curr = head.get();
        while (curr)
        {
            if (std::strncmp(curr->name, name, sizeof(curr->name)) == 0)
            {
                return curr;
            }
            curr = curr->next.get();
        }
        return nullptr;
    }

    NamedObjectHeader* find_ready(const char* name) const noexcept
    {
        NamedObjectHeader* header = find_any(name);
        if (header == nullptr || header->state != NamedObjectState::ready)
        {
            return nullptr;
        }
        return header;
    }

    void insert_front(NamedObjectHeader* header) noexcept
    {
        header->next = head;
        head = header;
    }

    void unlink(NamedObjectHeader* target) noexcept
    {
        if (!target)
        {
            return;
        }

        NamedObjectHeader* prev = nullptr;
        NamedObjectHeader* curr = head.get();
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
                    head = curr->next;
                }
                curr->next = nullptr;
                return;
            }
            prev = curr;
            curr = curr->next.get();
        }
    }

private:
    OffsetPtr<NamedObjectHeader> head;
};

} // namespace interprocess::detail
