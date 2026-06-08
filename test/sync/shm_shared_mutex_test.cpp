#include "interprocess/sync/posix_shared_mutex.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <new>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

using namespace interprocess;

namespace
{

struct SharedState
{
    InterprocessSharedMutex mutex;
    int value;

    SharedState() : mutex(), value(0)
    {
    }
};

bool write_byte(int fd)
{
    const char byte = 'x';
    return write(fd, &byte, 1) == 1;
}

bool read_byte(int fd)
{
    char byte = 0;
    return read(fd, &byte, 1) == 1;
}

bool wait_for_child(pid_t pid, const char* label)
{
    int status = 0;
    if (waitpid(pid, &status, 0) == -1)
    {
        std::cerr << "[Shared Mutex Test] waitpid failed for " << label << std::endl;
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        std::cerr << "[Shared Mutex Test] child failed for " << label << std::endl;
        return false;
    }
    return true;
}

bool test_single_process_api()
{
    InterprocessSharedMutex mutex;
    if (mutex.native_handle() == nullptr)
    {
        std::cerr << "[Shared Mutex Test] native_handle returned null" << std::endl;
        return false;
    }

    mutex.lock_shared();
    std::atomic<bool> shared_try_result{false};
    std::thread second_reader([&] {
        shared_try_result = mutex.try_lock_shared();
        if (shared_try_result)
        {
            mutex.unlock_shared();
        }
    });
    second_reader.join();
    if (!shared_try_result)
    {
        std::cerr << "[Shared Mutex Test] second shared lock should succeed" << std::endl;
        mutex.unlock_shared();
        return false;
    }

    std::atomic<bool> writer_timed_result{true};
    std::thread blocked_writer([&] {
        writer_timed_result = mutex.try_lock_for(std::chrono::milliseconds(10));
        if (writer_timed_result)
        {
            mutex.unlock();
        }
    });
    blocked_writer.join();
    mutex.unlock_shared();
    if (writer_timed_result)
    {
        std::cerr << "[Shared Mutex Test] exclusive timed lock acquired while reader held lock"
                  << std::endl;
        return false;
    }

    if (!mutex.try_lock_for(std::chrono::milliseconds(50)))
    {
        std::cerr << "[Shared Mutex Test] exclusive timed lock failed on unlocked mutex" << std::endl;
        return false;
    }

    std::atomic<bool> reader_timed_result{true};
    std::thread blocked_reader([&] {
        reader_timed_result = mutex.try_lock_shared_for(std::chrono::milliseconds(10));
        if (reader_timed_result)
        {
            mutex.unlock_shared();
        }
    });
    blocked_reader.join();
    mutex.unlock();
    if (reader_timed_result)
    {
        std::cerr << "[Shared Mutex Test] shared timed lock acquired while writer held lock"
                  << std::endl;
        return false;
    }

    if (!mutex.try_lock_shared_until(std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(50)))
    {
        std::cerr << "[Shared Mutex Test] shared timed lock failed on unlocked mutex" << std::endl;
        return false;
    }
    mutex.unlock_shared();

    return true;
}

bool test_cross_process_api()
{
    void* memory = mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
    {
        std::cerr << "[Shared Mutex Test] mmap failed" << std::endl;
        return false;
    }

    SharedState* state = new (memory) SharedState();

    int ready_pipe[2];
    int release_pipe[2];
    if (pipe(ready_pipe) == -1 || pipe(release_pipe) == -1)
    {
        std::cerr << "[Shared Mutex Test] pipe failed" << std::endl;
        state->~SharedState();
        munmap(memory, sizeof(SharedState));
        return false;
    }

    pid_t reader_pid = fork();
    if (reader_pid == -1)
    {
        std::cerr << "[Shared Mutex Test] fork failed for reader" << std::endl;
        state->~SharedState();
        munmap(memory, sizeof(SharedState));
        return false;
    }

    if (reader_pid == 0)
    {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        try
        {
            state->mutex.lock_shared();
            if (!write_byte(ready_pipe[1]))
            {
                _exit(2);
            }
            if (!read_byte(release_pipe[0]))
            {
                _exit(3);
            }
            state->mutex.unlock_shared();
        }
        catch (...)
        {
            _exit(4);
        }
        _exit(0);
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    if (!read_byte(ready_pipe[0]))
    {
        std::cerr << "[Shared Mutex Test] reader child did not become ready" << std::endl;
        write_byte(release_pipe[1]);
        wait_for_child(reader_pid, "reader cleanup");
        state->~SharedState();
        munmap(memory, sizeof(SharedState));
        return false;
    }

    if (state->mutex.try_lock_for(std::chrono::milliseconds(25)))
    {
        std::cerr << "[Shared Mutex Test] parent acquired exclusive lock while child held shared"
                  << std::endl;
        state->mutex.unlock();
        write_byte(release_pipe[1]);
        wait_for_child(reader_pid, "reader cleanup");
        state->~SharedState();
        munmap(memory, sizeof(SharedState));
        return false;
    }

    if (!write_byte(release_pipe[1]) || !wait_for_child(reader_pid, "reader"))
    {
        state->~SharedState();
        munmap(memory, sizeof(SharedState));
        return false;
    }

    pid_t writer_pid = fork();
    if (writer_pid == -1)
    {
        std::cerr << "[Shared Mutex Test] fork failed for writer" << std::endl;
        state->~SharedState();
        munmap(memory, sizeof(SharedState));
        return false;
    }

    if (writer_pid == 0)
    {
        try
        {
            state->mutex.lock();
            state->value = 42;
            state->mutex.unlock();
        }
        catch (...)
        {
            _exit(5);
        }
        _exit(0);
    }

    if (!wait_for_child(writer_pid, "writer"))
    {
        state->~SharedState();
        munmap(memory, sizeof(SharedState));
        return false;
    }

    state->mutex.lock_shared();
    int observed = state->value;
    state->mutex.unlock_shared();
    if (observed != 42)
    {
        std::cerr << "[Shared Mutex Test] shared value mismatch after writer child" << std::endl;
        state->~SharedState();
        munmap(memory, sizeof(SharedState));
        return false;
    }

    state->~SharedState();
    munmap(memory, sizeof(SharedState));
    return true;
}

} // namespace

int main()
{
    if (!InterprocessSharedMutex::process_shared_supported())
    {
        std::cout << "[Shared Mutex Test] SKIPPED: process-shared pthread rwlock is not supported "
                     "on this platform"
                  << std::endl;
        return 0;
    }

    try
    {
        if (!test_single_process_api() || !test_cross_process_api())
        {
            return 1;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Shared Mutex Test] Exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "[Shared Mutex Test] SUCCESS" << std::endl;
    return 0;
}
