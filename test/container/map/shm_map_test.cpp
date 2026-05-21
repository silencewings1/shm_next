#include "interprocess/container/shared_memory_map.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_mutex.h"
#include <iostream>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
using namespace interprocess;
namespace{ using Map=SharedMemoryMap<int,int>; struct Root{InterprocessMutex mutex; Map values; explicit Root(const SharedMemoryAllocator<std::pair<const int,int>>& a): values(a){}}; bool wait_ok(pid_t p){int s=0; return waitpid(p,&s,0)!=-1&&WIFEXITED(s)&&WEXITSTATUS(s)==0;} int child(const char* n){try{ManagedSharedMemory seg(open_only,n); Root* r=seg.find<Root>("Root"); if(!r)return 2; std::lock_guard<InterprocessMutex> lock(r->mutex); return r->values.size()==100&&r->values.at(42)==420?0:3;}catch(...){return 4;}} int busy(const char*n){try{ManagedSharedMemory seg(open_only,n); Root* r=seg.find<Root>("Root"); if(!r)return 2; return r->mutex.try_lock()?(r->mutex.unlock(),3):0;}catch(...){return 4;}}}
int main(){std::string name="shm_map_test_"+std::to_string(getpid()); ManagedSharedMemory::remove(name.c_str()); try{ManagedSharedMemory seg(create_only,name.c_str(),512*1024); Root* r=seg.construct<Root>("Root",seg.get_allocator<std::pair<const int,int>>()); for(int i=0;i<100;++i) r->values.try_emplace(i,i*10); r->values.insert_or_assign(42,420); if(r->values.lower_bound(41)->first!=41||r->values.upper_bound(42)->first!=43) return 1; r->mutex.lock(); pid_t b=fork(); if(b==0)_exit(busy(name.c_str())); if(!wait_ok(b)){r->mutex.unlock();return 1;} r->mutex.unlock(); pid_t c=fork(); if(c==0)_exit(child(name.c_str())); if(!wait_ok(c)) return 1; if(!seg.get_segment_manager()->check_sanity()) return 1; seg.destroy<Root>("Root"); ManagedSharedMemory::remove(name.c_str()); std::cout<<"[Map Test] SUCCESS"<<std::endl; return 0;}catch(const std::exception&e){std::cerr<<"[Map Test] Exception: "<<e.what()<<std::endl; ManagedSharedMemory::remove(name.c_str()); return 1;}}
