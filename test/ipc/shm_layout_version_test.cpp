#include "interprocess/error.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/ipc/posix_mapped_region.h"
#include "interprocess/ipc/posix_shared_memory_object.h"
#include <cstdint>
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
        std::cerr << "[Layout Version Test] " << message << std::endl;
        return false;
    }
    return true;
}

bool expect_error(const char* name, InterprocessErrc expected, bool read_only = false)
{
    try
    {
        if (read_only)
        {
            ManagedSharedMemory segment(open_read_only, name);
            (void)segment;
        }
        else
        {
            ManagedSharedMemory segment(open_only, name);
            (void)segment;
        }
    }
    catch (const InterprocessError& e)
    {
        return e.errc() == expected;
    }
    catch (...)
    {
    }
    return false;
}

void write_u32(void* base, std::size_t offset, uint32_t value)
{
    *reinterpret_cast<uint32_t*>(static_cast<char*>(base) + offset) = value;
}

bool mutate_header(const char* name, std::size_t offset, uint32_t value)
{
    SharedMemoryObject object(open_only, name, interprocess::mode_t::read_write);
    MappedRegion region(object, interprocess::mode_t::read_write);
    write_u32(region.get_address(), offset, value);
    region.flush();
    return true;
}

} // namespace

int main()
{
    const std::string name = "shm_layout_" + std::to_string(getpid());
    ManagedSharedMemory::remove(name.c_str());

    try
    {
        {
            ManagedSharedMemory segment(create_only, name.c_str(), 128 * 1024);
            if (!require(segment.get_layout_version() == SharedMemoryManager::current_layout_version(),
                         "new segment layout version mismatch"))
            {
                return 1;
            }
            if (!require(segment.get_segment_manager()->get_header_size() == sizeof(SharedMemoryManager),
                         "manager header size mismatch"))
            {
                return 1;
            }
        }
        {
            ManagedSharedMemory segment(open_only, name.c_str());
            if (!require(segment.get_layout_version() == SharedMemoryManager::current_layout_version(),
                         "open_only layout version mismatch"))
            {
                return 1;
            }
        }
        {
            ManagedSharedMemory segment(open_or_create, name.c_str(), 128 * 1024);
            if (!require(segment.get_layout_version() == SharedMemoryManager::current_layout_version(),
                         "open_or_create existing layout version mismatch"))
            {
                return 1;
            }
        }
        {
            ManagedSharedMemory segment(open_read_only, name.c_str());
            if (!require(segment.get_layout_version() == SharedMemoryManager::current_layout_version(),
                         "read-only layout version mismatch"))
            {
                return 1;
            }
        }

        mutate_header(name.c_str(), sizeof(uint32_t), 0x534D4D33u);
        if (!require(expect_error(name.c_str(), InterprocessErrc::magic_mismatch),
                     "magic mismatch should throw stable error"))
        {
            return 1;
        }

        ManagedSharedMemory::remove(name.c_str());
        {
            ManagedSharedMemory segment(create_only, name.c_str(), 128 * 1024);
            (void)segment;
        }
        mutate_header(name.c_str(), 2 * sizeof(uint32_t),
                      SharedMemoryManager::current_layout_version() + 1);
        if (!require(expect_error(name.c_str(), InterprocessErrc::unsupported_layout_version),
                     "unsupported layout should throw stable error"))
        {
            return 1;
        }
        if (!require(expect_error(name.c_str(), InterprocessErrc::unsupported_layout_version, true),
                     "read-only unsupported layout should throw stable error"))
        {
            return 1;
        }

        ManagedSharedMemory::remove(name.c_str());
        {
            ManagedSharedMemory segment(create_only, name.c_str(), 128 * 1024);
            (void)segment;
        }
        mutate_header(name.c_str(), 3 * sizeof(uint32_t), 16);
        if (!require(expect_error(name.c_str(), InterprocessErrc::layout_header_mismatch),
                     "header mismatch should throw stable error"))
        {
            return 1;
        }

        ManagedSharedMemory::remove(name.c_str());
        std::cout << "[Layout Version Test] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Layout Version Test] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(name.c_str());
        return 1;
    }
}
