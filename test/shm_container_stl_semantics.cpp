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

} // namespace

int main()
{
    const std::string shm_name = "shm_stl_" + std::to_string(getpid());
    ManagedSharedMemory::remove(shm_name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, shm_name.c_str(), 1024 * 1024);
        bool ok = test_vector(segment) && test_map(segment);
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
