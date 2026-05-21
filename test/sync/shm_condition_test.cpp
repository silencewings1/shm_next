#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_condition.h"
#include "interprocess/sync/posix_mutex.h"
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
using namespace interprocess;
struct Root{ InterprocessMutex mutex; InterprocessCondition cond; bool ready; Root(): ready(false) {} };
static bool wait_ok(pid_t p){ int s=0; return waitpid(p,&s,0)!=-1&&WIFEXITED(s)&&WEXITSTATUS(s)==0; }
int main(){ std::string name="shm_cond_"+std::to_string(getpid()); ManagedSharedMemory::remove(name.c_str()); try{ ManagedSharedMemory seg(create_only,name.c_str(),128*1024); Root* r=seg.construct<Root>("Root"); pid_t child=fork(); if(child==0){ try{ ManagedSharedMemory s(open_only,name.c_str()); Root* rr=s.find<Root>("Root"); rr->mutex.lock(); rr->cond.wait(rr->mutex,[&]{ return rr->ready; }); rr->mutex.unlock(); _exit(0);}catch(...){ _exit(1);} } r->mutex.lock(); r->ready=true; r->cond.notify_one(); r->mutex.unlock(); if(!wait_ok(child)) return 1; seg.destroy<Root>("Root"); ManagedSharedMemory::remove(name.c_str()); std::cout << "[Condition Test] SUCCESS" << std::endl; return 0; }catch(const std::exception& e){ std::cerr << e.what() << std::endl; ManagedSharedMemory::remove(name.c_str()); return 1; } }
