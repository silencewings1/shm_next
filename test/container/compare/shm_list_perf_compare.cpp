#include "interprocess/container/shared_memory_list.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_mutex.h"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace interprocess;

namespace
{

using Clock = std::chrono::steady_clock;

constexpr std::size_t record_bytes = 64;

struct Record
{
    std::uint64_t sequence;
    char payload[record_bytes - sizeof(std::uint64_t)];
};

using List = SharedMemoryList<Record>;
using ListAllocator = SharedMemoryAllocator<Record>;

struct SharedRoot
{
    InterprocessMutex mutex;
    List list;

    explicit SharedRoot(const ListAllocator& allocator) : list(allocator)
    {
    }
};

struct Config
{
    int workers = 4;
    std::size_t operations = 20000;
    std::size_t shm_size = 96 * 1024 * 1024;
};

Record make_record(std::uint64_t sequence)
{
    Record record{};
    record.sequence = sequence;
    std::memset(record.payload, static_cast<int>(sequence & 0x7f), sizeof(record.payload));
    return record;
}

std::size_t adjusted_operations(std::size_t operations, int workers)
{
    return operations - (operations % static_cast<std::size_t>(workers));
}

template <typename Func>
double measure_ms(Func&& func)
{
    auto start = Clock::now();
    func();
    auto finish = Clock::now();
    return std::chrono::duration<double, std::milli>(finish - start).count();
}

void print_result(const char* scenario, const char* api, const char* mode, int workers,
                  std::size_t operations, double elapsed_ms)
{
    const double ns_per_op = elapsed_ms * 1000000.0 / static_cast<double>(operations);
    std::cout << "RESULT project=shm_next container=list scenario=" << scenario << " api=" << api
              << " mode=" << mode << " workers=" << workers << " ops=" << operations
              << " record_bytes=" << record_bytes << " lock=external_interprocess_mutex"
              << " elapsed_ms=" << elapsed_ms << " ns_per_op=" << ns_per_op << std::endl;
}

bool wait_children(const std::vector<pid_t>& children)
{
    for (pid_t pid : children)
    {
        int status = 0;
        if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            return false;
        }
    }
    return true;
}

void prepare_list(SharedRoot* root, std::size_t operations)
{
    std::lock_guard<InterprocessMutex> lock(root->mutex);
    root->list.clear();
    for (std::size_t i = 0; i < operations; ++i)
    {
        root->list.push_back(make_record(i + 1));
    }
}

std::size_t list_size(SharedRoot* root)
{
    std::lock_guard<InterprocessMutex> lock(root->mutex);
    return root->list.size();
}

bool run_thread_push_back(SharedRoot* root, const Config& config)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    {
        std::lock_guard<InterprocessMutex> lock(root->mutex);
        root->list.clear();
    }

    const double elapsed_ms = measure_ms([&] {
        std::vector<std::thread> threads;
        threads.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            threads.emplace_back([root, worker, per_worker]() {
                const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
                for (std::size_t i = 0; i < per_worker; ++i)
                {
                    std::lock_guard<InterprocessMutex> lock(root->mutex);
                    root->list.push_back(make_record(base + i + 1));
                }
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
    });

    if (list_size(root) != operations)
    {
        std::cerr << "[shm_next list perf] thread push size mismatch" << std::endl;
        return false;
    }

    print_result("thread_push_back", "push_back", "thread", config.workers, operations, elapsed_ms);
    return true;
}

bool run_thread_read_endpoint(SharedRoot* root, const Config& config, bool use_back,
                              bool copy_to_local)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    prepare_list(root, operations);

    std::atomic<std::uint64_t> sink{0};
    const double elapsed_ms = measure_ms([&] {
        std::vector<std::thread> threads;
        threads.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            threads.emplace_back([root, per_worker, use_back, copy_to_local, &sink]() {
                std::uint64_t local = 0;
                for (std::size_t i = 0; i < per_worker; ++i)
                {
                    std::lock_guard<InterprocessMutex> lock(root->mutex);
                    const Record& record = use_back ? root->list.back() : root->list.front();
                    if (copy_to_local)
                    {
                        Record local_record{};
                        std::memcpy(&local_record, &record, sizeof(local_record));
                        local += local_record.sequence;
                    }
                    else
                    {
                        local += record.sequence;
                    }
                }
                sink.fetch_add(local, std::memory_order_relaxed);
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
    });

    if (sink.load(std::memory_order_relaxed) == 0)
    {
        std::cerr << "[shm_next list perf] thread read sink was zero" << std::endl;
        return false;
    }

    if (copy_to_local)
    {
        print_result("thread_fetch_front_copy", "front+memcpy", "thread", config.workers,
                     operations, elapsed_ms);
    }
    else
    {
        print_result(use_back ? "thread_read_back" : "thread_read_front",
                     use_back ? "back" : "front", "thread", config.workers, operations, elapsed_ms);
    }
    return true;
}

bool run_thread_pop_front(SharedRoot* root, const Config& config, bool copy_to_local)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    prepare_list(root, operations);

    std::atomic<std::uint64_t> sink{0};
    const double elapsed_ms = measure_ms([&] {
        std::vector<std::thread> threads;
        threads.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            threads.emplace_back([root, per_worker, copy_to_local, &sink]() {
                std::uint64_t local = 0;
                for (std::size_t i = 0; i < per_worker; ++i)
                {
                    std::lock_guard<InterprocessMutex> lock(root->mutex);
                    if (copy_to_local)
                    {
                        Record record = root->list.front();
                        root->list.pop_front();
                        local += record.sequence;
                    }
                    else
                    {
                        root->list.pop_front();
                    }
                }
                if (copy_to_local)
                {
                    sink.fetch_add(local, std::memory_order_relaxed);
                }
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
    });

    if (list_size(root) != 0)
    {
        std::cerr << "[shm_next list perf] thread pop size mismatch" << std::endl;
        return false;
    }
    if (copy_to_local && sink.load(std::memory_order_relaxed) == 0)
    {
        std::cerr << "[shm_next list perf] thread consume sink was zero" << std::endl;
        return false;
    }

    print_result(copy_to_local ? "thread_consume_front" : "thread_pop_front",
                 copy_to_local ? "front+memcpy+pop_front" : "pop_front", "thread", config.workers,
                 operations, elapsed_ms);
    return true;
}

int process_push_back_child(SharedRoot* root, int worker, std::size_t per_worker)
{
    const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
    for (std::size_t i = 0; i < per_worker; ++i)
    {
        std::lock_guard<InterprocessMutex> lock(root->mutex);
        root->list.push_back(make_record(base + i + 1));
    }
    return 0;
}

int process_read_endpoint_child(SharedRoot* root, std::size_t per_worker, bool use_back,
                                bool copy_to_local)
{
    volatile std::uint64_t sink = 0;
    for (std::size_t i = 0; i < per_worker; ++i)
    {
        std::lock_guard<InterprocessMutex> lock(root->mutex);
        const Record& record = use_back ? root->list.back() : root->list.front();
        if (copy_to_local)
        {
            Record local_record{};
            std::memcpy(&local_record, &record, sizeof(local_record));
            sink += local_record.sequence;
        }
        else
        {
            sink += record.sequence;
        }
    }
    return sink == 0 ? 2 : 0;
}

int process_pop_front_child(SharedRoot* root, std::size_t per_worker, bool copy_to_local)
{
    volatile std::uint64_t sink = 0;
    for (std::size_t i = 0; i < per_worker; ++i)
    {
        std::lock_guard<InterprocessMutex> lock(root->mutex);
        if (copy_to_local)
        {
            Record record = root->list.front();
            root->list.pop_front();
            sink += record.sequence;
        }
        else
        {
            root->list.pop_front();
        }
    }
    return copy_to_local && sink == 0 ? 2 : 0;
}

bool run_process_push_back(SharedRoot* root, const Config& config)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    {
        std::lock_guard<InterprocessMutex> lock(root->mutex);
        root->list.clear();
    }

    const double elapsed_ms = measure_ms([&] {
        std::vector<pid_t> children;
        children.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                _exit(process_push_back_child(root, worker, per_worker));
            }
            children.push_back(pid);
        }
        if (!wait_children(children))
        {
            std::cerr << "[shm_next list perf] process push child failed" << std::endl;
            std::exit(2);
        }
    });

    if (list_size(root) != operations)
    {
        std::cerr << "[shm_next list perf] process push size mismatch" << std::endl;
        return false;
    }

    print_result("process_push_back", "push_back", "process", config.workers, operations,
                 elapsed_ms);
    return true;
}

bool run_process_read_endpoint(SharedRoot* root, const Config& config, bool use_back,
                               bool copy_to_local)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    prepare_list(root, operations);

    const double elapsed_ms = measure_ms([&] {
        std::vector<pid_t> children;
        children.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                _exit(process_read_endpoint_child(root, per_worker, use_back, copy_to_local));
            }
            children.push_back(pid);
        }
        if (!wait_children(children))
        {
            std::cerr << "[shm_next list perf] process read child failed" << std::endl;
            std::exit(2);
        }
    });

    if (copy_to_local)
    {
        print_result("process_fetch_front_copy", "front+memcpy", "process", config.workers,
                     operations, elapsed_ms);
    }
    else
    {
        print_result(use_back ? "process_read_back" : "process_read_front",
                     use_back ? "back" : "front", "process", config.workers, operations,
                     elapsed_ms);
    }
    return true;
}

bool run_process_pop_front(SharedRoot* root, const Config& config, bool copy_to_local)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    prepare_list(root, operations);

    const double elapsed_ms = measure_ms([&] {
        std::vector<pid_t> children;
        children.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                _exit(process_pop_front_child(root, per_worker, copy_to_local));
            }
            children.push_back(pid);
        }
        if (!wait_children(children))
        {
            std::cerr << "[shm_next list perf] process pop child failed" << std::endl;
            std::exit(2);
        }
    });

    if (list_size(root) != 0)
    {
        std::cerr << "[shm_next list perf] process pop size mismatch" << std::endl;
        return false;
    }

    print_result(copy_to_local ? "process_consume_front" : "process_pop_front",
                 copy_to_local ? "front+memcpy+pop_front" : "pop_front", "process", config.workers,
                 operations, elapsed_ms);
    return true;
}

Config parse_config(int argc, char** argv)
{
    Config config;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        auto read_value = [&](const char* name) -> const char* {
            const std::string prefix = std::string(name) + "=";
            if (arg.rfind(prefix, 0) == 0)
            {
                return arg.c_str() + prefix.size();
            }
            return nullptr;
        };
        if (const char* value = read_value("--workers"))
        {
            config.workers = std::atoi(value);
        }
        else if (const char* value = read_value("--operations"))
        {
            config.operations = static_cast<std::size_t>(std::strtoull(value, nullptr, 10));
        }
    }

    if (config.workers <= 0)
    {
        config.workers = 1;
    }
    if (config.operations < static_cast<std::size_t>(config.workers))
    {
        config.operations = static_cast<std::size_t>(config.workers);
    }
    return config;
}

} // namespace

int main(int argc, char** argv)
{
    Config config = parse_config(argc, argv);
    const std::string shm_name = "shm_lperf_" + std::to_string(getpid());
    ManagedSharedMemory::remove(shm_name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, shm_name.c_str(), config.shm_size);
        SharedRoot* root =
            segment.construct<SharedRoot>("RootObject", segment.get_allocator<Record>());
        if (root == nullptr)
        {
            std::cerr << "[shm_next list perf] failed to construct root" << std::endl;
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        const bool ok =
            run_thread_push_back(root, config) &&
            run_thread_read_endpoint(root, config, false, false) &&
            run_thread_read_endpoint(root, config, true, false) &&
            run_thread_read_endpoint(root, config, false, true) &&
            run_thread_pop_front(root, config, false) && run_thread_pop_front(root, config, true) &&
            run_process_push_back(root, config) &&
            run_process_read_endpoint(root, config, false, false) &&
            run_process_read_endpoint(root, config, true, false) &&
            run_process_read_endpoint(root, config, false, true) &&
            run_process_pop_front(root, config, false) && run_process_pop_front(root, config, true);

        segment.destroy<SharedRoot>("RootObject");
        ManagedSharedMemory::remove(shm_name.c_str());
        return ok ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[shm_next list perf] exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name.c_str());
        return 1;
    }
}
