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

enum class MutexLockStatus
{
    acquired,
    owner_dead
};

enum class MutexTryLockStatus
{
    acquired,
    busy,
    owner_dead
};

class MutexOwnerDeadError : public std::system_error
{
public:
    MutexOwnerDeadError()
        : std::system_error(EOWNERDEAD, std::system_category(), "Interprocess mutex owner died")
    {
    }
};

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
        if (lock_with_recovery_status() == MutexLockStatus::owner_dead)
        {
            abandon_owner_dead_lock();
            throw MutexOwnerDeadError();
        }
    }

    bool try_lock()
    {
        MutexTryLockStatus status = try_lock_with_recovery_status();
        if (status == MutexTryLockStatus::acquired)
        {
            return true;
        }
        if (status == MutexTryLockStatus::busy)
        {
            return false;
        }

        abandon_owner_dead_lock();
        throw MutexOwnerDeadError();
    }

    MutexLockStatus lock_with_recovery_status()
    {
        int res = pthread_mutex_lock(&mutex);
        if (res == 0)
        {
            return MutexLockStatus::acquired;
        }
#if INTERPROCESS_HAS_ROBUST_MUTEX
        if (res == EOWNERDEAD)
        {
            return MutexLockStatus::owner_dead;
        }
        if (res == ENOTRECOVERABLE)
        {
            throw std::system_error(res, std::system_category(),
                                    "Interprocess mutex is not recoverable");
        }
#endif
        throw std::system_error(res, std::system_category(), "Failed to lock mutex");
    }

    MutexTryLockStatus try_lock_with_recovery_status()
    {
        int res = pthread_mutex_trylock(&mutex);
        if (res == 0)
        {
            return MutexTryLockStatus::acquired;
        }
#if INTERPROCESS_HAS_ROBUST_MUTEX
        if (res == EOWNERDEAD)
        {
            return MutexTryLockStatus::owner_dead;
        }
        if (res == ENOTRECOVERABLE)
        {
            throw std::system_error(res, std::system_category(),
                                    "Interprocess mutex is not recoverable");
        }
#endif
        if (res == EBUSY)
        {
            return MutexTryLockStatus::busy;
        }
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

    void mark_consistent()
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

    static constexpr bool robust_supported() noexcept
    {
        return INTERPROCESS_HAS_ROBUST_MUTEX != 0;
    }

private:
    void abandon_owner_dead_lock()
    {
#if INTERPROCESS_HAS_ROBUST_MUTEX
        int res = pthread_mutex_unlock(&mutex);
        if (res != 0)
        {
            throw std::system_error(res, std::system_category(),
                                    "Failed to abandon owner-dead mutex");
        }
#endif
    }

    pthread_mutex_t mutex;
};

#undef INTERPROCESS_HAS_ROBUST_MUTEX

} // namespace interprocess
