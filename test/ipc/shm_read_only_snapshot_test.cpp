#include "interprocess/container/shared_memory_string.h"
#include "interprocess/container/shared_memory_vector.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace interprocess;

namespace
{

using IntVector = SharedMemoryVector<int>;
using StringAllocator = SharedMemoryAllocator<char>;
using IntVectorAllocator = SharedMemoryAllocator<int>;

struct SharedRoot
{
    int version;
    SharedMemoryString message;
    IntVector values;

    SharedRoot(const StringAllocator& string_allocator, const IntVectorAllocator& vector_allocator)
        : version(0), message(string_allocator), values(vector_allocator)
    {
    }
};

bool require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "[Read Only Snapshot] " << message << std::endl;
        return false;
    }
    return true;
}

template <typename Func>
bool throws_runtime_error(Func&& func)
{
    try
    {
        func();
    }
    catch (const std::runtime_error&)
    {
        return true;
    }
    catch (...)
    {
    }
    return false;
}

int run_read_only_child(const char* shm_name)
{
    try
    {
        ManagedSharedMemory segment(open_read_only, shm_name);
        const SharedRoot* root = segment.find_read_only<SharedRoot>("RootObject");
        if (!require(segment.is_read_only(), "segment should report read-only mode") ||
            !require(root != nullptr, "root object not found from read-only mapping") ||
            !require(root->version == 7, "version mismatch") ||
            !require(root->message == "stable snapshot", "message mismatch") ||
            !require(root->values.size() == 16, "value vector size mismatch"))
        {
            return 1;
        }

        for (std::size_t i = 0; i < root->values.size(); ++i)
        {
            if (!require(root->values[i] == static_cast<int>(i * i), "value mismatch"))
            {
                return 1;
            }
        }

        std::size_t visits = 0;
        segment.for_each_named_object_read_only(
            [&](const char* name, const void* object, std::size_t instance_count) {
                if (std::string(name) == "RootObject" && object == root && instance_count == 1)
                {
                    ++visits;
                }
            });
        if (!require(visits == 1, "read-only iteration should visit root once"))
        {
            return 1;
        }

        if (!require(segment.get_segment_manager_read_only() != nullptr,
                     "read-only manager accessor should be available"))
        {
            return 1;
        }
        if (!require(segment.get_num_named_objects() == 1, "read-only named count mismatch"))
        {
            return 1;
        }
        if (!require(throws_runtime_error([&] { (void)segment.find_read_only<int>("RootObject"); }),
                     "read-only find should reject mismatched named object type"))
        {
            return 1;
        }
        if (!require(throws_runtime_error([&] {
                         std::size_t count = 0;
                         (void)segment.find_array_read_only<int>("RootObject", &count);
                     }),
                     "read-only find_array should reject mismatched named object type"))
        {
            return 1;
        }
        if (!require(segment.get_free_memory() > 0, "read-only free memory should be observable"))
        {
            return 1;
        }

        if (!require(throws_runtime_error([&] { (void)segment.construct<int>("Blocked", 1); }),
                     "read-only construct should throw") ||
            !require(throws_runtime_error([&] { (void)segment.find<SharedRoot>("RootObject"); }),
                     "read-only mutable find should throw") ||
            !require(throws_runtime_error([&] { (void)segment.get_allocator<int>(); }),
                     "read-only get_allocator should throw") ||
            !require(throws_runtime_error([&] { (void)segment.get_segment_manager(); }),
                     "read-only mutable manager accessor should throw"))
        {
            return 1;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Read Only Snapshot] child exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

} // namespace

int main()
{
    const std::string shm_name = "shmro_" + std::to_string(getpid());

    ManagedSharedMemory::remove(shm_name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, shm_name.c_str(), 128 * 1024);
        SharedRoot* root = segment.construct<SharedRoot>(
            "RootObject", segment.get_allocator<char>(), segment.get_allocator<int>());
        if (!require(root != nullptr, "failed to construct root"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        root->version = 7;
        root->message = "stable snapshot";
        for (int i = 0; i < 16; ++i)
        {
            root->values.push_back(i * i);
        }

        pid_t pid = fork();
        if (pid == -1)
        {
            std::cerr << "[Read Only Snapshot] fork failed" << std::endl;
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }
        if (pid == 0)
        {
            _exit(run_read_only_child(shm_name.c_str()));
        }

        int status = 0;
        if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            std::cerr << "[Read Only Snapshot] child failed" << std::endl;
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        if (!require(segment.find<SharedRoot>("RootObject") == root,
                     "writer mapping should remain usable after read-only child"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }
        if (!require(segment.get_segment_manager()->check_sanity(),
                     "manager sanity failed after read-only child"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        segment.destroy<SharedRoot>("RootObject");
        ManagedSharedMemory::remove(shm_name.c_str());
        std::cout << "[Read Only Snapshot] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Read Only Snapshot] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name.c_str());
        return 1;
    }
}
