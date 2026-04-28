#include "../interprocess/container/shared_memory_string.h"
#include "../interprocess/ipc/managed_shared_memory.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace interprocess;

namespace
{

struct SlowObject
{
    int value;

    SlowObject(std::atomic<bool>* entered, std::atomic<bool>* finish) : value(0)
    {
        entered->store(true, std::memory_order_release);
        while (!finish->load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        value = 7;
    }
};

struct ThrowingObject
{
    ThrowingObject()
    {
        throw std::runtime_error("construction failed");
    }
};

int array_constructed = 0;
int array_destroyed = 0;

struct ArrayThrower
{
    explicit ArrayThrower(int fail_at)
    {
        if (array_constructed == fail_at)
        {
            throw std::runtime_error("array construction failed");
        }
        ++array_constructed;
    }

    ~ArrayThrower()
    {
        ++array_destroyed;
    }
};

struct ExitInConstructor
{
    ExitInConstructor()
    {
        _exit(0);
    }
};

struct DestroyCounter
{
    explicit DestroyCounter(int* counter) : counter(counter)
    {
    }

    ~DestroyCounter()
    {
        ++(*counter);
    }

    int* counter;
};

struct alignas(64) OverAlignedObject
{
    int value;
};

bool require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "[Manager Lifecycle] " << message << std::endl;
        return false;
    }
    return true;
}

bool test_abandoned_construction_cleanup()
{
    const char* shm_name = "test_shm_abandoned_construct";
    const std::size_t shm_size = 128 * 1024;

    ManagedSharedMemory::remove(shm_name);

    pid_t pid = fork();
    if (pid == -1)
    {
        std::cerr << "[Manager Lifecycle] fork failed" << std::endl;
        return false;
    }

    if (pid == 0)
    {
        ManagedSharedMemory segment(create_only, shm_name, shm_size);
        (void)segment.construct<ExitInConstructor>("CrashedObject");
        _exit(2);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        std::cerr << "[Manager Lifecycle] crash-construction child failed" << std::endl;
        ManagedSharedMemory::remove(shm_name);
        return false;
    }

    try
    {
        ManagedSharedMemory segment(open_only, shm_name);
        if (!require(segment.get_num_total_named_objects() == 0,
                     "abandoned constructing object should be cleaned on attach"))
        {
            ManagedSharedMemory::remove(shm_name);
            return false;
        }

        int* reused = segment.construct<int>("CrashedObject", 77);
        if (!require(reused != nullptr && *reused == 77,
                     "cleaned abandoned name should be reusable"))
        {
            ManagedSharedMemory::remove(shm_name);
            return false;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Manager Lifecycle] abandoned cleanup exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name);
        return false;
    }

    ManagedSharedMemory::remove(shm_name);
    return true;
}

} // namespace

int main()
{
    const char* shm_name = "test_shm_manager_lifecycle";
    const std::size_t shm_size = 128 * 1024;

    ManagedSharedMemory::remove(shm_name);

    try
    {
        ManagedSharedMemory segment(create_only, shm_name, shm_size);
        SharedMemoryManager* manager = segment.get_segment_manager();
        if (!require(manager->check_sanity(), "fresh manager failed allocator sanity check"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(manager->all_memory_deallocated(),
                     "fresh manager should not have outstanding allocations"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(segment.get_num_named_objects() == 0 &&
                         segment.get_num_total_named_objects() == 0,
                     "fresh manager should not have named objects"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        std::atomic<bool> entered{false};
        std::atomic<bool> finish{false};
        SlowObject* slow = nullptr;

        std::thread constructor(
            [&] { slow = segment.construct<SlowObject>("SlowObject", &entered, &finish); });

        while (!entered.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (!require(segment.find<SlowObject>("SlowObject") == nullptr,
                     "constructing object should not be visible to find"))
        {
            finish.store(true, std::memory_order_release);
            constructor.join();
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        if (!require(segment.construct<SlowObject>("SlowObject", &entered, &finish) == nullptr,
                     "duplicate construct should fail while object is pending"))
        {
            finish.store(true, std::memory_order_release);
            constructor.join();
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        finish.store(true, std::memory_order_release);
        constructor.join();

        if (!require(slow != nullptr && slow->value == 7, "slow object did not finish"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        if (!require(segment.find<SlowObject>("SlowObject") == slow,
                     "constructed object should become visible"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        if (!require(segment.destroy<SlowObject>("SlowObject"), "destroy should remove object"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        if (!require(segment.find<SlowObject>("SlowObject") == nullptr,
                     "destroyed object should not be visible"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        bool threw = false;
        try
        {
            (void)segment.construct<ThrowingObject>("ThrowingObject");
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }

        if (!require(threw, "throwing constructor did not propagate exception"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        int* reused_name = segment.construct<int>("ThrowingObject", 42);
        if (!require(reused_name != nullptr && *reused_name == 42,
                     "failed construction should clean up the registry entry"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        segment.destroy<int>("ThrowingObject");

        int* first_value = segment.find_or_construct<int>("FindOrConstruct", 11);
        int* existing_value = segment.find_or_construct<int>("FindOrConstruct", 22);
        if (!require(first_value != nullptr && existing_value == first_value &&
                         *existing_value == 11,
                     "find_or_construct should return existing ready object"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(segment.get_num_named_objects() == 1, "named object count mismatch"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        std::size_t visited_named_objects = 0;
        segment.for_each_named_object(
            [&](const char* object_name, void* object_ptr, std::size_t instance_count) {
                if (std::string(object_name) == "FindOrConstruct" && object_ptr == first_value &&
                    instance_count == 1)
                {
                    ++visited_named_objects;
                }
            });
        if (!require(visited_named_objects == 1, "named object iteration did not visit object"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(segment.destroy<int>("FindOrConstruct"),
                     "failed to destroy find_or_construct object"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        std::string long_name_a(96, 'a');
        long_name_a += "_left";
        std::string long_name_b(96, 'a');
        long_name_b += "_right";
        int* long_a = segment.construct<int>(long_name_a.c_str(), 1);
        int* long_b = segment.construct<int>(long_name_b.c_str(), 2);
        if (!require(long_a != nullptr && long_b != nullptr &&
                         segment.find<int>(long_name_a.c_str()) == long_a &&
                         segment.find<int>(long_name_b.c_str()) == long_b && *long_a == 1 &&
                         *long_b == 2,
                     "long named objects should not be truncated or collide"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        segment.destroy<int>(long_name_a.c_str());
        segment.destroy<int>(long_name_b.c_str());

        std::size_t array_count = 0;
        int* numbers = segment.construct_array<int>("Numbers", 3, 7);
        int* found_numbers = segment.find_array<int>("Numbers", &array_count);
        if (!require(numbers != nullptr && found_numbers == numbers && array_count == 3 &&
                         numbers[0] == 7 && numbers[1] == 7 && numbers[2] == 7,
                     "construct_array/find_array mismatch"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(segment.destroy_array<int>("Numbers"), "failed to destroy int array"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        int source_values[] = {1, 2, 3, 4};
        int* range_values = segment.construct_array_from_range<int>(
            "RangeValues", std::begin(source_values), std::end(source_values));
        if (!require(range_values != nullptr && range_values[0] == 1 && range_values[3] == 4,
                     "construct_array_from_range mismatch"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        segment.destroy_array<int>("RangeValues");

        array_constructed = 0;
        array_destroyed = 0;
        bool array_threw = false;
        try
        {
            (void)segment.construct_array<ArrayThrower>("ThrowArray", 5, 3);
        }
        catch (const std::runtime_error&)
        {
            array_threw = true;
        }
        if (!require(array_threw && array_constructed == 3 && array_destroyed == 3,
                     "array construction rollback did not destroy constructed elements"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        int* reused_array_name = segment.construct<int>("ThrowArray", 9);
        if (!require(reused_array_name != nullptr && *reused_array_name == 9,
                     "failed array construction should release named entry"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        segment.destroy<int>("ThrowArray");

        int destroyed_count = 0;
        DestroyCounter* counter =
            segment.construct<DestroyCounter>("DestroyCounter", &destroyed_count);
        if (!require(counter != nullptr, "failed to construct destroy counter"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(segment.destroy<DestroyCounter>("DestroyCounter"),
                     "failed to destroy destroy counter"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(destroyed_count == 1, "destroy did not run the object destructor"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        DestroyCounter* ptr_counter =
            segment.construct<DestroyCounter>("DestroyPtrCounter", &destroyed_count);
        if (!require(ptr_counter != nullptr, "failed to construct destroy_ptr counter"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(segment.destroy_ptr(ptr_counter), "failed to destroy object by pointer"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(destroyed_count == 2, "destroy_ptr did not run destructor"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        auto char_allocator = segment.get_allocator<char>();
        SharedMemoryString* text =
            segment.construct<SharedMemoryString>("Text", "hello", char_allocator);
        if (!require(text != nullptr && *text == "hello", "failed to construct shared string"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(segment.destroy<SharedMemoryString>("Text"),
                     "failed to destroy shared string"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        OverAlignedObject* aligned =
            segment.construct<OverAlignedObject>("OverAlignedObject", OverAlignedObject{99});
        if (!require(aligned != nullptr && aligned->value == 99,
                     "failed to construct over-aligned object"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(reinterpret_cast<uintptr_t>(aligned) % alignof(OverAlignedObject) == 0,
                     "over-aligned object was not correctly aligned"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(manager->owns(aligned), "manager should report owning constructed object"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(manager->allocation_size(aligned) >= sizeof(OverAlignedObject),
                     "allocation size should cover constructed object"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(segment.destroy<OverAlignedObject>("OverAlignedObject"),
                     "failed to destroy over-aligned object"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        manager->zero_free_memory();
        if (!require(manager->check_sanity(), "manager failed allocator sanity check"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(manager->all_memory_deallocated(),
                     "manager should have no outstanding allocations"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
        if (!require(segment.get_num_named_objects() == 0 &&
                         segment.get_num_total_named_objects() == 0,
                     "all named objects should be removed"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        if (!require(test_abandoned_construction_cleanup(),
                     "abandoned construction cleanup test failed"))
        {
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        ManagedSharedMemory::remove(shm_name);
        std::cout << "[Manager Lifecycle] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Manager Lifecycle] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name);
        return 1;
    }
}
