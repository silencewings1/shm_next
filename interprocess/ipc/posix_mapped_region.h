#pragma once

#include "posix_shared_memory_object.h"
#include <sys/mman.h>
#include <unistd.h>
#include <cstddef>
#include <errno.h>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace interprocess
{

enum map_options_t
{
    default_map_options = 0
};

class MappedRegion
{
public:
    MappedRegion(const MappedRegion&) = delete;
    MappedRegion& operator=(const MappedRegion&) = delete;

    MappedRegion() noexcept : base(nullptr), size(0), page_offset(0), mode(read_only)
    {
    }

    template <class MemoryMappable>
    MappedRegion(const MemoryMappable& mapping, mode_t open_mode, std::size_t offset = 0,
                 std::size_t mapping_size = 0, const void* address = nullptr,
                 map_options_t map_options = default_map_options)
        : base(nullptr), size(0), page_offset(0), mode(open_mode)
    {

        int fd = mapping.get_fd();
        if (fd == -1)
        {
            throw std::runtime_error("Invalid mapping object.");
        }

        std::size_t page_size = get_page_size();
        std::size_t page_offset = offset % page_size;
        std::size_t aligned_offset = offset - page_offset;

        if (mapping_size == 0)
        {
            const std::size_t available_size = mapping.get_size();
            if (available_size <= offset)
            {
                throw std::runtime_error("Offset is larger than mapping size.");
            }
            mapping_size = available_size - offset;
        }

        int prot = 0;
        int flags = MAP_SHARED;

        if (open_mode == read_only)
        {
            prot |= PROT_READ;
        }
        else if (open_mode == read_write)
        {
            prot |= (PROT_READ | PROT_WRITE);
        }
        else
        {
            throw std::runtime_error("Unsupported mode.");
        }

        void* mapped_base = mmap(const_cast<void*>(address), mapping_size + page_offset, prot,
                                 flags, fd, aligned_offset);

        if (mapped_base == MAP_FAILED)
        {
            throw std::system_error(errno, std::system_category(), "mmap failed");
        }

        base = static_cast<char*>(mapped_base) + page_offset;
        size = mapping_size;
        this->page_offset = page_offset;
    }

    MappedRegion(MappedRegion&& other) noexcept
        : base(other.base), size(other.size), page_offset(other.page_offset), mode(other.mode)
    {
        other.base = nullptr;
        other.size = 0;
        other.page_offset = 0;
        other.mode = read_only;
    }

    MappedRegion& operator=(MappedRegion&& other) noexcept
    {
        if (this != &other)
        {
            priv_close();
            base = other.base;
            size = other.size;
            page_offset = other.page_offset;
            mode = other.mode;

            other.base = nullptr;
            other.size = 0;
            other.page_offset = 0;
            other.mode = read_only;
        }
        return *this;
    }

    ~MappedRegion()
    {
        priv_close();
    }

    std::size_t get_size() const noexcept
    {
        return size;
    }

    void* get_address() const noexcept
    {
        return base;
    }

    mode_t get_mode() const noexcept
    {
        return mode;
    }

    bool flush(std::size_t mapping_offset = 0, std::size_t numbytes = 0, bool async = true)
    {
        if (!base)
            return false;
        if (mapping_offset >= size)
            return false;

        if (numbytes == 0)
        {
            numbytes = size - mapping_offset;
        }

        if (numbytes > size - mapping_offset)
            return false;

        void* addr = static_cast<char*>(priv_map_address()) + mapping_offset;
        numbytes += page_offset;

        int flags = async ? MS_ASYNC : MS_SYNC;
        return msync(addr, numbytes, flags) == 0;
    }

    static std::size_t get_page_size() noexcept
    {
        long page_size = sysconf(_SC_PAGESIZE);
        return page_size > 0 ? static_cast<std::size_t>(page_size) : 4096;
    }

    void swap(MappedRegion& other) noexcept
    {
        std::swap(base, other.base);
        std::swap(size, other.size);
        std::swap(page_offset, other.page_offset);
        std::swap(mode, other.mode);
    }

private:
    void* priv_map_address() const noexcept
    {
        return static_cast<char*>(base) - page_offset;
    }

    std::size_t priv_map_size() const noexcept
    {
        return size + page_offset;
    }

    void priv_close() noexcept
    {
        if (base)
        {
            munmap(priv_map_address(), priv_map_size());
            base = nullptr;
        }
    }

    void* base;
    std::size_t size;
    std::size_t page_offset;
    mode_t mode;
};

} // namespace interprocess
