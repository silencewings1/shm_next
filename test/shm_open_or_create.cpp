#include "../interprocess/ipc/managed_shared_memory.h"
#include <iostream>

using namespace interprocess;

int main()
{
    const char* shm_name = "test_shm_open_or_create";
    const std::size_t shm_size = 1024 * 64;

    ManagedSharedMemory::remove(shm_name);

    try
    {
        ManagedSharedMemory first(open_or_create, shm_name, shm_size);
        int* value = first.construct<int>("Value", 42);
        if (!value)
        {
            std::cerr << "[open_or_create] Failed to construct Value" << std::endl;
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }

        ManagedSharedMemory second(open_or_create, shm_name, shm_size);
        int* found = second.find<int>("Value");
        if (!found || *found != 42)
        {
            std::cerr << "[open_or_create] Failed to reopen existing Value" << std::endl;
            ManagedSharedMemory::remove(shm_name);
            return 1;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[open_or_create] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name);
        return 1;
    }

    ManagedSharedMemory::remove(shm_name);
    std::cout << "[open_or_create] SUCCESS" << std::endl;
    return 0;
}
