#pragma once

#include "../diagnostics.h"
#include "../error.h"
#include "../sync/posix_mutex.h"
#include "detail/named_object_registry.h"
#include "detail/shared_memory_block_allocator.h"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace interprocess
{

class alignas(16) SharedMemoryManager
{
public:
    static constexpr uint32_t MAGIC = 0x534D4D34; // "SMM4"
    static constexpr uint32_t CURRENT_LAYOUT_VERSION = 2;
    static constexpr uint32_t MIN_SUPPORTED_LAYOUT_VERSION = 1;
    static constexpr uint32_t MAX_SUPPORTED_LAYOUT_VERSION = CURRENT_LAYOUT_VERSION;

    enum class InitializationState : uint32_t
    {
        uninitialized = 0,
        initializing = 1,
        initialized = 2,
        corrupted = 3
    };

    static std::size_t minimum_initialization_size() noexcept
    {
        return 4 * sizeof(uint32_t);
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

    static uint32_t current_layout_version() noexcept
    {
        return CURRENT_LAYOUT_VERSION;
    }

    static bool is_supported_layout_version(uint32_t version) noexcept
    {
        return version >= MIN_SUPPORTED_LAYOUT_VERSION && version <= MAX_SUPPORTED_LAYOUT_VERSION;
    }

    static SharedMemoryManager* create(void* base_addr, std::size_t total_size)
    {
        if (total_size < sizeof(SharedMemoryManager) + sizeof(detail::BlockHeader))
        {
            throw_interprocess_error(InterprocessErrc::segment_too_small,
                                     "Shared memory size is too small to initialize manager.");
        }

        auto* state_word = static_cast<uint32_t*>(base_addr);
        uint32_t expected = static_cast<uint32_t>(InitializationState::uninitialized);
        if (!compare_exchange_state_word(state_word, expected,
                                         static_cast<uint32_t>(InitializationState::initializing)))
        {
            InitializationState state = static_cast<InitializationState>(expected);
            const InterprocessErrc errc =
                state == InitializationState::corrupted
                    ? InterprocessErrc::initialization_corrupted
                    : InterprocessErrc::initialization_in_progress;
            throw_interprocess_error(
                errc, std::string("Shared memory segment is not ready to initialize: ") +
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
            const InterprocessErrc errc =
                state == InitializationState::corrupted
                    ? InterprocessErrc::initialization_corrupted
                    : InterprocessErrc::initialization_in_progress;
            throw_interprocess_error(errc,
                                     std::string("Shared memory manager is not initialized: ") +
                                         initialization_state_name(state));
        }
        if (manager->magic != MAGIC)
        {
            throw_interprocess_error(
                InterprocessErrc::magic_mismatch,
                "Shared memory manager magic mismatch! Segment might not be initialized.");
        }
        if (!is_supported_layout_version(manager->layout_version))
        {
            throw_interprocess_error(
                InterprocessErrc::unsupported_layout_version,
                "Unsupported shared memory layout version: " +
                    std::to_string(manager->layout_version));
        }
        if (manager->header_size != sizeof(SharedMemoryManager))
        {
            throw_interprocess_error(
                InterprocessErrc::layout_header_mismatch,
                "Shared memory manager header size mismatch: " +
                    std::to_string(manager->header_size));
        }
        return manager;
    }

    void* allocate(std::size_t size)
    {
        return allocate(size, 16);
    }

    void* allocate(std::size_t size, std::size_t alignment)
    {
        return with_manager_write_lock([&] { return block_allocator.allocate(size, alignment); });
    }

    void deallocate(void* ptr)
    {
        with_manager_write_lock([&] { block_allocator.deallocate(ptr); });
    }

    void allocate_many(std::size_t size, std::size_t count, std::size_t alignment, void** out)
    {
        if (out == nullptr && count != 0)
        {
            throw_interprocess_error(InterprocessErrc::invalid_pointer,
                                     "allocate_many output array must not be null");
        }
        with_manager_write_lock(
            [&] { block_allocator.allocate_many(size, count, alignment, out); });
    }

    void deallocate_many(void* const* ptrs, std::size_t count)
    {
        if (ptrs == nullptr && count != 0)
        {
            throw_interprocess_error(InterprocessErrc::invalid_pointer,
                                     "deallocate_many pointer array must not be null");
        }
        with_manager_write_lock([&] { block_allocator.deallocate_many(ptrs, count); });
    }

    bool try_expand(void* ptr, std::size_t new_size, std::size_t alignment)
    {
        return with_manager_write_lock(
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
            if (curr != nullptr)
            {
                validate_named_type<T>(*curr);
            }
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
            validate_named_type<T>(*curr);
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
        detail::NamedObjectHeader* header =
            with_manager_write_lock([&]() -> detail::NamedObjectHeader* {
                detail::NamedObjectHeader* found = named_objects.find_ready(name, name_length);
                if (found != nullptr)
                {
                    validate_named_type<T>(*found);
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

        detail::NamedObjectHeader* header =
            with_manager_write_lock([&]() -> detail::NamedObjectHeader* {
                detail::NamedObjectHeader* found = named_objects.find_ready_by_ptr(ptr);
                if (found != nullptr)
                {
                    validate_named_type<T>(*found);
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
        with_manager_write_lock([&] { named_objects.reserve(count); });
    }

    void shrink_to_fit_indexes()
    {
        with_manager_write_lock([&] { named_objects.shrink_to_fit(); });
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
        const uint64_t generation = wait_for_stable_metadata_generation();
        std::exception_ptr callback_error;

        try
        {
            named_objects.for_each([&](const detail::NamedObjectHeader& header) {
                if (header.state == detail::NamedObjectState::ready)
                {
                    func(header.name.get(), header.ptr.get(), header.instance_count);
                }
            });
        }
        catch (...)
        {
            callback_error = std::current_exception();
        }

        if (!is_metadata_generation_stable(generation))
        {
            throw_interprocess_error(
                InterprocessErrc::metadata_changed_during_read,
                "Shared memory metadata changed during read-only iteration");
        }
        if (callback_error)
        {
            std::rethrow_exception(callback_error);
        }
    }

    std::size_t recover_abandoned_named_objects()
    {
        return with_manager_write_lock([&] {
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

    SharedMemoryAllocatorStats get_stats() const
    {
        return with_manager_lock([&] { return block_allocator.get_stats(); });
    }

    SharedMemoryAllocatorStats get_allocator_stats() const
    {
        return with_manager_lock([&] { return block_allocator.get_stats(); });
    }

    std::size_t get_size() const noexcept
    {
        return total_size;
    }

    uint32_t get_layout_version() const noexcept
    {
        return layout_version;
    }

    uint32_t get_header_size() const noexcept
    {
        return header_size;
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
        with_manager_write_lock([&] { block_allocator.zero_free_memory(); });
    }

    bool grow_to_size(std::size_t new_total_size)
    {
        return with_manager_write_lock([&] {
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
        return with_manager_write_lock([&] {
            std::size_t new_total_size = block_allocator.shrink_to_fit();
            total_size = new_total_size;
            return new_total_size;
        });
    }

    template <typename T>
    const T* find_read_only(const char* name) const
    {
        std::size_t name_length = validate_name(name);
        return with_read_only_snapshot([&]() -> const T* {
            detail::NamedObjectHeader* curr = named_objects.find_ready(name, name_length);
            if (curr != nullptr)
            {
                validate_named_type<T>(*curr);
            }
            return curr ? static_cast<const T*>(curr->ptr.get()) : nullptr;
        });
    }

    template <typename T>
    const T* find_array_read_only(const char* name, std::size_t* count = nullptr) const
    {
        std::size_t name_length = validate_name(name);
        return with_read_only_snapshot([&]() -> const T* {
            detail::NamedObjectHeader* curr = named_objects.find_ready(name, name_length);
            if (curr == nullptr)
            {
                if (count != nullptr)
                {
                    *count = 0;
                }
                return nullptr;
            }
            validate_named_type<T>(*curr);
            if (count != nullptr)
            {
                *count = curr->instance_count;
            }
            return static_cast<const T*>(curr->ptr.get());
        });
    }

    std::size_t get_num_named_objects_read_only() const
    {
        return with_read_only_snapshot([&] { return named_objects.ready_size(); });
    }

    std::size_t get_num_total_named_objects_read_only() const
    {
        return with_read_only_snapshot([&] { return named_objects.total_size(); });
    }

    std::size_t get_reserved_named_objects_read_only() const
    {
        return with_read_only_snapshot([&] { return named_objects.reserved_size(); });
    }

    std::size_t get_free_memory_read_only() const
    {
        return with_read_only_snapshot([&] { return block_allocator.get_free_memory(); });
    }

    SharedMemoryAllocatorStats get_stats_read_only() const
    {
        return with_read_only_snapshot([&] { return block_allocator.get_stats(); });
    }

    SharedMemoryAllocatorStats get_allocator_stats_read_only() const
    {
        return with_read_only_snapshot([&] { return block_allocator.get_stats(); });
    }

    void validate_read_only_access() const
    {
        with_read_only_snapshot([&] {
            if (!check_sanity_unlocked())
            {
                throw_interprocess_error(InterprocessErrc::metadata_changed_during_read,
                                         "Shared memory manager read-only sanity check failed");
            }
        });
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
            throw_interprocess_error(InterprocessErrc::invalid_name,
                                     "Named shared memory object name must not be empty");
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
            throw_interprocess_error(InterprocessErrc::invalid_name,
                                     "Named object array count must be greater than zero");
        }

        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
        {
            throw_interprocess_error(InterprocessErrc::allocation_size_overflow,
                                     "Named object allocation size overflow");
        }

        return count * sizeof(T);
    }

    static constexpr uint64_t fnv1a_hash(const char* value) noexcept
    {
        uint64_t hash = 1469598103934665603ull;
        while (*value != '\0')
        {
            hash ^= static_cast<unsigned char>(*value);
            hash *= 1099511628211ull;
            ++value;
        }
        return hash;
    }

    template <typename T>
    static constexpr uint64_t named_type_hash() noexcept
    {
#if defined(_MSC_VER)
        return fnv1a_hash(__FUNCSIG__);
#else
        return fnv1a_hash(__PRETTY_FUNCTION__);
#endif
    }

    template <typename T>
    static void validate_named_type(const detail::NamedObjectHeader& header)
    {
        if (header.object_size != sizeof(T) || header.type_hash != named_type_hash<T>())
        {
            throw_interprocess_error(InterprocessErrc::named_object_type_mismatch,
                                     "Named shared memory object type mismatch");
        }
    }

    template <typename T>
    NamedReservation reserve_named_storage(const char* name, std::size_t count,
                                           bool return_existing_ready)
    {
        std::size_t name_length = validate_name(name);
        std::size_t object_bytes = checked_object_bytes<T>(count);
        uint64_t name_hash = detail::NamedObjectRegistry::hash_name(name, name_length);
        NamedReservation reservation;

        with_manager_write_lock([&] {
            detail::NamedObjectHeader* existing = named_objects.find_any(name, name_length);
            if (existing != nullptr)
            {
                if (return_existing_ready && existing->state == detail::NamedObjectState::ready)
                {
                    validate_named_type<T>(*existing);
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
            header->type_hash = named_type_hash<T>();
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
        with_manager_write_lock([&] {
            header->owner_pid = 0;
            named_objects.mark_ready(header);
        });
    }

    void release_failed_construction(detail::NamedObjectHeader* header)
    {
        with_manager_write_lock([&] { deallocate_named_storage_unlocked(header); });
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

        with_manager_write_lock([&] { deallocate_named_storage_unlocked(header); });
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

    static uint64_t load_generation_word(const uint64_t* word) noexcept
    {
#if defined(__GNUC__) || defined(__clang__)
        return __atomic_load_n(word, __ATOMIC_ACQUIRE);
#else
        return *reinterpret_cast<const volatile uint64_t*>(word);
#endif
    }

    static uint64_t add_generation_word(uint64_t* word, uint64_t increment) noexcept
    {
#if defined(__GNUC__) || defined(__clang__)
        return __atomic_add_fetch(word, increment, __ATOMIC_ACQ_REL);
#else
        *reinterpret_cast<volatile uint64_t*>(word) += increment;
        return *reinterpret_cast<volatile uint64_t*>(word);
#endif
    }

    uint64_t load_metadata_generation() const noexcept
    {
        return load_generation_word(&metadata_generation);
    }

    void begin_metadata_write() const noexcept
    {
        if ((load_metadata_generation() & 1u) == 0)
        {
            (void)add_generation_word(&metadata_generation, 1);
        }
    }

    void end_metadata_write() const noexcept
    {
        if ((load_metadata_generation() & 1u) != 0)
        {
            (void)add_generation_word(&metadata_generation, 1);
        }
    }

    bool is_metadata_generation_stable(uint64_t generation) const noexcept
    {
        return load_metadata_generation() == generation;
    }

    uint64_t wait_for_stable_metadata_generation() const
    {
        constexpr int max_attempts = 1024;
        for (int attempt = 0; attempt < max_attempts; ++attempt)
        {
            const uint64_t generation = load_metadata_generation();
            if ((generation & 1u) == 0)
            {
                return generation;
            }
            std::this_thread::yield();
        }

        throw_interprocess_error(InterprocessErrc::metadata_changed_during_read,
                                 "Shared memory metadata writer is active");
    }

    class MetadataWriteGuard
    {
    public:
        explicit MetadataWriteGuard(const SharedMemoryManager& manager) noexcept
            : manager(&manager), active(true)
        {
            manager.begin_metadata_write();
        }

        ~MetadataWriteGuard()
        {
            finish();
        }

        void finish() noexcept
        {
            if (active)
            {
                manager->end_metadata_write();
                active = false;
            }
        }

    private:
        const SharedMemoryManager* manager;
        bool active;
    };

    bool check_sanity_unlocked() const noexcept
    {
        return get_initialization_state(this) == InitializationState::initialized &&
               magic == MAGIC && is_supported_layout_version(layout_version) &&
               header_size == sizeof(SharedMemoryManager) && block_allocator.check_sanity();
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
            throw_interprocess_error(
                InterprocessErrc::initialization_corrupted,
                "Shared memory manager mutex owner died and sanity check failed");
        }

        try
        {
            mutex.mark_consistent();
            end_metadata_write();
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

    template <typename Func>
    auto with_manager_write_lock(Func&& func) const -> std::invoke_result_t<Func>
    {
        lock_for_manager_recovery();
        MetadataWriteGuard metadata_write(*this);
        try
        {
            if constexpr (std::is_void_v<std::invoke_result_t<Func>>)
            {
                std::forward<Func>(func)();
                metadata_write.finish();
                mutex.unlock();
            }
            else
            {
                std::invoke_result_t<Func> result = std::forward<Func>(func)();
                metadata_write.finish();
                mutex.unlock();
                return result;
            }
        }
        catch (...)
        {
            metadata_write.finish();
            mutex.unlock();
            throw;
        }
    }

    template <typename Func>
    auto with_read_only_snapshot(Func&& func) const -> std::invoke_result_t<Func>
    {
        constexpr int max_attempts = 1024;
        for (int attempt = 0; attempt < max_attempts; ++attempt)
        {
            const uint64_t generation = wait_for_stable_metadata_generation();

            if constexpr (std::is_void_v<std::invoke_result_t<Func>>)
            {
                try
                {
                    std::forward<Func>(func)();
                    if (is_metadata_generation_stable(generation))
                    {
                        return;
                    }
                }
                catch (...)
                {
                    if (is_metadata_generation_stable(generation))
                    {
                        throw;
                    }
                }
            }
            else
            {
                try
                {
                    std::invoke_result_t<Func> result = std::forward<Func>(func)();
                    if (is_metadata_generation_stable(generation))
                    {
                        return result;
                    }
                }
                catch (...)
                {
                    if (is_metadata_generation_stable(generation))
                    {
                        throw;
                    }
                }
            }
        }

        throw_interprocess_error(InterprocessErrc::metadata_changed_during_read,
                                 "Shared memory metadata changed during read-only access");
    }

private:
    uint32_t initialization_state;
    uint32_t magic;
    uint32_t layout_version;
    uint32_t header_size;
    mutable InterprocessMutex mutex;
    mutable uint64_t metadata_generation;
    std::size_t total_size;
    detail::SharedMemoryBlockAllocator block_allocator;
    detail::NamedObjectRegistry named_objects;

    explicit SharedMemoryManager(std::size_t total_size)
        : initialization_state(static_cast<uint32_t>(InitializationState::initializing)), magic(0),
          layout_version(CURRENT_LAYOUT_VERSION),
          header_size(static_cast<uint32_t>(sizeof(SharedMemoryManager))),
          metadata_generation(0), total_size(total_size), block_allocator(), named_objects()
    {
        named_objects.initialize();
        block_allocator.initialize(this, sizeof(SharedMemoryManager), total_size);
        magic = MAGIC;
        store_state_word(&initialization_state,
                         static_cast<uint32_t>(InitializationState::initialized));
    }
};

} // namespace interprocess
