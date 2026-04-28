#include "../interprocess/container/shared_memory_map.h"
#include "../interprocess/ipc/managed_shared_memory.h"
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace interprocess;

namespace
{

using Clock = std::chrono::steady_clock;

template <typename Func>
double measure_ms(Func&& func)
{
    auto start = Clock::now();
    func();
    auto finish = Clock::now();
    return std::chrono::duration<double, std::milli>(finish - start).count();
}

void print_metric(const std::string& name, double milliseconds, std::size_t operations)
{
    const double ns_per_op = milliseconds * 1000000.0 / static_cast<double>(operations);
    std::cout << std::left << std::setw(28) << name << std::right << std::setw(10) << std::fixed
              << std::setprecision(3) << milliseconds << " ms  " << std::setw(10)
              << std::setprecision(1) << ns_per_op << " ns/op" << std::endl;
}

} // namespace

int main()
{
    const char* shm_name = "test_shm_benchmark";
    const std::size_t shm_size = 8 * 1024 * 1024;
    const std::size_t allocation_count = 20000;
    const std::size_t map_count = 15000;

    ManagedSharedMemory::remove(shm_name);

    try
    {
        ManagedSharedMemory segment(create_only, shm_name, shm_size);
        SharedMemoryManager* manager = segment.get_segment_manager();

        std::vector<void*> allocations;
        allocations.reserve(allocation_count);

        double allocation_ms = measure_ms([&] {
            for (std::size_t i = 0; i < allocation_count; ++i)
            {
                allocations.push_back(manager->allocate(64, alignof(std::max_align_t)));
            }
            for (void* ptr : allocations)
            {
                manager->deallocate(ptr);
            }
            allocations.clear();
        });

        std::vector<void*> batch(allocation_count, nullptr);
        double batch_ms = measure_ms([&] {
            manager->allocate_many(32, allocation_count, alignof(std::max_align_t), batch.data());
            manager->deallocate_many(batch.data(), allocation_count);
        });

        using IntMap = SharedMemoryMap<int, int>;
        using IntMapAllocator = SharedMemoryAllocator<std::pair<const int, int>>;
        IntMap* map = segment.construct<IntMap>("BenchmarkMap", IntMapAllocator(manager));

        double map_insert_ms = measure_ms([&] {
            for (std::size_t i = 0; i < map_count; ++i)
            {
                map->emplace(static_cast<int>(i), static_cast<int>(i * 2));
            }
        });

        double map_find_ms = measure_ms([&] {
            volatile int sink = 0;
            for (std::size_t i = 0; i < map_count; ++i)
            {
                auto it = map->find(static_cast<int>(i));
                if (it != map->end())
                {
                    sink += it->second;
                }
            }
        });

        double map_erase_ms = measure_ms([&] {
            for (std::size_t i = 0; i < map_count; ++i)
            {
                map->erase(static_cast<int>(i));
            }
            map->shrink_to_fit();
        });

        segment.destroy<IntMap>("BenchmarkMap");

        print_metric("allocate/free 64B", allocation_ms, allocation_count * 2);
        print_metric("allocate_many/free_many 32B", batch_ms, allocation_count * 2);
        print_metric("map emplace", map_insert_ms, map_count);
        print_metric("map find", map_find_ms, map_count);
        print_metric("map erase+shrink", map_erase_ms, map_count);
        std::cout << "free memory: " << manager->get_free_memory() << " bytes" << std::endl;

        ManagedSharedMemory::remove(shm_name);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Benchmark] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name);
        return 1;
    }
}
