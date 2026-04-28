#pragma once

#include <errno.h>
#include <pthread.h>
#include <system_error>

namespace interprocess
{

#if defined(__linux__) && defined(PTHREAD_MUTEX_ROBUST)
#define INTERPROCESS_HAS_ROBUST_MUTEX 1
#else
#define INTERPROCESS_HAS_ROBUST_MUTEX 0
#endif

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

#if INTERPROCESS_HAS_ROBUST_MUTEX
        res = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        if (res != 0)
        {
            pthread_mutexattr_destroy(&attr);
            throw std::system_error(res, std::system_category(), "Failed to set robust attribute");
        }
#endif

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
#if INTERPROCESS_HAS_ROBUST_MUTEX
        if (res == EOWNERDEAD)
        {
            make_consistent();
            return;
        }
        if (res == ENOTRECOVERABLE)
        {
            throw std::system_error(res, std::system_category(),
                                    "Interprocess mutex is not recoverable");
        }
#endif
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
#if INTERPROCESS_HAS_ROBUST_MUTEX
        if (res == EOWNERDEAD)
        {
            make_consistent();
            return true;
        }
        if (res == ENOTRECOVERABLE)
        {
            throw std::system_error(res, std::system_category(),
                                    "Interprocess mutex is not recoverable");
        }
#endif
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

    static constexpr bool robust_supported() noexcept
    {
        return INTERPROCESS_HAS_ROBUST_MUTEX != 0;
    }

private:
    void make_consistent()
    {
#if INTERPROCESS_HAS_ROBUST_MUTEX
        int res = pthread_mutex_consistent(&mutex);
        if (res != 0)
        {
            throw std::system_error(res, std::system_category(),
                                    "Failed to mark robust mutex consistent");
        }
#endif
    }

    pthread_mutex_t mutex;
};

#undef INTERPROCESS_HAS_ROBUST_MUTEX

} // namespace interprocess
