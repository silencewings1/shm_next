#include "interprocess/container/shared_memory_hash_map.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_mutex.h"
#include <iostream>
#include <mutex>
using namespace interprocess;
struct Root{ InterprocessMutex mutex; SharedMemoryHashMap<int,int> values; explicit Root(const SharedMemoryAllocator<std::pair<const int,int>>& a): values(8,std::hash<int>{},std::equal_to<int>{},a){} };
int main(){ try{ ManagedSharedMemory seg(open_only,"test_shm_hash_map_catalog"); Root* r=seg.find<Root>("RootObject"); if(!r) return 1; std::lock_guard<InterprocessMutex> lock(r->mutex); if(r->values.size()!=15||r->values.count(3)!=0||r->values.at(7)!=77||r->values.at(9)!=81) return 2; std::cout << "[Consumer] SUCCESS: SharedMemoryHashMap data matched expectations." << std::endl; return 0; }catch(const std::exception& e){ std::cerr << "[Consumer] Exception: " << e.what() << std::endl; return 1; } }
