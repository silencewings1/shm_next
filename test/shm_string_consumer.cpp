#include "../interprocess/ipc/managed_shared_memory.h"
#include "../interprocess/allocator/shared_memory_string.h"
#include <iostream>

using namespace interprocess;

struct RootObject
{
    BasicSharedMemoryString<char> my_string;
    RootObject(const SharedMemoryAllocator<char>& alloc) : my_string(alloc)
    {
    }
};

int main()
{
    const char* shm_name = "test_shm_string_split";

    try
    {
        // Re-open shared memory
        ManagedSharedMemory managed_shm(open_only, shm_name);
        std::cout << "[Consumer] Shared memory opened at " << managed_shm.get_segment_manager()
                  << std::endl;

        // Find root object
        RootObject* root = managed_shm.find<RootObject>("RootObject");
        std::cout << "[Consumer] RootObject found at " << root << std::endl;

        if (root)
        {
            std::cout << "[Consumer] String value read: " << root->my_string.c_str() << std::endl;
            if (root->my_string == "Hello from Split Producer!")
            {
                std::cout << "[Consumer] SUCCESS: String matched!" << std::endl;
            }
            else
            {
                std::cout << "[Consumer] FAILURE: String mismatch!" << std::endl;
                return 1;
            }
        }
        else
        {
            std::cerr << "[Consumer] FAILURE: RootObject not found!" << std::endl;
            return 1;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Consumer] Exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[Consumer] Finished." << std::endl;

    return 0;
}
