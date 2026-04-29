#include "../interprocess/ipc/managed_shared_memory.h"
#include "../interprocess/sync/posix_mutex.h"
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace interprocess;

namespace
{

struct SharedRoot
{
    InterprocessMutex mutex;
    int committed_value;
    int partial_value;
    int recovery_count;

    SharedRoot() : committed_value(0), partial_value(0), recovery_count(0)
    {
    }
};

bool require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "[Crash Recovery Complex] " << message << std::endl;
        return false;
    }
    return true;
}

int run_crashing_writer(const char* shm_name)
{
    try
    {
        ManagedSharedMemory segment(open_only, shm_name);
        SharedRoot* root = segment.find<SharedRoot>("RootObject");
        if (root == nullptr)
        {
            return 2;
        }

        root->mutex.lock();
        root->partial_value = 12345;
        root->committed_value = -1;
        _exit(0);
    }
    catch (...)
    {
    }

    return 3;
}

} // namespace

int main()
{
    if (!InterprocessMutex::robust_supported())
    {
        std::cout << "[Crash Recovery Complex] SKIPPED: robust pthread mutex is not supported on "
                     "this platform"
                  << std::endl;
        return 0;
    }

    const std::string shm_name = "shmcrash_" + std::to_string(getpid());

    ManagedSharedMemory::remove(shm_name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, shm_name.c_str(), 64 * 1024);
        SharedRoot* root = segment.construct<SharedRoot>("RootObject");
        if (!require(root != nullptr, "failed to construct root"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        pid_t pid = fork();
        if (pid == -1)
        {
            std::cerr << "[Crash Recovery Complex] fork failed" << std::endl;
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }
        if (pid == 0)
        {
            _exit(run_crashing_writer(shm_name.c_str()));
        }

        int status = 0;
        if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            std::cerr << "[Crash Recovery Complex] crashing writer failed unexpectedly"
                      << std::endl;
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        MutexLockStatus lock_status = root->mutex.lock_with_recovery_status();
        if (!require(lock_status == MutexLockStatus::owner_dead,
                     "expected owner-dead status after writer crash"))
        {
            if (lock_status == MutexLockStatus::acquired)
            {
                root->mutex.unlock();
            }
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        if (!require(root->partial_value == 12345,
                     "partial value should remain available for recovery"))
        {
            root->mutex.unlock();
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        root->committed_value = root->partial_value;
        ++root->recovery_count;
        root->mutex.mark_consistent();
        root->mutex.unlock();

        root->mutex.lock();
        bool recovered = root->committed_value == 12345 && root->recovery_count == 1;
        root->mutex.unlock();
        if (!require(recovered, "recovered values mismatch"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        if (!require(segment.get_segment_manager()->check_sanity(),
                     "manager sanity failed after crash recovery"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        segment.destroy<SharedRoot>("RootObject");
        ManagedSharedMemory::remove(shm_name.c_str());
        std::cout << "[Crash Recovery Complex] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Crash Recovery Complex] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name.c_str());
        return 1;
    }
}
