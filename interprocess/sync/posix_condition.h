#pragma once

#include "posix_mutex.h"
#include <pthread.h>
#include <system_error>

namespace interprocess
{

class InterprocessCondition
{
public:
    InterprocessCondition(const InterprocessCondition&) = delete;
    InterprocessCondition& operator=(const InterprocessCondition&) = delete;

    InterprocessCondition()
    {
        pthread_condattr_t attr;
        int res = pthread_condattr_init(&attr);
        if (res != 0)
        {
            throw std::system_error(res, std::system_category(),
                                    "Failed to init condition attributes");
        }

        res = pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (res != 0)
        {
            pthread_condattr_destroy(&attr);
            throw std::system_error(res, std::system_category(), "Failed to set pshared attribute");
        }

        res = pthread_cond_init(&cond, &attr);
        pthread_condattr_destroy(&attr);

        if (res != 0)
        {
            throw std::system_error(res, std::system_category(),
                                    "Failed to init interprocess condition");
        }
    }

    ~InterprocessCondition()
    {
        pthread_cond_destroy(&cond);
    }

    void wait(InterprocessMutex& mutex)
    {
        int res = pthread_cond_wait(&cond, mutex.native_handle());
        if (res != 0)
        {
            throw std::system_error(res, std::system_category(), "Failed to wait on condition");
        }
    }

    template <typename Predicate>
    void wait(InterprocessMutex& mutex, Predicate pred)
    {
        while (!pred())
        {
            wait(mutex);
        }
    }

    void notify_one()
    {
        int res = pthread_cond_signal(&cond);
        if (res != 0)
        {
            throw std::system_error(res, std::system_category(), "Failed to signal condition");
        }
    }

    void notify_all()
    {
        int res = pthread_cond_broadcast(&cond);
        if (res != 0)
        {
            throw std::system_error(res, std::system_category(), "Failed to broadcast condition");
        }
    }

private:
    pthread_cond_t cond;
};

} // namespace interprocess
