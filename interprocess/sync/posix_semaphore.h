#pragma once

#include "posix_condition.h"
#include "posix_mutex.h"
#include <limits>
#include <mutex>
#include <stdexcept>

namespace interprocess
{

class InterprocessSemaphore
{
public:
    InterprocessSemaphore(const InterprocessSemaphore&) = delete;
    InterprocessSemaphore& operator=(const InterprocessSemaphore&) = delete;

    explicit InterprocessSemaphore(unsigned int initial_count) : count(initial_count)
    {
    }

    ~InterprocessSemaphore() = default;

    void post()
    {
        std::lock_guard<InterprocessMutex> lock(mutex);
        if (count == std::numeric_limits<unsigned int>::max())
        {
            throw std::overflow_error("InterprocessSemaphore count overflow");
        }

        ++count;
        condition.notify_one();
    }

    void wait()
    {
        std::unique_lock<InterprocessMutex> lock(mutex);
        condition.wait(mutex, [this] { return count > 0; });
        --count;
    }

    bool try_wait()
    {
        std::lock_guard<InterprocessMutex> lock(mutex);
        if (count == 0)
        {
            return false;
        }

        --count;
        return true;
    }

private:
    InterprocessMutex mutex;
    InterprocessCondition condition;
    unsigned int count;
};

} // namespace interprocess
