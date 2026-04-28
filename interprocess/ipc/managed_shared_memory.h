#pragma once

#include "../allocator/shared_memory_manager.h"
#include "../allocator/shared_memory_allocator.h"
#include "posix_mapped_region.h"
#include "posix_shared_memory_object.h"
#include <chrono>
#include <cstring>
#include <cstdint>
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
        : shm(create_only, name, mode_t::read_write, permissions), region(), manager(nullptr)
    {
        shm.truncate(size);
        region = MappedRegion(shm, mode_t::read_write);
        initialize_new_segment();
    }

    // Opens an existing shared memory segment and attaches to the manager
    ManagedSharedMemory(open_only_t, const char* name)
        : shm(open_only, name, mode_t::read_write), region(), manager(nullptr)
    {
        attach_existing_with_retry();
    }

    // Opens or creates a shared memory segment
    ManagedSharedMemory(open_or_create_t, const char* name, std::size_t size,
                        ::mode_t permissions = 0666)
        : shm(open_or_create, name, mode_t::read_write, permissions), region(), manager(nullptr)
    {
        if (shm.was_created())
        {
            shm.truncate(size);
            region = MappedRegion(shm, mode_t::read_write);
            initialize_new_segment();
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

    template <typename T>
    bool destroy(const char* name)
    {
        return manager->destroy<T>(name);
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
    void initialize_new_segment()
    {
        std::memset(region.get_address(), 0, region.get_size());
        try
        {
            manager = SharedMemoryManager::create(region.get_address(), region.get_size());
        }
        catch (...)
        {
            if (region.get_size() >= SharedMemoryManager::minimum_initialization_size())
            {
                SharedMemoryManager::mark_corrupted(region.get_address());
            }
            throw;
        }
    }

    void attach_existing_with_retry()
    {
        constexpr int max_attempts = 100;
        std::string last_error;

        for (int attempt = 0; attempt < max_attempts; ++attempt)
        {
            std::size_t current_size = shm.get_size();
            if (current_size >= SharedMemoryManager::minimum_initialization_size())
            {
                try
                {
                    if (region.get_address() == nullptr)
                    {
                        region = MappedRegion(shm, mode_t::read_write);
                    }

                    SharedMemoryManager::InitializationState state =
                        SharedMemoryManager::get_initialization_state(region.get_address());
                    if (!SharedMemoryManager::is_known_initialization_state(state))
                    {
                        throw std::runtime_error(
                            std::string("unknown shared memory initialization state: ") +
                            std::to_string(static_cast<uint32_t>(state)));
                    }

                    if (state == SharedMemoryManager::InitializationState::initialized)
                    {
                        manager = SharedMemoryManager::attach(region.get_address());
                        return;
                    }

                    if (state == SharedMemoryManager::InitializationState::corrupted)
                    {
                        throw std::runtime_error("shared memory segment is marked corrupted");
                    }

                    last_error = std::string("shared memory manager is ") +
                                 SharedMemoryManager::initialization_state_name(state);
                }
                catch (const std::runtime_error& e)
                {
                    last_error = e.what();
                    if (last_error.find("corrupted") != std::string::npos ||
                        last_error.find("unknown shared memory initialization state") !=
                            std::string::npos ||
                        last_error.find("magic mismatch") != std::string::npos)
                    {
                        throw;
                    }
                }
            }
            else
            {
                last_error = "shared memory object is still empty";
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
