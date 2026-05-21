#include "interprocess/ipc/posix_mapped_region.h"
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
using namespace interprocess;
int main(){ std::string name="shmmapreg_"+std::to_string(getpid()); SharedMemoryObject::remove(name.c_str()); try{ SharedMemoryObject obj(create_only,name.c_str(),interprocess::mode_t::read_write,0666); obj.truncate(4096); MappedRegion region(obj,interprocess::mode_t::read_write); std::strcpy(static_cast<char*>(region.get_address()),"hello"); if(!region.flush(0,6,false)) return 1; SharedMemoryObject ro(open_only,name.c_str(),interprocess::mode_t::read_only); MappedRegion region_ro(ro,interprocess::mode_t::read_only); if(std::string(static_cast<const char*>(region_ro.get_address()))!="hello") return 2; SharedMemoryObject::remove(name.c_str()); std::cout << "[MappedRegion Test] SUCCESS" << std::endl; return 0; }catch(const std::exception& e){ std::cerr << e.what() << std::endl; SharedMemoryObject::remove(name.c_str()); return 1; } }
