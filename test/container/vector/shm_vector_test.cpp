#include "interprocess/container/shared_memory_vector.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_mutex.h"
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
using namespace interprocess;
namespace { using Vec=SharedMemoryVector<int>; struct Root{ InterprocessMutex mutex; Vec values; explicit Root(const SharedMemoryAllocator<int>& a): values(a){} }; bool req(bool ok,const std::string& m){ if(!ok) std::cerr<<"[Vector Test] "<<m<<std::endl; return ok;} bool wait_ok(pid_t p){int s=0; return waitpid(p,&s,0)!=-1&&WIFEXITED(s)&&WEXITSTATUS(s)==0;} int child(const char* n){ try{ ManagedSharedMemory seg(open_only,n); Root* r=seg.find<Root>("Root"); if(!r) return 2; std::lock_guard<InterprocessMutex> lock(r->mutex); if(r->values.size()!=100) return 3; int sum=0; for(auto it=r->values.begin(); it!=r->values.end(); ++it) sum+=*it; return sum==4950?0:4;}catch(...){return 5;}} int busy(const char* n){ try{ ManagedSharedMemory seg(open_only,n); Root* r=seg.find<Root>("Root"); if(!r)return 2; return r->mutex.try_lock()?(r->mutex.unlock(),3):0;}catch(...){return 4;}} }
int main(){ const std::string name="shm_vector_test_"+std::to_string(getpid()); ManagedSharedMemory::remove(name.c_str()); try{ ManagedSharedMemory seg(create_only,name.c_str(),256*1024); Root* r=seg.construct<Root>("Root",seg.get_allocator<int>()); if(!req(r,"construct root")) return 1; for(int i=0;i<100;++i) r->values.push_back(i); r->values.erase(r->values.end()-1); r->values.push_back(99); r->values.resize(120,7); r->values.resize(100); if(!req(r->values.front()==0&&r->values.back()==99&&r->values.at(50)==50,"vector interface mismatch")) return 1; r->mutex.lock(); pid_t b=fork(); if(b==0)_exit(busy(name.c_str())); if(!wait_ok(b)){r->mutex.unlock();return 1;} r->mutex.unlock(); pid_t c=fork(); if(c==0)_exit(child(name.c_str())); if(!wait_ok(c)) return 1; if(!req(seg.get_segment_manager()->check_sanity(),"manager sanity")) return 1; seg.destroy<Root>("Root"); ManagedSharedMemory::remove(name.c_str()); std::cout<<"[Vector Test] SUCCESS"<<std::endl; return 0;}catch(const std::exception& e){std::cerr<<"[Vector Test] Exception: "<<e.what()<<std::endl; ManagedSharedMemory::remove(name.c_str()); return 1;} }
