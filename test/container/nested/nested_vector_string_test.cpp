#include "nested_test_common.h"
#include "interprocess/container/shared_memory_string.h"
#include "interprocess/container/shared_memory_vector.h"
#include "interprocess/sync/posix_mutex.h"
#include <mutex>
using namespace interprocess;
using Vec=SharedMemoryVector<SharedMemoryString>;
struct Root{InterprocessMutex mutex; Vec values; explicit Root(const SharedMemoryAllocator<SharedMemoryString>& a):values(a){}};
int main(){std::string name="nested_vector_string_"+std::to_string(getpid()); ManagedSharedMemory::remove(name.c_str()); try{ManagedSharedMemory seg(create_only,name.c_str(),256*1024); auto* r=seg.construct<Root>("RootObject",seg.get_allocator<SharedMemoryString>()); auto ca=seg.get_allocator<char>(); {std::lock_guard<InterprocessMutex> lock(r->mutex); r->values.emplace_back("alpha",ca); r->values.emplace_back("beta",ca);} auto validate=[](Root& root){return root.values.size()==2&&root.values[0]=="alpha"&&root.values[1]=="beta";}; if(!nested_run_lock_and_reader<Root>(name,validate)) return 1; bool ok=seg.get_segment_manager()->check_sanity(); seg.destroy<Root>("RootObject"); ManagedSharedMemory::remove(name.c_str()); if(!ok)return 1; std::cout<<"[nested_vector_string_test] SUCCESS"<<std::endl; return 0;}catch(...){ManagedSharedMemory::remove(name.c_str()); return 1;}}
