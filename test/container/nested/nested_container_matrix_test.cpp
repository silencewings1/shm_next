#include "interprocess/container/shared_memory_hash_map.h"
#include "interprocess/container/shared_memory_list.h"
#include "interprocess/container/shared_memory_map.h"
#include "interprocess/container/shared_memory_string.h"
#include "interprocess/container/shared_memory_vector.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include <iostream>
#include <string>
#include <unistd.h>

using namespace interprocess;

namespace
{

using ShmString = SharedMemoryString;
using IntList = SharedMemoryList<int>;
using ListVector = SharedMemoryVector<IntList>;
using StringList = SharedMemoryList<ShmString>;
using HashToStringList = SharedMemoryHashMap<ShmString, StringList>;
using IntStringHash = SharedMemoryHashMap<int, ShmString>;
using HashList = SharedMemoryList<IntStringHash>;
using HashToIntList = SharedMemoryHashMap<int, IntList>;
using MapToHash = SharedMemoryMap<ShmString, HashToIntList>;

struct RootObject
{
    ListVector vector_of_lists;
    HashToStringList hash_to_list;
    HashList list_of_hashes;
    MapToHash map_to_hash;

    RootObject(const SharedMemoryAllocator<IntList>& vector_allocator,
               const SharedMemoryAllocator<std::pair<const ShmString, StringList>>& hash_allocator,
               const SharedMemoryAllocator<IntStringHash>& hash_list_allocator,
               const SharedMemoryAllocator<std::pair<const ShmString, HashToIntList>>& map_allocator)
        : vector_of_lists(vector_allocator), hash_to_list(hash_allocator),
          list_of_hashes(hash_list_allocator), map_to_hash(map_allocator)
    {
    }
};

bool require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "[Nested Container Matrix] " << message << std::endl;
        return false;
    }
    return true;
}

} // namespace

int main()
{
    const std::string shm_name = "shmnestmatrix_" + std::to_string(getpid());
    ManagedSharedMemory::remove(shm_name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, shm_name.c_str(), 1024 * 1024);

        auto char_alloc = segment.get_allocator<char>();
        auto int_list_alloc = segment.get_allocator<int>();
        auto string_list_alloc = segment.get_allocator<ShmString>();
        auto int_string_hash_alloc = segment.get_allocator<std::pair<const int, ShmString>>();
        auto hash_to_int_list_alloc = segment.get_allocator<std::pair<const int, IntList>>();

        RootObject* root = segment.construct<RootObject>(
            "RootObject", segment.get_allocator<IntList>(),
            segment.get_allocator<std::pair<const ShmString, StringList>>(),
            segment.get_allocator<IntStringHash>(),
            segment.get_allocator<std::pair<const ShmString, HashToIntList>>());
        if (!require(root != nullptr, "failed to construct root"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        IntList& odds = root->vector_of_lists.emplace_back(int_list_alloc);
        odds.assign({5, 1, 3});
        odds.sort();
        IntList& evens = root->vector_of_lists.emplace_back(int_list_alloc);
        evens.assign({2, 4, 6});

        StringList colors(string_list_alloc);
        colors.emplace_back("red", char_alloc);
        colors.emplace_back("green", char_alloc);
        colors.emplace_back("blue", char_alloc);
        root->hash_to_list.emplace(ShmString("colors", char_alloc), std::move(colors));

        StringList animals(string_list_alloc);
        animals.emplace_back("cat", char_alloc);
        animals.emplace_back("dog", char_alloc);
        root->hash_to_list.emplace(ShmString("animals", char_alloc), std::move(animals));

        IntStringHash& first_hash =
            root->list_of_hashes.emplace_back(8, std::hash<int>{}, std::equal_to<int>{},
                                              int_string_hash_alloc);
        first_hash.try_emplace(1, "one", char_alloc);
        first_hash.try_emplace(2, "two", char_alloc);

        IntStringHash& second_hash =
            root->list_of_hashes.emplace_back(8, std::hash<int>{}, std::equal_to<int>{},
                                              int_string_hash_alloc);
        second_hash.try_emplace(10, "ten", char_alloc);

        HashToIntList grouped(8, std::hash<int>{}, std::equal_to<int>{}, hash_to_int_list_alloc);
        IntList low(int_list_alloc);
        low.assign({1, 2, 3});
        grouped.emplace(1, std::move(low));
        IntList high(int_list_alloc);
        high.assign({8, 9});
        grouped.emplace(2, std::move(high));
        root->map_to_hash.emplace(ShmString("ranges", char_alloc), std::move(grouped));

        if (!require(root->vector_of_lists.size() == 2 &&
                         root->vector_of_lists[0].front() == 1 &&
                         root->vector_of_lists[0].back() == 5 &&
                         root->vector_of_lists[1].size() == 3,
                     "vector<list<int>> mismatch"))
        {
            return 1;
        }

        auto colors_it = root->hash_to_list.find(ShmString("colors", char_alloc));
        if (!require(colors_it != root->hash_to_list.end() && colors_it->second.size() == 3 &&
                         colors_it->second.front() == "red" &&
                         colors_it->second.back() == "blue",
                     "hash<string, list<string>> mismatch"))
        {
            return 1;
        }

        if (!require(root->list_of_hashes.size() == 2 &&
                         root->list_of_hashes.front().at(2) == "two" &&
                         root->list_of_hashes.back().at(10) == "ten",
                     "list<hash<int,string>> mismatch"))
        {
            return 1;
        }

        auto ranges_it = root->map_to_hash.find(ShmString("ranges", char_alloc));
        if (!require(ranges_it != root->map_to_hash.end() && ranges_it->second.size() == 2 &&
                         ranges_it->second.at(1).front() == 1 &&
                         ranges_it->second.at(2).back() == 9,
                     "map<string, hash<int, list<int>>> mismatch"))
        {
            return 1;
        }

        if (!require(segment.get_segment_manager()->check_sanity(),
                     "manager sanity failed after nested matrix"))
        {
            return 1;
        }

        segment.destroy<RootObject>("RootObject");
        ManagedSharedMemory::remove(shm_name.c_str());
        std::cout << "[Nested Container Matrix] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Nested Container Matrix] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name.c_str());
        return 1;
    }
}
