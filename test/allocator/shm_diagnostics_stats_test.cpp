#include "interprocess/diagnostics.h"
#include "interprocess/error.h"
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
        std::cerr << "[Diagnostics Stats Test] " << message << std::endl;
        return false;
    }
    return true;
}

} // namespace

int main()
{
    const std::string name = "shm_diag_" + std::to_string(getpid());
    ManagedSharedMemory::remove(name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, name.c_str(), 256 * 1024);
        SharedMemoryAllocatorStats initial = segment.get_allocator_stats();
        if (!require(initial.sane, "initial stats should be sane") ||
            !require(initial.segment_size == segment.get_size(), "segment size mismatch") ||
            !require(initial.free_block_count == 1, "initial free block count should be one") ||
            !require(initial.free_list_count == 1, "initial free list count should be one") ||
            !require(initial.largest_free_payload == initial.free_payload_bytes,
                     "initial largest free should equal total free") ||
            !require(initial.total_allocate_calls == 0, "initial allocate calls should be zero") ||
            !require(initial.total_deallocate_calls == 0, "initial deallocate calls should be zero") ||
            !require(initial.total_failed_allocations == 0, "initial failed allocations should be zero") ||
            !require(initial.total_split_count == 0, "initial split count should be zero") ||
            !require(initial.total_merge_count == 0, "initial merge count should be zero") ||
            !require(initial.total_try_expand_calls == 0, "initial try_expand calls should be zero"))
        {
            return 1;
        }

        SharedMemoryManager* manager = segment.get_segment_manager();
        void* a = manager->allocate(64);
        void* b = manager->allocate(512);
        void* c = manager->allocate(128);
        SharedMemoryAllocatorStats after_alloc = segment.get_allocator_stats();
        if (!require(after_alloc.sane, "stats after allocation should be sane") ||
            !require(after_alloc.allocated_block_count >= initial.allocated_block_count + 3,
                     "allocated block count should increase") ||
            !require(after_alloc.free_payload_bytes < initial.free_payload_bytes,
                     "free payload should decrease") ||
            !require(after_alloc.total_allocate_calls >= initial.total_allocate_calls + 3,
                     "allocate call counter should grow") ||
            !require(after_alloc.total_split_count >= initial.total_split_count + 3,
                     "split counter should grow"))
        {
            return 1;
        }

        bool expand_ok = manager->try_expand(a, 64, 16);
        bool expand_fail = manager->try_expand(const_cast<std::string*>(&name), 96, 16);
        SharedMemoryAllocatorStats after_expand = segment.get_allocator_stats();
        if (!require(expand_ok, "try_expand should succeed when allocation already fits") ||
            !require(!expand_fail, "try_expand should fail for invalid pointer") ||
            !require(after_expand.total_try_expand_calls >= 2, "try_expand call counter should grow") ||
            !require(after_expand.total_try_expand_successes >= 1,
                     "try_expand success counter should grow") ||
            !require(after_expand.total_try_expand_failures >= 1,
                     "try_expand failure counter should grow"))
        {
            return 1;
        }

        manager->deallocate(b);
        SharedMemoryAllocatorStats fragmented = segment.get_allocator_stats();
        if (!require(fragmented.sane, "fragmented stats should be sane") ||
            !require(fragmented.free_block_count >= 2, "fragmented free block count should grow") ||
            !require(fragmented.largest_free_payload < fragmented.free_payload_bytes,
                     "fragmented largest free should be less than total free"))
        {
            return 1;
        }

        void* many[2] = {};
        manager->allocate_many(32, 2, 16, many);
        manager->deallocate_many(many, 2);

        try
        {
            (void)manager->allocate(segment.get_size() * 2);
            std::cerr << "[Diagnostics Stats Test] huge allocation should fail" << std::endl;
            return 1;
        }
        catch (const InterprocessError& e)
        {
            if (!require(e.errc() == InterprocessErrc::bad_alloc,
                         "huge allocation should throw bad_alloc"))
            {
                return 1;
            }
        }

        manager->deallocate(a);
        manager->deallocate(c);
        SharedMemoryAllocatorStats cleaned = segment.get_allocator_stats();
        if (!require(cleaned.sane, "cleaned stats should be sane") ||
            !require(cleaned.free_block_count == 1, "free block count should merge to one") ||
            !require(manager->all_memory_deallocated(), "all direct allocations should be returned") ||
            !require(cleaned.total_deallocate_calls >= 5, "deallocate call counter should grow") ||
            !require(cleaned.total_allocate_many_calls >= 1,
                     "allocate_many counter should grow") ||
            !require(cleaned.total_deallocate_many_calls >= 1,
                     "deallocate_many counter should grow") ||
            !require(cleaned.total_failed_allocations >= 1,
                     "failed allocation counter should grow") ||
            !require(cleaned.total_merge_count >= 1, "merge counter should grow"))
        {
            return 1;
        }

        {
            ManagedSharedMemory snapshot(open_read_only, name.c_str());
            SharedMemoryAllocatorStats ro = snapshot.get_allocator_stats();
            if (!require(ro.sane, "read-only stats should be sane") ||
                !require(ro.free_block_count == cleaned.free_block_count,
                         "read-only stats should match writable stats") ||
                !require(ro.total_allocate_calls == cleaned.total_allocate_calls,
                         "read-only cumulative allocate stats should match") ||
                !require(ro.total_failed_allocations == cleaned.total_failed_allocations,
                         "read-only cumulative failure stats should match"))
            {
                return 1;
            }
        }

        ManagedSharedMemory::remove(name.c_str());
        std::cout << "[Diagnostics Stats Test] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Diagnostics Stats Test] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(name.c_str());
        return 1;
    }
}
