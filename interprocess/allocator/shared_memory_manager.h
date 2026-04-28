#pragma once

#include "../sync/posix_mutex.h"
#include "detail/named_object_registry.h"
#include "detail/shared_memory_block_allocator.h"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unistd.h>
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

    void allocate_many(std::size_t size, std::size_t count, std::size_t alignment, void** out)
    {
        if (out == nullptr && count != 0)
        {
            throw std::invalid_argument("allocate_many output array must not be null");
        }
        with_manager_lock([&] { block_allocator.allocate_many(size, count, alignment, out); });
    }

    void deallocate_many(void* const* ptrs, std::size_t count)
    {
        if (ptrs == nullptr && count != 0)
        {
            throw std::invalid_argument("deallocate_many pointer array must not be null");
        }
        with_manager_lock([&] { block_allocator.deallocate_many(ptrs, count); });
    }

    bool try_expand(void* ptr, std::size_t new_size, std::size_t alignment)
    {
        return with_manager_lock(
            [&] { return block_allocator.try_expand(ptr, new_size, alignment); });
    }

    template <typename T, typename... Args>
    T* construct(const char* name, Args&&... args)
    {
        NamedReservation reservation = reserve_named_storage<T>(name, 1, false);
        if (reservation.existing_object != nullptr)
        {
            return static_cast<T*>(reservation.existing_object);
        }
        if (reservation.header == nullptr)
        {
            return nullptr;
        }

        T* obj = static_cast<T*>(reservation.object_storage);
        try
        {
            new (obj) T(std::forward<Args>(args)...);
        }
        catch (...)
        {
            release_failed_construction(reservation.header);
            throw;
        }

        mark_named_object_ready(reservation.header);
        return obj;
    }

    template <typename T, typename... Args>
    T* find_or_construct(const char* name, Args&&... args)
    {
        NamedReservation reservation = reserve_named_storage<T>(name, 1, true);
        if (reservation.existing_object != nullptr)
        {
            return static_cast<T*>(reservation.existing_object);
        }
        if (reservation.header == nullptr)
        {
            return nullptr;
        }

        T* obj = static_cast<T*>(reservation.object_storage);
        try
        {
            new (obj) T(std::forward<Args>(args)...);
        }
        catch (...)
        {
            release_failed_construction(reservation.header);
            throw;
        }

        mark_named_object_ready(reservation.header);
        return obj;
    }

    template <typename T, typename... Args>
    T* construct_array(const char* name, std::size_t count, Args&&... args)
    {
        NamedReservation reservation = reserve_named_storage<T>(name, count, false);
        if (reservation.existing_object != nullptr)
        {
            return static_cast<T*>(reservation.existing_object);
        }
        if (reservation.header == nullptr)
        {
            return nullptr;
        }

        T* array = static_cast<T*>(reservation.object_storage);
        std::size_t constructed = 0;
        try
        {
            for (; constructed < count; ++constructed)
            {
                new (array + constructed) T(args...);
            }
        }
        catch (...)
        {
            destroy_constructed_range(array, constructed);
            release_failed_construction(reservation.header);
            throw;
        }

        mark_named_object_ready(reservation.header);
        return array;
    }

    template <typename T, typename InputIt>
    T* construct_array_from_range(const char* name, InputIt first, InputIt last)
    {
        std::size_t count = static_cast<std::size_t>(std::distance(first, last));
        NamedReservation reservation = reserve_named_storage<T>(name, count, false);
        if (reservation.header == nullptr)
        {
            return nullptr;
        }

        T* array = static_cast<T*>(reservation.object_storage);
        std::size_t constructed = 0;
        try
        {
            for (InputIt it = first; it != last; ++it, ++constructed)
            {
                new (array + constructed) T(*it);
            }
        }
        catch (...)
        {
            destroy_constructed_range(array, constructed);
            release_failed_construction(reservation.header);
            throw;
        }

        mark_named_object_ready(reservation.header);
        return array;
    }

    template <typename T>
    T* find(const char* name)
    {
        std::size_t name_length = validate_name(name);
        return with_manager_lock([&]() -> T* {
            detail::NamedObjectHeader* curr = named_objects.find_ready(name, name_length);
            return curr ? static_cast<T*>(curr->ptr.get()) : nullptr;
        });
    }

    template <typename T>
    T* find_array(const char* name, std::size_t* count = nullptr)
    {
        std::size_t name_length = validate_name(name);
        return with_manager_lock([&]() -> T* {
            detail::NamedObjectHeader* curr = named_objects.find_ready(name, name_length);
            if (curr == nullptr)
            {
                if (count != nullptr)
                {
                    *count = 0;
                }
                return nullptr;
            }
            if (count != nullptr)
            {
                *count = curr->instance_count;
            }
            return static_cast<T*>(curr->ptr.get());
        });
    }

    template <typename T>
    bool destroy(const char* name)
    {
        std::size_t name_length = validate_name(name);
        detail::NamedObjectHeader* header = with_manager_lock([&]() -> detail::NamedObjectHeader* {
            detail::NamedObjectHeader* found = named_objects.find_ready(name, name_length);
            if (found != nullptr)
            {
                named_objects.mark_not_ready(found, detail::NamedObjectState::destroying);
                found->owner_pid = current_process_id();
            }
            return found;
        });
        return destroy_reserved_object<T>(header);
    }

    template <typename T>
    bool destroy_array(const char* name)
    {
        return destroy<T>(name);
    }

    template <typename T>
    bool destroy_ptr(T* ptr)
    {
        if (ptr == nullptr)
        {
            return false;
        }

        detail::NamedObjectHeader* header = with_manager_lock([&]() -> detail::NamedObjectHeader* {
            detail::NamedObjectHeader* found = named_objects.find_ready_by_ptr(ptr);
            if (found != nullptr)
            {
                named_objects.mark_not_ready(found, detail::NamedObjectState::destroying);
                found->owner_pid = current_process_id();
            }
            return found;
        });
        return destroy_reserved_object<T>(header);
    }

    std::size_t get_num_named_objects() const
    {
        return with_manager_lock([&] { return named_objects.ready_size(); });
    }

    std::size_t get_num_total_named_objects() const
    {
        return with_manager_lock([&] { return named_objects.total_size(); });
    }

    std::size_t get_reserved_named_objects() const
    {
        return with_manager_lock([&] { return named_objects.reserved_size(); });
    }

    void reserve_named_objects(std::size_t count)
    {
        with_manager_lock([&] { named_objects.reserve(count); });
    }

    void shrink_to_fit_indexes()
    {
        with_manager_lock([&] { named_objects.shrink_to_fit(); });
    }

    template <typename Func>
    void for_each_named_object(Func&& func) const
    {
        with_manager_lock([&] {
            named_objects.for_each([&](const detail::NamedObjectHeader& header) {
                if (header.state == detail::NamedObjectState::ready)
                {
                    func(header.name.get(), header.ptr.get(), header.instance_count);
                }
            });
        });
    }

    template <typename Func>
    void for_each_named_object_read_only(Func&& func) const
    {
        named_objects.for_each([&](const detail::NamedObjectHeader& header) {
            if (header.state == detail::NamedObjectState::ready)
            {
                func(header.name.get(), header.ptr.get(), header.instance_count);
            }
        });
    }

    std::size_t recover_abandoned_named_objects()
    {
        return with_manager_lock([&] {
            std::size_t recovered = 0;
            while (true)
            {
                detail::NamedObjectHeader* abandoned =
                    named_objects.find_if([&](const detail::NamedObjectHeader& header) {
                        return header.state != detail::NamedObjectState::ready &&
                               !is_process_alive(header.owner_pid);
                    });
                if (abandoned == nullptr)
                {
                    return recovered;
                }

                deallocate_named_storage_unlocked(abandoned);
                ++recovered;
            }
        });
    }

    std::size_t get_free_memory() const
    {
        return with_manager_lock([&] { return block_allocator.get_free_memory(); });
    }

    std::size_t get_size() const noexcept
    {
        return total_size;
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

    bool grow_to_size(std::size_t new_total_size)
    {
        return with_manager_lock([&] {
            if (new_total_size <= total_size)
            {
                return false;
            }
            bool changed = block_allocator.grow(new_total_size);
            if (changed)
            {
                total_size = new_total_size;
            }
            return changed;
        });
    }

    std::size_t shrink_to_fit()
    {
        return with_manager_lock([&] {
            std::size_t new_total_size = block_allocator.shrink_to_fit();
            total_size = new_total_size;
            return new_total_size;
        });
    }

    template <typename T>
    const T* find_read_only(const char* name) const
    {
        std::size_t name_length = validate_name(name);
        detail::NamedObjectHeader* curr = named_objects.find_ready(name, name_length);
        return curr ? static_cast<const T*>(curr->ptr.get()) : nullptr;
    }

    template <typename T>
    const T* find_array_read_only(const char* name, std::size_t* count = nullptr) const
    {
        std::size_t name_length = validate_name(name);
        detail::NamedObjectHeader* curr = named_objects.find_ready(name, name_length);
        if (curr == nullptr)
        {
            if (count != nullptr)
            {
                *count = 0;
            }
            return nullptr;
        }
        if (count != nullptr)
        {
            *count = curr->instance_count;
        }
        return static_cast<const T*>(curr->ptr.get());
    }

    std::size_t get_num_named_objects_read_only() const
    {
        return named_objects.ready_size();
    }

    std::size_t get_num_total_named_objects_read_only() const
    {
        return named_objects.total_size();
    }

    std::size_t get_reserved_named_objects_read_only() const
    {
        return named_objects.reserved_size();
    }

    std::size_t get_free_memory_read_only() const
    {
        return block_allocator.get_free_memory();
    }

private:
    struct NamedReservation
    {
        detail::NamedObjectHeader* header = nullptr;
        void* object_storage = nullptr;
        void* existing_object = nullptr;
    };

    static std::size_t validate_name(const char* name)
    {
        if (name == nullptr || name[0] == '\0')
        {
            throw std::invalid_argument("Named shared memory object name must not be empty");
        }
        return std::strlen(name);
    }

    static uint64_t current_process_id() noexcept
    {
        return static_cast<uint64_t>(getpid());
    }

    static bool is_process_alive(uint64_t process_id) noexcept
    {
        if (process_id == 0)
        {
            return false;
        }

        if (kill(static_cast<pid_t>(process_id), 0) == 0)
        {
            return true;
        }

        return errno == EPERM;
    }

    template <typename T>
    static std::size_t checked_object_bytes(std::size_t count)
    {
        if (count == 0)
        {
            throw std::invalid_argument("Named object array count must be greater than zero");
        }

        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
        {
            throw std::length_error("Named object allocation size overflow");
        }

        return count * sizeof(T);
    }

    template <typename T>
    NamedReservation reserve_named_storage(const char* name, std::size_t count,
                                           bool return_existing_ready)
    {
        std::size_t name_length = validate_name(name);
        std::size_t object_bytes = checked_object_bytes<T>(count);
        uint64_t name_hash = detail::NamedObjectRegistry::hash_name(name, name_length);
        NamedReservation reservation;

        with_manager_lock([&] {
            detail::NamedObjectHeader* existing = named_objects.find_any(name, name_length);
            if (existing != nullptr)
            {
                if (return_existing_ready && existing->state == detail::NamedObjectState::ready)
                {
                    reservation.existing_object = existing->ptr.get();
                }
                return;
            }

            void* object_storage = nullptr;
            void* header_storage = nullptr;
            char* name_storage = nullptr;

            try
            {
                object_storage = block_allocator.allocate(object_bytes, alignof(T));
                header_storage = block_allocator.allocate(sizeof(detail::NamedObjectHeader),
                                                          alignof(detail::NamedObjectHeader));
                name_storage =
                    static_cast<char*>(block_allocator.allocate(name_length + 1, alignof(char)));
            }
            catch (...)
            {
                block_allocator.deallocate(name_storage);
                block_allocator.deallocate(header_storage);
                block_allocator.deallocate(object_storage);
                throw;
            }

            std::memcpy(name_storage, name, name_length);
            name_storage[name_length] = '\0';

            auto* header = new (header_storage) detail::NamedObjectHeader();
            header->ptr = object_storage;
            header->name = name_storage;
            header->next = nullptr;
            header->name_length = name_length;
            header->instance_count = count;
            header->object_size = sizeof(T);
            header->name_hash = name_hash;
            header->owner_pid = current_process_id();
            header->state = detail::NamedObjectState::constructing;
            header->reserved = 0;

            named_objects.insert(header);
            reservation.header = header;
            reservation.object_storage = object_storage;
        });

        return reservation;
    }

    void mark_named_object_ready(detail::NamedObjectHeader* header)
    {
        with_manager_lock([&] {
            header->owner_pid = 0;
            named_objects.mark_ready(header);
        });
    }

    void release_failed_construction(detail::NamedObjectHeader* header)
    {
        with_manager_lock([&] { deallocate_named_storage_unlocked(header); });
    }

    template <typename T>
    static void destroy_constructed_range(T* objects, std::size_t count) noexcept
    {
        while (count > 0)
        {
            --count;
            objects[count].~T();
        }
    }

    template <typename T>
    bool destroy_reserved_object(detail::NamedObjectHeader* header)
    {
        if (header == nullptr)
        {
            return false;
        }

        T* objects = static_cast<T*>(header->ptr.get());
        std::size_t count = header->instance_count;
        destroy_constructed_range(objects, count);

        with_manager_lock([&] { deallocate_named_storage_unlocked(header); });
        return true;
    }

    void deallocate_named_storage_unlocked(detail::NamedObjectHeader* header)
    {
        if (header == nullptr)
        {
            return;
        }

        void* object_storage = header->ptr.get();
        char* name_storage = header->name.get();
        named_objects.unlink(header);
        header->~NamedObjectHeader();
        block_allocator.deallocate(name_storage);
        block_allocator.deallocate(header);
        block_allocator.deallocate(object_storage);
    }

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
