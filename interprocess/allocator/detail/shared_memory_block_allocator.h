#pragma once

#include "../offset_ptr.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>

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
    SharedMemoryBlockAllocator() noexcept : free_list_head(nullptr)
    {
    }

    void initialize(void* segment_base, std::size_t manager_header_size, std::size_t total_size)
    {
        const std::size_t header_size = align_up(manager_header_size);
        BlockHeader* first_block =
            reinterpret_cast<BlockHeader*>(static_cast<char*>(segment_base) + header_size);

        first_block->size = total_size - header_size;
        first_block->is_free = true;
        first_block->next_free = nullptr;

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
                return allocate_from_block(prev, curr, layout);
            }

            prev = curr;
            curr = curr->next_free.get();
        }

        throw std::bad_alloc();
    }

    void deallocate(void* ptr)
    {
        if (!ptr)
        {
            return;
        }

        AllocationHeader* allocation_header =
            reinterpret_cast<AllocationHeader*>(static_cast<char*>(ptr) - sizeof(AllocationHeader));
        BlockHeader* block = allocation_header->block.get();

        assert(!block->is_free && "Double free detected");

        block->is_free = true;
        insert_free_block(block);
        merge_with_next(block);
        merge_with_previous(block);
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

    void* allocate_from_block(BlockHeader* prev, BlockHeader* curr, const AllocationLayout& layout)
    {
        const std::size_t header_size = sizeof(BlockHeader);
        const std::size_t remaining_size = curr->size - layout.total_size;

        if (remaining_size >= header_size + alignment)
        {
            BlockHeader* new_block =
                reinterpret_cast<BlockHeader*>(reinterpret_cast<char*>(curr) + layout.total_size);
            new_block->size = remaining_size;
            new_block->is_free = true;
            new_block->next_free = curr->next_free;

            curr->size = layout.total_size;

            if (prev)
            {
                prev->next_free = new_block;
            }
            else
            {
                free_list_head = new_block;
            }
        }
        else
        {
            if (prev)
            {
                prev->next_free = curr->next_free;
            }
            else
            {
                free_list_head = curr->next_free;
            }
        }

        curr->is_free = false;
        curr->next_free = nullptr;

        AllocationHeader* allocation_header =
            reinterpret_cast<AllocationHeader*>(layout.payload - sizeof(AllocationHeader));
        allocation_header->block = curr;

        return layout.payload;
    }

    void insert_free_block(BlockHeader* block)
    {
        BlockHeader* prev = nullptr;
        BlockHeader* curr = free_list_head.get();

        while (curr != nullptr && curr < block)
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

    void merge_with_next(BlockHeader* block)
    {
        BlockHeader* curr = block->next_free.get();
        if (curr == nullptr)
        {
            return;
        }

        char* block_end = reinterpret_cast<char*>(block) + block->size;
        if (block_end == reinterpret_cast<char*>(curr))
        {
            block->size += curr->size;
            block->next_free = curr->next_free;
        }
    }

    void merge_with_previous(BlockHeader* block)
    {
        BlockHeader* prev = nullptr;
        BlockHeader* curr = free_list_head.get();

        while (curr != nullptr && curr != block)
        {
            prev = curr;
            curr = curr->next_free.get();
        }

        if (prev == nullptr)
        {
            return;
        }

        char* prev_end = reinterpret_cast<char*>(prev) + prev->size;
        if (prev_end == reinterpret_cast<char*>(block))
        {
            prev->size += block->size;
            prev->next_free = block->next_free;
        }
    }

    OffsetPtr<BlockHeader> free_list_head;
};

} // namespace interprocess::detail
