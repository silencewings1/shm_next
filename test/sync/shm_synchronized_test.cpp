#include "interprocess/container/shared_memory_vector.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_mutex.h"
#include "interprocess/sync/synchronized.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace interprocess;

namespace
{

using Vector = SharedMemoryVector<int>;
using SyncedVector = Synchronized<Vector, InterprocessMutex>;

bool wait_ok(pid_t pid)
{
    int status = 0;
    return waitpid(pid, &status, 0) != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool test_single_process_api()
{
    InterprocessMutex mutex;
    int value = 0;
    with_lock(mutex, [&] { value = 42; });
    if (value != 42)
    {
        std::cerr << "[Synchronized Test] free with_lock failed" << std::endl;
        return false;
    }

    Synchronized<int, InterprocessMutex> synced_int(7);
    int observed = synced_int.with_lock([](int& current) {
        current += 5;
        return current;
    });
    if (observed != 12)
    {
        std::cerr << "[Synchronized Test] member with_lock failed" << std::endl;
        return false;
    }

    try
    {
        synced_int.with_lock([](int&) { throw std::runtime_error("expected"); });
    }
    catch (const std::runtime_error&)
    {
    }

    bool relocked = synced_int.native_mutex().try_lock();
    if (relocked)
    {
        synced_int.native_mutex().unlock();
    }
    if (!relocked)
    {
        std::cerr << "[Synchronized Test] lock was not released after exception" << std::endl;
        return false;
    }

    auto locked = synced_int.lock();
    if (locked.get() != 12 || *locked != 12)
    {
        std::cerr << "[Synchronized Test] locked ref value mismatch" << std::endl;
        return false;
    }
    if (synced_int.native_mutex().try_lock())
    {
        synced_int.native_mutex().unlock();
        std::cerr << "[Synchronized Test] mutex should be held by locked ref" << std::endl;
        return false;
    }

    return true;
}

int child_read(const char* name)
{
    try
    {
        ManagedSharedMemory segment(open_only, name);
        auto* synced = segment.find<SyncedVector>("vector");
        if (!synced)
        {
            return 2;
        }
        int sum = synced->with_lock([](const Vector& values) {
            int local = 0;
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                local += values.at(i);
            }
            return local;
        });
        return sum == 45 ? 0 : 3;
    }
    catch (...)
    {
        return 4;
    }
}

bool test_cross_process_vector()
{
    const std::string name = "shm_sync_" + std::to_string(getpid());
    ManagedSharedMemory::remove(name.c_str());
    try
    {
        ManagedSharedMemory segment(create_only, name.c_str(), 1024 * 1024);
        auto allocator = segment.get_allocator<int>();
        auto* synced = segment.construct<SyncedVector>("vector", allocator);
        if (!synced)
        {
            ManagedSharedMemory::remove(name.c_str());
            return false;
        }

        synced->with_lock([](Vector& values) {
            for (int i = 0; i < 10; ++i)
            {
                values.push_back(i);
            }
        });

        pid_t pid = fork();
        if (pid == 0)
        {
            _exit(child_read(name.c_str()));
        }

        bool ok = wait_ok(pid);
        segment.destroy<SyncedVector>("vector");
        ManagedSharedMemory::remove(name.c_str());
        return ok;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Synchronized Test] exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(name.c_str());
        return false;
    }
}

} // namespace

int main()
{
    if (!test_single_process_api() || !test_cross_process_vector())
    {
        return 1;
    }

    std::cout << "[Synchronized Test] SUCCESS" << std::endl;
    return 0;
}
