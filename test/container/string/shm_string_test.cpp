#include "interprocess/container/shared_memory_string.h"
#include "interprocess/ipc/managed_shared_memory.h"
#include "interprocess/sync/posix_mutex.h"
#include <iostream>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace interprocess;

namespace {
struct Root { InterprocessMutex mutex; SharedMemoryString value; explicit Root(const SharedMemoryAllocator<char>& a): value(a) {} };
bool require(bool ok, const std::string& msg){ if(!ok) std::cerr << "[String Test] " << msg << std::endl; return ok; }
int child_read(const char* name){ try { ManagedSharedMemory s(open_only, name); Root* r=s.find<Root>("Root"); if(!r) return 2; std::lock_guard<InterprocessMutex> lock(r->mutex); return (r->value == "hello shared world") ? 0 : 3; } catch(...) { return 4; } }
int child_try_busy(const char* name){ try { ManagedSharedMemory s(open_only, name); Root* r=s.find<Root>("Root"); if(!r) return 2; return r->mutex.try_lock() ? (r->mutex.unlock(), 3) : 0; } catch(...) { return 4; } }
bool wait_ok(pid_t pid){ int st=0; return waitpid(pid,&st,0)!=-1 && WIFEXITED(st) && WEXITSTATUS(st)==0; }
}

int main(){ const std::string name="shm_string_test_"+std::to_string(getpid()); ManagedSharedMemory::remove(name.c_str()); try { ManagedSharedMemory seg(create_only,name.c_str(),128*1024); Root* r=seg.construct<Root>("Root",seg.get_allocator<char>()); if(!require(r,"construct root")) return 1; r->value = "hello"; r->value += " shared"; r->value.append(" world"); if(!require(r->value.size()==18 && r->value=="hello shared world","string append/size")) return 1; r->mutex.lock(); pid_t busy=fork(); if(busy==0) _exit(child_try_busy(name.c_str())); if(!wait_ok(busy)){ r->mutex.unlock(); return 1; } r->mutex.unlock(); pid_t reader=fork(); if(reader==0) _exit(child_read(name.c_str())); if(!wait_ok(reader)) return 1; if(!require(seg.get_segment_manager()->check_sanity(),"manager sanity")) return 1; seg.destroy<Root>("Root"); ManagedSharedMemory::remove(name.c_str()); std::cout << "[String Test] SUCCESS" << std::endl; return 0; } catch(const std::exception& e){ std::cerr << "[String Test] Exception: " << e.what() << std::endl; ManagedSharedMemory::remove(name.c_str()); return 1; } }
