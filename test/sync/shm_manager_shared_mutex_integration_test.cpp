#include "interprocess/ipc/managed_shared_memory.h"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

using namespace interprocess;

namespace
{

struct SharedState
{
    uint32_t start;
    uint32_t stop;
    uint32_t failures;
    uint64_t reader_ops;
    uint64_t writer_ops;
};

uint32_t load_u32(const uint32_t* ptr)
{
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void store_u32(uint32_t* ptr, uint32_t value)
{
    __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

void add_u32(uint32_t* ptr, uint32_t value)
{
    (void)__atomic_add_fetch(ptr, value, __ATOMIC_ACQ_REL);
}

void add_u64(uint64_t* ptr, uint64_t value)
{
    (void)__atomic_add_fetch(ptr, value, __ATOMIC_ACQ_REL);
}

void wait_for_start(SharedState* state)
{
    while (load_u32(&state->start) == 0)
    {
        std::this_thread::yield();
    }
}

void reader_child(const std::string& name, SharedState* state)
{
    try
    {
        ManagedSharedMemory segment(open_only, name.c_str());
        wait_for_start(state);
        uint64_t local_ops = 0;
        while (load_u32(&state->stop) == 0)
        {
            int* value = segment.find<int>("value");
            SharedMemoryAllocatorStats stats = segment.get_allocator_stats();
            if (value == nullptr || *value < 0 || !stats.sane)
            {
                add_u32(&state->failures, 1);
                break;
            }
            ++local_ops;
        }
        add_u64(&state->reader_ops, local_ops);
        _exit(0);
    }
    catch (...)
    {
        add_u32(&state->failures, 1);
        _exit(2);
    }
}

void writer_child(const std::string& name, SharedState* state)
{
    try
    {
        ManagedSharedMemory segment(open_only, name.c_str());
        SharedMemoryManager* manager = segment.get_segment_manager();
        wait_for_start(state);
        uint64_t local_ops = 0;
        while (load_u32(&state->stop) == 0)
        {
            void* ptr = manager->allocate(32);
            manager->deallocate(ptr);
            ++local_ops;
        }
        add_u64(&state->writer_ops, local_ops);
        _exit(0);
    }
    catch (...)
    {
        add_u32(&state->failures, 1);
        _exit(3);
    }
}

bool wait_all(const std::vector<pid_t>& pids)
{
    bool ok = true;
    for (pid_t pid : pids)
    {
        int status = 0;
        if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            ok = false;
        }
    }
    return ok;
}

bool require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "[Manager Shared Mutex Integration Test] " << message << std::endl;
        return false;
    }
    return true;
}

} // namespace

int main()
{
    const std::string name = "shm_mgr_shared_lock_" + std::to_string(getpid());
    ManagedSharedMemory::remove(name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, name.c_str(), 2 * 1024 * 1024);
        *segment.construct<int>("value", 42) = 42;

        void* memory = mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED)
        {
            std::cerr << "[Manager Shared Mutex Integration Test] mmap failed" << std::endl;
            return 1;
        }
        auto* state = new (memory) SharedState{};

        std::vector<pid_t> pids;
        for (int i = 0; i < 4; ++i)
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                reader_child(name, state);
            }
            if (pid < 0)
            {
                std::cerr << "[Manager Shared Mutex Integration Test] reader fork failed" << std::endl;
                return 1;
            }
            pids.push_back(pid);
        }
        pid_t writer = fork();
        if (writer == 0)
        {
            writer_child(name, state);
        }
        if (writer < 0)
        {
            std::cerr << "[Manager Shared Mutex Integration Test] writer fork failed" << std::endl;
            return 1;
        }
        pids.push_back(writer);

        store_u32(&state->start, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        store_u32(&state->stop, 1);

        bool ok = wait_all(pids);
        if (!require(ok, "child process failed") ||
            !require(load_u32(&state->failures) == 0, "reader/writer reported failure") ||
            !require(__atomic_load_n(&state->reader_ops, __ATOMIC_ACQUIRE) > 0,
                     "reader should make progress") ||
            !require(__atomic_load_n(&state->writer_ops, __ATOMIC_ACQUIRE) > 0,
                     "writer should make progress") ||
            !require(segment.get_segment_manager()->check_sanity(), "manager sanity failed"))
        {
            munmap(memory, sizeof(SharedState));
            ManagedSharedMemory::remove(name.c_str());
            return 1;
        }

        {
            ManagedSharedMemory snapshot(open_read_only, name.c_str());
            const int* value = snapshot.find_read_only<int>("value");
            if (!require(value != nullptr && *value == 42, "read-only snapshot failed") ||
                !require(snapshot.get_allocator_stats().sane, "read-only stats should be sane"))
            {
                munmap(memory, sizeof(SharedState));
                ManagedSharedMemory::remove(name.c_str());
                return 1;
            }
        }

        munmap(memory, sizeof(SharedState));
        ManagedSharedMemory::remove(name.c_str());
        std::cout << "[Manager Shared Mutex Integration Test] SUCCESS mode="
                  << (SHM_NEXT_ENABLE_MANAGER_SHARED_MUTEX ? "shared_mutex" : "robust_mutex")
                  << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Manager Shared Mutex Integration Test] exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(name.c_str());
        return 1;
    }
}
