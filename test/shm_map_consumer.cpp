#include "../interprocess/container/shared_memory_map.h"
#include "../interprocess/container/shared_memory_string.h"
#include "../interprocess/container/shared_memory_vector.h"
#include "../interprocess/ipc/managed_shared_memory.h"
#include "../interprocess/sync/posix_mutex.h"
#include <iostream>
#include <mutex>
#include <string>

using namespace interprocess;

using ShmString = SharedMemoryString;
using ShmStringAllocator = SharedMemoryAllocator<char>;
using ShmStringVector = SharedMemoryVector<ShmString>;
using ProductMap = SharedMemoryMap<ShmString, int>;
using CounterMap = SharedMemoryMap<int, int>;

struct ProductRecord
{
    ShmString name;
    int quantity;
    double price;
    ShmStringVector tags;

    ProductRecord(const ShmStringAllocator& char_allocator,
                  const SharedMemoryAllocator<ShmString>& tag_allocator)
        : name(char_allocator), quantity(0), price(0.0), tags(tag_allocator)
    {
    }
};

using ProductRecordMap = SharedMemoryMap<ShmString, ProductRecord>;

struct SharedRoot
{
    InterprocessMutex mutex;
    ProductRecordMap products;
    CounterMap counters;

    SharedRoot(const SharedMemoryAllocator<std::pair<const ShmString, ProductRecord>>& product_allocator,
               const SharedMemoryAllocator<std::pair<const int, int>>& counter_allocator)
        : products(product_allocator), counters(counter_allocator)
    {
    }
};

static bool require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "[Consumer] " << message << std::endl;
        return false;
    }
    return true;
}

int main()
{
    const char* shm_name = "test_shm_map_catalog";

    try
    {
        ManagedSharedMemory managed(open_only, shm_name);
        auto char_allocator = managed.get_allocator<char>();

        SharedRoot* root = managed.find<SharedRoot>("RootObject");
        if (!root)
        {
            std::cerr << "[Consumer] RootObject not found" << std::endl;
            return 1;
        }

        std::lock_guard<InterprocessMutex> lock(root->mutex);

        if (!require(root->products.size() == 5, "expected 5 products after producer updates"))
        {
            return 1;
        }

        if (!require(!root->products.contains(ShmString("sku-050", char_allocator)),
                     "sku-050 should have been erased"))
        {
            return 1;
        }

        ProductRecordMap::iterator updated = root->products.find(ShmString("sku-120", char_allocator));
        if (!require(updated != root->products.end(), "sku-120 should exist"))
        {
            return 1;
        }

        if (!require(updated->second.quantity == 27, "sku-120 quantity mismatch"))
        {
            return 1;
        }

        if (!require(updated->second.price == 57.5, "sku-120 price mismatch"))
        {
            return 1;
        }

        if (!require(updated->second.tags.size() == 4, "sku-120 tags count mismatch"))
        {
            return 1;
        }

        if (!require(updated->second.tags[2] == "updated", "sku-120 tag[2] mismatch"))
        {
            return 1;
        }

        if (!require(updated->second.tags[3] == "featured", "sku-120 tag[3] mismatch"))
        {
            return 1;
        }

        ProductRecordMap::iterator lower = root->products.lower_bound(ShmString("sku-100", char_allocator));
        if (!require(lower != root->products.end() && lower->first == "sku-120",
                     "lower_bound(sku-100) should point to sku-120"))
        {
            return 1;
        }

        ProductRecordMap::iterator upper = root->products.upper_bound(ShmString("sku-200", char_allocator));
        if (!require(upper != root->products.end() && upper->first == "sku-310",
                     "upper_bound(sku-200) should point to sku-310"))
        {
            return 1;
        }

        std::pair<ProductRecordMap::iterator, ProductRecordMap::iterator> range =
            root->products.equal_range(ShmString("sku-200", char_allocator));
        if (!require(range.first != root->products.end() && range.first->first == "sku-200",
                     "equal_range first mismatch for sku-200"))
        {
            return 1;
        }

        if (!require(range.second != root->products.end() && range.second->first == "sku-310",
                     "equal_range second mismatch for sku-200"))
        {
            return 1;
        }

        const char* expected_keys[] = {"sku-075", "sku-120", "sku-200", "sku-310", "sku-420"};
        std::size_t index = 0;
        for (ProductRecordMap::iterator it = root->products.begin(); it != root->products.end();
             ++it, ++index)
        {
            if (!require(index < 5 && it->first == expected_keys[index],
                         "product iteration order mismatch"))
            {
                return 1;
            }
        }

        if (!require(index == 5, "product iteration count mismatch"))
        {
            return 1;
        }

        if (!require(root->counters.at(7) == 7, "counter 7 mismatch"))
        {
            return 1;
        }

        if (!require(root->counters.at(2) == 11, "counter 2 mismatch"))
        {
            return 1;
        }

        if (!require(root->counters.count(9) == 1, "counter 9 should exist"))
        {
            return 1;
        }

        std::cout << "[Consumer] SUCCESS: SharedMemoryMap data matched expectations." << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Consumer] Exception: " << e.what() << std::endl;
        return 1;
    }
}
