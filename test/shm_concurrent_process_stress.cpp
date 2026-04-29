#include "../interprocess/container/shared_memory_map.h"
#include "../interprocess/container/shared_memory_vector.h"
#include "../interprocess/ipc/managed_shared_memory.h"
#include "../interprocess/sync/posix_mutex.h"
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace interprocess;

namespace
{

using IntVector = SharedMemoryVector<int>;
using IntVectorAllocator = SharedMemoryAllocator<int>;
using CounterMap = SharedMemoryMap<int, int>;
using CounterMapAllocator = SharedMemoryAllocator<std::pair<const int, int>>;

struct SharedRoot
{
    InterprocessMutex mutex;
    int total_updates;
    IntVector checkpoints;
    CounterMap counters;

    SharedRoot(const IntVectorAllocator& vector_allocator, const CounterMapAllocator& map_allocator)
        : total_updates(0), checkpoints(vector_allocator), counters(map_allocator)
    {
    }
};

bool require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "[Concurrent Process Stress] " << message << std::endl;
        return false;
    }
    return true;
}

int run_child(const char* shm_name, int child_index, int iterations)
{
    try
    {
        ManagedSharedMemory segment(open_only, shm_name);
        SharedRoot* root = segment.find<SharedRoot>("RootObject");
        if (root == nullptr)
        {
            return 2;
        }

        for (int i = 0; i < iterations; ++i)
        {
            {
                std::lock_guard<InterprocessMutex> lock(root->mutex);
                ++root->total_updates;
                root->counters[child_index] += 1;
                if (i % 50 == 0)
                {
                    root->checkpoints.push_back(child_index * 100000 + i);
                }
            }

            if (i % 97 == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    catch (...)
    {
        return 3;
    }

    return 0;
}

} // namespace

int main()
{
    const std::string shm_name = "shmconc_" + std::to_string(getpid());
    constexpr std::size_t shm_size = 1024 * 1024;
    constexpr int child_count = 4;
    constexpr int iterations = 400;
    constexpr int expected_checkpoints_per_child = iterations / 50;

    ManagedSharedMemory::remove(shm_name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, shm_name.c_str(), shm_size);
        SharedRoot* root =
            segment.construct<SharedRoot>("RootObject", segment.get_allocator<int>(),
                                          segment.get_allocator<std::pair<const int, int>>());
        if (!require(root != nullptr, "failed to construct shared root"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        std::vector<pid_t> children;
        for (int child_index = 0; child_index < child_count; ++child_index)
        {
            pid_t pid = fork();
            if (pid == -1)
            {
                std::cerr << "[Concurrent Process Stress] fork failed" << std::endl;
                ManagedSharedMemory::remove(shm_name.c_str());
                return 1;
            }
            if (pid == 0)
            {
                _exit(run_child(shm_name.c_str(), child_index, iterations));
            }
            children.push_back(pid);
        }

        for (pid_t pid : children)
        {
            int status = 0;
            if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            {
                std::cerr << "[Concurrent Process Stress] child failed" << std::endl;
                ManagedSharedMemory::remove(shm_name.c_str());
                return 1;
            }
        }

        {
            std::lock_guard<InterprocessMutex> lock(root->mutex);
            if (!require(root->total_updates == child_count * iterations,
                         "total update count mismatch"))
            {
                ManagedSharedMemory::remove(shm_name.c_str());
                return 1;
            }
            if (!require(root->counters.size() == static_cast<std::size_t>(child_count),
                         "counter map size mismatch"))
            {
                ManagedSharedMemory::remove(shm_name.c_str());
                return 1;
            }
            for (int child_index = 0; child_index < child_count; ++child_index)
            {
                auto it = root->counters.find(child_index);
                if (!require(it != root->counters.end() && it->second == iterations,
                             "per-child counter mismatch"))
                {
                    ManagedSharedMemory::remove(shm_name.c_str());
                    return 1;
                }
            }
            if (!require(root->checkpoints.size() ==
                             static_cast<std::size_t>(child_count * expected_checkpoints_per_child),
                         "checkpoint vector size mismatch"))
            {
                ManagedSharedMemory::remove(shm_name.c_str());
                return 1;
            }
        }

        if (!require(segment.get_segment_manager()->check_sanity(),
                     "manager sanity failed after concurrent updates"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        segment.destroy<SharedRoot>("RootObject");
        ManagedSharedMemory::remove(shm_name.c_str());
        std::cout << "[Concurrent Process Stress] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Concurrent Process Stress] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name.c_str());
        return 1;
    }
}
