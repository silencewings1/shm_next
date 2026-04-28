#include "../interprocess/container/shared_memory_string.h"
#include "../interprocess/ipc/managed_shared_memory.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

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
