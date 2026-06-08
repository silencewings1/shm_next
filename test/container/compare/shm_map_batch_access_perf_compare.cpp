#include "interprocess/container/shared_memory_map.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_mutex.h"
#include "interprocess/sync/synchronized.h"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
using SynchronizedMap = Synchronized<Map, InterprocessMutex>;
using MapAllocator = SharedMemoryAllocator<std::pair<const Key, Record>>;

struct SharedRoot
{
    SynchronizedMap map;

    explicit SharedRoot(const MapAllocator& allocator) : map(allocator)
    {
    }
};

struct Config
{
    int workers = 4;
    std::size_t operations = 20000;
    std::size_t batch_size = 256;
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
                  std::size_t operations, std::size_t batch_size, double elapsed_ms)
{
    const double ns_per_op = elapsed_ms * 1000000.0 / static_cast<double>(operations);
    std::cout << "RESULT project=shm_next container=map scenario=" << scenario << " api=" << api
              << " mode=" << mode << " workers=" << workers << " ops=" << operations
              << " batch_size=" << batch_size << " record_bytes=" << record_bytes
              << " lock=synchronized_interprocess_mutex_batch"
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
    root->map.with_lock([operations](Map& map) {
        map.clear();
        for (std::size_t i = 0; i < operations; ++i)
        {
            map.try_emplace(static_cast<Key>(i), make_record(i));
        }
    });
}

bool run_thread_read(SharedRoot* root, const Config& config, bool copy_to_local)
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
            threads.emplace_back([root, worker, per_worker, copy_to_local, &sink, &ok,
                                  batch_size = config.batch_size]() {
                std::uint64_t local = 0;
                const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
                for (std::size_t offset = 0; offset < per_worker; offset += batch_size)
                {
                    const std::size_t count = std::min(batch_size, per_worker - offset);
                    root->map.with_lock([&](Map& map) {
                        for (std::size_t i = 0; i < count; ++i)
                        {
                            auto it = map.find(static_cast<Key>(base + offset + i));
                            if (it == map.end())
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
                    });
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
        std::cerr << "[shm_next map batch access perf] thread read failed" << std::endl;
        return false;
    }

    print_result(copy_to_local ? "thread_batch_fetch_copy" : "thread_batch_find",
                 copy_to_local ? "find+memcpy" : "find", "thread", config.workers, operations,
                 config.batch_size, elapsed_ms);
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
            threads.emplace_back([root, worker, per_worker, &ok, batch_size = config.batch_size]() {
                const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
                for (std::size_t offset = 0; offset < per_worker; offset += batch_size)
                {
                    const std::size_t count = std::min(batch_size, per_worker - offset);
                    root->map.with_lock([&](Map& map) {
                        for (std::size_t i = 0; i < count; ++i)
                        {
                            auto it = map.find(static_cast<Key>(base + offset + i));
                            if (it == map.end())
                            {
                                ok.store(false, std::memory_order_relaxed);
                                continue;
                            }
                            it->second = make_record(base + offset + i + 1000000);
                        }
                    });
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
        std::cerr << "[shm_next map batch access perf] thread update failed" << std::endl;
        return false;
    }

    print_result("thread_batch_update_existing", "find+assign", "thread", config.workers,
                 operations, config.batch_size, elapsed_ms);
    return true;
}

int process_read_child(SharedRoot* root, int worker, std::size_t per_worker, std::size_t batch_size,
                       bool copy_to_local)
{
    volatile std::uint64_t sink = 0;
    const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
    for (std::size_t offset = 0; offset < per_worker; offset += batch_size)
    {
        const std::size_t count = std::min(batch_size, per_worker - offset);
        bool ok = root->map.with_lock([&](Map& map) {
            for (std::size_t i = 0; i < count; ++i)
            {
                auto it = map.find(static_cast<Key>(base + offset + i));
                if (it == map.end())
                {
                    return false;
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
            return true;
        });
        if (!ok)
        {
            return 2;
        }
    }
    return sink == 0 ? 3 : 0;
}

int process_update_child(SharedRoot* root, int worker, std::size_t per_worker,
                         std::size_t batch_size)
{
    const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
    for (std::size_t offset = 0; offset < per_worker; offset += batch_size)
    {
        const std::size_t count = std::min(batch_size, per_worker - offset);
        bool ok = root->map.with_lock([&](Map& map) {
            for (std::size_t i = 0; i < count; ++i)
            {
                auto it = map.find(static_cast<Key>(base + offset + i));
                if (it == map.end())
                {
                    return false;
                }
                it->second = make_record(base + offset + i + 1000000);
            }
            return true;
        });
        if (!ok)
        {
            return 2;
        }
    }
    return 0;
}

bool run_process_read(SharedRoot* root, const Config& config, bool copy_to_local)
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
                _exit(
                    process_read_child(root, worker, per_worker, config.batch_size, copy_to_local));
            }
            children.push_back(pid);
        }
        if (!wait_children(children))
        {
            std::cerr << "[shm_next map batch access perf] process read child failed" << std::endl;
            std::exit(2);
        }
    });

    print_result(copy_to_local ? "process_batch_fetch_copy" : "process_batch_find",
                 copy_to_local ? "find+memcpy" : "find", "process", config.workers, operations,
                 config.batch_size, elapsed_ms);
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
                _exit(process_update_child(root, worker, per_worker, config.batch_size));
            }
            children.push_back(pid);
        }
        if (!wait_children(children))
        {
            std::cerr << "[shm_next map batch access perf] process update child failed"
                      << std::endl;
            std::exit(2);
        }
    });

    print_result("process_batch_update_existing", "find+assign", "process", config.workers,
                 operations, config.batch_size, elapsed_ms);
    return true;
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
        else if (const char* value = parse_value("--batch-size="))
        {
            config.batch_size = std::max<std::size_t>(
                1, static_cast<std::size_t>(std::strtoull(value, nullptr, 10)));
        }
    }
    return config;
}

} // namespace

int main(int argc, char** argv)
{
    Config config = parse_config(argc, argv);
    const std::string name = "shm_map_batch_" + std::to_string(getpid());
    ManagedSharedMemory::remove(name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, name.c_str(), config.shm_size);
        auto allocator = segment.get_allocator<std::pair<const Key, Record>>();
        SharedRoot* root = segment.construct<SharedRoot>("root", allocator);
        if (!root)
        {
            std::cerr << "[shm_next map batch access perf] failed to construct root" << std::endl;
            ManagedSharedMemory::remove(name.c_str());
            return 1;
        }

        bool ok = true;
        ok = run_thread_read(root, config, false) && ok;
        ok = run_thread_read(root, config, true) && ok;
        ok = run_thread_update(root, config) && ok;
        ok = run_process_read(root, config, false) && ok;
        ok = run_process_read(root, config, true) && ok;
        ok = run_process_update(root, config) && ok;

        segment.destroy<SharedRoot>("root");
        ManagedSharedMemory::remove(name.c_str());
        return ok ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[shm_next map batch access perf] exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(name.c_str());
        return 1;
    }
}
