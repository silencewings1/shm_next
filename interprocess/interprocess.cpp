#include "allocator/offset_ptr.h"
#include "allocator/shared_memory_allocator.h"
#include "allocator/shared_memory_manager.h"
#include "container/shared_memory_map.h"
#include "container/shared_memory_string.h"
#include "container/shared_memory_vector.h"
#include "ipc/managed_shared_memory.h"
#include "ipc/posix_mapped_region.h"
#include "ipc/posix_shared_memory_object.h"
#include "sync/posix_condition.h"
#include "sync/posix_mutex.h"
#include "sync/posix_semaphore.h"

namespace interprocess
{

void interprocess_static_library_anchor()
{
}

} // namespace interprocess
