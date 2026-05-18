#include "../interprocess/container/shared_memory_hash_map.h"
#include "../interprocess/container/shared_memory_list.h"
#include "../interprocess/container/shared_memory_string.h"
#include "../interprocess/container/shared_memory_vector.h"
#include "../interprocess/ipc/managed_shared_memory.h"
#include "../interprocess/sync/posix_mutex.h"
#include <iostream>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace interprocess;

namespace
{

using ShmString = SharedMemoryString;
using StringAllocator = SharedMemoryAllocator<char>;
using IntList = SharedMemoryList<int>;
using IntListAllocator = SharedMemoryAllocator<int>;
using IntVector = SharedMemoryVector<int>;
using IntVectorAllocator = SharedMemoryAllocator<int>;
using HashMap = SharedMemoryHashMap<ShmString, IntVector>;
using HashMapAllocator = SharedMemoryAllocator<std::pair<const ShmString, IntVector>>;

struct SharedRoot
{
    InterprocessMutex mutex;
    IntList numbers;
    HashMap lookup;

    SharedRoot(const IntListAllocator& list_allocator, const HashMapAllocator& map_allocator)
        : numbers(list_allocator), lookup(map_allocator)
    {
    }
};

bool require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "[List Hash Cross Process] " << message << std::endl;
        return false;
    }
    return true;
}

int run_child(const char* shm_name)
{
    try
    {
        ManagedSharedMemory segment(open_only, shm_name);
        SharedRoot* root = segment.find<SharedRoot>("RootObject");
        if (!require(root != nullptr, "child failed to find root"))
        {
            return 1;
        }

        auto char_allocator = segment.get_allocator<char>();

        std::lock_guard<InterprocessMutex> lock(root->mutex);

        if (!require(root->numbers.size() == 5, "numbers list size mismatch"))
        {
            return 1;
        }

        const int expected_list[] = {1, 3, 5, 7, 9};
        std::size_t index = 0;
        for (IntList::const_iterator it = root->numbers.cbegin(); it != root->numbers.cend();
             ++it, ++index)
        {
            if (!require(index < 5 && *it == expected_list[index], "numbers list order mismatch"))
            {
                return 1;
            }
        }

        HashMap::iterator odd = root->lookup.find(ShmString("odd", char_allocator));
        HashMap::iterator even = root->lookup.find(ShmString("even", char_allocator));
        if (!require(odd != root->lookup.end() && even != root->lookup.end(),
                     "expected hash map keys not found"))
        {
            return 1;
        }

        if (!require(odd->second.size() == 3 && odd->second[0] == 1 && odd->second[2] == 5,
                     "odd vector mismatch"))
        {
            return 1;
        }

        if (!require(even->second.size() == 2 && even->second[0] == 2 && even->second[1] == 4,
                     "even vector mismatch"))
        {
            return 1;
        }

        const std::size_t odd_bucket = root->lookup.bucket(ShmString("odd", char_allocator));
        bool bucket_contains_odd = false;
        for (auto it = root->lookup.begin(odd_bucket); it != root->lookup.end(odd_bucket); ++it)
        {
            if (it->first == "odd")
            {
                bucket_contains_odd = true;
            }
        }

        if (!require(bucket_contains_odd, "bucket-local iteration failed to find odd key"))
        {
            return 1;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[List Hash Cross Process] child exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

} // namespace

int main()
{
    const std::string shm_name = "shmlhxp_" + std::to_string(getpid());
    ManagedSharedMemory::remove(shm_name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, shm_name.c_str(), 256 * 1024);
        SharedRoot* root = segment.construct<SharedRoot>(
            "RootObject", segment.get_allocator<int>(),
            segment.get_allocator<std::pair<const ShmString, IntVector>>());
        if (!require(root != nullptr, "failed to construct root"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        auto char_allocator = segment.get_allocator<char>();
        auto int_allocator = segment.get_allocator<int>();

        {
            std::lock_guard<InterprocessMutex> lock(root->mutex);

            root->numbers.assign({9, 7, 5, 3, 1});
            root->numbers.sort();

            IntVector odd(int_allocator);
            odd.push_back(1);
            odd.push_back(3);
            odd.push_back(5);

            IntVector even(int_allocator);
            even.push_back(2);
            even.push_back(4);

            root->lookup.emplace(ShmString("odd", char_allocator), std::move(odd));
            root->lookup.emplace(ShmString("even", char_allocator), std::move(even));
        }

        pid_t pid = fork();
        if (pid == -1)
        {
            std::cerr << "[List Hash Cross Process] fork failed" << std::endl;
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        if (pid == 0)
        {
            _exit(run_child(shm_name.c_str()));
        }

        int status = 0;
        if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            std::cerr << "[List Hash Cross Process] child failed" << std::endl;
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        if (!require(segment.get_segment_manager()->check_sanity(),
                     "manager sanity failed after cross-process validation"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        segment.destroy<SharedRoot>("RootObject");
        ManagedSharedMemory::remove(shm_name.c_str());
        std::cout << "[List Hash Cross Process] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[List Hash Cross Process] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name.c_str());
        return 1;
    }
}
