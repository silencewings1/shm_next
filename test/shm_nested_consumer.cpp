#include "../interprocess/ipc/managed_shared_memory.h"
#include "../interprocess/container/shared_memory_string.h"
#include "../interprocess/container/shared_memory_vector.h"
#include <iostream>

using namespace interprocess;

using ShmString = SharedMemoryString;
using ShmStringVec = SharedMemoryVector<ShmString>;
using ShmIntVec = SharedMemoryVector<int>;
using ShmIntVecVec = SharedMemoryVector<ShmIntVec>;

struct RootObject
{
    ShmStringVec strings;
    ShmIntVecVec nested_ints;

    RootObject(const SharedMemoryAllocator<ShmString>& strings_alloc,
               const SharedMemoryAllocator<ShmIntVec>& nested_alloc)
        : strings(strings_alloc), nested_ints(nested_alloc)
    {
    }
};

int main()
{
    const char* shm_name = "test_nested_containers_split";

    try
    {
        ManagedSharedMemory managed(open_only, shm_name);
        std::cout << "[Consumer] Shared memory opened at " << managed.get_segment_manager()
                  << std::endl;

        RootObject* root = managed.find<RootObject>("RootObject");
        std::cout << "[Consumer] RootObject found at " << root << std::endl;
        if (!root)
        {
            std::cerr << "[Consumer] RootObject not found" << std::endl;
            return 1;
        }

        if (!(root->strings.size() == 3 && root->strings[0] == "alpha" &&
              root->strings[1] == "beta" && root->strings[2] == "gamma"))
        {
            std::cerr << "[Consumer] String vector mismatch" << std::endl;
            return 1;
        }

        if (!(root->nested_ints.size() == 3 && root->nested_ints[0].size() == 3 &&
              root->nested_ints[0][0] == 1 && root->nested_ints[0][1] == 2 &&
              root->nested_ints[0][2] == 3 && root->nested_ints[1].size() == 2 &&
              root->nested_ints[1][0] == 10 && root->nested_ints[1][1] == 20 &&
              root->nested_ints[2].size() == 1 && root->nested_ints[2][0] == 42))
        {
            std::cerr << "[Consumer] Nested int vectors mismatch" << std::endl;
            return 1;
        }

        std::cout << "[Consumer] SUCCESS: nested containers matched!" << std::endl;
        std::cout << "[Consumer] strings[0]=" << root->strings[0] << std::endl;
        std::cout << "[Consumer] nested_ints[0].back=" << root->nested_ints[0].back() << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Consumer] Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
