#include "../interprocess/container/shared_memory_hash_map.h"
#include "../interprocess/container/shared_memory_list.h"
#include "../interprocess/container/shared_memory_string.h"
#include "../interprocess/container/shared_memory_vector.h"
#include "../interprocess/ipc/managed_shared_memory.h"
#include <iostream>
#include <unistd.h>

using namespace interprocess;

int main()
{
    const std::string shm_name = "shm_lh_" + std::to_string(getpid());
    ManagedSharedMemory::remove(shm_name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, shm_name.c_str(), 256 * 1024);

        using IntList = SharedMemoryList<int>;
        using StringList = SharedMemoryList<SharedMemoryString>;
        using IntVector = SharedMemoryVector<int>;
        using HashMap = SharedMemoryHashMap<SharedMemoryString, IntVector>;

        IntList* list = segment.construct<IntList>("List", segment.get_allocator<int>());
        StringList* strings =
            segment.construct<StringList>("Strings", segment.get_allocator<SharedMemoryString>());
        HashMap* map = segment.construct<HashMap>(
            "Map", 8, std::hash<SharedMemoryString>{}, std::equal_to<SharedMemoryString>{},
            segment.get_allocator<std::pair<const SharedMemoryString, IntVector>>());

        auto char_alloc = segment.get_allocator<char>();
        auto int_alloc = segment.get_allocator<int>();
        auto string_alloc = segment.get_allocator<SharedMemoryString>();

        list->push_back(1);
        list->push_front(0);
        strings->emplace_back("alpha", char_alloc);
        IntVector values(int_alloc);
        values.push_back(7);
        values.push_back(8);
        map->emplace(SharedMemoryString("key", char_alloc), std::move(values));

        bool ok = list->size() == 2 && strings->front() == "alpha" &&
                  map->find(SharedMemoryString("key", char_alloc)) != map->end() &&
                  map->at(SharedMemoryString("key", char_alloc)).size() == 2;

        segment.destroy<HashMap>("Map");
        segment.destroy<StringList>("Strings");
        segment.destroy<IntList>("List");
        ManagedSharedMemory::remove(shm_name.c_str());

        if (!ok)
        {
            std::cerr << "[List Hash Compile Smoke] semantic check failed" << std::endl;
            return 1;
        }

        std::cout << "[List Hash Compile Smoke] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[List Hash Compile Smoke] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name.c_str());
        return 1;
    }
}
