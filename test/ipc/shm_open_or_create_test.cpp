#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/ipc/posix_mapped_region.h"
#include "interprocess/ipc/posix_shared_memory_object.h"
#include <cstring>
#include <iostream>
#include <string>

using namespace interprocess;

int main()
{
    const char* shm_name = "test_shm_open_or_create";
    const char* corrupted_shm_name = "test_shm_corrupted_state";
    const std::size_t shm_size = 1024 * 64;

    ManagedSharedMemory::remove(shm_name);
    ManagedSharedMemory::remove(corrupted_shm_name);

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

        SharedMemoryObject raw(create_only, corrupted_shm_name, interprocess::mode_t::read_write,
                               0666);
        raw.truncate(shm_size);
        MappedRegion raw_region(raw, interprocess::mode_t::read_write);
        std::memset(raw_region.get_address(), 0, raw_region.get_size());
        SharedMemoryManager::mark_corrupted(raw_region.get_address());

        bool rejected_corrupted_segment = false;
        try
        {
            ManagedSharedMemory corrupted(open_only, corrupted_shm_name);
        }
        catch (const std::runtime_error& e)
        {
            rejected_corrupted_segment =
                std::string(e.what()).find("corrupted") != std::string::npos;
        }

        if (!rejected_corrupted_segment)
        {
            std::cerr << "[open_or_create] Failed to reject corrupted segment" << std::endl;
            ManagedSharedMemory::remove(shm_name);
            ManagedSharedMemory::remove(corrupted_shm_name);
            return 1;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[open_or_create] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name);
        ManagedSharedMemory::remove(corrupted_shm_name);
        return 1;
    }

    ManagedSharedMemory::remove(shm_name);
    ManagedSharedMemory::remove(corrupted_shm_name);
    std::cout << "[open_or_create] SUCCESS" << std::endl;
    return 0;
}
