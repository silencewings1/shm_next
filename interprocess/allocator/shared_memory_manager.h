#pragma once

#include "../sync/posix_mutex.h"
#include "offset_ptr.h"
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <cassert>
#include <cstring>
#include <mutex>
#include <utility>

namespace interprocess
{

// Represents the header of each memory block (both free and allocated)
struct alignas(16) BlockHeader
{
    std::size_t size; // Size of the block including this header
    bool is_free;
    char padding[7]; // Explicit padding for alignment
    OffsetPtr<BlockHeader> next_free;
    char padding2[8]; // Ensure total size is multiple of 16
};

static_assert(sizeof(BlockHeader) % 16 == 0, "BlockHeader size must be a multiple of 16");

// Represents a named object entry
struct alignas(16) NamedObjectHeader
{
    char name[64]; // Fixed size for simplicity
    OffsetPtr<void> ptr;
    OffsetPtr<NamedObjectHeader> next;
};

// The main manager placed at the beginning of the shared memory segment
class alignas(16) SharedMemoryManager
{
public:
    static constexpr uint32_t MAGIC = 0x534D4D31; // "SMM1"

    // Initializes the manager on a raw memory buffer.
    static SharedMemoryManager* create(void* base_addr, std::size_t total_size)
    {
        if (total_size < sizeof(SharedMemoryManager) + sizeof(BlockHeader))
        {
            throw std::runtime_error("Shared memory size is too small to initialize manager.");
        }

        // Construct the manager at the base address
        SharedMemoryManager* manager = new (base_addr) SharedMemoryManager(total_size);
        return manager;
    }

    static SharedMemoryManager* attach(void* base_addr)
    {
        SharedMemoryManager* manager = static_cast<SharedMemoryManager*>(base_addr);
        if (manager->magic != MAGIC)
        {
            throw std::runtime_error(
                "Shared memory manager magic mismatch! Segment might not be initialized.");
        }
        return manager;
    }

    void* allocate(std::size_t size)
    {
        if (size == 0)
            return nullptr;

        const std::size_t alignment = 16;
        std::size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
        std::size_t header_size = sizeof(BlockHeader);
        std::size_t total_needed = aligned_size + header_size;

        std::lock_guard<InterprocessMutex> lock(mutex);

        BlockHeader* prev = nullptr;
        BlockHeader* curr = free_list_head.get();

        while (curr != nullptr)
        {
            if (curr->is_free && curr->size >= total_needed)
            {
                std::size_t remaining_size = curr->size - total_needed;

                if (remaining_size >= header_size + alignment)
                {
                    BlockHeader* new_block = reinterpret_cast<BlockHeader*>(
                        reinterpret_cast<char*>(curr) + total_needed);
                    new_block->size = remaining_size;
                    new_block->is_free = true;
                    new_block->next_free = curr->next_free;

                    curr->size = total_needed;

                    if (prev)
                    {
                        prev->next_free = new_block;
                    }
                    else
                    {
                        free_list_head = new_block;
                    }
                }
                else
                {
                    if (prev)
                    {
                        prev->next_free = curr->next_free;
                    }
                    else
                    {
                        free_list_head = curr->next_free;
                    }
                }

                curr->is_free = false;
                curr->next_free = nullptr;

                return reinterpret_cast<char*>(curr) + header_size;
            }

            prev = curr;
            curr = curr->next_free.get();
        }

        throw std::bad_alloc();
    }

    void deallocate(void* ptr)
    {
        if (!ptr)
            return;

        std::lock_guard<InterprocessMutex> lock(mutex);
        deallocate_unlocked(ptr);
    }

    void deallocate_unlocked(void* ptr)
    {
        if (!ptr)
            return;

        BlockHeader* block =
            reinterpret_cast<BlockHeader*>(static_cast<char*>(ptr) - sizeof(BlockHeader));

        assert(!block->is_free && "Double free detected");

        block->is_free = true;

        BlockHeader* prev = nullptr;
        BlockHeader* curr = free_list_head.get();

        while (curr != nullptr && curr < block)
        {
            prev = curr;
            curr = curr->next_free.get();
        }

        block->next_free = curr;

        if (prev)
        {
            prev->next_free = block;
        }
        else
        {
            free_list_head = block;
        }

        if (curr != nullptr)
        {
            char* block_end = reinterpret_cast<char*>(block) + block->size;
            if (block_end == reinterpret_cast<char*>(curr))
            {
                block->size += curr->size;
                block->next_free = curr->next_free;
                curr = nullptr;
            }
        }

        if (prev != nullptr)
        {
            char* prev_end = reinterpret_cast<char*>(prev) + prev->size;
            if (prev_end == reinterpret_cast<char*>(block))
            {
                prev->size += block->size;
                prev->next_free = block->next_free;
            }
        }
    }

    // Named object support
    template <typename T, typename... Args>
    T* construct(const char* name, Args&&... args)
    {
        void* object_storage = nullptr;
        NamedObjectHeader* header = nullptr;

        {
            std::lock_guard<InterprocessMutex> lock(mutex);

            if (find_named_object_unlocked(name) != nullptr)
            {
                return nullptr;
            }

            object_storage = allocate_unlocked(sizeof(T));
            void* header_storage = allocate_unlocked(sizeof(NamedObjectHeader));

            header = new (header_storage) NamedObjectHeader();
            std::strncpy(header->name, name, 63);
            header->name[63] = '\0';
            header->ptr = nullptr;
            header->next = named_objects_head;
            named_objects_head = header;
        }

        T* obj = nullptr;
        try
        {
            obj = new (object_storage) T(std::forward<Args>(args)...);
        }
        catch (...)
        {
            std::lock_guard<InterprocessMutex> lock(mutex);
            unlink_named_object_unlocked(header);
            header->~NamedObjectHeader();
            deallocate_unlocked(header);
            deallocate_unlocked(object_storage);
            throw;
        }

        {
            std::lock_guard<InterprocessMutex> lock(mutex);
            header->ptr = obj;
        }

        return obj;
    }

    template <typename T>
    T* find(const char* name)
    {
        std::lock_guard<InterprocessMutex> lock(mutex);
        NamedObjectHeader* curr = find_named_object_unlocked(name);
        return curr ? static_cast<T*>(curr->ptr.get()) : nullptr;
    }

    std::size_t get_free_memory() const
    {
        std::lock_guard<InterprocessMutex> lock(const_cast<InterprocessMutex&>(mutex));
        std::size_t free_mem = 0;
        BlockHeader* curr = free_list_head.get();
        while (curr != nullptr)
        {
            free_mem += (curr->size - sizeof(BlockHeader));
            curr = curr->next_free.get();
        }
        return free_mem;
    }

private:
    uint32_t magic;
    mutable InterprocessMutex mutex;
    std::size_t total_size;
    OffsetPtr<BlockHeader> free_list_head;
    OffsetPtr<NamedObjectHeader> named_objects_head;

    SharedMemoryManager(std::size_t total_size)
        : magic(MAGIC), total_size(total_size), named_objects_head(nullptr)
    {
        std::size_t header_size = (sizeof(SharedMemoryManager) + 15) & ~15;
        BlockHeader* first_block =
            reinterpret_cast<BlockHeader*>(reinterpret_cast<char*>(this) + header_size);

        first_block->size = total_size - header_size;
        first_block->is_free = true;
        first_block->next_free = nullptr;

        free_list_head = first_block;
    }

    void* allocate_unlocked(std::size_t size)
    {
        if (size == 0)
            return nullptr;

        const std::size_t alignment = 16;
        std::size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
        std::size_t header_size = sizeof(BlockHeader);
        std::size_t total_needed = aligned_size + header_size;

        BlockHeader* prev = nullptr;
        BlockHeader* curr = free_list_head.get();

        while (curr != nullptr)
        {
            if (curr->is_free && curr->size >= total_needed)
            {
                std::size_t remaining_size = curr->size - total_needed;

                if (remaining_size >= header_size + alignment)
                {
                    BlockHeader* new_block = reinterpret_cast<BlockHeader*>(
                        reinterpret_cast<char*>(curr) + total_needed);
                    new_block->size = remaining_size;
                    new_block->is_free = true;
                    new_block->next_free = curr->next_free;

                    curr->size = total_needed;

                    if (prev)
                    {
                        prev->next_free = new_block;
                    }
                    else
                    {
                        free_list_head = new_block;
                    }
                }
                else
                {
                    if (prev)
                    {
                        prev->next_free = curr->next_free;
                    }
                    else
                    {
                        free_list_head = curr->next_free;
                    }
                }

                curr->is_free = false;
                curr->next_free = nullptr;

                return reinterpret_cast<char*>(curr) + header_size;
            }

            prev = curr;
            curr = curr->next_free.get();
        }

        throw std::bad_alloc();
    }

    NamedObjectHeader* find_named_object_unlocked(const char* name) const
    {
        NamedObjectHeader* curr = named_objects_head.get();
        while (curr)
        {
            if (std::strncmp(curr->name, name, 64) == 0)
            {
                return curr;
            }
            curr = curr->next.get();
        }
        return nullptr;
    }

    void unlink_named_object_unlocked(NamedObjectHeader* target)
    {
        if (!target)
            return;

        NamedObjectHeader* prev = nullptr;
        NamedObjectHeader* curr = named_objects_head.get();
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
                    named_objects_head = curr->next;
                }
                return;
            }
            prev = curr;
            curr = curr->next.get();
        }
    }
};

} // namespace interprocess
