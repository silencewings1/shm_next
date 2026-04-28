#pragma once

#include "../sync/posix_mutex.h"
#include "detail/named_object_registry.h"
#include "detail/shared_memory_block_allocator.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace interprocess
{

class alignas(16) SharedMemoryManager
{
public:
    static constexpr uint32_t MAGIC = 0x534D4D32; // "SMM2"

    static SharedMemoryManager* create(void* base_addr, std::size_t total_size)
    {
        if (total_size < sizeof(SharedMemoryManager) + sizeof(detail::BlockHeader))
        {
            throw std::runtime_error("Shared memory size is too small to initialize manager.");
        }

        return new (base_addr) SharedMemoryManager(total_size);
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
        return allocate(size, 16);
    }

    void* allocate(std::size_t size, std::size_t alignment)
    {
        std::lock_guard<InterprocessMutex> lock(mutex);
        return block_allocator.allocate(size, alignment);
    }

    void deallocate(void* ptr)
    {
        std::lock_guard<InterprocessMutex> lock(mutex);
        block_allocator.deallocate(ptr);
    }

    template <typename T, typename... Args>
    T* construct(const char* name, Args&&... args)
    {
        void* object_storage = nullptr;
        void* header_storage = nullptr;
        detail::NamedObjectHeader* header = nullptr;

        {
            std::lock_guard<InterprocessMutex> lock(mutex);

            if (named_objects.find_any(name) != nullptr)
            {
                return nullptr;
            }

            try
            {
                object_storage = block_allocator.allocate(sizeof(T), alignof(T));
                header_storage = block_allocator.allocate(sizeof(detail::NamedObjectHeader),
                                                          alignof(detail::NamedObjectHeader));
            }
            catch (...)
            {
                block_allocator.deallocate(header_storage);
                block_allocator.deallocate(object_storage);
                throw;
            }

            header = new (header_storage) detail::NamedObjectHeader();
            std::strncpy(header->name, name, sizeof(header->name) - 1);
            header->name[sizeof(header->name) - 1] = '\0';
            header->ptr = nullptr;
            header->state = detail::NamedObjectState::constructing;
            header->reserved = 0;
            named_objects.insert_front(header);
        }

        T* obj = nullptr;
        try
        {
            obj = new (object_storage) T(std::forward<Args>(args)...);
        }
        catch (...)
        {
            std::lock_guard<InterprocessMutex> lock(mutex);
            named_objects.unlink(header);
            header->~NamedObjectHeader();
            block_allocator.deallocate(header);
            block_allocator.deallocate(object_storage);
            throw;
        }

        {
            std::lock_guard<InterprocessMutex> lock(mutex);
            header->ptr = obj;
            header->state = detail::NamedObjectState::ready;
        }

        return obj;
    }

    template <typename T>
    T* find(const char* name)
    {
        std::lock_guard<InterprocessMutex> lock(mutex);
        detail::NamedObjectHeader* curr = named_objects.find_ready(name);
        return curr ? static_cast<T*>(curr->ptr.get()) : nullptr;
    }

    template <typename T>
    bool destroy(const char* name)
    {
        detail::NamedObjectHeader* header = nullptr;
        void* object_storage = nullptr;

        {
            std::lock_guard<InterprocessMutex> lock(mutex);
            header = named_objects.find_ready(name);
            if (header == nullptr)
            {
                return false;
            }

            header->state = detail::NamedObjectState::destroying;
            object_storage = header->ptr.get();
        }

        static_cast<T*>(object_storage)->~T();

        {
            std::lock_guard<InterprocessMutex> lock(mutex);
            named_objects.unlink(header);
            header->~NamedObjectHeader();
            block_allocator.deallocate(header);
            block_allocator.deallocate(object_storage);
        }

        return true;
    }

    std::size_t get_free_memory() const
    {
        std::lock_guard<InterprocessMutex> lock(mutex);
        return block_allocator.get_free_memory();
    }

private:
    uint32_t magic;
    mutable InterprocessMutex mutex;
    std::size_t total_size;
    detail::SharedMemoryBlockAllocator block_allocator;
    detail::NamedObjectRegistry named_objects;

    explicit SharedMemoryManager(std::size_t total_size)
        : magic(0), total_size(total_size), block_allocator(), named_objects()
    {
        named_objects.initialize();
        block_allocator.initialize(this, sizeof(SharedMemoryManager), total_size);
        magic = MAGIC;
    }
};

} // namespace interprocess
