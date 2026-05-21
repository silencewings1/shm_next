#include "interprocess/container/shared_memory_hash_map.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include <chrono>
#include <iostream>
#include <string>
#include <unistd.h>
using namespace interprocess;
int main(){std::string name="shm_hash_bench_"+std::to_string(getpid()); ManagedSharedMemory::remove(name.c_str()); try{ManagedSharedMemory seg(create_only,name.c_str(),64*1024*1024); auto* m=seg.construct<SharedMemoryHashMap<int,int>>("HashMap",8,std::hash<int>{},std::equal_to<int>{},seg.get_allocator<std::pair<const int,int>>()); constexpr int N=50000; auto t0=std::chrono::steady_clock::now(); for(int i=0;i<N;++i)m->try_emplace(i,i*3); auto t1=std::chrono::steady_clock::now(); long long sum=0; for(int i=0;i<N;++i) sum+=m->at(i); auto t2=std::chrono::steady_clock::now(); for(int i=0;i<N;i+=4)m->erase(i); auto t3=std::chrono::steady_clock::now(); std::cout<<"[HashMap Benchmark] insert_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count()<<" lookup_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(t2-t1).count()<<" erase_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(t3-t2).count()<<" sum="<<sum<<std::endl; seg.destroy<SharedMemoryHashMap<int,int>>("HashMap"); ManagedSharedMemory::remove(name.c_str()); return 0;}catch(const std::exception&e){std::cerr<<"[HashMap Benchmark] Exception: "<<e.what()<<std::endl; ManagedSharedMemory::remove(name.c_str()); return 1;}}
