#include "interprocess/container/shared_memory_vector.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_shared_mutex.h"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <shared_mutex>
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

using Vector = SharedMemoryVector<Record>;
using VectorAllocator = SharedMemoryAllocator<Record>;

struct SharedRoot
{
    InterprocessSharedMutex mutex;
    Vector vector;

    explicit SharedRoot(const VectorAllocator& allocator) : vector(allocator)
    {
    }
};

struct Config
{
    int workers = 4;
    std::size_t operations = 20000;
    std::size_t shm_size = 64 * 1024 * 1024;
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
    std::cout << "RESULT project=shm_next container=vector scenario=" << scenario << " api=" << api
              << " mode=" << mode << " workers=" << workers << " ops=" << operations
              << " record_bytes=" << record_bytes << " lock=external_interprocess_shared_mutex"
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

void prepare_vector(SharedRoot* root, std::size_t operations)
{
    std::unique_lock<InterprocessSharedMutex> lock(root->mutex);
    root->vector.clear();
    root->vector.reserve(operations);
    for (std::size_t i = 0; i < operations; ++i)
    {
        root->vector.push_back(make_record(i));
    }
}

bool run_thread_push(SharedRoot* root, const Config& config)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    {
        std::unique_lock<InterprocessSharedMutex> lock(root->mutex);
        root->vector.clear();
        root->vector.reserve(operations);
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
                    std::unique_lock<InterprocessSharedMutex> lock(root->mutex);
                    root->vector.push_back(make_record(base + i));
                }
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }
    });

    if (root->vector.size() != operations)
    {
        std::cerr << "[shm_next vector shared_mutex perf] thread push size mismatch" << std::endl;
        return false;
    }
    print_result("thread_push_back", "push_back", "thread", config.workers, operations, elapsed_ms);
    return true;
}

bool run_thread_read(SharedRoot* root, const Config& config, bool use_at, bool copy_to_local)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    prepare_vector(root, operations);

    std::atomic<std::uint64_t> sink{0};
    const double elapsed_ms = measure_ms([&] {
        std::vector<std::thread> threads;
        threads.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            threads.emplace_back([root, worker, per_worker, use_at, copy_to_local, &sink]() {
                std::uint64_t local = 0;
                const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
                for (std::size_t i = 0; i < per_worker; ++i)
                {
                    std::shared_lock<InterprocessSharedMutex> lock(root->mutex);
                    const std::size_t index = base + i;
                    if (copy_to_local)
                    {
                        Record record{};
                        std::memcpy(&record, &root->vector[index], sizeof(record));
                        local += record.sequence;
                    }
                    else
                    {
                        local +=
                            use_at ? root->vector.at(index).sequence : root->vector[index].sequence;
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
        std::cerr << "[shm_next vector shared_mutex perf] thread read sink was zero" << std::endl;
        return false;
    }
    if (copy_to_local)
    {
        print_result("thread_read_copy", "operator[]+memcpy", "thread", config.workers, operations,
                     elapsed_ms);
    }
    else
    {
        print_result(use_at ? "thread_read_at" : "thread_read_index", use_at ? "at" : "operator[]",
                     "thread", config.workers, operations, elapsed_ms);
    }
    return true;
}

int process_push_child(SharedRoot* root, int worker, std::size_t per_worker)
{
    const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
    for (std::size_t i = 0; i < per_worker; ++i)
    {
        std::unique_lock<InterprocessSharedMutex> lock(root->mutex);
        root->vector.push_back(make_record(base + i));
    }
    return 0;
}

int process_read_child(SharedRoot* root, int worker, std::size_t per_worker, bool use_at,
                       bool copy_to_local)
{
    volatile std::uint64_t sink = 0;
    const std::size_t base = static_cast<std::size_t>(worker) * per_worker;
    for (std::size_t i = 0; i < per_worker; ++i)
    {
        std::shared_lock<InterprocessSharedMutex> lock(root->mutex);
        const std::size_t index = base + i;
        if (copy_to_local)
        {
            Record record{};
            std::memcpy(&record, &root->vector[index], sizeof(record));
            sink += record.sequence;
        }
        else
        {
            sink += use_at ? root->vector.at(index).sequence : root->vector[index].sequence;
        }
    }
    return sink == 0 ? 2 : 0;
}

bool run_process_push(SharedRoot* root, const Config& config)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    {
        std::unique_lock<InterprocessSharedMutex> lock(root->mutex);
        root->vector.clear();
        root->vector.reserve(operations);
    }

    const double elapsed_ms = measure_ms([&] {
        std::vector<pid_t> children;
        children.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                _exit(process_push_child(root, worker, per_worker));
            }
            children.push_back(pid);
        }
        if (!wait_children(children))
        {
            std::cerr << "[shm_next vector shared_mutex perf] process push child failed"
                      << std::endl;
            std::exit(1);
        }
    });

    if (root->vector.size() != operations)
    {
        std::cerr << "[shm_next vector shared_mutex perf] process push size mismatch" << std::endl;
        return false;
    }
    print_result("process_push_back", "push_back", "process", config.workers, operations,
                 elapsed_ms);
    return true;
}

bool run_process_read(SharedRoot* root, const Config& config, bool use_at, bool copy_to_local)
{
    const std::size_t operations = adjusted_operations(config.operations, config.workers);
    const std::size_t per_worker = operations / static_cast<std::size_t>(config.workers);
    prepare_vector(root, operations);

    const double elapsed_ms = measure_ms([&] {
        std::vector<pid_t> children;
        children.reserve(config.workers);
        for (int worker = 0; worker < config.workers; ++worker)
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                _exit(process_read_child(root, worker, per_worker, use_at, copy_to_local));
            }
            children.push_back(pid);
        }
        if (!wait_children(children))
        {
            std::cerr << "[shm_next vector shared_mutex perf] process read child failed"
                      << std::endl;
            std::exit(1);
        }
    });

    if (copy_to_local)
    {
        print_result("process_read_copy", "operator[]+memcpy", "process", config.workers,
                     operations, elapsed_ms);
    }
    else
    {
        print_result(use_at ? "process_read_at" : "process_read_index",
                     use_at ? "at" : "operator[]", "process", config.workers, operations,
                     elapsed_ms);
    }
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
    }
    return config;
}

} // namespace

int main(int argc, char** argv)
{
    if (!InterprocessSharedMutex::process_shared_supported())
    {
        std::cout << "[shm_next vector shared_mutex perf] SKIPPED: process-shared pthread rwlock "
                     "is not supported"
                  << std::endl;
        return 0;
    }

    Config config = parse_config(argc, argv);
    const std::string name = "shm_vec_rw_" + std::to_string(getpid());
    ManagedSharedMemory::remove(name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, name.c_str(), config.shm_size);
        auto allocator = segment.get_allocator<Record>();
        SharedRoot* root = segment.construct<SharedRoot>("root", allocator);
        if (!root)
        {
            std::cerr << "[shm_next vector shared_mutex perf] failed to construct root"
                      << std::endl;
            ManagedSharedMemory::remove(name.c_str());
            return 1;
        }

        bool ok = true;
        ok = run_thread_push(root, config) && ok;
        ok = run_thread_read(root, config, true, false) && ok;
        ok = run_thread_read(root, config, false, false) && ok;
        ok = run_thread_read(root, config, false, true) && ok;
        ok = run_process_push(root, config) && ok;
        ok = run_process_read(root, config, true, false) && ok;
        ok = run_process_read(root, config, false, false) && ok;
        ok = run_process_read(root, config, false, true) && ok;

        segment.destroy<SharedRoot>("root");
        ManagedSharedMemory::remove(name.c_str());
        return ok ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[shm_next vector shared_mutex perf] exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(name.c_str());
        return 1;
    }
}
