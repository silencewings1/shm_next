#include "interprocess/container/shared_memory_hash_map.h"
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

using HashMap = SharedMemoryHashMap<Key, Record>;
using HashMapAllocator = SharedMemoryAllocator<std::pair<const Key, Record>>;

struct SharedRoot
{
    InterprocessMutex mutex;
    HashMap map;

    explicit SharedRoot(const HashMapAllocator& allocator)
        : map(1024, std::hash<Key>{}, std::equal_to<Key>{}, allocator)
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
    std::cout << "RESULT project=shm_next container=hash_map scenario=" << scenario
              << " api=" << api << " mode=" << mode << " workers=" << workers
              << " ops=" << operations << " record_bytes=" << record_bytes
              << " lock=external_interprocess_mutex"
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
    root->map.reserve(operations);
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
        root->map.reserve(operations);
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
        std::cerr << "[shm_next hash_map perf] thread insert mismatch" << std::endl;
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
        std::cerr << "[shm_next hash_map perf] thread find failed" << std::endl;
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
        std::cerr << "[shm_next hash_map perf] thread update failed" << std::endl;
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
        std::cerr << "[shm_next hash_map perf] thread erase failed" << std::endl;
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

template <typename ChildFunc>
bool run_process_case(SharedRoot* root, const Config& config, ChildFunc child_func,
                      const char* scenario, const char* api, bool expect_empty = false)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);

    const double elapsed_ms = measure_ms([&] {
        std::vector<pid_t> children;
        children.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                _exit(child_func(worker, per_worker));
            }
            children.push_back(pid);
        }
        if (!wait_children(children))
        {
            std::cerr << "[shm_next hash_map perf] process child failed" << std::endl;
            std::exit(1);
        }
    });

    const std::size_t expected_size = expect_empty ? 0 : operations;
    if (map_size(root) != expected_size)
    {
        std::cerr << "[shm_next hash_map perf] process size mismatch" << std::endl;
        return false;
    }
    print_result(scenario, api, "process", config.workers, operations, elapsed_ms);
    return true;
}

bool run_process_insert(SharedRoot* root, const Config& config)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    {
        std::lock_guard<InterprocessMutex> lock(root->mutex);
        root->map.clear();
        root->map.reserve(operations);
    }
    return run_process_case(
        root, config,
        [root](int worker, std::size_t per_worker) {
            return process_insert_child(root, worker, per_worker);
        },
        "process_insert_new", "try_emplace");
}

bool run_process_find(SharedRoot* root, const Config& config, bool copy_to_local)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    prepare_map(root, operations);
    return run_process_case(
        root, config,
        [root, copy_to_local](int worker, std::size_t per_worker) {
            return process_find_child(root, worker, per_worker, copy_to_local);
        },
        copy_to_local ? "process_fetch_copy" : "process_find",
        copy_to_local ? "find+memcpy" : "find");
}

bool run_process_update(SharedRoot* root, const Config& config)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    prepare_map(root, operations);
    return run_process_case(
        root, config,
        [root](int worker, std::size_t per_worker) {
            return process_update_child(root, worker, per_worker);
        },
        "process_update_existing", "find+assign");
}

bool run_process_erase(SharedRoot* root, const Config& config)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    prepare_map(root, operations);
    return run_process_case(
        root, config,
        [root](int worker, std::size_t per_worker) {
            return process_erase_child(root, worker, per_worker);
        },
        "process_erase_key", "erase", true);
}

Config parse_config(int argc, char** argv)
{
    Config config;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        auto parse_value = [&](const char* prefix) -> const char* {
            const std::string prefix_string(prefix);
            if (arg.rfind(prefix_string, 0) == 0)
            {
                return arg.c_str() + prefix_string.size();
            }
            return nullptr;
        };

        if (const char* value = parse_value("--workers="))
        {
            config.workers = std::max(1, std::atoi(value));
        }
        else if (const char* value = parse_value("--operations="))
        {
            config.operations = static_cast<std::size_t>(std::strtoull(value, nullptr, 10));
        }
    }
    return config;
}

} // namespace

int main(int argc, char** argv)
{
    Config config = parse_config(argc, argv);
    const std::string name = "shm_next_hash_map_perf_" + std::to_string(getpid());
    ManagedSharedMemory::remove(name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, name.c_str(), config.shm_size);
        auto allocator = segment.get_allocator<std::pair<const Key, Record>>();
        SharedRoot* root = segment.construct<SharedRoot>("root", allocator);
        if (!root)
        {
            std::cerr << "[shm_next hash_map perf] failed to construct root" << std::endl;
            ManagedSharedMemory::remove(name.c_str());
            return 1;
        }

        bool ok = true;
        ok = run_thread_insert(root, config) && ok;
        ok = run_thread_find(root, config, false) && ok;
        ok = run_thread_find(root, config, true) && ok;
        ok = run_thread_update(root, config) && ok;
        ok = run_thread_erase(root, config) && ok;
        ok = run_process_insert(root, config) && ok;
        ok = run_process_find(root, config, false) && ok;
        ok = run_process_find(root, config, true) && ok;
        ok = run_process_update(root, config) && ok;
        ok = run_process_erase(root, config) && ok;

        segment.destroy<SharedRoot>("root");
        ManagedSharedMemory::remove(name.c_str());
        return ok ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[shm_next hash_map perf] exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(name.c_str());
        return 1;
    }
}
