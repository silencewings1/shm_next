#include "interprocess/ipc/managed_shared_memory.h"
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

using namespace interprocess;

namespace
{

bool require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "[Allocator Fragmentation] " << message << std::endl;
        return false;
    }
    return true;
}

} // namespace

int main()
{
    const std::string shm_name = "shmfrag_" + std::to_string(getpid());

    ManagedSharedMemory::remove(shm_name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, shm_name.c_str(), 512 * 1024);
        SharedMemoryManager* manager = segment.get_segment_manager();

        std::vector<void*> blocks;
        blocks.reserve(256);

        const std::size_t pattern[] = {8, 17, 64, 129, 513, 2048, 4095};
        for (std::size_t i = 0; i < 256; ++i)
        {
            std::size_t size = pattern[i % (sizeof(pattern) / sizeof(pattern[0]))];
            blocks.push_back(manager->allocate(size, alignof(std::max_align_t)));
        }

        SharedMemoryAllocatorStats stats_after_alloc = manager->get_stats();
        if (!require(stats_after_alloc.sane && stats_after_alloc.allocated_block_count >= blocks.size() &&
                         stats_after_alloc.free_block_count >= 1 &&
                         stats_after_alloc.allocated_block_bytes > 0,
                     "allocator stats should scan allocated and free blocks"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }
        if (!require(manager->check_sanity(), "sanity failed after initial allocations"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        for (std::size_t i = 0; i < blocks.size(); i += 3)
        {
            manager->deallocate(blocks[i]);
            blocks[i] = nullptr;
        }

        if (!require(manager->check_sanity(), "sanity failed after sparse deallocations"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        std::vector<void*> batch(64, nullptr);
        manager->allocate_many(48, batch.size(), alignof(std::max_align_t), batch.data());
        if (!require(manager->check_sanity(), "sanity failed after allocate_many"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }
        manager->deallocate_many(batch.data(), batch.size());

        for (void* block : blocks)
        {
            if (block != nullptr)
            {
                manager->deallocate(block);
            }
        }

        if (!require(manager->check_sanity(), "sanity failed after fragmented cleanup"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        void* expandable = manager->allocate(64, alignof(std::max_align_t));
        void* adjacent = manager->allocate(512, alignof(std::max_align_t));
        manager->deallocate(adjacent);
        if (!require(manager->try_expand(expandable, 384, alignof(std::max_align_t)),
                     "try_expand should consume adjacent free space"))
        {
            manager->deallocate(expandable);
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }
        if (!require(manager->allocation_size(expandable) == 384,
                     "expanded allocation size mismatch"))
        {
            manager->deallocate(expandable);
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }
        manager->deallocate(expandable);

        manager->zero_free_memory();
        if (!require(manager->check_sanity(), "sanity failed after full cleanup"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }
        if (!require(manager->all_memory_deallocated(),
                     "all direct allocations should be returned"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }
        SharedMemoryAllocatorStats stats_after_cleanup = manager->get_stats();
        if (!require(stats_after_cleanup.sane && stats_after_cleanup.allocated_block_count == 0 &&
                         stats_after_cleanup.free_block_count == 1 &&
                         stats_after_cleanup.free_payload_bytes == manager->get_free_memory(),
                     "allocator stats should reflect fully coalesced free memory"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        ManagedSharedMemory::remove(shm_name.c_str());
        std::cout << "[Allocator Fragmentation] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Allocator Fragmentation] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name.c_str());
        return 1;
    }
}
