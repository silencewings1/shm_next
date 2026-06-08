#include "interprocess/container/shared_memory_list.h"
#include "interprocess/ipc/managed_shared_memory.h"

#include "interprocess/sync/posix_mutex.h"
#include <sys/wait.h>
#include <mutex>
#include <array>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace interprocess;

namespace
{
struct LockedListRoot { InterprocessMutex mutex; SharedMemoryList<int> values; explicit LockedListRoot(const SharedMemoryAllocator<int>& a): values(a) {} };

bool require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "[List Semantics] " << message << std::endl;
        return false;
    }
    return true;
}

template <typename List, std::size_t N>
bool require_list_equals(const List& list, const std::array<int, N>& expected,
                         const std::string& message)
{
    if (!require(list.size() == expected.size(), message + ": size mismatch"))
    {
        return false;
    }

    std::size_t index = 0;
    for (typename List::const_iterator it = list.cbegin(); it != list.cend(); ++it, ++index)
    {
        if (!require(index < expected.size() && *it == expected[index], message))
        {
            return false;
        }
    }
    return require(index == expected.size(), message + ": iteration count mismatch");
}

bool wait_ok(pid_t pid){ int st=0; return waitpid(pid,&st,0)!=-1 && WIFEXITED(st) && WEXITSTATUS(st)==0; }
int child_read(const char* shm_name){ try{ ManagedSharedMemory seg(open_only,shm_name); LockedListRoot* r=seg.find<LockedListRoot>("LockedRoot"); if(!r) return 2; std::lock_guard<InterprocessMutex> lock(r->mutex); int expected[]={1,2,3,4,5}; std::size_t i=0; for(auto it=r->values.begin(); it!=r->values.end(); ++it,++i){ if(i>=5 || *it!=expected[i]) return 3; } return i==5?0:4; }catch(...){ return 5; }}
int child_busy(const char* shm_name){ try{ ManagedSharedMemory seg(open_only,shm_name); LockedListRoot* r=seg.find<LockedListRoot>("LockedRoot"); if(!r) return 2; return r->mutex.try_lock() ? (r->mutex.unlock(),3) : 0; }catch(...){ return 4; }}
} // namespace

int main()
{
    const std::string shm_name = "shmlist_" + std::to_string(getpid());
    ManagedSharedMemory::remove(shm_name.c_str());

    try
    {
        ManagedSharedMemory segment(create_only, shm_name.c_str(), 256 * 1024);
        using IntList = SharedMemoryList<int>;
        IntList* list = segment.construct<IntList>("List", segment.get_allocator<int>());
        IntList* other = segment.construct<IntList>("Other", segment.get_allocator<int>());
        IntList* source = segment.construct<IntList>("Source", segment.get_allocator<int>());

        if (!require(list && other && source, "failed to construct lists"))
        {
            ManagedSharedMemory::remove(shm_name.c_str());
            return 1;
        }

        IntList* pooled = segment.construct<IntList>("Pooled", segment.get_allocator<int>());
        if (!require(pooled != nullptr, "failed to construct pooled list"))
        {
            return 1;
        }
        pooled->push_back(1);
        pooled->push_back(2);
        const std::size_t list_allocations_after_insert = pooled->node_pool_allocations();
        pooled->erase(pooled->begin());
        if (!require(pooled->cached_node_count() == 1,
                     "list erase should cache one node for reuse"))
        {
            return 1;
        }
        pooled->push_back(3);
        if (!require(pooled->node_pool_hits() == 1 &&
                         pooled->node_pool_allocations() == list_allocations_after_insert &&
                         pooled->cached_node_count() == 0,
                     "list insert should reuse cached node without a new allocation"))
        {
            return 1;
        }
        pooled->pop_front();
        if (!require(pooled->cached_node_count() == 1,
                     "list pop should cache reusable node"))
        {
            return 1;
        }
        pooled->shrink_to_fit();
        if (!require(pooled->cached_node_count() == 0,
                     "list shrink_to_fit should release cached nodes"))
        {
            return 1;
        }

        list->assign({5, 1, 3, 3, 2, 4});
        list->sort();
        if (!require_list_equals(*list, std::array<int, 6>{1, 2, 3, 3, 4, 5},
                                 "sort should order elements"))
        {
            return 1;
        }

        list->unique();
        if (!require_list_equals(*list, std::array<int, 5>{1, 2, 3, 4, 5},
                                 "unique should remove adjacent duplicates"))
        {
            return 1;
        }

        list->remove_if([](int value) { return value % 2 == 0; });
        if (!require_list_equals(*list, std::array<int, 3>{1, 3, 5},
                                 "remove_if should remove matching values"))
        {
            return 1;
        }

        list->reverse();
        if (!require_list_equals(*list, std::array<int, 3>{5, 3, 1},
                                 "reverse should invert order"))
        {
            return 1;
        }

        source->assign({8, 9});
        list->splice(++list->begin(), *source);
        if (!require(source->empty(), "splice(list) should empty source") ||
            !require_list_equals(*list, std::array<int, 5>{5, 8, 9, 3, 1},
                                 "splice(list) should move source nodes"))
        {
            return 1;
        }

        source->assign({7, 6});
        list->splice(list->end(), *source, source->begin());
        if (!require(source->size() == 1 && source->front() == 6,
                     "splice(single) should move one node") ||
            !require(list->back() == 7, "splice(single) should append selected node"))
        {
            return 1;
        }

        list->sort();
        source->assign({0, 2, 10});
        source->sort();
        list->merge(*source);
        if (!require(source->empty(), "merge should empty source") ||
            !require_list_equals(*list, std::array<int, 9>{0, 1, 2, 3, 5, 7, 8, 9, 10},
                                 "merge should combine sorted lists"))
        {
            return 1;
        }

        other->assign({42, 43});
        list->swap(*other);
        if (!require_list_equals(*list, std::array<int, 2>{42, 43},
                                 "swap should exchange list content") ||
            !require(other->size() == 9, "swap should move original content to other"))
        {
            return 1;
        }

        auto* locked = segment.construct<LockedListRoot>("LockedRoot", segment.get_allocator<int>());
        locked->values.assign({1,2,3,4,5});
        locked->mutex.lock();
        pid_t busy = fork();
        if (busy == 0) _exit(child_busy(shm_name.c_str()));
        if (!wait_ok(busy)) { locked->mutex.unlock(); return 1; }
        locked->mutex.unlock();
        pid_t reader = fork();
        if (reader == 0) _exit(child_read(shm_name.c_str()));
        if (!wait_ok(reader)) return 1;
        segment.destroy<LockedListRoot>("LockedRoot");
        segment.destroy<IntList>("Pooled");
        segment.destroy<IntList>("Source");
        segment.destroy<IntList>("Other");
        segment.destroy<IntList>("List");
        ManagedSharedMemory::remove(shm_name.c_str());
        std::cout << "[List Semantics] SUCCESS" << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[List Semantics] Exception: " << e.what() << std::endl;
        ManagedSharedMemory::remove(shm_name.c_str());
        return 1;
    }
}
