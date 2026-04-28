#include "../interprocess/sync/posix_mutex.h"
#include <chrono>
#include <iostream>
#include <new>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

using namespace interprocess;

namespace
{

struct SharedState
{
    InterprocessMutex mutex;
    int value;

    SharedState() : mutex(), value(0)
    {
    }
};

bool test_timed_lock_api()
{
    InterprocessMutex mutex;
    mutex.lock();

    if (mutex.try_lock_for(std::chrono::milliseconds(5)))
    {
        std::cerr << "[Robust Mutex Test] try_lock_for acquired an already locked mutex"
                  << std::endl;
        mutex.unlock();
        mutex.unlock();
        return false;
    }

    if (mutex.try_lock_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(5)))
    {
        std::cerr << "[Robust Mutex Test] try_lock_until acquired an already locked mutex"
                  << std::endl;
        mutex.unlock();
        mutex.unlock();
        return false;
    }

    if (mutex.try_lock_for_with_recovery_status(std::chrono::milliseconds(5)) !=
        MutexTryLockStatus::busy)
    {
        std::cerr << "[Robust Mutex Test] timed recovery status should report busy" << std::endl;
        mutex.unlock();
        return false;
    }

    mutex.unlock();

    if (!mutex.try_lock_for(std::chrono::milliseconds(50)))
    {
        std::cerr << "[Robust Mutex Test] try_lock_for failed on an unlocked mutex" << std::endl;
        return false;
    }
    mutex.unlock();

    if (mutex.try_lock_for_with_recovery_status(std::chrono::milliseconds(50)) !=
        MutexTryLockStatus::acquired)
    {
        std::cerr << "[Robust Mutex Test] timed recovery status should acquire unlocked mutex"
                  << std::endl;
        return false;
    }
    mutex.unlock();

    return true;
}

} // namespace

int main()
{
    if (!test_timed_lock_api())
    {
        return 1;
    }

    if (!InterprocessMutex::robust_supported())
    {
        std::cout << "[Robust Mutex Test] SKIPPED: robust pthread mutex is not supported on this "
                     "platform"
                  << std::endl;
        return 0;
    }

    void* memory = mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
    {
        std::cerr << "[Robust Mutex Test] mmap failed" << std::endl;
        return 1;
    }

    SharedState* state = new (memory) SharedState();

    pid_t pid = fork();
    if (pid == -1)
    {
        std::cerr << "[Robust Mutex Test] fork failed" << std::endl;
        state->~SharedState();
        munmap(memory, sizeof(SharedState));
        return 1;
    }

    if (pid == 0)
    {
        state->mutex.lock();
        state->value = 123;
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        std::cerr << "[Robust Mutex Test] child process failed" << std::endl;
        state->~SharedState();
        munmap(memory, sizeof(SharedState));
        return 1;
    }

    MutexLockStatus recovery_status = state->mutex.lock_with_recovery_status();
    if (recovery_status != MutexLockStatus::owner_dead)
    {
        std::cerr << "[Robust Mutex Test] expected owner-dead status" << std::endl;
        state->mutex.unlock();
        state->~SharedState();
        munmap(memory, sizeof(SharedState));
        return 1;
    }
    if (state->value != 123)
    {
        std::cerr << "[Robust Mutex Test] shared value mismatch after owner-dead recovery"
                  << std::endl;
        state->mutex.unlock();
        state->~SharedState();
        munmap(memory, sizeof(SharedState));
        return 1;
    }
    state->mutex.mark_consistent();
    state->value = 456;
    state->mutex.unlock();

    if (!state->mutex.try_lock())
    {
        std::cerr << "[Robust Mutex Test] try_lock failed after recovery" << std::endl;
        state->~SharedState();
        munmap(memory, sizeof(SharedState));
        return 1;
    }
    state->mutex.unlock();

    state->~SharedState();
    munmap(memory, sizeof(SharedState));

    std::cout << "[Robust Mutex Test] SUCCESS" << std::endl;
    return 0;
}
