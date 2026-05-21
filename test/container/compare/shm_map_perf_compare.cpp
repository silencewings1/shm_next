#include "interprocess/container/shared_memory_map.h"
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
using Key = std::int64_t;

constexpr std::size_t record_bytes = 64;

struct Record
{
    std::uint64_t sequence;
    char payload[record_bytes - sizeof(std::uint64_t)];
};

using Map = SharedMemoryMap<Key, Record>;
using MapAllocator = SharedMemoryAllocator<std::pair<const Key, Record>>;

struct SharedRoot
{
    InterprocessMutex mutex;
    Map map;

    explicit SharedRoot(const MapAllocator& allocator) : map(allocator)
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
    std::cout << "RESULT project=shm_next container=map scenario=" << scenario << " api=" << api
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

void prepare_map(SharedRoot* root, std::size_t operations)
{
    std::lock_guard<InterprocessMutex> lock(root->mutex);
    root->map.clear();
    for (std::size_t i = 0; i < operations; ++i)
    {
        root->map.try_emplace(static_cast<Key>(i), make_record(i));
    }
}

std::size_t map_size(SharedRoot* root)
{
    std::lock_guard<InterprocessMutex> lock(root->mutex);
    return root->map.size();
}

bool run_thread_insert(SharedRoot* root, const Config& config)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    {
        std::lock_guard<InterprocessMutex> lock(root->mutex);
        root->map.clear();
    }

    std::atomic<bool> ok{true};
    const double elapsed_ms = measure_ms([&] {
        std::vector<std::thread> threads;
        threads.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            threads.emplace_back([root, worker, per_worker, &ok]() {
                const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
                for (std::size_t i = 0; i < per_worker; ++i)
                {
                    const Key key = static_cast<Key>(base + i);
                    std::lock_guard<InterprocessMutex> lock(root->mutex);
                    if (!root->map.try_emplace(key, make_record(base + i)).second)
                    {
                        ok.store(false, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
    });

    if (!ok.load(std::memory_order_relaxed) || map_size(root) != operations)
    {
        std::cerr << "[shm_next map perf] thread insert mismatch" << std::endl;
        return false;
    }
    print_result("thread_insert_new", "try_emplace", "thread", config.workers, operations,
                 elapsed_ms);
    return true;
}

bool run_thread_find(SharedRoot* root, const Config& config, bool copy_to_local)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    prepare_map(root, operations);

    std::atomic<std::uint64_t> sink{0};
    std::atomic<bool> ok{true};
    const double elapsed_ms = measure_ms([&] {
        std::vector<std::thread> threads;
        threads.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            threads.emplace_back([root, worker, per_worker, copy_to_local, &sink, &ok]() {
                std::uint64_t local = 0;
                const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
                for (std::size_t i = 0; i < per_worker; ++i)
                {
                    std::lock_guard<InterprocessMutex> lock(root->mutex);
                    auto it = root->map.find(static_cast<Key>(base + i));
                    if (it == root->map.end())
                    {
                        ok.store(false, std::memory_order_relaxed);
                        continue;
                    }
                    if (copy_to_local)
                    {
                        Record record{};
                        std::memcpy(&record, &it->second, sizeof(record));
                        local += record.sequence;
                    }
                    else
                    {
                        local += it->second.sequence;
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

    if (!ok.load(std::memory_order_relaxed) || sink.load(std::memory_order_relaxed) == 0)
    {
        std::cerr << "[shm_next map perf] thread find failed" << std::endl;
        return false;
    }
    print_result(copy_to_local ? "thread_fetch_copy" : "thread_find",
                 copy_to_local ? "find+memcpy" : "find", "thread", config.workers, operations,
                 elapsed_ms);
    return true;
}

bool run_thread_update(SharedRoot* root, const Config& config)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    prepare_map(root, operations);

    std::atomic<bool> ok{true};
    const double elapsed_ms = measure_ms([&] {
        std::vector<std::thread> threads;
        threads.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            threads.emplace_back([root, worker, per_worker, &ok]() {
                const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
                for (std::size_t i = 0; i < per_worker; ++i)
                {
                    std::lock_guard<InterprocessMutex> lock(root->mutex);
                    auto it = root->map.find(static_cast<Key>(base + i));
                    if (it == root->map.end())
                    {
                        ok.store(false, std::memory_order_relaxed);
                        continue;
                    }
                    it->second = make_record(base + i + 1000000);
                }
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
    });

    if (!ok.load(std::memory_order_relaxed))
    {
        std::cerr << "[shm_next map perf] thread update failed" << std::endl;
        return false;
    }
    print_result("thread_update_existing", "find+assign", "thread", config.workers, operations,
                 elapsed_ms);
    return true;
}

bool run_thread_erase(SharedRoot* root, const Config& config)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    prepare_map(root, operations);

    std::atomic<bool> ok{true};
    const double elapsed_ms = measure_ms([&] {
        std::vector<std::thread> threads;
        threads.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            threads.emplace_back([root, worker, per_worker, &ok]() {
                const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
                for (std::size_t i = 0; i < per_worker; ++i)
                {
                    std::lock_guard<InterprocessMutex> lock(root->mutex);
                    if (root->map.erase(static_cast<Key>(base + i)) != 1)
                    {
                        ok.store(false, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
    });

    if (!ok.load(std::memory_order_relaxed) || map_size(root) != 0)
    {
        std::cerr << "[shm_next map perf] thread erase mismatch" << std::endl;
        return false;
    }
    print_result("thread_erase_key", "erase", "thread", config.workers, operations, elapsed_ms);
    return true;
}

int process_insert_child(SharedRoot* root, int worker, std::size_t per_worker)
{
    const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
    for (std::size_t i = 0; i < per_worker; ++i)
    {
        const Key key = static_cast<Key>(base + i);
        std::lock_guard<InterprocessMutex> lock(root->mutex);
        if (!root->map.try_emplace(key, make_record(base + i)).second)
        {
            return 2;
        }
    }
    return 0;
}

int process_find_child(SharedRoot* root, int worker, std::size_t per_worker, bool copy_to_local)
{
    volatile std::uint64_t sink = 0;
    const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
    for (std::size_t i = 0; i < per_worker; ++i)
    {
        std::lock_guard<InterprocessMutex> lock(root->mutex);
        auto it = root->map.find(static_cast<Key>(base + i));
        if (it == root->map.end())
        {
            return 2;
        }
        if (copy_to_local)
        {
            Record record{};
            std::memcpy(&record, &it->second, sizeof(record));
            sink += record.sequence;
        }
        else
        {
            sink += it->second.sequence;
        }
    }
    return sink == 0 ? 3 : 0;
}

int process_update_child(SharedRoot* root, int worker, std::size_t per_worker)
{
    const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
    for (std::size_t i = 0; i < per_worker; ++i)
    {
        std::lock_guard<InterprocessMutex> lock(root->mutex);
        auto it = root->map.find(static_cast<Key>(base + i));
        if (it == root->map.end())
        {
            return 2;
        }
        it->second = make_record(base + i + 1000000);
    }
    return 0;
}

int process_erase_child(SharedRoot* root, int worker, std::size_t per_worker)
{
    const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
    for (std::size_t i = 0; i < per_worker; ++i)
    {
        std::lock_guard<InterprocessMutex> lock(root->mutex);
        if (root->map.erase(static_cast<Key>(base + i)) != 1)
        {
            return 2;
        }
    }
    return 0;
}

bool run_process_insert(SharedRoot* root, const Config& config)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    {
        std::lock_guard<InterprocessMutex> lock(root->mutex);
        root->map.clear();
    }

    const double elapsed_ms = measure_ms([&] {
        std::vector<pid_t> children;
        children.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                _exit(process_insert_child(root, worker, per_worker));
            }
            children.push_back(pid);
        }
        if (!wait_children(children))
        {
            std::cerr << "[shm_next map perf] process insert child failed" << std::endl;
            std::exit(2);
        }
    });

    if (map_size(root) != operations)
    {
        std::cerr << "[shm_next map perf] process insert mismatch" << std::endl;
        return false;
    }
    print_result("process_insert_new", "try_emplace", "process", config.workers, operations,
                 elapsed_ms);
    return true;
}

bool run_process_find(SharedRoot* root, const Config& config, bool copy_to_local)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    prepare_map(root, operations);

    const double elapsed_ms = measure_ms([&] {
        std::vector<pid_t> children;
        children.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                _exit(process_find_child(root, worker, per_worker, copy_to_local));
            }
            children.push_back(pid);
        }
        if (!wait_children(children))
        {
            std::cerr << "[shm_next map perf] process find child failed" << std::endl;
            std::exit(2);
        }
    });

    print_result(copy_to_local ? "process_fetch_copy" : "process_find",
                 copy_to_local ? "find+memcpy" : "find", "process", config.workers, operations,
                 elapsed_ms);
    return true;
}

bool run_process_update(SharedRoot* root, const Config& config)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    prepare_map(root, operations);

    const double elapsed_ms = measure_ms([&] {
        std::vector<pid_t> children;
        children.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                _exit(process_update_child(root, worker, per_worker));
            }
            children.push_back(pid);
        }
        if (!wait_children(children))
        {
            std::cerr << "[shm_next map perf] process update child failed" << std::endl;
            std::exit(2);
        }
    });

    print_result("process_update_existing", "find+assign", "process", config.workers, operations,
                 elapsed_ms);
    return true;
}

bool run_process_erase(SharedRoot* root, const Config& config)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    prepare_map(root, operations);

    const double elapsed_ms = measure_ms([&] {
        std::vector<pid_t> children;
        children.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                _exit(process_erase_child(root, worker, per_worker));
            }
            children.push_back(pid);
        }
        if (!wait_children(children))
        {
            std::cerr << "[shm_next map perf] process erase child failed" << std::endl;
            std::exit(2);
        }
    });

    if (map_size(root) != 0)
    {
        std::cerr << "[shm_next map perf] process erase mismatch" << std::endl;
        return false;
    }
    print_result("process_erase_key", "erase", "process", config.workers, operations, elapsed_ms);
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
    const std::string shm_name = "shm_mperf_" + std::to_string(getpid());
    ManagedSharedMemory::remove(shm_name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, shm_name.c_str(), config.shm_size);
        SharedRoot* root = segment.construct<SharedRoot>(
            "RootObject", segment.get_allocator<std::pair<const Key, Record>>());
        if (root == nullptr)
        {
            std::cerr << "[shm_next map perf] failed to construct root" << std::endl;
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        const bool ok = run_thread_insert(root, config) && run_thread_find(root, config, false) &&
                        run_thread_find(root, config, true) && run_thread_update(root, config) &&
                        run_thread_erase(root, config) && run_process_insert(root, config) &&
                        run_process_find(root, config, false) &&
                        run_process_find(root, config, true) && run_process_update(root, config) &&
                        run_process_erase(root, config);

        segment.destroy<SharedRoot>("RootObject");
        ManagedSharedMemory::remove(shm_name.c_str());
        return ok ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[shm_next map perf] exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name.c_str());
        return 1;
    }
}
