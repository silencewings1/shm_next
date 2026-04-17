#include "../interprocess/ipc/managed_shared_memory.h"
#include "../interprocess/container/shared_memory_string.h"
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
    const std::size_t shm_size = 1024 * 64;

    // Remove old SHM if it exists
    ManagedSharedMemory::remove(shm_name);

    try
    {
        // Create managed shared memory
        ManagedSharedMemory managed_shm(create_only, shm_name, shm_size);
        std::cout << "[Producer] Shared memory created at " << managed_shm.get_segment_manager()
                  << std::endl;

        // Allocate root object
        RootObject* root =
            managed_shm.construct<RootObject>("RootObject", managed_shm.get_allocator<char>());
        if (!root)
        {
            std::cerr << "[Producer] Failed to construct RootObject!" << std::endl;
            return 1;
        }
        std::cout << "[Producer] RootObject created at " << root << std::endl;

        root->my_string = "Hello from Split Producer!";
        std::cout << "[Producer] String value set to: " << root->my_string.c_str() << std::endl;

        std::cout << "[Producer] Press Enter to cleanup and exit..." << std::endl;
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
