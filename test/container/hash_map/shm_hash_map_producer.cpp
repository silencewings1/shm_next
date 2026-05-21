#include "interprocess/container/shared_memory_hash_map.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_mutex.h"
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
using namespace interprocess;
struct Root{ InterprocessMutex mutex; SharedMemoryHashMap<int,int> values; explicit Root(const SharedMemoryAllocator<std::pair<const int,int>>& a): values(8,std::hash<int>{},std::equal_to<int>{},a){} };
int main(){ const char* name="test_shm_hash_map_catalog"; ManagedSharedMemory::remove(name); try{ ManagedSharedMemory seg(create_only,name,256*1024); Root* r=seg.construct<Root>("RootObject",seg.get_allocator<std::pair<const int,int>>()); if(!r) return 1; { std::lock_guard<InterprocessMutex> lock(r->mutex); for(int i=0;i<16;++i) r->values.try_emplace(i,i*i); r->values.erase(3); r->values.insert_or_assign(7,77); } std::cout << "[Producer] Shared hash_map ready. Waiting 10 seconds for consumer..." << std::endl; std::this_thread::sleep_for(std::chrono::seconds(10)); ManagedSharedMemory::remove(name); return 0; }catch(const std::exception& e){ std::cerr << "[Producer] Exception: " << e.what() << std::endl; return 1; } }
