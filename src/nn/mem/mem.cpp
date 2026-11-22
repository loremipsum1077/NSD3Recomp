// nn::mem::StandardAllocator.
//
// The game defines malloc, free and operator new itself and sends them all to one
// StandardAllocator over a region it reserves at startup. Here that allocator hands
// every request to the host's own allocator instead, for two reasons: the host's
// libraries allocate with it too, and memory can then cross between them and the
// game safely; and the host's memory debugging tools keep working.
//
// The game's malloc replaces the C library's inside this program, so the host
// allocator has to be reached by another name.
#include <stddef.h>

#ifdef __APPLE__
#include <malloc/malloc.h>
static void * HostAlloc(size_t size, size_t alignment)
{
    malloc_zone_t * zone = malloc_default_zone();
    return alignment > 16 ? malloc_zone_memalign(zone, alignment, size) : malloc_zone_malloc(zone, size);
}
static void * HostRealloc(void * p, size_t size) { return malloc_zone_realloc(malloc_default_zone(), p, size); }
static void HostFree(void * p) { malloc_zone_free(malloc_default_zone(), p); }
#else
#error "nn::mem: name the host allocator for this platform"
#endif

namespace nn { namespace mem {

class StandardAllocator {
public:
    StandardAllocator();
    void Initialize(void * address, unsigned long size);
    void * Allocate(unsigned long size);
    void * Allocate(unsigned long size, unsigned long alignment);
    void * Reallocate(void * address, unsigned long size);
    void Free(void * address);
};

StandardAllocator::StandardAllocator()
{
}

void StandardAllocator::Initialize(void *, unsigned long)
{
}

void * StandardAllocator::Allocate(unsigned long size)
{
    return HostAlloc(size, 16);
}

void * StandardAllocator::Allocate(unsigned long size, unsigned long alignment)
{
    return HostAlloc(size, alignment);
}

void * StandardAllocator::Reallocate(void * address, unsigned long size)
{
    return HostRealloc(address, size);
}

void StandardAllocator::Free(void * address)
{
    if (address)
    {
        HostFree(address);
    }
}

}}  // namespace nn::mem
