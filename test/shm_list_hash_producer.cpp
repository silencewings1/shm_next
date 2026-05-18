#include "../interprocess/container/shared_memory_hash_map.h"
#include "../interprocess/container/shared_memory_list.h"
#include "../interprocess/container/shared_memory_string.h"
#include "../interprocess/container/shared_memory_vector.h"
#include "../interprocess/ipc/managed_shared_memory.h"
#include "../interprocess/sync/posix_mutex.h"
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

using namespace interprocess;

using ShmString = SharedMemoryString;
using ShmStringAllocator = SharedMemoryAllocator<char>;
using IntList = SharedMemoryList<int>;
using IntListAllocator = SharedMemoryAllocator<int>;
using IntVector = SharedMemoryVector<int>;
using IntVectorAllocator = SharedMemoryAllocator<int>;
using HashMap = SharedMemoryHashMap<ShmString, IntVector>;
using HashMapAllocator = SharedMemoryAllocator<std::pair<const ShmString, IntVector>>;

struct SharedRoot
{
    InterprocessMutex mutex;
    IntList timeline;
    HashMap groups;

    SharedRoot(const IntListAllocator& list_allocator, const HashMapAllocator& map_allocator)
        : timeline(list_allocator), groups(map_allocator)
    {
    }
};

static IntVector build_vector(std::initializer_list<int> values, const IntVectorAllocator& alloc)
{
    IntVector vector(alloc);
    for (int value : values)
    {
        vector.push_back(value);
    }
    return vector;
}

int main()
{
    const char* shm_name = "test_shm_list_hash_catalog";
    const std::size_t shm_size = 1024 * 512;

    ManagedSharedMemory::remove(shm_name);

    try
    {
        ManagedSharedMemory managed(create_only, shm_name, shm_size);
        auto char_allocator = managed.get_allocator<char>();
        auto int_allocator = managed.get_allocator<int>();

        SharedRoot* root = managed.construct<SharedRoot>(
            "RootObject", int_allocator,
            managed.get_allocator<std::pair<const ShmString, IntVector>>());
        if (!root)
        {
            std::cerr << "[Producer] Failed to construct RootObject" << std::endl;
            return 1;
        }

        {
            std::lock_guard<InterprocessMutex> lock(root->mutex);

            root->timeline.assign({4, 2, 2, 1, 3});
            root->timeline.sort();
            root->timeline.unique();
            root->timeline.push_back(5);

            root->groups.emplace(ShmString("primes", char_allocator),
                                 build_vector({2, 3, 5}, int_allocator));
            root->groups.emplace(ShmString("fibonacci", char_allocator),
                                 build_vector({1, 1, 2, 3, 5}, int_allocator));
            root->groups.insert_or_assign(ShmString("evens", char_allocator),
                                          build_vector({2, 4, 6}, int_allocator));

            std::cout << "[Producer] Timeline:";
            for (auto it = root->timeline.begin(); it != root->timeline.end(); ++it)
            {
                std::cout << ' ' << *it;
            }
            std::cout << std::endl;

            std::cout << "[Producer] Groups ready:" << std::endl;
            for (auto it = root->groups.begin(); it != root->groups.end(); ++it)
            {
                std::cout << "  " << it->first << " => size " << it->second.size() << std::endl;
            }
        }

        std::cout << "[Producer] Shared list/hash ready. Waiting 10 seconds for consumer..."
                  << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Producer] Exception: " << e.what() << std::endl;
        return 1;
    }

    ManagedSharedMemory::remove(shm_name);
    std::cout << "[Producer] Cleanup finished." << std::endl;
    return 0;
}
