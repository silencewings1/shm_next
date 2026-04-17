#include "../interprocess/ipc/managed_shared_memory.h"
#include "../interprocess/sync/posix_mutex.h"
#include "../interprocess/container/shared_memory_vector.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <mutex>

using namespace interprocess;

// 定义共享内存中的数据类型（必须与producer完全一致）
struct SensorData
{
    int sensor_id;
    double value;
    long timestamp;
    char unit[16];

    SensorData(int id = 0, double val = 0.0, const char* u = "")
        : sensor_id(id), value(val), timestamp(0)
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

std::string format_time(long timestamp)
{
    char buffer[80];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&timestamp));
    return std::string(buffer);
}

void log_message(const std::string& msg)
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::cout << std::ctime(&time) << " [Consumer] " << msg << std::endl;
}

int main()
{
    const char* SHARED_MEMORY_NAME = "SensorDataSharedMemory";

    try
    {
        log_message("Opening shared memory...");
        ManagedSharedMemory segment(open_only, SHARED_MEMORY_NAME);

        SharedRoot* root = segment.find<SharedRoot>("RootObject");

        if (!root)
        {
            log_message("Root object not found! Is producer running?");
            return 1;
        }

        SensorDataVector* sensor_data = &root->sensor_data;
        InterprocessMutex& mutex = root->mutex;

        log_message("Found sensor data vector. Starting to consume data...");
        std::cout << "Consumer is running. Press Ctrl+C to stop." << std::endl;
        std::cout << "========================================" << std::endl;

        int iteration = 0;
        std::size_t last_size = 0;

        while (iteration < 25)
        {
            {
                std::lock_guard<InterprocessMutex> lock(mutex);

                const std::size_t current_size = sensor_data->size();

                if (current_size > 0)
                {
                    if (current_size > last_size)
                    {
                        const SensorData& latest = sensor_data->back();

                        std::cout << "\n[Consumer] New data arrived!" << std::endl;
                        std::cout << "  Sensor " << latest.sensor_id << " = " << latest.value << " "
                                  << latest.unit << " at " << format_time(latest.timestamp)
                                  << std::endl;

                        double sum[3] = {0};
                        int count[3] = {0};

                        for (size_t i = 0; i < sensor_data->size(); ++i)
                        {
                            const auto& data = (*sensor_data)[i];
                            int idx = data.sensor_id - 1;
                            if (idx >= 0 && idx < 3)
                            {
                                sum[idx] += data.value;
                                count[idx]++;
                            }
                        }

                        std::cout << "\n[Consumer] Statistics:" << std::endl;
                        std::cout << "  Sensor 1 (Temperature): ";
                        if (count[0] > 0)
                            std::cout << "Avg = " << (sum[0] / count[0]) << " Celsius";
                        else
                            std::cout << "No data";
                        std::cout << std::endl;

                        std::cout << "  Sensor 2 (Humidity):    ";
                        if (count[1] > 0)
                            std::cout << "Avg = " << (sum[1] / count[1]) << " %";
                        else
                            std::cout << "No data";
                        std::cout << std::endl;

                        std::cout << "  Sensor 3 (Pressure):    ";
                        if (count[2] > 0)
                            std::cout << "Avg = " << (sum[2] / count[2]) << " hPa";
                        else
                            std::cout << "No data";
                        std::cout << std::endl;

                        std::cout << "  Total records: " << current_size << std::endl;
                    }
                    else if (current_size < last_size)
                    {
                        std::cout << "[Consumer] Oldest record was removed." << std::endl;
                    }

                    last_size = current_size;
                }
                else
                {
                    std::cout << "[Consumer] Waiting for data..." << std::endl;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            iteration++;
        }

        {
            std::lock_guard<InterprocessMutex> lock(mutex);

            log_message("Final data summary:");
            std::cout << "\n========================================" << std::endl;
            std::cout << "Final sensor data in shared memory:" << std::endl;
            std::cout << "========================================" << std::endl;

            if (sensor_data->empty())
            {
                std::cout << "No data available." << std::endl;
            }
            else
            {
                std::cout << std::left << std::setw(8) << "Index" << std::setw(10) << "Sensor"
                          << std::setw(12) << "Value" << std::setw(12) << "Unit"
                          << "Timestamp" << std::endl;
                std::cout << std::string(60, '-') << std::endl;

                for (size_t i = 0; i < sensor_data->size(); ++i)
                {
                    const SensorData& data = (*sensor_data)[i];
                    std::cout << std::left << std::setw(8) << i << std::setw(10) << data.sensor_id
                              << std::setw(12) << data.value << std::setw(12) << data.unit
                              << format_time(data.timestamp) << std::endl;
                }
            }
        }

        log_message("Consumer finished successfully.");
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Consumer Error] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
