#include "interprocess/ipc/managed_shared_memory.h"
#include <chrono>
#include <cstdint>
#include <cstdlib>
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

struct SharedBenchState
{
    uint32_t start;
    uint32_t stop;
    uint64_t reader_ops[16];
    uint64_t writer_ops;
    uint32_t failures;
};

uint32_t load_u32(const uint32_t* ptr)
{
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void store_u32(uint32_t* ptr, uint32_t value)
{
    __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

void add_u64(uint64_t* ptr, uint64_t value)
{
    (void)__atomic_add_fetch(ptr, value, __ATOMIC_ACQ_REL);
}

void add_failure(SharedBenchState* state)
{
    (void)__atomic_add_fetch(&state->failures, 1u, __ATOMIC_ACQ_REL);
}

void wait_for_start(SharedBenchState* state)
{
    while (load_u32(&state->start) == 0)
    {
        std::this_thread::yield();
    }
}

void reader_process(const std::string& name, SharedBenchState* state, int reader_index)
{
    try
    {
        ManagedSharedMemory segment(open_only, name.c_str());
        wait_for_start(state);
        uint64_t local_ops = 0;
        while (load_u32(&state->stop) == 0)
        {
            int* value = segment.find<int>("value");
            if (value == nullptr || *value < 0)
            {
                add_failure(state);
                break;
            }
            (void)segment.get_free_memory();
            if ((local_ops & 0x0Fu) == 0)
            {
                SharedMemoryAllocatorStats stats = segment.get_allocator_stats();
                if (!stats.sane)
                {
                    add_failure(state);
                    break;
                }
            }
            ++local_ops;
        }
        add_u64(&state->reader_ops[reader_index], local_ops);
        _exit(0);
    }
    catch (...)
    {
        add_failure(state);
        _exit(2);
    }
}

void writer_process(const std::string& name, SharedBenchState* state)
{
    try
    {
        ManagedSharedMemory segment(open_only, name.c_str());
        SharedMemoryManager* manager = segment.get_segment_manager();
        wait_for_start(state);
        uint64_t local_ops = 0;
        while (load_u32(&state->stop) == 0)
        {
            void* ptr = manager->allocate(64);
            manager->deallocate(ptr);
            ++local_ops;
        }
        add_u64(&state->writer_ops, local_ops);
        _exit(0);
    }
    catch (...)
    {
        add_failure(state);
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

bool run_case(const std::string& name, int readers, bool mixed, int duration_ms)
{
    void* memory = mmap(nullptr, sizeof(SharedBenchState), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
    {
        std::cerr << "[Manager RWLock Benchmark] mmap failed" << std::endl;
        return false;
    }
    auto* state = new (memory) SharedBenchState{};

    std::vector<pid_t> pids;
    for (int i = 0; i < readers; ++i)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            reader_process(name, state, i);
        }
        if (pid < 0)
        {
            std::cerr << "[Manager RWLock Benchmark] reader fork failed" << std::endl;
            store_u32(&state->stop, 1);
            wait_all(pids);
            munmap(memory, sizeof(SharedBenchState));
            return false;
        }
        pids.push_back(pid);
    }

    if (mixed)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            writer_process(name, state);
        }
        if (pid < 0)
        {
            std::cerr << "[Manager RWLock Benchmark] writer fork failed" << std::endl;
            store_u32(&state->stop, 1);
            wait_all(pids);
            munmap(memory, sizeof(SharedBenchState));
            return false;
        }
        pids.push_back(pid);
    }

    const auto begin = std::chrono::steady_clock::now();
    store_u32(&state->start, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    store_u32(&state->stop, 1);
    bool ok = wait_all(pids);
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - begin).count();

    uint64_t reader_total = 0;
    for (int i = 0; i < readers; ++i)
    {
        reader_total += __atomic_load_n(&state->reader_ops[i], __ATOMIC_ACQUIRE);
    }
    uint64_t writer_total = __atomic_load_n(&state->writer_ops, __ATOMIC_ACQUIRE);
    uint32_t failures = load_u32(&state->failures);

    std::cout << "mode=" << (SHM_NEXT_ENABLE_MANAGER_SHARED_MUTEX ? "shared_mutex" : "robust_mutex")
              << ",scenario=" << (mixed ? "mixed" : "reader_only")
              << ",readers=" << readers
              << ",seconds=" << seconds
              << ",reader_ops=" << reader_total
              << ",reader_ops_per_sec=" << static_cast<uint64_t>(reader_total / seconds)
              << ",writer_ops=" << writer_total
              << ",writer_ops_per_sec=" << static_cast<uint64_t>(writer_total / seconds)
              << ",failures=" << failures << std::endl;

    munmap(memory, sizeof(SharedBenchState));
    return ok && failures == 0;
}

} // namespace

int main(int argc, char** argv)
{
    int duration_ms = 1000;
    if (argc > 1)
    {
        duration_ms = std::max(100, std::atoi(argv[1]));
    }

    const std::string name = "shm_mgr_rwbench_" + std::to_string(getpid());
    ManagedSharedMemory::remove(name.c_str());
    try
    {
        ManagedSharedMemory segment(create_only, name.c_str(), 4 * 1024 * 1024);
        int* value = segment.construct<int>("value", 42);
        if (value == nullptr)
        {
            std::cerr << "[Manager RWLock Benchmark] failed to construct value" << std::endl;
            return 1;
        }

        bool ok = true;
        for (int readers : {1, 2, 4, 8})
        {
            ok = run_case(name, readers, false, duration_ms) && ok;
        }
        for (int readers : {1, 2, 4, 8})
        {
            ok = run_case(name, readers, true, duration_ms) && ok;
        }

        ManagedSharedMemory::remove(name.c_str());
        return ok ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Manager RWLock Benchmark] exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(name.c_str());
        return 1;
    }
}
