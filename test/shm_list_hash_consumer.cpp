#include "../interprocess/container/shared_memory_hash_map.h"
#include "../interprocess/container/shared_memory_list.h"
#include "../interprocess/container/shared_memory_string.h"
#include "../interprocess/container/shared_memory_vector.h"
#include "../interprocess/ipc/managed_shared_memory.h"
#include "../interprocess/sync/posix_mutex.h"
#include <iostream>
#include <mutex>
#include <string>

using namespace interprocess;

using ShmString = SharedMemoryString;
using ShmStringAllocator = SharedMemoryAllocator<char>;
using IntList = SharedMemoryList<int>;
using IntVector = SharedMemoryVector<int>;
using HashMap = SharedMemoryHashMap<ShmString, IntVector>;

struct SharedRoot
{
    InterprocessMutex mutex;
    IntList timeline;
    HashMap groups;

    SharedRoot(const SharedMemoryAllocator<int>& list_allocator,
               const SharedMemoryAllocator<std::pair<const ShmString, IntVector>>& map_allocator)
        : timeline(list_allocator), groups(map_allocator)
    {
    }
};

static bool require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "[Consumer] " << message << std::endl;
        return false;
    }
    return true;
}

int main()
{
    const char* shm_name = "test_shm_list_hash_catalog";

    try
    {
        ManagedSharedMemory managed(open_only, shm_name);
        auto char_allocator = managed.get_allocator<char>();

        SharedRoot* root = managed.find<SharedRoot>("RootObject");
        if (!root)
        {
            std::cerr << "[Consumer] RootObject not found" << std::endl;
            return 1;
        }

        std::lock_guard<InterprocessMutex> lock(root->mutex);

        const int expected_timeline[] = {1, 2, 3, 4, 5};
        std::size_t index = 0;
        for (auto it = root->timeline.begin(); it != root->timeline.end(); ++it, ++index)
        {
            if (!require(index < 5 && *it == expected_timeline[index],
                         "timeline order mismatch"))
            {
                return 1;
            }
        }
        if (!require(index == 5, "timeline size mismatch"))
        {
            return 1;
        }

        auto primes = root->groups.find(ShmString("primes", char_allocator));
        auto fib = root->groups.find(ShmString("fibonacci", char_allocator));
        auto evens = root->groups.find(ShmString("evens", char_allocator));
        if (!require(primes != root->groups.end() && fib != root->groups.end() &&
                         evens != root->groups.end(),
                     "missing expected group key"))
        {
            return 1;
        }

        if (!require(primes->second.size() == 3 && primes->second[0] == 2 &&
                         primes->second[2] == 5,
                     "primes vector mismatch"))
        {
            return 1;
        }

        if (!require(fib->second.size() == 5 && fib->second[4] == 5,
                     "fibonacci vector mismatch"))
        {
            return 1;
        }

        if (!require(evens->second.size() == 3 && evens->second[1] == 4,
                     "evens vector mismatch"))
        {
            return 1;
        }

        std::cout << "[Consumer] SUCCESS: SharedMemoryList/HashMap data matched expectations."
                  << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Consumer] Exception: " << e.what() << std::endl;
        return 1;
    }
}
