#include "interprocess/sync/posix_semaphore.h"
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace interprocess;

int main()
{
    try
    {
        InterprocessSemaphore semaphore(2);

        if (!semaphore.try_wait())
        {
            std::cerr << "[Semaphore Test] try_wait should succeed for initial count 2" << std::endl;
            return 1;
        }

        if (!semaphore.try_wait())
        {
            std::cerr << "[Semaphore Test] second try_wait should succeed for initial count 2"
                      << std::endl;
            return 1;
        }

        if (semaphore.try_wait())
        {
            std::cerr << "[Semaphore Test] try_wait should fail when count reaches 0" << std::endl;
            return 1;
        }

        std::thread producer([&semaphore] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            semaphore.post();
        });

        semaphore.wait();
        producer.join();

        InterprocessSemaphore overflow_semaphore(std::numeric_limits<unsigned int>::max());
        bool overflow_thrown = false;
        try
        {
            overflow_semaphore.post();
        }
        catch (const std::overflow_error&)
        {
            overflow_thrown = true;
        }

        if (!overflow_thrown)
        {
            std::cerr << "[Semaphore Test] overflow_error was not thrown" << std::endl;
            return 1;
        }

        std::cout << "[Semaphore Test] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Semaphore Test] Exception: " << e.what() << std::endl;
        return 1;
    }
}
