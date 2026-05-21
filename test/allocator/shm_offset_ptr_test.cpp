#include "interprocess/allocator/offset_ptr.h"
#include <iostream>
using namespace interprocess;
int main(){ int value=42; OffsetPtr<int> p(&value); if(!p || *p!=42) return 1; OffsetPtr<const int> cp(p); if(cp.get()!=&value) return 2; std::cout << "[OffsetPtr Test] SUCCESS" << std::endl; return 0; }
