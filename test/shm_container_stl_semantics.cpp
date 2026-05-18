#include "../interprocess/container/shared_memory_hash_map.h"
#include "../interprocess/container/shared_memory_list.h"
#include "../interprocess/container/shared_memory_map.h"
#include "../interprocess/container/shared_memory_vector.h"
#include "../interprocess/ipc/managed_shared_memory.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

using namespace interprocess;

namespace
{

bool require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "[Container STL Semantics] " << message << std::endl;
        return false;
    }
    return true;
}

struct NonDefaultMapped
{
    int value;

    NonDefaultMapped() = delete;

    explicit NonDefaultMapped(int input) : value(input)
    {
        if (input == 99)
        {
            throw std::runtime_error("constructor should not run for duplicate try_emplace");
        }
    }

    NonDefaultMapped(const NonDefaultMapped&) = default;
    NonDefaultMapped(NonDefaultMapped&&) noexcept = default;
    NonDefaultMapped& operator=(const NonDefaultMapped&) = default;
    NonDefaultMapped& operator=(NonDefaultMapped&&) noexcept = default;
};

struct ThrowingLess
{
    static bool throw_on_thirteen;

    bool operator()(int lhs, int rhs) const
    {
        if (throw_on_thirteen && (lhs == 13 || rhs == 13))
        {
            throw std::runtime_error("comparison rejected key 13");
        }
        return lhs < rhs;
    }
};

bool ThrowingLess::throw_on_thirteen = false;

bool test_vector(ManagedSharedMemory& segment)
{
    SharedMemoryManager* manager = segment.get_segment_manager();
    using IntVector = SharedMemoryVector<int>;
    using IntVectorAllocator = SharedMemoryAllocator<int>;

    IntVector* vector = segment.construct<IntVector>("Vector", IntVectorAllocator(manager));
    if (!require(vector != nullptr, "failed to construct vector"))
    {
        return false;
    }

    if (!require(vector->empty() && vector->data() == nullptr, "new vector should be empty"))
    {
        return false;
    }

    vector->resize(3, 7);
    if (!require(vector->size() == 3 && vector->front() == 7 && vector->back() == 7,
                 "resize(count, value) should fill new elements"))
    {
        return false;
    }

    vector->resize(5);
    if (!require(vector->size() == 5 && (*vector)[3] == 0 && vector->at(4) == 0,
                 "resize(count) should value-initialize appended ints"))
    {
        return false;
    }

    bool at_threw = false;
    try
    {
        (void)vector->at(5);
    }
    catch (const std::out_of_range&)
    {
        at_threw = true;
    }
    if (!require(at_threw, "at() should throw for out-of-range access"))
    {
        return false;
    }

    vector->push_back(11);
    const IntVector* const_vector = vector;
    int forward_sum = 0;
    for (IntVector::const_iterator it = const_vector->cbegin(); it != const_vector->cend(); ++it)
    {
        forward_sum += *it;
    }
    if (!require(forward_sum == 32, "const iterators should traverse all values"))
    {
        return false;
    }

    if (!require(*vector->rbegin() == 11 && *const_vector->crbegin() == 11,
                 "reverse iterators should start from the last element"))
    {
        return false;
    }

    IntVector::iterator erased_next = vector->erase(vector->begin() + 1);
    if (!require(vector->size() == 5 && *erased_next == 7,
                 "erase should remove one element and return the next iterator"))
    {
        return false;
    }

    vector->resize(2);
    if (!require(vector->size() == 2 && vector->front() == 7 && vector->back() == 7,
                 "shrinking resize should preserve prefix values"))
    {
        return false;
    }

    if (!require(segment.destroy<IntVector>("Vector"), "failed to destroy vector"))
    {
        return false;
    }
    return true;
}

bool test_list(ManagedSharedMemory& segment)
{
    SharedMemoryManager* manager = segment.get_segment_manager();
    using IntList = SharedMemoryList<int>;
    using IntListAllocator = SharedMemoryAllocator<int>;

    IntList* list = segment.construct<IntList>("List", IntListAllocator(manager));
    if (!require(list != nullptr, "failed to construct list"))
    {
        return false;
    }

    if (!require(list->empty() && list->begin() == list->end(), "new list should be empty"))
    {
        return false;
    }

    list->push_back(1);
    list->push_back(2);
    list->push_back(3);
    list->push_front(0);
    if (!require(list->size() == 4 && list->front() == 0 && list->back() == 3,
                 "push_front/push_back should update list ends"))
    {
        return false;
    }

    const std::array<int, 4> expected = {0, 1, 2, 3};
    std::size_t index = 0;
    for (IntList::const_iterator it = list->cbegin(); it != list->cend(); ++it, ++index)
    {
        if (!require(index < expected.size() && *it == expected[index],
                     "list iteration order mismatch"))
        {
            return false;
        }
    }
    if (!require(index == expected.size(), "list iteration count mismatch"))
    {
        return false;
    }

    if (!require(*list->rbegin() == 3, "reverse iterators should start from the tail"))
    {
        return false;
    }

    IntList::iterator erased_next = list->erase(++list->begin());
    if (!require(list->size() == 3 && *erased_next == 2,
                 "erase should remove one node and return the next iterator"))
    {
        return false;
    }

    list->insert(list->begin(), 2, -1);
    const std::array<int, 5> expected_after_insert = {-1, -1, 0, 2, 3};
    index = 0;
    for (IntList::const_iterator it = list->cbegin(); it != list->cend(); ++it, ++index)
    {
        if (!require(index < expected_after_insert.size() && *it == expected_after_insert[index],
                     "fill insert should preserve order"))
        {
            return false;
        }
    }

    IntList* other = segment.construct<IntList>("OtherList", IntListAllocator(manager));
    if (!require(other != nullptr, "failed to construct second list"))
    {
        return false;
    }
    list->swap(*other);
    if (!require(list->empty() && other->size() == expected_after_insert.size() &&
                     other->front() == -1 && other->back() == 3,
                 "swap should exchange empty and non-empty list contents"))
    {
        return false;
    }

    other->resize(2);
    if (!require(other->size() == 2 && other->front() == -1 && other->back() == -1,
                 "resize shrink should preserve prefix elements"))
    {
        return false;
    }

    other->assign({3, 1, 2, 2, 1});
    other->sort();
    if (!require(other->front() == 1 && other->back() == 3,
                 "sort should reorder list ascending"))
    {
        return false;
    }

    other->unique();
    if (!require(other->size() == 3, "unique should collapse adjacent duplicates"))
    {
        return false;
    }

    other->push_back(4);
    other->push_back(5);
    other->reverse();
    if (!require(other->front() == 5 && other->back() == 1,
                 "reverse should invert list order"))
    {
        return false;
    }

    IntList* splice_source = segment.construct<IntList>("SpliceList", IntListAllocator(manager));
    if (!require(splice_source != nullptr, "failed to construct splice source list"))
    {
        return false;
    }
    splice_source->assign({8, 6, 7});
    other->splice(other->begin(), *splice_source);
    if (!require(splice_source->empty() && other->front() == 8 && other->size() == 8,
                 "splice should move nodes without copying"))
    {
        return false;
    }

    IntList* merge_source = segment.construct<IntList>("MergeList", IntListAllocator(manager));
    if (!require(merge_source != nullptr, "failed to construct merge source list"))
    {
        return false;
    }
    other->assign({1, 3, 5});
    merge_source->assign({2, 4, 6});
    other->merge(*merge_source);
    if (!require(merge_source->empty() && other->size() == 6 && other->front() == 1 &&
                     other->back() == 6,
                 "merge should combine sorted lists"))
    {
        return false;
    }

    if (!require(segment.destroy<IntList>("MergeList"), "failed to destroy merge source list") ||
        !require(segment.destroy<IntList>("SpliceList"), "failed to destroy splice source list") ||
        !require(segment.destroy<IntList>("OtherList"), "failed to destroy second list") ||
        !require(segment.destroy<IntList>("List"), "failed to destroy list"))
    {
        return false;
    }
    return true;
}

bool test_map(ManagedSharedMemory& segment)
{
    SharedMemoryManager* manager = segment.get_segment_manager();
    using IntMap = SharedMemoryMap<int, int>;
    using IntMapAllocator = SharedMemoryAllocator<std::pair<const int, int>>;

    IntMap* map = segment.construct<IntMap>("Map", IntMapAllocator(manager));
    if (!require(map != nullptr, "failed to construct map"))
    {
        return false;
    }

    const std::array<int, 12> keys = {5, 1, 9, 3, 7, 0, 2, 4, 6, 8, 10, 11};
    for (int key : keys)
    {
        auto result = map->try_emplace(key, key * 10);
        if (!require(result.second && result.first->second == key * 10,
                     "try_emplace insert failed"))
        {
            return false;
        }
    }

    auto duplicate = map->try_emplace(5, 5000);
    if (!require(!duplicate.second && duplicate.first->second == 50,
                 "try_emplace should not overwrite an existing value"))
    {
        return false;
    }

    auto assigned = map->insert_or_assign(5, 55);
    if (!require(!assigned.second && map->at(5) == 55, "insert_or_assign should update value"))
    {
        return false;
    }

    int expected_key = 0;
    for (IntMap::const_iterator it = map->cbegin(); it != map->cend(); ++it, ++expected_key)
    {
        if (!require(it->first == expected_key, "map iteration should be sorted by key"))
        {
            return false;
        }
    }
    if (!require(expected_key == 12, "map iteration count mismatch"))
    {
        return false;
    }

    IntMap::iterator lower = map->lower_bound(6);
    IntMap::iterator upper = map->upper_bound(6);
    if (!require(lower != map->end() && lower->first == 6 && upper != map->end() &&
                     upper->first == 7,
                 "lower_bound/upper_bound returned unexpected iterators"))
    {
        return false;
    }

    const std::size_t allocations_before_reuse = map->node_pool_allocations();
    map->erase(3);
    if (!require(map->cached_node_count() == 1, "erase should cache removed node"))
    {
        return false;
    }
    map->try_emplace(13, 130);
    if (!require(map->node_pool_hits() == 1 &&
                     map->node_pool_allocations() == allocations_before_reuse &&
                     map->cached_node_count() == 0,
                 "try_emplace should reuse cached nodes"))
    {
        return false;
    }

    using NonDefaultMap = SharedMemoryMap<int, NonDefaultMapped>;
    using NonDefaultMapAllocator = SharedMemoryAllocator<std::pair<const int, NonDefaultMapped>>;
    NonDefaultMap* non_default_map =
        segment.construct<NonDefaultMap>("NonDefaultMap", NonDefaultMapAllocator(manager));
    if (!require(non_default_map != nullptr, "failed to construct non-default map"))
    {
        return false;
    }
    non_default_map->try_emplace(1, 10);
    bool duplicate_construct_threw = false;
    try
    {
        auto result = non_default_map->try_emplace(1, 99);
        if (!require(!result.second && result.first->second.value == 10,
                     "duplicate try_emplace should preserve existing non-default value"))
        {
            return false;
        }
    }
    catch (...)
    {
        duplicate_construct_threw = true;
    }
    if (!require(!duplicate_construct_threw,
                 "duplicate try_emplace should not construct mapped value"))
    {
        return false;
    }

    using ThrowingMap = SharedMemoryMap<int, int, ThrowingLess>;
    using ThrowingMapAllocator = SharedMemoryAllocator<std::pair<const int, int>>;
    ThrowingMap* throwing_map = segment.construct<ThrowingMap>("ThrowingMap", ThrowingLess{},
                                                               ThrowingMapAllocator(manager));
    if (!require(throwing_map != nullptr, "failed to construct throwing comparator map"))
    {
        return false;
    }
    throwing_map->try_emplace(10, 100);
    const std::size_t throwing_allocations = throwing_map->node_pool_allocations();
    ThrowingLess::throw_on_thirteen = true;
    bool compare_threw = false;
    try
    {
        throwing_map->try_emplace(13, 130);
    }
    catch (const std::runtime_error&)
    {
        compare_threw = true;
    }
    ThrowingLess::throw_on_thirteen = false;
    if (!require(compare_threw && throwing_map->size() == 1 &&
                     throwing_map->node_pool_allocations() == throwing_allocations &&
                     throwing_map->cached_node_count() == 0,
                 "comparison failure should leave map size and node pool unchanged"))
    {
        return false;
    }

    IntMap* other_map = segment.construct<IntMap>("OtherMap", IntMapAllocator(manager));
    if (!require(other_map != nullptr, "failed to construct second map"))
    {
        return false;
    }
    other_map->try_emplace(100, 1000);
    map->swap(*other_map);
    if (!require(map->size() == 1 && map->contains(100) && other_map->contains(13),
                 "swap should exchange map contents"))
    {
        return false;
    }

    if (!require(segment.destroy<IntMap>("OtherMap"), "failed to destroy second map") ||
        !require(segment.destroy<ThrowingMap>("ThrowingMap"),
                 "failed to destroy throwing comparator map") ||
        !require(segment.destroy<NonDefaultMap>("NonDefaultMap"),
                 "failed to destroy non-default map") ||
        !require(segment.destroy<IntMap>("Map"), "failed to destroy map"))
    {
        return false;
    }
    return true;
}

bool test_hash_map(ManagedSharedMemory& segment)
{
    SharedMemoryManager* manager = segment.get_segment_manager();
    using IntHashMap = SharedMemoryHashMap<int, int>;
    using IntHashMapAllocator = SharedMemoryAllocator<std::pair<const int, int>>;

    IntHashMap* map = segment.construct<IntHashMap>("HashMap", 4, std::hash<int>{},
                                                    std::equal_to<int>{},
                                                    IntHashMapAllocator(manager));
    if (!require(map != nullptr, "failed to construct hash map"))
    {
        return false;
    }

    for (int key = 0; key < 6; ++key)
    {
        auto result = map->try_emplace(key, key * 100);
        if (!require(result.second && result.first->second == key * 100,
                     "hash map try_emplace insert failed"))
        {
            return false;
        }
    }

    auto duplicate = map->try_emplace(3, 9999);
    if (!require(!duplicate.second && duplicate.first->second == 300,
                 "hash map duplicate try_emplace should preserve existing value"))
    {
        return false;
    }

    map->insert_or_assign(3, 333);
    if (!require(map->at(3) == 333 && map->contains(5) && map->count(42) == 0,
                 "hash map insert_or_assign/contains/count mismatch"))
    {
        return false;
    }

    bool at_threw = false;
    try
    {
        (void)map->at(42);
    }
    catch (const std::out_of_range&)
    {
        at_threw = true;
    }
    if (!require(at_threw, "hash map at() should throw for missing key"))
    {
        return false;
    }

    const std::size_t bucket_index = map->bucket(2);
    bool found_in_bucket = false;
    for (auto it = map->begin(bucket_index); it != map->end(bucket_index); ++it)
    {
        if (it->first == 2)
        {
            found_in_bucket = true;
            break;
        }
    }
    if (!require(found_in_bucket, "local iterators should traverse bucket members"))
    {
        return false;
    }

    const std::size_t allocations_before_reuse = map->node_pool_allocations();
    map->erase(1);
    if (!require(map->cached_node_count() == 1, "hash map erase should cache removed node"))
    {
        return false;
    }
    map->try_emplace(100, 10000);
    if (!require(map->node_pool_hits() == 1 &&
                     map->node_pool_allocations() == allocations_before_reuse &&
                     map->cached_node_count() == 0,
                 "hash map insertion should reuse cached nodes"))
    {
        return false;
    }

    map->max_load_factor(0.25f);
    if (!require(map->bucket_count() >= 24 && map->load_factor() <= map->max_load_factor(),
                 "lowering max_load_factor should trigger a sufficient rehash"))
    {
        return false;
    }

    map->rehash(1);
    if (!require(map->load_factor() <= map->max_load_factor(),
                 "rehash should not leave load factor above the configured limit"))
    {
        return false;
    }

    IntHashMap* range_map = segment.construct<IntHashMap>(
        "HashMapRange", IntHashMap::allocator_type(manager));
    if (!require(range_map != nullptr, "failed to construct range hash map"))
    {
        return false;
    }
    std::array<std::pair<const int, int>, 3> init = {
        std::pair<const int, int>(7, 70), std::pair<const int, int>(8, 80),
        std::pair<const int, int>(9, 90)};
    range_map->insert(init.begin(), init.end());
    range_map->insert({std::pair<const int, int>(10, 100), std::pair<const int, int>(11, 110)});
    if (!require(range_map->size() == 5 && range_map->at(11) == 110,
                 "range/initializer_list insert should populate hash map"))
    {
        return false;
    }

    if (!require(segment.destroy<IntHashMap>("HashMapRange"),
                 "failed to destroy range hash map") ||
        !require(segment.destroy<IntHashMap>("HashMap"), "failed to destroy hash map"))
    {
        return false;
    }
    return true;
}

} // namespace

int main()
{
    const std::string shm_name = "shm_stl_" + std::to_string(getpid());
    ManagedSharedMemory::remove(shm_name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, shm_name.c_str(), 1024 * 1024);
        bool ok =
            test_vector(segment) && test_list(segment) && test_map(segment) && test_hash_map(segment);
        ManagedSharedMemory::remove(shm_name.c_str());
        if (!ok)
        {
            return 1;
        }
        std::cout << "[Container STL Semantics] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Container STL Semantics] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name.c_str());
        return 1;
    }
}
