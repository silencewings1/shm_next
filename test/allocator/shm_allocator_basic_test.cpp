#include "interprocess/allocator/shared_memory_allocator.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include <iostream>
#include <string>
#include <unistd.h>
using namespace interprocess;
int main(){ std::string name="shm_alloc_basic_"+std::to_string(getpid()); ManagedSharedMemory::remove(name.c_str()); try{ ManagedSharedMemory seg(create_only,name.c_str(),128*1024); auto alloc=seg.get_allocator<int>(); int* p=alloc.allocate(4); for(int i=0;i<4;++i) alloc.construct(p+i,i*10); int ok=(p[0]==0&&p[3]==30); for(int i=0;i<4;++i) alloc.destroy(p+i); alloc.deallocate(p,4); ManagedSharedMemory::remove(name.c_str()); std::cout << "[Allocator Basic Test] SUCCESS" << std::endl; return ok?0:1; }catch(const std::exception& e){ std::cerr << e.what() << std::endl; ManagedSharedMemory::remove(name.c_str()); return 1; } }
