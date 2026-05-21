#include "interprocess/container/shared_memory_list.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_mutex.h"
#include <iostream>
#include <mutex>
using namespace interprocess;
struct Root{ InterprocessMutex mutex; SharedMemoryList<int> values; explicit Root(const SharedMemoryAllocator<int>& a): values(a){} };
int main(){ try{ ManagedSharedMemory seg(open_only,"test_shm_list_catalog"); Root* r=seg.find<Root>("RootObject"); if(!r) return 1; std::lock_guard<InterprocessMutex> lock(r->mutex); int expected[]={1,2,3,4,5,6}; std::size_t i=0; for(auto it=r->values.begin(); it!=r->values.end(); ++it,++i){ if(i>=6||*it!=expected[i]) return 2; } if(i!=6) return 3; std::cout << "[Consumer] SUCCESS: SharedMemoryList data matched expectations." << std::endl; return 0; }catch(const std::exception& e){ std::cerr << "[Consumer] Exception: " << e.what() << std::endl; return 1; } }
