#pragma once

#include <cstddef>

namespace interprocess
{

struct SharedMemoryAllocatorStats
{
    std::size_t segment_size = 0;
    std::size_t managed_bytes = 0;
    std::size_t allocated_block_bytes = 0;
    std::size_t free_block_bytes = 0;
    std::size_t free_payload_bytes = 0;
    std::size_t largest_free_block = 0;
    std::size_t largest_free_payload = 0;
    std::size_t block_count = 0;
    std::size_t allocated_block_count = 0;
    std::size_t free_block_count = 0;
    std::size_t free_list_count = 0;
    bool sane = false;

    std::size_t total_allocate_calls = 0;
    std::size_t total_deallocate_calls = 0;
    std::size_t total_allocate_many_calls = 0;
    std::size_t total_deallocate_many_calls = 0;
    std::size_t total_failed_allocations = 0;
    std::size_t total_split_count = 0;
    std::size_t total_merge_count = 0;
    std::size_t total_try_expand_calls = 0;
    std::size_t total_try_expand_successes = 0;
    std::size_t total_try_expand_failures = 0;
};

} // namespace interprocess
