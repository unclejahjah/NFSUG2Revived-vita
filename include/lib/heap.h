#ifndef LIB_HEAP_H_
#define LIB_HEAP_H_

#include <x86.h>

namespace win32
{

// Guest heap behind HeapAlloc and GlobalAlloc: 16-byte granular blocks in guest memory. Small and medium sizes
// come from size classes carved out of 1 MB chunks; large blocks are whole guest memory blocks. Any thread.
namespace heap
{

// Guest address of a new block (16-byte aligned, zeroed when asked), 0 when guest memory is exhausted.
x86::reg32 allocate(x86::reg32 size, bool zero);
// False if the address is not a live block.
bool release(x86::reg32 address);
// Sets aside one block for a later request of size bytes (or up to 64 fewer) (it is lent to that request and kept when
// freed), while guest memory still has a range that large. False when it does not fit, or another size is reserved.
bool reserve(x86::reg32 size);
// Frees the block kept from the last track streamer pool (32 MB or more, released and not lent again), for guest
// memory exhaustion. True when there was one. Any thread.
bool letGoKept();
// Gives back the reserve() block while it is not lent; reserve() may be called again after. True when there was one.
// The same size is set aside again by itself right after the next allocation of KeepSize or more (a track's stream pool).
bool unreserve();
// Requested size of a live block, or 0xffffffff.
x86::reg32 size(x86::reg32 address);
// Bytes the guest holds in live blocks (log statistics).
x86::reg32 used();

}

}

#endif
