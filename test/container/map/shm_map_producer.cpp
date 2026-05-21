#include "interprocess/container/shared_memory_map.h"
#include "interprocess/container/shared_memory_string.h"
#include "interprocess/container/shared_memory_vector.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_mutex.h"
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

using namespace interprocess;

using ShmString = SharedMemoryString;
using ShmStringAllocator = SharedMemoryAllocator<char>;
using ShmStringVector = SharedMemoryVector<ShmString>;
using ShmStringVectorAllocator = SharedMemoryAllocator<ShmString>;
using ProductMap = SharedMemoryMap<ShmString, int>;
using ProductMapAllocator = SharedMemoryAllocator<std::pair<const ShmString, int>>;
using CounterMap = SharedMemoryMap<int, int>;
using CounterMapAllocator = SharedMemoryAllocator<std::pair<const int, int>>;

struct ProductRecord
{
    ShmString name;
    int quantity;
    double price;
    ShmStringVector tags;

    ProductRecord(const ShmStringAllocator& char_allocator,
                  const ShmStringVectorAllocator& tag_allocator)
        : name(char_allocator), quantity(0), price(0.0), tags(tag_allocator)
    {
    }

    ProductRecord(const char* product_name, int product_quantity, double product_price,
                  const ShmStringAllocator& char_allocator,
                  const ShmStringVectorAllocator& tag_allocator)
        : name(product_name, char_allocator), quantity(product_quantity), price(product_price),
          tags(tag_allocator)
    {
    }
};

using ProductRecordMap = SharedMemoryMap<ShmString, ProductRecord>;
using ProductRecordMapAllocator = SharedMemoryAllocator<std::pair<const ShmString, ProductRecord>>;

struct SharedRoot
{
    InterprocessMutex mutex;
    ProductRecordMap products;
    CounterMap counters;

    SharedRoot(const ProductRecordMapAllocator& product_allocator,
               const CounterMapAllocator& counter_allocator)
        : products(product_allocator), counters(counter_allocator)
    {
    }
};

static ProductRecord build_record(const char* key, const char* display_name, int quantity,
                                  double price, const ShmStringAllocator& char_allocator,
                                  const ShmStringVectorAllocator& tag_allocator)
{
    ProductRecord record(display_name, quantity, price, char_allocator, tag_allocator);
    record.tags.emplace_back(key, char_allocator);
    record.tags.emplace_back("inventory", char_allocator);
    return record;
}

int main()
{
    const char* shm_name = "test_shm_map_catalog";
    const std::size_t shm_size = 1024 * 512;

    ManagedSharedMemory::remove(shm_name);

    try
    {
        ManagedSharedMemory managed(create_only, shm_name, shm_size);

        auto char_allocator = managed.get_allocator<char>();
        auto string_allocator = managed.get_allocator<ShmString>();
        auto product_allocator = managed.get_allocator<std::pair<const ShmString, ProductRecord>>();
        auto counter_allocator = managed.get_allocator<std::pair<const int, int>>();

        SharedRoot* root =
            managed.construct<SharedRoot>("RootObject", product_allocator, counter_allocator);
        if (!root)
        {
            std::cerr << "[Producer] Failed to construct RootObject" << std::endl;
            return 1;
        }

        {
            std::lock_guard<InterprocessMutex> lock(root->mutex);

            root->products.emplace(ShmString("sku-200", char_allocator),
                                   build_record("sku-200", "Mechanical Keyboard", 15, 89.5,
                                                char_allocator, string_allocator));
            root->products.emplace(ShmString("sku-050", char_allocator),
                                   build_record("sku-050", "USB-C Hub", 42, 39.9, char_allocator,
                                                string_allocator));
            root->products.emplace(ShmString("sku-120", char_allocator),
                                   build_record("sku-120", "Laptop Stand", 23, 54.0, char_allocator,
                                                string_allocator));
            root->products.emplace(ShmString("sku-310", char_allocator),
                                   build_record("sku-310", "Noise Cancelling Headphones", 8,
                                                199.0, char_allocator, string_allocator));

            ProductRecordMap::iterator record_it =
                root->products.find(ShmString("sku-120", char_allocator));
            if (record_it == root->products.end())
            {
                std::cerr << "[Producer] Failed to find sku-120 for update" << std::endl;
                return 1;
            }

            record_it->second.quantity = 27;
            record_it->second.price = 57.5;
            record_it->second.tags.emplace_back("updated", char_allocator);
            record_it->second.tags.emplace_back("featured", char_allocator);

            root->products.erase(ShmString("sku-050", char_allocator));
            root->products.emplace(ShmString("sku-075", char_allocator),
                                   build_record("sku-075", "Portable SSD", 31, 129.0,
                                                char_allocator, string_allocator));
            root->products.emplace(ShmString("sku-420", char_allocator),
                                   build_record("sku-420", "4K Monitor", 6, 329.0, char_allocator,
                                                string_allocator));

            root->counters[7] += 3;
            root->counters[2] = 11;
            root->counters[7] += 4;
            root->counters[9] = 1;

            std::cout << "[Producer] Product iteration order:" << std::endl;
            for (ProductRecordMap::iterator it = root->products.begin(); it != root->products.end();
                 ++it)
            {
                std::cout << "  " << it->first << " => " << it->second.name << ", qty="
                          << it->second.quantity << ", price=" << it->second.price << std::endl;
            }

            std::cout << "[Producer] Counters:" << std::endl;
            for (CounterMap::iterator it = root->counters.begin(); it != root->counters.end(); ++it)
            {
                std::cout << "  " << it->first << " => " << it->second << std::endl;
            }
        }

        std::cout << "[Producer] Shared map ready. Waiting 10 seconds for consumer..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(10));
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
