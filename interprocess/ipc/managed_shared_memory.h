#pragma once

#include "../allocator/shared_memory_manager.h"
#include "../allocator/shared_memory_allocator.h"
#include "posix_mapped_region.h"
#include "posix_shared_memory_object.h"
#include <errno.h>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
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
        : shm(create_only, name, mode_t::read_write, permissions), region(), manager(nullptr),
          read_only_mode(false)
    {
        shm.truncate(size);
        region = MappedRegion(shm, mode_t::read_write);
        initialize_new_segment();
    }

    // Opens an existing shared memory segment and attaches to the manager
    ManagedSharedMemory(open_only_t, const char* name)
        : shm(open_only, name, mode_t::read_write), region(), manager(nullptr),
          read_only_mode(false)
    {
        attach_existing_with_retry(mode_t::read_write, true);
    }

    // Opens an existing shared memory segment with a read-only mapping.
    ManagedSharedMemory(open_read_only_t, const char* name)
        : shm(open_only, name, mode_t::read_only), region(), manager(nullptr), read_only_mode(true)
    {
        attach_existing_with_retry(mode_t::read_only, false);
    }

    // Opens or creates a shared memory segment
    ManagedSharedMemory(open_or_create_t, const char* name, std::size_t size,
                        ::mode_t permissions = 0666)
        : shm(open_or_create, name, mode_t::read_write, permissions), region(), manager(nullptr),
          read_only_mode(false)
    {
        if (shm.was_created())
        {
            shm.truncate(size);
            region = MappedRegion(shm, mode_t::read_write);
            initialize_new_segment();
            return;
        }

        attach_existing_with_retry(mode_t::read_write, true);
    }

    ~ManagedSharedMemory() = default;

    // Get the allocator for a specific type
    template <typename T>
    SharedMemoryAllocator<T> get_allocator()
    {
        ensure_writable("get_allocator");
        return SharedMemoryAllocator<T>(manager);
    }

    // Construct a named object
    template <typename T, typename... Args>
    T* construct(const char* name, Args&&... args)
    {
        ensure_writable("construct");
        return manager->construct<T>(name, std::forward<Args>(args)...);
    }

    template <typename T, typename... Args>
    T* find_or_construct(const char* name, Args&&... args)
    {
        ensure_writable("find_or_construct");
        return manager->find_or_construct<T>(name, std::forward<Args>(args)...);
    }

    template <typename T, typename... Args>
    T* construct_array(const char* name, std::size_t count, Args&&... args)
    {
        ensure_writable("construct_array");
        return manager->construct_array<T>(name, count, std::forward<Args>(args)...);
    }

    template <typename T, typename InputIt>
    T* construct_array_from_range(const char* name, InputIt first, InputIt last)
    {
        ensure_writable("construct_array_from_range");
        return manager->construct_array_from_range<T>(name, first, last);
    }

    // Find a named object
    template <typename T>
    T* find(const char* name)
    {
        ensure_writable("find");
        return manager->find<T>(name);
    }

    template <typename T>
    T* find_array(const char* name, std::size_t* count = nullptr)
    {
        ensure_writable("find_array");
        return manager->find_array<T>(name, count);
    }

    template <typename T>
    const T* find_read_only(const char* name) const
    {
        return manager->find_read_only<T>(name);
    }

    template <typename T>
    const T* find_array_read_only(const char* name, std::size_t* count = nullptr) const
    {
        return manager->find_array_read_only<T>(name, count);
    }

    template <typename T>
    bool destroy(const char* name)
    {
        ensure_writable("destroy");
        return manager->destroy<T>(name);
    }

    template <typename T>
    bool destroy_array(const char* name)
    {
        ensure_writable("destroy_array");
        return manager->destroy_array<T>(name);
    }

    template <typename T>
    bool destroy_ptr(T* ptr)
    {
        ensure_writable("destroy_ptr");
        return manager->destroy_ptr(ptr);
    }

    std::size_t get_num_named_objects() const
    {
        if (read_only_mode)
        {
            return manager->get_num_named_objects_read_only();
        }
        return manager->get_num_named_objects();
    }

    std::size_t get_num_total_named_objects() const
    {
        if (read_only_mode)
        {
            return manager->get_num_total_named_objects_read_only();
        }
        return manager->get_num_total_named_objects();
    }

    std::size_t get_reserved_named_objects() const
    {
        if (read_only_mode)
        {
            return manager->get_reserved_named_objects_read_only();
        }
        return manager->get_reserved_named_objects();
    }

    void reserve_named_objects(std::size_t count)
    {
        ensure_writable("reserve_named_objects");
        manager->reserve_named_objects(count);
    }

    void shrink_to_fit_indexes()
    {
        ensure_writable("shrink_to_fit_indexes");
        manager->shrink_to_fit_indexes();
    }

    template <typename Func>
    void for_each_named_object(Func&& func) const
    {
        ensure_writable("for_each_named_object");
        manager->for_each_named_object(std::forward<Func>(func));
    }

    template <typename Func>
    void for_each_named_object_read_only(Func&& func) const
    {
        manager->for_each_named_object_read_only(
            [&](const char* object_name, void* object_ptr, std::size_t instance_count) {
                func(object_name, static_cast<const void*>(object_ptr), instance_count);
            });
    }

    // Direct access to the segment manager
    SharedMemoryManager* get_segment_manager() const
    {
        ensure_writable("get_segment_manager");
        return manager;
    }

    const SharedMemoryManager* get_segment_manager_read_only() const noexcept
    {
        return manager;
    }

    bool is_read_only() const noexcept
    {
        return read_only_mode;
    }

    std::size_t get_size() const
    {
        return manager != nullptr ? manager->get_size() : region.get_size();
    }

    std::size_t get_free_memory() const
    {
        if (read_only_mode)
        {
            return manager->get_free_memory_read_only();
        }
        return manager->get_free_memory();
    }

    static bool remove(const char* name)
    {
        return SharedMemoryObject::remove(name);
    }

    static bool grow(const char* name, std::size_t extra_bytes)
    {
        if (extra_bytes == 0)
        {
            return false;
        }

        SharedMemoryObject shared_memory(open_only, name, mode_t::read_write);
        const std::size_t old_size = shared_memory.get_size();
        if (extra_bytes > std::numeric_limits<std::size_t>::max() - old_size)
        {
            throw std::length_error("Shared memory segment grow size overflow");
        }

        const std::size_t new_size = align_file_size(old_size + extra_bytes);
        if (new_size <= old_size)
        {
            return false;
        }

        if (!try_truncate(shared_memory, new_size))
        {
            return false;
        }
        try
        {
            MappedRegion mapped_region(shared_memory, mode_t::read_write);
            SharedMemoryManager* segment_manager =
                SharedMemoryManager::attach(mapped_region.get_address());
            if (!segment_manager->grow_to_size(new_size))
            {
                (void)try_truncate(shared_memory, old_size);
                return false;
            }
        }
        catch (...)
        {
            try
            {
                (void)try_truncate(shared_memory, old_size);
            }
            catch (...)
            {
            }
            throw;
        }

        return true;
    }

    static bool shrink_to_fit(const char* name)
    {
        SharedMemoryObject shared_memory(open_only, name, mode_t::read_write);
        const std::size_t old_file_size = shared_memory.get_size();
        std::size_t old_logical_size = 0;
        std::size_t new_logical_size = 0;
        std::size_t new_file_size = 0;

        {
            MappedRegion mapped_region(shared_memory, mode_t::read_write);
            SharedMemoryManager* segment_manager =
                SharedMemoryManager::attach(mapped_region.get_address());
            old_logical_size = segment_manager->get_size();
            new_logical_size = segment_manager->shrink_to_fit();
            new_file_size = align_file_size(new_logical_size);
            if (new_logical_size >= old_logical_size && new_file_size >= old_file_size)
            {
                return false;
            }
        }

        if (new_file_size >= old_file_size)
        {
            MappedRegion rollback_region(shared_memory, mode_t::read_write);
            SharedMemoryManager* segment_manager =
                SharedMemoryManager::attach(rollback_region.get_address());
            segment_manager->grow_to_size(old_logical_size);
            return false;
        }

        try
        {
            if (!try_truncate(shared_memory, new_file_size))
            {
                MappedRegion rollback_region(shared_memory, mode_t::read_write);
                SharedMemoryManager* segment_manager =
                    SharedMemoryManager::attach(rollback_region.get_address());
                segment_manager->grow_to_size(old_logical_size);
                return false;
            }
        }
        catch (...)
        {
            try
            {
                MappedRegion rollback_region(shared_memory, mode_t::read_write);
                SharedMemoryManager* segment_manager =
                    SharedMemoryManager::attach(rollback_region.get_address());
                segment_manager->grow_to_size(old_logical_size);
            }
            catch (...)
            {
            }
            throw;
        }

        return true;
    }

private:
    static std::size_t align_file_size(std::size_t size)
    {
        return align_size(size, MappedRegion::get_page_size());
    }

    static std::size_t align_size(std::size_t size, std::size_t alignment)
    {
        if (size > std::numeric_limits<std::size_t>::max() - (alignment - 1))
        {
            throw std::length_error("Shared memory segment size overflow");
        }
        return (size + alignment - 1) & ~(alignment - 1);
    }

    static bool try_truncate(SharedMemoryObject& shared_memory, std::size_t size)
    {
        try
        {
            shared_memory.truncate(size);
            return true;
        }
        catch (const std::system_error& e)
        {
            if (e.code().value() == EINVAL || e.code().value() == ENOTSUP ||
                e.code().value() == EOPNOTSUPP)
            {
                return false;
            }
            throw;
        }
    }

    void ensure_writable(const char* operation) const
    {
        if (read_only_mode)
        {
            throw std::runtime_error(std::string(operation) +
                                     " is not available on read-only shared memory mappings");
        }
    }

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

    void attach_existing_with_retry(mode_t mapping_mode, bool recover_abandoned_named_objects)
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
                        region = MappedRegion(shm, mapping_mode);
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
                        if (recover_abandoned_named_objects)
                        {
                            manager->recover_abandoned_named_objects();
                        }
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
    bool read_only_mode;
};

} // namespace interprocess
