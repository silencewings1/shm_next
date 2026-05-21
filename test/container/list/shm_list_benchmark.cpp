#include "interprocess/container/shared_memory_list.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include <chrono>
#include <iostream>
#include <string>
#include <unistd.h>
using namespace interprocess;
int main(){ const std::string name="shm_list_bench_"+std::to_string(getpid()); ManagedSharedMemory::remove(name.c_str()); try{ ManagedSharedMemory seg(create_only,name.c_str(),64*1024*1024); auto* l=seg.construct<SharedMemoryList<int>>("List",seg.get_allocator<int>()); constexpr int N=300000; auto t0=std::chrono::steady_clock::now(); for(int i=0;i<N;++i) l->push_back(i); auto t1=std::chrono::steady_clock::now(); long long sum=0; for(auto it=l->begin(); it!=l->end(); ++it) sum+=*it; auto t2=std::chrono::steady_clock::now(); for(int i=0;i<50000;++i) l->pop_front(); auto t3=std::chrono::steady_clock::now(); std::cout<<"[List Benchmark] push_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count()<<" iterate_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(t2-t1).count()<<" pop_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(t3-t2).count()<<" sum="<<sum<<std::endl; seg.destroy<SharedMemoryList<int>>("List"); ManagedSharedMemory::remove(name.c_str()); return 0;}catch(const std::exception& e){std::cerr<<"[List Benchmark] Exception: "<<e.what()<<std::endl; ManagedSharedMemory::remove(name.c_str()); return 1;} }
