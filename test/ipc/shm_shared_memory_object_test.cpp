#include "interprocess/ipc/posix_shared_memory_object.h"
#include <iostream>
#include <string>
#include <unistd.h>
using namespace interprocess;
int main(){ std::string name="shmobj_"+std::to_string(getpid()); SharedMemoryObject::remove(name.c_str()); try{ SharedMemoryObject obj(create_only,name.c_str(),interprocess::mode_t::read_write,0666); obj.truncate(4096); if(obj.get_size()!=4096 || !obj.was_created()) return 1; SharedMemoryObject obj2(open_only,name.c_str(),interprocess::mode_t::read_write); if(obj2.get_size()!=4096) return 2; SharedMemoryObject::remove(name.c_str()); std::cout << "[SharedMemoryObject Test] SUCCESS" << std::endl; return 0; }catch(const std::exception& e){ std::cerr << e.what() << std::endl; SharedMemoryObject::remove(name.c_str()); return 1; } }
