#pragma once

#include <pthread.h>
#include <system_error>

namespace interprocess
{

class InterprocessMutex
{
public:
    InterprocessMutex(const InterprocessMutex&) = delete;
    InterprocessMutex& operator=(const InterprocessMutex&) = delete;

    InterprocessMutex()
    {
        pthread_mutexattr_t attr;
        int res = pthread_mutexattr_init(&attr);
        if (res != 0)
        {
            throw std::system_error(res, std::system_category(), "Failed to init mutex attributes");
        }

        res = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (res != 0)
        {
            pthread_mutexattr_destroy(&attr);
            throw std::system_error(res, std::system_category(), "Failed to set pshared attribute");
        }

        res = pthread_mutex_init(&mutex, &attr);
        pthread_mutexattr_destroy(&attr);

        if (res != 0)
        {
            throw std::system_error(res, std::system_category(),
                                    "Failed to init interprocess mutex");
        }
    }

    ~InterprocessMutex()
    {
        pthread_mutex_destroy(&mutex);
    }

    void lock()
    {
        int res = pthread_mutex_lock(&mutex);
        if (res != 0)
        {
            throw std::system_error(res, std::system_category(), "Failed to lock mutex");
        }
    }

    bool try_lock()
    {
        int res = pthread_mutex_trylock(&mutex);
        if (res == 0)
            return true;
        if (res == EBUSY)
            return false;
        throw std::system_error(res, std::system_category(), "Failed to try_lock mutex");
    }

    void unlock()
    {
        int res = pthread_mutex_unlock(&mutex);
        if (res != 0)
        {
            throw std::system_error(res, std::system_category(), "Failed to unlock mutex");
        }
    }

    pthread_mutex_t* native_handle()
    {
        return &mutex;
    }

private:
    pthread_mutex_t mutex;
};

} // namespace interprocess
