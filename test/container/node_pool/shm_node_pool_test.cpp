#include "interprocess/container/shared_memory_hash_map.h"
#include "interprocess/container/shared_memory_list.h"
#include "interprocess/container/shared_memory_map.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include <iostream>
#include <string>
#include <unistd.h>

using namespace interprocess;

namespace
{

bool require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "[Node Pool Test] " << message << std::endl;
        return false;
    }
    return true;
}

template <typename Container>
bool exercise_assoc_pool(Container& c, const char* label)
{
    for (int i = 0; i < 32; ++i)
    {
        c.emplace(i, i * 10);
    }
    const auto allocations = c.node_pool_allocations();
    if (!require(allocations >= 32, label))
    {
        return false;
    }
    for (int i = 0; i < 16; ++i)
    {
        c.erase(i);
    }
    if (!require(c.cached_node_count() >= 16, "erase should cache assoc nodes"))
    {
        return false;
    }
    const auto hits_before = c.node_pool_hits();
    const auto allocations_before = c.node_pool_allocations();
    for (int i = 100; i < 116; ++i)
    {
        c.emplace(i, i * 10);
    }
    return require(c.node_pool_hits() >= hits_before + 16, "assoc insert should hit node pool") &&
           require(c.node_pool_allocations() == allocations_before,
                   "assoc insert from cache should not allocate new nodes");
}

} // namespace

int main()
{
    const std::string name = "shm_nodepool_" + std::to_string(getpid());
    ManagedSharedMemory::remove(name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, name.c_str(), 512 * 1024);

        using List = SharedMemoryList<int>;
        auto* list = segment.construct<List>("list", segment.get_allocator<int>());
        for (int i = 0; i < 32; ++i)
        {
            list->push_back(i);
        }
        const auto list_allocations = list->node_pool_allocations();
        for (int i = 0; i < 16; ++i)
        {
            list->pop_front();
        }
        if (!require(list->cached_node_count() >= 16, "list pop should cache nodes"))
        {
            return 1;
        }
        const auto list_hits = list->node_pool_hits();
        for (int i = 0; i < 16; ++i)
        {
            list->push_back(i);
        }
        if (!require(list->node_pool_hits() >= list_hits + 16, "list push should reuse cache") ||
            !require(list->node_pool_allocations() == list_allocations,
                     "list cache reuse should not allocate"))
        {
            return 1;
        }
        list->clear();
        if (!require(list->cached_node_count() >= 32, "list clear should cache nodes"))
        {
            return 1;
        }
        list->shrink_to_fit();
        if (!require(list->cached_node_count() == 0, "list shrink_to_fit should release cache"))
        {
            return 1;
        }

        using MapValue = std::pair<const int, int>;
        using Map = SharedMemoryMap<int, int>;
        auto* map = segment.construct<Map>("map", segment.get_allocator<MapValue>());
        if (!exercise_assoc_pool(*map, "map should allocate nodes"))
        {
            return 1;
        }
        map->clear();
        if (!require(map->cached_node_count() >= 32, "map clear should cache nodes"))
        {
            return 1;
        }
        map->shrink_to_fit();
        if (!require(map->cached_node_count() == 0, "map shrink_to_fit should release cache"))
        {
            return 1;
        }

        using HashMap = SharedMemoryHashMap<int, int>;
        auto* hash = segment.construct<HashMap>("hash", segment.get_allocator<MapValue>());
        if (!exercise_assoc_pool(*hash, "hash_map should allocate nodes"))
        {
            return 1;
        }
        hash->clear();
        if (!require(hash->cached_node_count() >= 32, "hash_map clear should cache nodes"))
        {
            return 1;
        }
        hash->shrink_to_fit();
        if (!require(hash->cached_node_count() == 0, "hash_map shrink_to_fit should release cache"))
        {
            return 1;
        }

        segment.destroy<List>("list");
        segment.destroy<Map>("map");
        segment.destroy<HashMap>("hash");
        if (!require(segment.get_segment_manager()->check_sanity(), "manager sanity failed"))
        {
            return 1;
        }
        if (!require(segment.get_segment_manager()->all_memory_deallocated(),
                     "all container memory should be returned"))
        {
            return 1;
        }

        ManagedSharedMemory::remove(name.c_str());
        std::cout << "[Node Pool Test] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Node Pool Test] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(name.c_str());
        return 1;
    }
}
