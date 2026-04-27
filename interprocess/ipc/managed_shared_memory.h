#pragma once

#include "../allocator/shared_memory_manager.h"
#include "../allocator/shared_memory_allocator.h"
#include "posix_mapped_region.h"
#include "posix_shared_memory_object.h"
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

namespace interprocess
{

class ManagedSharedMemory
{
public:
    // Creates a new shared memory segment and initializes the manager
    ManagedSharedMemory(create_only_t, const char* name, std::size_t size,
                        ::mode_t permissions = 0666)
        : shm(create_only, name, mode_t::read_write, permissions)
    {
        shm.truncate(size);
        region = MappedRegion(shm, mode_t::read_write);
        // Zero the memory to avoid garbage
        std::memset(region.get_address(), 0, region.get_size());
        manager = SharedMemoryManager::create(region.get_address(), region.get_size());
    }

    // Opens an existing shared memory segment and attaches to the manager
    ManagedSharedMemory(open_only_t, const char* name) : shm(open_only, name, mode_t::read_write)
    {
        region = MappedRegion(shm, mode_t::read_write);
        manager = SharedMemoryManager::attach(region.get_address());
    }

    // Opens or creates a shared memory segment
    ManagedSharedMemory(open_or_create_t, const char* name, std::size_t size,
                        ::mode_t permissions = 0666)
        : shm(open_or_create, name, mode_t::read_write, permissions)
    {
        if (shm.was_created())
        {
            shm.truncate(size);
            region = MappedRegion(shm, mode_t::read_write);
            std::memset(region.get_address(), 0, region.get_size());
            manager = SharedMemoryManager::create(region.get_address(), region.get_size());
            return;
        }

        attach_existing_with_retry();
    }

    ~ManagedSharedMemory() = default;

    // Get the allocator for a specific type
    template <typename T>
    SharedMemoryAllocator<T> get_allocator()
    {
        return SharedMemoryAllocator<T>(manager);
    }

    // Construct a named object
    template <typename T, typename... Args>
    T* construct(const char* name, Args&&... args)
    {
        return manager->construct<T>(name, std::forward<Args>(args)...);
    }

    // Find a named object
    template <typename T>
    T* find(const char* name)
    {
        return manager->find<T>(name);
    }

    // Direct access to the segment manager
    SharedMemoryManager* get_segment_manager() const
    {
        return manager;
    }

    std::size_t get_size() const
    {
        return region.get_size();
    }

    std::size_t get_free_memory() const
    {
        return manager->get_free_memory();
    }

    static bool remove(const char* name)
    {
        return SharedMemoryObject::remove(name);
    }

private:
    void attach_existing_with_retry()
    {
        constexpr int max_attempts = 100;
        std::string last_error;

        for (int attempt = 0; attempt < max_attempts; ++attempt)
        {
            if (shm.get_size() != 0)
            {
                try
                {
                    region = MappedRegion(shm, mode_t::read_write);
                    manager = SharedMemoryManager::attach(region.get_address());
                    return;
                }
                catch (const std::runtime_error& e)
                {
                    last_error = e.what();
                    region = MappedRegion();
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (last_error.empty())
        {
            last_error = "shared memory object is still empty";
        }
        throw std::runtime_error("Failed to attach shared memory manager: " + last_error);
    }

    SharedMemoryObject shm;
    MappedRegion region;
    SharedMemoryManager* manager;
};

} // namespace interprocess
