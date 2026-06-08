#include "interprocess/container/shared_memory_map.h"
#include "interprocess/error.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_mutex.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

using namespace interprocess;

namespace
{

bool require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "[Error System Test] " << message << std::endl;
        return false;
    }
    return true;
}

template <typename Func>
bool expect_errc(Func&& func, InterprocessErrc expected)
{
    try
    {
        func();
    }
    catch (const InterprocessError& e)
    {
        const std::runtime_error* as_runtime = &e;
        return as_runtime != nullptr && e.errc() == expected && e.code().category() == interprocess_error_category();
    }
    catch (...)
    {
    }
    return false;
}

} // namespace

int main()
{
    const std::string name = "shm_errsys_" + std::to_string(getpid());
    ManagedSharedMemory::remove(name.c_str());

    try
    {
        if (!require(interprocess_error_category().name() == std::string("shm_next.interprocess"),
                     "error category name mismatch"))
        {
            return 1;
        }
        if (!require(make_error_code(InterprocessErrc::read_only_violation).category() ==
                         interprocess_error_category(),
                     "make_error_code category mismatch"))
        {
            return 1;
        }
        if (!require(make_error_code(InterprocessErrc::owner_dead).category() ==
                         interprocess_error_category(),
                     "owner_dead category mismatch"))
        {
            return 1;
        }
        try
        {
            throw MutexOwnerDeadError();
        }
        catch (const std::system_error& e)
        {
            if (!require(e.code() == make_error_code(InterprocessErrc::owner_dead),
                         "MutexOwnerDeadError should use interprocess owner_dead"))
            {
                return 1;
            }
        }

        if (!require(expect_errc([&] {
                         alignas(16) unsigned char tiny[16] = {};
                         (void)SharedMemoryManager::create(tiny, sizeof(tiny));
                     },
                                 InterprocessErrc::segment_too_small),
                     "small segment should throw segment_too_small"))
        {
            ManagedSharedMemory::remove(name.c_str());
            return 1;
        }
        ManagedSharedMemory::remove(name.c_str());

        ManagedSharedMemory segment(create_only, name.c_str(), 128 * 1024);
        int* value = segment.construct<int>("value", 7);
        if (!require(value != nullptr && *value == 7, "failed to construct test value"))
        {
            return 1;
        }
        if (!require(expect_errc([&] { (void)segment.find<double>("value"); },
                                 InterprocessErrc::named_object_type_mismatch),
                     "type mismatch should throw stable error"))
        {
            return 1;
        }
        if (!require(expect_errc([&] { (void)segment.construct<int>("", 1); },
                                 InterprocessErrc::invalid_name),
                     "empty name should throw invalid_name"))
        {
            return 1;
        }

        SharedMemoryManager* manager = segment.get_segment_manager();
        void* block = manager->allocate(32);
        manager->deallocate(block);
        if (!require(expect_errc([&] { manager->deallocate(block); }, InterprocessErrc::double_free),
                     "double free should throw stable error"))
        {
            return 1;
        }
        int local = 0;
        if (!require(expect_errc([&] { manager->deallocate(&local); }, InterprocessErrc::invalid_pointer),
                     "invalid pointer should throw stable error"))
        {
            return 1;
        }

        {
            ManagedSharedMemory snapshot(open_read_only, name.c_str());
            if (!require(expect_errc([&] { (void)snapshot.get_allocator<int>(); },
                                     InterprocessErrc::read_only_violation),
                         "read-only get_allocator should throw read_only_violation"))
            {
                return 1;
            }
            if (!require(expect_errc([&] { (void)snapshot.find<int>("value"); },
                                     InterprocessErrc::read_only_violation),
                         "read-only writable find should throw read_only_violation"))
            {
                return 1;
            }
        }

        ManagedSharedMemory::remove(name.c_str());
        std::cout << "[Error System Test] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Error System Test] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(name.c_str());
        return 1;
    }
}
