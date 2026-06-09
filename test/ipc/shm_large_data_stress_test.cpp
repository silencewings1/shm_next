#include "interprocess/allocator/offset_ptr.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_mutex.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace interprocess;

namespace
{

constexpr const char* metadata_name = "LargeDataMetadata";
constexpr std::uint64_t default_seed = 0x9e3779b97f4a7c15ull;
constexpr std::size_t default_chunk_size = 64ull * 1024ull * 1024ull;
constexpr std::size_t default_payload_size = 256ull * 1024ull * 1024ull;
constexpr std::size_t segment_slack_bytes = 16ull * 1024ull * 1024ull;

enum class DataState : std::uint32_t
{
    empty = 0,
    writing = 1,
    ready = 2,
    failed = 3
};

struct LargeDataMetadata
{
    InterprocessMutex mutex;
    OffsetPtr<std::uint8_t> payload;
    OffsetPtr<std::uint64_t> chunk_checksums;
    std::uint64_t payload_size;
    std::uint64_t chunk_size;
    std::uint64_t chunk_count;
    std::uint64_t seed;
    std::uint64_t global_checksum;
    std::uint64_t bytes_written;
    std::uint32_t state;
    std::uint32_t reserved;

    LargeDataMetadata()
        : mutex(), payload(nullptr), chunk_checksums(nullptr), payload_size(0), chunk_size(0),
          chunk_count(0), seed(default_seed), global_checksum(0), bytes_written(0),
          state(static_cast<std::uint32_t>(DataState::empty)), reserved(0)
    {
    }
};

struct Config
{
    std::size_t payload_size = default_payload_size;
    std::size_t chunk_size = default_chunk_size;
    int readers = 2;
    int writers = 1;
    std::string name = "shm_large_data_" + std::to_string(getpid());
    bool keep_on_failure = false;
};

struct ChunkResult
{
    std::uint64_t checksum = 0;
    bool pattern_ok = true;
};

std::uint32_t load_u32(const std::uint32_t* ptr) noexcept
{
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void store_u32(std::uint32_t* ptr, std::uint32_t value) noexcept
{
    __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

std::uint64_t load_u64(const std::uint64_t* ptr) noexcept
{
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void store_u64(std::uint64_t* ptr, std::uint64_t value) noexcept
{
    __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

std::uint64_t pattern_word(std::uint64_t seed, std::uint64_t word_index) noexcept
{
    return seed ^ (word_index * 0x9e3779b97f4a7c15ull) ^ (word_index << 17) ^
           (word_index >> 11);
}

std::uint64_t combine_checksum(std::uint64_t checksum, std::uint64_t value) noexcept
{
    checksum ^= value + 0x9e3779b97f4a7c15ull + (checksum << 6) + (checksum >> 2);
    return checksum;
}

ChunkResult process_chunk(std::uint8_t* data, std::uint64_t offset, std::size_t length,
                          std::uint64_t seed, bool write_pattern)
{
    ChunkResult result{0xcbf29ce484222325ull ^ offset, true};

    const std::size_t word_count = length / sizeof(std::uint64_t);
    auto* words = reinterpret_cast<std::uint64_t*>(data);
    const std::uint64_t first_word_index = offset / sizeof(std::uint64_t);

    for (std::size_t i = 0; i < word_count; ++i)
    {
        const std::uint64_t expected = pattern_word(seed, first_word_index + i);
        if (write_pattern)
        {
            words[i] = expected;
            result.checksum = combine_checksum(result.checksum, expected);
        }
        else
        {
            const std::uint64_t actual = words[i];
            if (actual != expected)
            {
                result.pattern_ok = false;
            }
            result.checksum = combine_checksum(result.checksum, actual);
        }
    }

    const std::size_t tail_begin = word_count * sizeof(std::uint64_t);
    if (tail_begin < length)
    {
        const std::uint64_t expected_word = pattern_word(seed, first_word_index + word_count);
        const auto* expected_bytes = reinterpret_cast<const std::uint8_t*>(&expected_word);
        for (std::size_t i = tail_begin; i < length; ++i)
        {
            const std::uint8_t expected = expected_bytes[i - tail_begin];
            if (write_pattern)
            {
                data[i] = expected;
                result.checksum = combine_checksum(result.checksum, expected);
            }
            else
            {
                if (data[i] != expected)
                {
                    result.pattern_ok = false;
                }
                result.checksum = combine_checksum(result.checksum, data[i]);
            }
        }
    }

    return result;
}

std::uint64_t parse_size(const std::string& text)
{
    if (text.empty())
    {
        throw std::invalid_argument("empty size");
    }

    std::size_t suffix_pos = text.size();
    while (suffix_pos > 0 && std::isalpha(static_cast<unsigned char>(text[suffix_pos - 1])))
    {
        --suffix_pos;
    }
    if (suffix_pos == 0)
    {
        throw std::invalid_argument("size must start with a number: " + text);
    }

    const std::uint64_t value = std::stoull(text.substr(0, suffix_pos));
    std::string suffix = text.substr(suffix_pos);
    for (char& ch : suffix)
    {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }

    std::uint64_t multiplier = 1;
    if (suffix.empty() || suffix == "B")
    {
        multiplier = 1;
    }
    else if (suffix == "K" || suffix == "KB" || suffix == "KIB")
    {
        multiplier = 1024ull;
    }
    else if (suffix == "M" || suffix == "MB" || suffix == "MIB")
    {
        multiplier = 1024ull * 1024ull;
    }
    else if (suffix == "G" || suffix == "GB" || suffix == "GIB")
    {
        multiplier = 1024ull * 1024ull * 1024ull;
    }
    else
    {
        throw std::invalid_argument("unsupported size suffix: " + suffix);
    }

    if (value > std::numeric_limits<std::uint64_t>::max() / multiplier)
    {
        throw std::overflow_error("size overflows uint64_t: " + text);
    }
    return value * multiplier;
}

void print_usage(const char* program)
{
    std::cerr << "Usage: " << program
              << " [--size 1G|10G|bytes] [--chunk 64M] [--readers N] [--writers 1]"
                 " [--name shm_name] [--keep-on-failure]"
              << std::endl;
}

Config parse_args(int argc, char** argv)
{
    Config config;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto require_value = [&](const char* option) -> std::string {
            if (i + 1 >= argc)
            {
                throw std::invalid_argument(std::string(option) + " requires a value");
            }
            return argv[++i];
        };

        if (arg == "--size")
        {
            config.payload_size = static_cast<std::size_t>(parse_size(require_value("--size")));
        }
        else if (arg == "--chunk")
        {
            config.chunk_size = static_cast<std::size_t>(parse_size(require_value("--chunk")));
        }
        else if (arg == "--readers")
        {
            config.readers = std::stoi(require_value("--readers"));
        }
        else if (arg == "--writers")
        {
            config.writers = std::stoi(require_value("--writers"));
        }
        else if (arg == "--name")
        {
            config.name = require_value("--name");
        }
        else if (arg == "--keep-on-failure")
        {
            config.keep_on_failure = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            print_usage(argv[0]);
            std::exit(0);
        }
        else
        {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }

    if (config.payload_size == 0)
    {
        throw std::invalid_argument("--size must be greater than zero");
    }
    if (config.chunk_size == 0 || config.chunk_size % sizeof(std::uint64_t) != 0)
    {
        throw std::invalid_argument("--chunk must be greater than zero and 8-byte aligned");
    }
    if (config.readers < 0 || config.readers > 64)
    {
        throw std::invalid_argument("--readers must be in [0, 64]");
    }
    if (config.writers != 1)
    {
        throw std::invalid_argument("only --writers 1 is currently supported");
    }
    if (config.name.empty())
    {
        throw std::invalid_argument("--name must not be empty");
    }
    return config;
}

void print_storage_status(const char* label)
{
    std::cout << "[LargeData] Storage status " << label << ':' << std::endl;
    (void)std::system("df -h / /tmp . 2>/dev/null || true; ls -lh /dev/shm 2>/dev/null || true");
}

std::size_t chunk_count_for(std::size_t payload_size, std::size_t chunk_size)
{
    return (payload_size + chunk_size - 1) / chunk_size;
}

std::size_t segment_size_for(std::size_t payload_size, std::size_t chunk_count)
{
    const std::size_t checksum_bytes = chunk_count * sizeof(std::uint64_t);
    if (payload_size > std::numeric_limits<std::size_t>::max() - checksum_bytes -
                           segment_slack_bytes)
    {
        throw std::overflow_error("segment size overflow");
    }
    return payload_size + checksum_bytes + segment_slack_bytes;
}

void publish_failed(LargeDataMetadata* metadata) noexcept
{
    try
    {
        metadata->mutex.lock();
        store_u32(&metadata->state, static_cast<std::uint32_t>(DataState::failed));
        metadata->mutex.unlock();
    }
    catch (...)
    {
    }
}

int writer_main(const std::string& name)
{
    try
    {
        ManagedSharedMemory segment(open_only, name.c_str());
        LargeDataMetadata* metadata = segment.find<LargeDataMetadata>(metadata_name);
        if (metadata == nullptr || metadata->payload.get() == nullptr ||
            metadata->chunk_checksums.get() == nullptr)
        {
            std::cerr << "[LargeData Writer] metadata is incomplete" << std::endl;
            return 2;
        }

        metadata->mutex.lock();
        try
        {
            store_u32(&metadata->state, static_cast<std::uint32_t>(DataState::writing));
            store_u64(&metadata->bytes_written, 0);

            std::uint64_t global_checksum = 0xcbf29ce484222325ull;
            auto* payload = metadata->payload.get();
            auto* chunk_checksums = metadata->chunk_checksums.get();
            for (std::uint64_t chunk = 0; chunk < metadata->chunk_count; ++chunk)
            {
                const std::uint64_t offset = chunk * metadata->chunk_size;
                const std::size_t length = static_cast<std::size_t>(
                    std::min<std::uint64_t>(metadata->chunk_size, metadata->payload_size - offset));
                const ChunkResult result =
                    process_chunk(payload + offset, offset, length, metadata->seed, true);
                store_u64(&chunk_checksums[chunk], result.checksum);
                global_checksum = combine_checksum(global_checksum, result.checksum);
                store_u64(&metadata->bytes_written, offset + length);
            }

            store_u64(&metadata->global_checksum, global_checksum);
            store_u32(&metadata->state, static_cast<std::uint32_t>(DataState::ready));
            metadata->mutex.unlock();
            return 0;
        }
        catch (...)
        {
            store_u32(&metadata->state, static_cast<std::uint32_t>(DataState::failed));
            metadata->mutex.unlock();
            throw;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[LargeData Writer] exception: " << e.what() << std::endl;
        return 3;
    }
}

bool wait_until_ready(LargeDataMetadata* metadata)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (metadata->mutex.try_lock_for(std::chrono::milliseconds(50)))
        {
            const auto state = static_cast<DataState>(load_u32(&metadata->state));
            metadata->mutex.unlock();
            if (state == DataState::ready)
            {
                return true;
            }
            if (state == DataState::failed)
            {
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int reader_main(const std::string& name, int reader_index)
{
    try
    {
        ManagedSharedMemory segment(open_only, name.c_str());
        LargeDataMetadata* metadata = segment.find<LargeDataMetadata>(metadata_name);
        if (metadata == nullptr)
        {
            std::cerr << "[LargeData Reader " << reader_index << "] metadata not found"
                      << std::endl;
            return 2;
        }
        if (!wait_until_ready(metadata))
        {
            std::cerr << "[LargeData Reader " << reader_index << "] data did not become ready"
                      << std::endl;
            return 3;
        }

        auto* payload = metadata->payload.get();
        auto* chunk_checksums = metadata->chunk_checksums.get();
        if (payload == nullptr || chunk_checksums == nullptr)
        {
            std::cerr << "[LargeData Reader " << reader_index << "] null payload/checksum"
                      << std::endl;
            return 4;
        }

        std::uint64_t global_checksum = 0xcbf29ce484222325ull;
        for (std::uint64_t chunk = 0; chunk < metadata->chunk_count; ++chunk)
        {
            const std::uint64_t offset = chunk * metadata->chunk_size;
            const std::size_t length = static_cast<std::size_t>(
                std::min<std::uint64_t>(metadata->chunk_size, metadata->payload_size - offset));
            const ChunkResult result =
                process_chunk(payload + offset, offset, length, metadata->seed, false);
            const std::uint64_t expected_checksum = load_u64(&chunk_checksums[chunk]);
            if (!result.pattern_ok || result.checksum != expected_checksum)
            {
                std::cerr << "[LargeData Reader " << reader_index << "] chunk mismatch at "
                          << chunk << ": got checksum " << result.checksum << ", expected "
                          << expected_checksum << std::endl;
                return 5;
            }
            global_checksum = combine_checksum(global_checksum, result.checksum);
        }

        const std::uint64_t expected_global = load_u64(&metadata->global_checksum);
        if (global_checksum != expected_global)
        {
            std::cerr << "[LargeData Reader " << reader_index << "] global checksum mismatch: got "
                      << global_checksum << ", expected " << expected_global << std::endl;
            return 6;
        }
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[LargeData Reader " << reader_index << "] exception: " << e.what()
                  << std::endl;
        return 7;
    }
}

pid_t fork_child_or_throw()
{
    pid_t pid = fork();
    if (pid == -1)
    {
        throw std::system_error(errno, std::system_category(), "fork failed");
    }
    return pid;
}

bool wait_for_children(const std::vector<pid_t>& children)
{
    bool ok = true;
    for (pid_t child : children)
    {
        int status = 0;
        if (waitpid(child, &status, 0) == -1)
        {
            std::cerr << "[LargeData] waitpid failed for child " << child << std::endl;
            ok = false;
            continue;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            std::cerr << "[LargeData] child " << child << " failed";
            if (WIFEXITED(status))
            {
                std::cerr << " with exit code " << WEXITSTATUS(status);
            }
            std::cerr << std::endl;
            ok = false;
        }
    }
    return ok;
}

bool cleanup_segment(ManagedSharedMemory& segment, LargeDataMetadata* metadata)
{
    bool ok = true;
    try
    {
        SharedMemoryManager* manager = segment.get_segment_manager();
        void* payload = metadata != nullptr ? metadata->payload.get() : nullptr;
        void* checksums = metadata != nullptr ? metadata->chunk_checksums.get() : nullptr;
        if (metadata != nullptr)
        {
            metadata->payload = nullptr;
            metadata->chunk_checksums = nullptr;
        }
        manager->deallocate(payload);
        manager->deallocate(checksums);
        if (metadata != nullptr)
        {
            ok = segment.destroy<LargeDataMetadata>(metadata_name) && ok;
        }
        ok = manager->check_sanity() && ok;
        ok = manager->all_memory_deallocated() && ok;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[LargeData] cleanup exception: " << e.what() << std::endl;
        ok = false;
    }
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    Config config;
    try
    {
        config = parse_args(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::cerr << "[LargeData] invalid arguments: " << e.what() << std::endl;
        print_usage(argv[0]);
        return 2;
    }

    const std::size_t chunk_count = chunk_count_for(config.payload_size, config.chunk_size);
    const std::size_t segment_size = segment_size_for(config.payload_size, chunk_count);

    std::cout << "[LargeData] shm name: " << config.name << std::endl;
    std::cout << "[LargeData] payload_size=" << config.payload_size
              << " chunk_size=" << config.chunk_size << " chunk_count=" << chunk_count
              << " readers=" << config.readers << " segment_size=" << segment_size
              << std::endl;
    print_storage_status("before");

    ManagedSharedMemory::remove(config.name.c_str());

    bool success = false;
    try
    {
        ManagedSharedMemory segment(create_only, config.name.c_str(), segment_size);
        SharedMemoryManager* manager = segment.get_segment_manager();

        auto* payload = static_cast<std::uint8_t*>(manager->allocate(config.payload_size, 64));
        auto* checksums = static_cast<std::uint64_t*>(
            manager->allocate(chunk_count * sizeof(std::uint64_t), alignof(std::uint64_t)));
        std::memset(checksums, 0, chunk_count * sizeof(std::uint64_t));

        LargeDataMetadata* metadata = segment.construct<LargeDataMetadata>(metadata_name);
        if (metadata == nullptr)
        {
            throw std::runtime_error("failed to construct metadata");
        }
        metadata->payload = payload;
        metadata->chunk_checksums = checksums;
        metadata->payload_size = config.payload_size;
        metadata->chunk_size = config.chunk_size;
        metadata->chunk_count = chunk_count;
        metadata->seed = default_seed ^ static_cast<std::uint64_t>(config.payload_size);
        metadata->global_checksum = 0;
        metadata->bytes_written = 0;
        store_u32(&metadata->state, static_cast<std::uint32_t>(DataState::empty));

        std::vector<pid_t> children;
        children.reserve(static_cast<std::size_t>(config.readers + 1));
        for (int reader = 0; reader < config.readers; ++reader)
        {
            pid_t pid = fork_child_or_throw();
            if (pid == 0)
            {
                _exit(reader_main(config.name, reader));
            }
            children.push_back(pid);
        }

        pid_t writer = fork_child_or_throw();
        if (writer == 0)
        {
            _exit(writer_main(config.name));
        }
        children.push_back(writer);

        const bool children_ok = wait_for_children(children);
        if (!children_ok)
        {
            publish_failed(metadata);
        }

        const bool ready = static_cast<DataState>(load_u32(&metadata->state)) == DataState::ready;
        const bool all_written = load_u64(&metadata->bytes_written) == config.payload_size;
        const bool sane = manager->check_sanity();
        const bool cleaned = cleanup_segment(segment, metadata);

        success = children_ok && ready && all_written && sane && cleaned;
        if (!success)
        {
            std::cerr << "[LargeData] validation failed: children_ok=" << children_ok
                      << " ready=" << ready << " all_written=" << all_written
                      << " sane=" << sane << " cleaned=" << cleaned << std::endl;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[LargeData] exception: " << e.what() << std::endl;
        success = false;
    }

    if (success || !config.keep_on_failure)
    {
        if (ManagedSharedMemory::remove(config.name.c_str()))
        {
            std::cout << "[LargeData] removed shm: " << config.name << std::endl;
        }
        else
        {
            std::cout << "[LargeData] shm already removed or could not be unlinked: " << config.name
                      << std::endl;
        }
    }
    else
    {
        std::cout << "[LargeData] keeping shm for debugging: " << config.name << std::endl;
    }

    print_storage_status("after");
    std::cout << "[LargeData] " << (success ? "SUCCESS" : "FAILED") << std::endl;
    return success ? 0 : 1;
}
