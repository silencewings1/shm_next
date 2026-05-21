#include "interprocess/container/shared_memory_string.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include <chrono>
#include <iostream>
#include <string>
#include <unistd.h>
using namespace interprocess;
int main(){ const std::string name="shm_string_bench_"+std::to_string(getpid()); ManagedSharedMemory::remove(name.c_str()); try { ManagedSharedMemory seg(create_only,name.c_str(),16*1024*1024); auto alloc=seg.get_allocator<char>(); auto* s=seg.construct<SharedMemoryString>("String",alloc); constexpr int N=200000; auto t0=std::chrono::steady_clock::now(); for(int i=0;i<N;++i){ s->append("x"); if(s->size()>4096) s->clear(); } auto t1=std::chrono::steady_clock::now(); std::size_t checksum=0; for(int i=0;i<N;++i) checksum += s->size(); auto t2=std::chrono::steady_clock::now(); std::cout << "[String Benchmark] writes_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(t1-t0).count() << " reads_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(t2-t1).count() << " checksum=" << checksum << std::endl; seg.destroy<SharedMemoryString>("String"); ManagedSharedMemory::remove(name.c_str()); return 0; } catch(const std::exception& e){ std::cerr << "[String Benchmark] Exception: " << e.what() << std::endl; ManagedSharedMemory::remove(name.c_str()); return 1; } }
