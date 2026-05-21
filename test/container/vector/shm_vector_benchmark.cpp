#include "interprocess/container/shared_memory_vector.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include <chrono>
#include <iostream>
#include <string>
#include <unistd.h>
using namespace interprocess;
int main(){ const std::string name="shm_vector_bench_"+std::to_string(getpid()); ManagedSharedMemory::remove(name.c_str()); try{ ManagedSharedMemory seg(create_only,name.c_str(),64*1024*1024); auto* v=seg.construct<SharedMemoryVector<int>>("Vector",seg.get_allocator<int>()); constexpr int N=200000; auto t0=std::chrono::steady_clock::now(); for(int i=0;i<N;++i) v->push_back(i); auto t1=std::chrono::steady_clock::now(); long long sum=0; for(auto it=v->begin(); it!=v->end(); ++it) sum+=*it; auto t2=std::chrono::steady_clock::now(); for(int i=0;i<1000;++i) v->erase(v->begin()); auto t3=std::chrono::steady_clock::now(); std::cout<<"[Vector Benchmark] push_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count()<<" iterate_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(t2-t1).count()<<" erase_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(t3-t2).count()<<" sum="<<sum<<std::endl; seg.destroy<SharedMemoryVector<int>>("Vector"); ManagedSharedMemory::remove(name.c_str()); return 0;}catch(const std::exception& e){std::cerr<<"[Vector Benchmark] Exception: "<<e.what()<<std::endl; ManagedSharedMemory::remove(name.c_str()); return 1;} }
