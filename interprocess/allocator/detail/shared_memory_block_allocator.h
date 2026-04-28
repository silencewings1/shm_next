#pragma once

#include "../offset_ptr.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>

namespace interprocess::detail
{

struct alignas(16) BlockHeader
{
    std::size_t size;
    bool is_free;
    char padding[7];
    OffsetPtr<BlockHeader> next_free;
    char padding2[8];
};

static_assert(sizeof(BlockHeader) % 16 == 0, "BlockHeader size must be a multiple of 16");

struct AllocationHeader
{
    OffsetPtr<BlockHeader> block;
    std::size_t requested_size;
};

class SharedMemoryBlockAllocator
{
    static constexpr std::size_t alignment = 16;

    struct AllocationLayout
    {
        char* payload;
        std::size_t total_size;
    };

public:
    SharedMemoryBlockAllocator() noexcept
        : segment_base(nullptr), segment_size(0), first_block(nullptr), free_list_head(nullptr)
    {
    }

    void initialize(void* segment_base, std::size_t manager_header_size, std::size_t total_size)
    {
        const std::size_t header_size = align_up(manager_header_size);
        if (total_size < header_size + sizeof(BlockHeader))
        {
            throw std::runtime_error("Shared memory block allocator segment is too small.");
        }

        BlockHeader* first_block =
            reinterpret_cast<BlockHeader*>(static_cast<char*>(segment_base) + header_size);

        first_block->size = total_size - header_size;
        first_block->is_free = true;
        first_block->next_free = nullptr;

        this->segment_base = segment_base;
        this->segment_size = total_size;
        this->first_block = first_block;
        free_list_head = first_block;
    }

    void* allocate(std::size_t size, std::size_t requested_alignment = alignment)
    {
        if (size == 0)
        {
            return nullptr;
        }

        const std::size_t payload_alignment = normalize_alignment(requested_alignment);

        BlockHeader* prev = nullptr;
        BlockHeader* curr = free_list_head.get();

        while (curr != nullptr)
        {
            const AllocationLayout layout = make_layout(curr, size, payload_alignment);
            if (curr->is_free && curr->size >= layout.total_size)
            {
                return allocate_from_block(prev, curr, layout, size);
            }

            prev = curr;
            curr = curr->next_free.get();
        }

        throw std::bad_alloc();
    }

    void allocate_many(std::size_t size, std::size_t count, std::size_t requested_alignment,
                       void** out)
    {
        std::size_t allocated = 0;
        try
        {
            for (; allocated < count; ++allocated)
            {
                out[allocated] = allocate(size, requested_alignment);
            }
        }
        catch (...)
        {
            while (allocated > 0)
            {
                --allocated;
                deallocate(out[allocated]);
                out[allocated] = nullptr;
            }
            throw;
        }
    }

    void deallocate_many(void* const* ptrs, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            deallocate(ptrs[i]);
        }
    }

    bool try_expand(void* ptr, std::size_t new_size, std::size_t requested_alignment = alignment)
    {
        if (ptr == nullptr || new_size == 0 || !has_allocation_header(ptr))
        {
            return false;
        }

        AllocationHeader* allocation_header =
            reinterpret_cast<AllocationHeader*>(static_cast<char*>(ptr) - sizeof(AllocationHeader));
        BlockHeader* block = allocation_header->block.get();
        if (!is_valid_payload(ptr, block) || block->is_free)
        {
            return false;
        }

        const std::size_t payload_alignment = normalize_alignment(requested_alignment);
        AllocationLayout expanded_layout = make_layout(block, new_size, payload_alignment);
        if (expanded_layout.payload != ptr)
        {
            return false;
        }

        if (expanded_layout.total_size <= block->size)
        {
            allocation_header->requested_size = new_size;
            return true;
        }

        BlockHeader* next = physical_next(block);
        if (next == nullptr || !next->is_free)
        {
            return false;
        }

        const std::size_t combined_size = block->size + next->size;
        if (combined_size < expanded_layout.total_size)
        {
            return false;
        }

        remove_free_block(next);
        block->size = combined_size;
        split_allocated_block(block, expanded_layout.total_size);
        allocation_header->requested_size = new_size;
        return true;
    }

    void deallocate(void* ptr)
    {
        if (!ptr)
        {
            return;
        }

        if (!has_allocation_header(ptr))
        {
            throw std::runtime_error("Invalid shared memory deallocation pointer");
        }

        AllocationHeader* allocation_header =
            reinterpret_cast<AllocationHeader*>(static_cast<char*>(ptr) - sizeof(AllocationHeader));
        BlockHeader* block = allocation_header->block.get();

        if (!is_valid_payload(ptr, block))
        {
            throw std::runtime_error("Invalid shared memory deallocation pointer");
        }

        assert(!block->is_free && "Double free detected");
        if (block->is_free)
        {
            throw std::runtime_error("Double free detected");
        }

        deallocate_block(block);
    }

    std::size_t get_free_memory() const noexcept
    {
        std::size_t free_mem = 0;
        BlockHeader* curr = free_list_head.get();
        while (curr != nullptr)
        {
            free_mem += (curr->size - sizeof(BlockHeader));
            curr = curr->next_free.get();
        }
        return free_mem;
    }

    bool owns(const void* ptr) const noexcept
    {
        return ptr != nullptr && is_address_in_segment(reinterpret_cast<uintptr_t>(ptr));
    }

    std::size_t allocation_size(const void* ptr) const
    {
        if (ptr == nullptr)
        {
            return 0;
        }

        if (!has_allocation_header(ptr))
        {
            throw std::runtime_error("Invalid shared memory allocation pointer");
        }

        AllocationHeader* allocation_header = reinterpret_cast<AllocationHeader*>(
            const_cast<char*>(static_cast<const char*>(ptr)) - sizeof(AllocationHeader));
        BlockHeader* block = allocation_header->block.get();
        if (!is_valid_payload(ptr, block) || block->is_free)
        {
            throw std::runtime_error("Invalid shared memory allocation pointer");
        }

        return allocation_header->requested_size;
    }

    bool check_sanity() const noexcept
    {
        if (segment_base.get() == nullptr || segment_size == 0 || first_block.get() == nullptr)
        {
            return false;
        }

        if (!is_valid_free_list())
        {
            return false;
        }

        const uintptr_t segment_end = segment_end_address();
        uintptr_t current = reinterpret_cast<uintptr_t>(first_block.get());
        std::size_t visited_blocks = 0;
        const std::size_t max_blocks = segment_size / alignment + 1;

        while (current < segment_end)
        {
            if (!is_aligned_address(current))
            {
                return false;
            }

            const BlockHeader* block = reinterpret_cast<const BlockHeader*>(current);
            if (block->size < sizeof(BlockHeader) || block->size % alignment != 0)
            {
                return false;
            }

            uintptr_t next = current + block->size;
            if (next <= current || next > segment_end)
            {
                return false;
            }

            if (block->is_free != is_in_free_list(block))
            {
                return false;
            }

            current = next;
            ++visited_blocks;
            if (visited_blocks > max_blocks)
            {
                return false;
            }
        }

        return current == segment_end;
    }

    bool all_memory_deallocated() const noexcept
    {
        BlockHeader* first = first_block.get();
        if (first == nullptr)
        {
            return false;
        }

        return first->is_free &&
               first->size == segment_end_address() - reinterpret_cast<uintptr_t>(first) &&
               first->next_free.get() == nullptr && free_list_head.get() == first;
    }

    void zero_free_memory() noexcept
    {
        BlockHeader* curr = free_list_head.get();
        while (curr != nullptr)
        {
            std::memset(reinterpret_cast<char*>(curr) + sizeof(BlockHeader), 0,
                        curr->size - sizeof(BlockHeader));
            curr = curr->next_free.get();
        }
    }

private:
    static std::size_t align_up(std::size_t value, std::size_t align = alignment) noexcept
    {
        return (value + align - 1) & ~(align - 1);
    }

    static uintptr_t align_up_address(uintptr_t value, std::size_t align) noexcept
    {
        return (value + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
    }

    static std::size_t normalize_alignment(std::size_t requested_alignment) noexcept
    {
        std::size_t normalized = alignment;
        while (normalized < requested_alignment)
        {
            normalized <<= 1;
        }
        return normalized;
    }

    static AllocationLayout make_layout(BlockHeader* block, std::size_t size,
                                        std::size_t payload_alignment) noexcept
    {
        char* block_begin = reinterpret_cast<char*>(block);
        uintptr_t payload_start =
            align_up_address(reinterpret_cast<uintptr_t>(block_begin) + sizeof(BlockHeader) +
                                 sizeof(AllocationHeader),
                             payload_alignment);
        std::size_t used_size = align_up(static_cast<std::size_t>(
            (reinterpret_cast<char*>(payload_start) + size) - block_begin));

        return {reinterpret_cast<char*>(payload_start), used_size};
    }

    void* allocate_from_block(BlockHeader* prev, BlockHeader* curr, const AllocationLayout& layout,
                              std::size_t requested_size)
    {
        remove_free_block(prev, curr);
        split_allocated_block(curr, layout.total_size);

        curr->is_free = false;
        curr->next_free = nullptr;

        AllocationHeader* allocation_header =
            reinterpret_cast<AllocationHeader*>(layout.payload - sizeof(AllocationHeader));
        allocation_header->block = curr;
        allocation_header->requested_size = requested_size;

        return layout.payload;
    }

    void insert_free_block(BlockHeader* block)
    {
        BlockHeader* prev = nullptr;
        BlockHeader* curr = free_list_head.get();

        while (curr != nullptr &&
               (curr->size < block->size || (curr->size == block->size && curr < block)))
        {
            prev = curr;
            curr = curr->next_free.get();
        }

        block->next_free = curr;

        if (prev)
        {
            prev->next_free = block;
        }
        else
        {
            free_list_head = block;
        }
    }

    void remove_free_block(BlockHeader* target)
    {
        BlockHeader* prev = nullptr;
        BlockHeader* curr = free_list_head.get();
        while (curr != nullptr)
        {
            if (curr == target)
            {
                remove_free_block(prev, curr);
                return;
            }
            prev = curr;
            curr = curr->next_free.get();
        }
    }

    void remove_free_block(BlockHeader* prev, BlockHeader* target)
    {
        if (prev != nullptr)
        {
            prev->next_free = target->next_free;
        }
        else
        {
            free_list_head = target->next_free;
        }
        target->next_free = nullptr;
    }

    void split_allocated_block(BlockHeader* block, std::size_t used_size)
    {
        const std::size_t remaining_size = block->size - used_size;
        if (remaining_size < sizeof(BlockHeader) + alignment)
        {
            return;
        }

        BlockHeader* new_block =
            reinterpret_cast<BlockHeader*>(reinterpret_cast<char*>(block) + used_size);
        new_block->size = remaining_size;
        new_block->is_free = true;
        new_block->next_free = nullptr;

        block->size = used_size;
        insert_free_block(new_block);
    }

    void deallocate_block(BlockHeader* block)
    {
        block->is_free = true;
        block->next_free = nullptr;

        BlockHeader* next = physical_next(block);
        if (next != nullptr && next->is_free)
        {
            remove_free_block(next);
            block->size += next->size;
        }

        BlockHeader* prev = physical_previous(block);
        if (prev != nullptr && prev->is_free)
        {
            remove_free_block(prev);
            prev->size += block->size;
            block = prev;
        }

        block->is_free = true;
        block->next_free = nullptr;
        insert_free_block(block);
    }

    BlockHeader* physical_next(BlockHeader* block) const noexcept
    {
        uintptr_t next_address = reinterpret_cast<uintptr_t>(block) + block->size;
        if (next_address >= segment_end_address())
        {
            return nullptr;
        }
        return reinterpret_cast<BlockHeader*>(next_address);
    }

    BlockHeader* physical_previous(BlockHeader* block) const noexcept
    {
        uintptr_t target = reinterpret_cast<uintptr_t>(block);
        uintptr_t current = reinterpret_cast<uintptr_t>(first_block.get());
        BlockHeader* prev = nullptr;
        std::size_t visited_blocks = 0;
        const std::size_t max_blocks = segment_size / alignment + 1;

        while (current < target)
        {
            BlockHeader* curr = reinterpret_cast<BlockHeader*>(current);
            if (curr->size < sizeof(BlockHeader) || curr->size % alignment != 0)
            {
                return nullptr;
            }

            uintptr_t next = current + curr->size;
            if (next <= current || next > target)
            {
                return nullptr;
            }

            prev = curr;
            current = next;
            ++visited_blocks;
            if (visited_blocks > max_blocks)
            {
                return nullptr;
            }
        }

        return current == target ? prev : nullptr;
    }

    uintptr_t segment_begin_address() const noexcept
    {
        return reinterpret_cast<uintptr_t>(segment_base.get());
    }

    uintptr_t segment_end_address() const noexcept
    {
        return segment_begin_address() + segment_size;
    }

    static bool is_aligned_address(uintptr_t value) noexcept
    {
        return value % alignment == 0;
    }

    bool is_address_in_segment(uintptr_t value) const noexcept
    {
        return value >= segment_begin_address() && value < segment_end_address();
    }

    bool is_range_in_segment(uintptr_t begin, std::size_t size) const noexcept
    {
        return begin >= segment_begin_address() && begin <= segment_end_address() &&
               size <= segment_end_address() - begin;
    }

    bool is_block_in_chain(const BlockHeader* target) const noexcept
    {
        const uintptr_t target_address = reinterpret_cast<uintptr_t>(target);
        uintptr_t current = reinterpret_cast<uintptr_t>(first_block.get());
        const uintptr_t segment_end = segment_end_address();
        std::size_t visited_blocks = 0;
        const std::size_t max_blocks = segment_size / alignment + 1;

        while (current < segment_end)
        {
            if (current == target_address)
            {
                return true;
            }

            const BlockHeader* block = reinterpret_cast<const BlockHeader*>(current);
            if (block->size < sizeof(BlockHeader) || block->size % alignment != 0)
            {
                return false;
            }

            uintptr_t next = current + block->size;
            if (next <= current || next > segment_end)
            {
                return false;
            }

            current = next;
            ++visited_blocks;
            if (visited_blocks > max_blocks)
            {
                return false;
            }
        }

        return false;
    }

    bool is_in_free_list(const BlockHeader* target) const noexcept
    {
        const BlockHeader* curr = free_list_head.get();
        std::size_t visited_blocks = 0;
        const std::size_t max_blocks = segment_size / alignment + 1;

        while (curr != nullptr)
        {
            if (curr == target)
            {
                return true;
            }
            curr = curr->next_free.get();
            ++visited_blocks;
            if (visited_blocks > max_blocks)
            {
                return false;
            }
        }

        return false;
    }

    bool is_valid_free_list() const noexcept
    {
        const std::size_t max_blocks = segment_size / alignment + 1;
        std::size_t visited_blocks = 0;

        const BlockHeader* curr = free_list_head.get();
        while (curr != nullptr)
        {
            uintptr_t current_address = reinterpret_cast<uintptr_t>(curr);
            if (!is_address_in_segment(current_address) || !is_aligned_address(current_address) ||
                !curr->is_free || !is_block_in_chain(curr))
            {
                return false;
            }

            curr = curr->next_free.get();
            ++visited_blocks;
            if (visited_blocks > max_blocks)
            {
                return false;
            }
        }

        return true;
    }

    bool has_allocation_header(const void* ptr) const noexcept
    {
        if (!owns(ptr))
        {
            return false;
        }

        uintptr_t payload_address = reinterpret_cast<uintptr_t>(ptr);
        if (payload_address < sizeof(AllocationHeader))
        {
            return false;
        }

        return is_range_in_segment(payload_address - sizeof(AllocationHeader),
                                   sizeof(AllocationHeader));
    }

    bool is_valid_payload(const void* ptr, const BlockHeader* block) const noexcept
    {
        uintptr_t payload_address = reinterpret_cast<uintptr_t>(ptr);
        if (!has_allocation_header(ptr) || block == nullptr)
        {
            return false;
        }

        uintptr_t block_address = reinterpret_cast<uintptr_t>(block);
        if (!is_address_in_segment(block_address) || !is_aligned_address(block_address) ||
            !is_block_in_chain(block))
        {
            return false;
        }

        uintptr_t block_payload_begin =
            block_address + sizeof(BlockHeader) + sizeof(AllocationHeader);
        uintptr_t block_end = block_address + block->size;
        const AllocationHeader* allocation_header =
            reinterpret_cast<const AllocationHeader*>(payload_address - sizeof(AllocationHeader));
        return payload_address >= block_payload_begin && payload_address < block_end &&
               allocation_header->requested_size <= block_end - payload_address;
    }

    OffsetPtr<void> segment_base;
    std::size_t segment_size;
    OffsetPtr<BlockHeader> first_block;
    OffsetPtr<BlockHeader> free_list_head;
};

} // namespace interprocess::detail
