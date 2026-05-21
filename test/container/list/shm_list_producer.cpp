#include "interprocess/container/shared_memory_list.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_mutex.h"
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
using namespace interprocess;
struct Root{ InterprocessMutex mutex; SharedMemoryList<int> values; explicit Root(const SharedMemoryAllocator<int>& a): values(a){} };
int main(){ const char* name="test_shm_list_catalog"; ManagedSharedMemory::remove(name); try{ ManagedSharedMemory seg(create_only,name,256*1024); Root* r=seg.construct<Root>("RootObject",seg.get_allocator<int>()); if(!r) return 1; { std::lock_guard<InterprocessMutex> lock(r->mutex); r->values.assign({5,3,1,3,2,4}); r->values.sort(); r->values.unique(); r->values.push_back(6); } std::cout << "[Producer] Shared list ready. Waiting 10 seconds for consumer..." << std::endl; std::this_thread::sleep_for(std::chrono::seconds(10)); ManagedSharedMemory::remove(name); return 0; }catch(const std::exception& e){ std::cerr << "[Producer] Exception: " << e.what() << std::endl; return 1; } }
