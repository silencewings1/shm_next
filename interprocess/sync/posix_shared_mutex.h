#pragma once

#include <chrono>
#include <errno.h>
#include <pthread.h>
#include <system_error>
#include <thread>

namespace interprocess
{

class InterprocessSharedMutex
{
public:
    InterprocessSharedMutex(const InterprocessSharedMutex&) = delete;
    InterprocessSharedMutex& operator=(const InterprocessSharedMutex&) = delete;

    InterprocessSharedMutex()
    {
        pthread_rwlockattr_t attr;
        int res = pthread_rwlockattr_init(&attr);
        if (res != 0)
        {
            throw std::system_error(res, std::system_category(),
                                    "Failed to init rwlock attributes");
        }

        res = pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (res != 0)
        {
            pthread_rwlockattr_destroy(&attr);
            throw std::system_error(res, std::system_category(), "Failed to set pshared attribute");
        }

        res = pthread_rwlock_init(&rwlock, &attr);
        pthread_rwlockattr_destroy(&attr);

        if (res != 0)
        {
            throw std::system_error(res, std::system_category(),
                                    "Failed to init interprocess shared mutex");
        }
    }

    ~InterprocessSharedMutex()
    {
        pthread_rwlock_destroy(&rwlock);
    }

    void lock()
    {
        int res = pthread_rwlock_wrlock(&rwlock);
        if (res != 0)
        {
            throw std::system_error(res, std::system_category(),
                                    "Failed to lock shared mutex exclusively");
        }
    }

    bool try_lock()
    {
        int res = pthread_rwlock_trywrlock(&rwlock);
        if (res == 0)
        {
            return true;
        }
        if (is_busy_result(res))
        {
            return false;
        }
        throw std::system_error(res, std::system_category(),
                                "Failed to try_lock shared mutex exclusively");
    }

    template <typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& timeout_time)
    {
        return poll_until(timeout_time, [this] { return try_lock(); });
    }

    template <typename Rep, typename Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& timeout_duration)
    {
        return try_lock_until(std::chrono::steady_clock::now() + timeout_duration);
    }

    bool timed_lock(const std::chrono::system_clock::time_point& timeout_time)
    {
        return try_lock_until(timeout_time);
    }

    void unlock()
    {
        int res = pthread_rwlock_unlock(&rwlock);
        if (res != 0)
        {
            throw std::system_error(res, std::system_category(),
                                    "Failed to unlock shared mutex");
        }
    }

    void lock_shared()
    {
        int res = pthread_rwlock_rdlock(&rwlock);
        if (res != 0)
        {
            throw std::system_error(res, std::system_category(),
                                    "Failed to lock shared mutex in shared mode");
        }
    }

    bool try_lock_shared()
    {
        int res = pthread_rwlock_tryrdlock(&rwlock);
        if (res == 0)
        {
            return true;
        }
        if (is_busy_result(res))
        {
            return false;
        }
        throw std::system_error(res, std::system_category(),
                                "Failed to try_lock shared mutex in shared mode");
    }

    template <typename Clock, typename Duration>
    bool try_lock_shared_until(const std::chrono::time_point<Clock, Duration>& timeout_time)
    {
        return poll_until(timeout_time, [this] { return try_lock_shared(); });
    }

    template <typename Rep, typename Period>
    bool try_lock_shared_for(const std::chrono::duration<Rep, Period>& timeout_duration)
    {
        return try_lock_shared_until(std::chrono::steady_clock::now() + timeout_duration);
    }

    bool timed_lock_shared(const std::chrono::system_clock::time_point& timeout_time)
    {
        return try_lock_shared_until(timeout_time);
    }

    void unlock_shared()
    {
        unlock();
    }

    pthread_rwlock_t* native_handle()
    {
        return &rwlock;
    }

    const pthread_rwlock_t* native_handle() const
    {
        return &rwlock;
    }

    static bool process_shared_supported() noexcept
    {
        pthread_rwlockattr_t attr;
        int res = pthread_rwlockattr_init(&attr);
        if (res != 0)
        {
            return false;
        }

        res = pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (res != 0)
        {
            pthread_rwlockattr_destroy(&attr);
            return false;
        }

        pthread_rwlock_t probe;
        res = pthread_rwlock_init(&probe, &attr);
        pthread_rwlockattr_destroy(&attr);
        if (res != 0)
        {
            return false;
        }

        pthread_rwlock_destroy(&probe);
        return true;
    }

private:
    static bool is_busy_result(int res) noexcept
    {
        return res == EBUSY || res == EAGAIN;
    }

    template <typename Clock, typename Duration, typename TryLock>
    static bool poll_until(const std::chrono::time_point<Clock, Duration>& timeout_time,
                           TryLock try_lock)
    {
        while (true)
        {
            if (try_lock())
            {
                return true;
            }

            auto now = Clock::now();
            if (now >= timeout_time)
            {
                return false;
            }

            auto remaining = timeout_time - now;
            auto sleep_time =
                remaining < std::chrono::milliseconds(1) ? remaining : std::chrono::milliseconds(1);
            std::this_thread::sleep_for(sleep_time);
        }
    }

    pthread_rwlock_t rwlock;
};

} // namespace interprocess
