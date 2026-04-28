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
#include <string>
#include <type_traits>
#include <utility>

namespace interprocess
{

class alignas(16) SharedMemoryManager
{
public:
    static constexpr uint32_t MAGIC = 0x534D4D32; // "SMM2"

    enum class InitializationState : uint32_t
    {
        uninitialized = 0,
        initializing = 1,
        initialized = 2,
        corrupted = 3
    };

    static std::size_t minimum_initialization_size() noexcept
    {
        return sizeof(uint32_t);
    }

    static InitializationState get_initialization_state(const void* base_addr) noexcept
    {
        return static_cast<InitializationState>(
            load_state_word(static_cast<const uint32_t*>(base_addr)));
    }

    static bool is_known_initialization_state(InitializationState state) noexcept
    {
        switch (state)
        {
        case InitializationState::uninitialized:
        case InitializationState::initializing:
        case InitializationState::initialized:
        case InitializationState::corrupted:
            return true;
        }
        return false;
    }

    static const char* initialization_state_name(InitializationState state) noexcept
    {
        switch (state)
        {
        case InitializationState::uninitialized:
            return "uninitialized";
        case InitializationState::initializing:
            return "initializing";
        case InitializationState::initialized:
            return "initialized";
        case InitializationState::corrupted:
            return "corrupted";
        }
        return "unknown";
    }

    static void mark_corrupted(void* base_addr) noexcept
    {
        store_state_word(static_cast<uint32_t*>(base_addr),
                         static_cast<uint32_t>(InitializationState::corrupted));
    }

    static SharedMemoryManager* create(void* base_addr, std::size_t total_size)
    {
        if (total_size < sizeof(SharedMemoryManager) + sizeof(detail::BlockHeader))
        {
            throw std::runtime_error("Shared memory size is too small to initialize manager.");
        }

        auto* state_word = static_cast<uint32_t*>(base_addr);
        uint32_t expected = static_cast<uint32_t>(InitializationState::uninitialized);
        if (!compare_exchange_state_word(state_word, expected,
                                         static_cast<uint32_t>(InitializationState::initializing)))
        {
            InitializationState state = static_cast<InitializationState>(expected);
            throw std::runtime_error(
                std::string("Shared memory segment is not ready to initialize: ") +
                initialization_state_name(state));
        }

        try
        {
            return new (base_addr) SharedMemoryManager(total_size);
        }
        catch (...)
        {
            mark_corrupted(base_addr);
            throw;
        }
    }

    static SharedMemoryManager* attach(void* base_addr)
    {
        SharedMemoryManager* manager = static_cast<SharedMemoryManager*>(base_addr);
        InitializationState state = get_initialization_state(base_addr);
        if (state != InitializationState::initialized)
        {
            throw std::runtime_error(std::string("Shared memory manager is not initialized: ") +
                                     initialization_state_name(state));
        }
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
        return with_manager_lock([&] { return block_allocator.allocate(size, alignment); });
    }

    void deallocate(void* ptr)
    {
        with_manager_lock([&] { block_allocator.deallocate(ptr); });
    }

    template <typename T, typename... Args>
    T* construct(const char* name, Args&&... args)
    {
        void* object_storage = nullptr;
        void* header_storage = nullptr;
        detail::NamedObjectHeader* header = nullptr;

        {
            bool already_exists = false;

            with_manager_lock([&] {
                if (named_objects.find_any(name) != nullptr)
                {
                    already_exists = true;
                    return;
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
            });

            if (already_exists)
            {
                return nullptr;
            }
        }

        T* obj = nullptr;
        try
        {
            obj = new (object_storage) T(std::forward<Args>(args)...);
        }
        catch (...)
        {
            with_manager_lock([&] {
                named_objects.unlink(header);
                header->~NamedObjectHeader();
                block_allocator.deallocate(header);
                block_allocator.deallocate(object_storage);
            });
            throw;
        }

        {
            with_manager_lock([&] {
                header->ptr = obj;
                header->state = detail::NamedObjectState::ready;
            });
        }

        return obj;
    }

    template <typename T>
    T* find(const char* name)
    {
        return with_manager_lock([&]() -> T* {
            detail::NamedObjectHeader* curr = named_objects.find_ready(name);
            return curr ? static_cast<T*>(curr->ptr.get()) : nullptr;
        });
    }

    template <typename T>
    bool destroy(const char* name)
    {
        detail::NamedObjectHeader* header = nullptr;
        void* object_storage = nullptr;

        {
            bool found = false;
            with_manager_lock([&] {
                header = named_objects.find_ready(name);
                if (header == nullptr)
                {
                    return;
                }

                header->state = detail::NamedObjectState::destroying;
                object_storage = header->ptr.get();
                found = true;
            });

            if (!found)
            {
                return false;
            }
        }

        static_cast<T*>(object_storage)->~T();

        {
            with_manager_lock([&] {
                named_objects.unlink(header);
                header->~NamedObjectHeader();
                block_allocator.deallocate(header);
                block_allocator.deallocate(object_storage);
            });
        }

        return true;
    }

    std::size_t get_free_memory() const
    {
        return with_manager_lock([&] { return block_allocator.get_free_memory(); });
    }

    bool owns(const void* ptr) const
    {
        return with_manager_lock([&] { return block_allocator.owns(ptr); });
    }

    std::size_t allocation_size(const void* ptr) const
    {
        return with_manager_lock([&] { return block_allocator.allocation_size(ptr); });
    }

    bool check_sanity() const
    {
        return with_manager_lock([&] { return check_sanity_unlocked(); });
    }

    bool all_memory_deallocated() const
    {
        return with_manager_lock([&] { return block_allocator.all_memory_deallocated(); });
    }

    void zero_free_memory()
    {
        with_manager_lock([&] { block_allocator.zero_free_memory(); });
    }

private:
    static uint32_t load_state_word(const uint32_t* word) noexcept
    {
#if defined(__GNUC__) || defined(__clang__)
        return __atomic_load_n(word, __ATOMIC_ACQUIRE);
#else
        return *reinterpret_cast<const volatile uint32_t*>(word);
#endif
    }

    static void store_state_word(uint32_t* word, uint32_t value) noexcept
    {
#if defined(__GNUC__) || defined(__clang__)
        __atomic_store_n(word, value, __ATOMIC_RELEASE);
#else
        *reinterpret_cast<volatile uint32_t*>(word) = value;
#endif
    }

    static bool compare_exchange_state_word(uint32_t* word, uint32_t& expected,
                                            uint32_t desired) noexcept
    {
#if defined(__GNUC__) || defined(__clang__)
        return __atomic_compare_exchange_n(word, &expected, desired, false, __ATOMIC_ACQ_REL,
                                           __ATOMIC_ACQUIRE);
#else
        if (*reinterpret_cast<volatile uint32_t*>(word) == expected)
        {
            *reinterpret_cast<volatile uint32_t*>(word) = desired;
            return true;
        }
        expected = *reinterpret_cast<volatile uint32_t*>(word);
        return false;
#endif
    }

    bool check_sanity_unlocked() const noexcept
    {
        return get_initialization_state(this) == InitializationState::initialized &&
               magic == MAGIC && block_allocator.check_sanity();
    }

    void lock_for_manager_recovery() const
    {
        MutexLockStatus status = mutex.lock_with_recovery_status();
        if (status == MutexLockStatus::acquired)
        {
            return;
        }

        if (!check_sanity_unlocked())
        {
            mutex.unlock();
            throw std::runtime_error(
                "Shared memory manager mutex owner died and sanity check failed");
        }

        try
        {
            mutex.mark_consistent();
        }
        catch (...)
        {
            mutex.unlock();
            throw;
        }
    }

    template <typename Func>
    auto with_manager_lock(Func&& func) const -> std::invoke_result_t<Func>
    {
        lock_for_manager_recovery();
        try
        {
            if constexpr (std::is_void_v<std::invoke_result_t<Func>>)
            {
                std::forward<Func>(func)();
                mutex.unlock();
            }
            else
            {
                std::invoke_result_t<Func> result = std::forward<Func>(func)();
                mutex.unlock();
                return result;
            }
        }
        catch (...)
        {
            mutex.unlock();
            throw;
        }
    }

private:
    uint32_t initialization_state;
    uint32_t magic;
    mutable InterprocessMutex mutex;
    std::size_t total_size;
    detail::SharedMemoryBlockAllocator block_allocator;
    detail::NamedObjectRegistry named_objects;

    explicit SharedMemoryManager(std::size_t total_size)
        : initialization_state(static_cast<uint32_t>(InitializationState::initializing)), magic(0),
          total_size(total_size), block_allocator(), named_objects()
    {
        named_objects.initialize();
        block_allocator.initialize(this, sizeof(SharedMemoryManager), total_size);
        magic = MAGIC;
        store_state_word(&initialization_state,
                         static_cast<uint32_t>(InitializationState::initialized));
    }
};

} // namespace interprocess
