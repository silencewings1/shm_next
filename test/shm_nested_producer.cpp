#include "../interprocess/ipc/managed_shared_memory.h"
#include "../interprocess/allocator/shared_memory_string.h"
#include "../interprocess/allocator/shared_memory_vector.h"
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
    const std::size_t shm_size = 1024 * 256;

    ManagedSharedMemory::remove(shm_name);

    try
    {
        ManagedSharedMemory managed(create_only, shm_name, shm_size);
        std::cout << "[Producer] Shared memory created at " << managed.get_segment_manager()
                  << std::endl;

        RootObject* root = managed.construct<RootObject>(
            "RootObject", managed.get_allocator<ShmString>(), managed.get_allocator<ShmIntVec>());
        if (!root)
        {
            std::cerr << "[Producer] Failed to construct RootObject" << std::endl;
            return 1;
        }

        auto char_alloc = managed.get_allocator<char>();
        root->strings.emplace_back("alpha", char_alloc);
        root->strings.emplace_back("beta", char_alloc);
        root->strings.emplace_back("gamma", char_alloc);

        auto int_alloc = managed.get_allocator<int>();
        ShmIntVec& v0 = root->nested_ints.emplace_back(int_alloc);
        v0.push_back(1);
        v0.push_back(2);
        v0.push_back(3);

        ShmIntVec& v1 = root->nested_ints.emplace_back(int_alloc);
        v1.push_back(10);
        v1.push_back(20);

        ShmIntVec& v2 = root->nested_ints.emplace_back(int_alloc);
        v2.push_back(42);

        std::cout << "[Producer] Data written. Press Enter to cleanup and exit..." << std::endl;
        std::cin.get();
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
