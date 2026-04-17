#include "../interprocess/ipc/managed_shared_memory.h"
#include "../interprocess/sync/posix_mutex.h"
#include "../interprocess/container/shared_memory_vector.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>
#include <mutex>

using namespace interprocess;

// 定义共享内存中的数据类型
struct SensorData
{
    int sensor_id;
    double value;
    long timestamp;
    char unit[16];

    SensorData(int id = 0, double val = 0.0, const char* u = "")
        : sensor_id(id), value(val), timestamp(std::time(nullptr))
    {
        strncpy(unit, u, sizeof(unit) - 1);
        unit[sizeof(unit) - 1] = '\0';
    }
};

// 使用我们新实现的 SharedMemoryVector
using SensorDataAllocator = SharedMemoryAllocator<SensorData>;
using SensorDataVector = SharedMemoryVector<SensorData, SensorDataAllocator>;

// 定义相同的“根”结构体，用于获取共享对象入口
struct SharedRoot
{
    InterprocessMutex mutex;
    SensorDataVector sensor_data;

    SharedRoot(const SensorDataAllocator& alloc) : sensor_data(alloc)
    {
    }
};

void log_message(const std::string& msg)
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::cout << std::ctime(&time) << " [Producer] " << msg << std::endl;
}

int main()
{
    const char* SHARED_MEMORY_NAME = "SensorDataSharedMemory";
    const std::size_t SHARED_MEMORY_SIZE = 65536; // 64KB

    try
    {
        log_message("Cleaning up previous shared memory...");
        ManagedSharedMemory::remove(SHARED_MEMORY_NAME);

        log_message("Creating shared memory...");
        ManagedSharedMemory segment(create_only, SHARED_MEMORY_NAME, SHARED_MEMORY_SIZE);

        auto allocator = segment.get_allocator<SensorData>();

        log_message("Constructing root object and SharedMemoryVector in shared memory...");
        SharedRoot* root = segment.construct<SharedRoot>("RootObject", allocator);
        if (!root)
        {
            std::cerr << "[Producer Error] Failed to construct RootObject" << std::endl;
            return 1;
        }

        SensorDataVector* sensor_data = &root->sensor_data;
        InterprocessMutex& mutex = root->mutex;

        log_message("Starting to produce sensor data...");
        std::cout << "Producer is running. Press Ctrl+C to stop." << std::endl;
        std::cout << "========================================" << std::endl;

        int iteration = 0;
        while (iteration < 20)
        {
            {
                std::lock_guard<InterprocessMutex> lock(mutex);

                SensorData data;
                data.sensor_id = iteration % 3 + 1;

                if (data.sensor_id == 1)
                {
                    data.value = 20.0 + (std::rand() % 100) / 10.0;
                    strcpy(data.unit, "Celsius");
                }
                else if (data.sensor_id == 2)
                {
                    data.value = 50.0 + (std::rand() % 200) / 10.0;
                    strcpy(data.unit, "%");
                }
                else
                {
                    data.value = 1000.0 + (std::rand() % 500) / 10.0;
                    strcpy(data.unit, "hPa");
                }

                data.timestamp = std::time(nullptr);

                sensor_data->push_back(data);

                std::cout << "[Producer] Added: Sensor " << data.sensor_id << " = " << data.value
                          << " " << data.unit << " (Total: " << sensor_data->size() << " records)"
                          << std::endl;

                if (sensor_data->size() > 10)
                {
                    sensor_data->erase(sensor_data->begin());
                    std::cout << "[Producer] Removed oldest record (max 10 records kept)"
                              << std::endl;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1000 + (std::rand() % 1000)));
            iteration++;
        }

        {
            std::lock_guard<InterprocessMutex> lock(mutex);
            log_message("Finished producing data.");
            std::cout << "[Producer] Final vector size: " << sensor_data->size() << std::endl;

            for (size_t i = 0; i < sensor_data->size(); ++i)
            {
                const SensorData& data = (*sensor_data)[i];
                char time_buf[64];
                std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S",
                              std::localtime(&data.timestamp));

                std::cout << "Index " << i << ": Sensor " << data.sensor_id << " = " << data.value
                          << " " << data.unit << " at " << time_buf << std::endl;
            }
        }

        log_message("Waiting for consumer to read data (10 seconds)...");
        std::this_thread::sleep_for(std::chrono::seconds(10));

        log_message("Cleaning up shared memory...");
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Producer Error] " << e.what() << std::endl;
        return 1;
    }

    ManagedSharedMemory::remove(SHARED_MEMORY_NAME);
    log_message("Producer finished successfully.");
    return 0;
}
