#pragma once
#include "interprocess/ipc/managed_shared_memory.h"
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

inline bool nested_require(bool ok, const std::string& test_name, const std::string& message)
{
    if (!ok) std::cerr << '[' << test_name << "] " << message << std::endl;
    return ok;
}

inline bool nested_wait_ok(pid_t pid)
{
    int status = 0;
    return waitpid(pid, &status, 0) != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

template <typename Root>
int nested_busy_child(const char* shm_name)
{
    try
    {
        interprocess::ManagedSharedMemory segment(interprocess::open_only, shm_name);
        Root* root = segment.find<Root>("RootObject");
        if (!root) return 2;
        return root->mutex.try_lock() ? (root->mutex.unlock(), 3) : 0;
    }
    catch (...)
    {
        return 4;
    }
}

template <typename Root, typename Validate>
bool nested_run_lock_and_reader(const std::string& shm_name, Validate validate)
{
    {
        interprocess::ManagedSharedMemory segment(interprocess::open_only, shm_name.c_str());
        Root* root = segment.find<Root>("RootObject");
        if (!root) return false;
        root->mutex.lock();
        pid_t busy = fork();
        if (busy == 0) _exit(nested_busy_child<Root>(shm_name.c_str()));
        if (!nested_wait_ok(busy))
        {
            root->mutex.unlock();
            return false;
        }
        root->mutex.unlock();
    }

    pid_t reader = fork();
    if (reader == 0)
    {
        try
        {
            interprocess::ManagedSharedMemory segment(interprocess::open_only, shm_name.c_str());
            Root* root = segment.find<Root>("RootObject");
            if (!root) _exit(2);
            std::lock_guard<interprocess::InterprocessMutex> lock(root->mutex);
            _exit(validate(*root) ? 0 : 3);
        }
        catch (...)
        {
            _exit(4);
        }
    }
    return nested_wait_ok(reader);
}
