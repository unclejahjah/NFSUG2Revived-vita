#ifndef UG2_NATIVE_TEXTURE_LOOKUP_H_
#define UG2_NATIVE_TEXTURE_LOOKUP_H_

// Texture-info lookup of sub_4901d0 and the name hash of sub_43db50, as plain code on guest memory.

#include <native/common.h>

#include <atomic>

// Free roam's own textures (the flags before friends' names: src/lib/winapi/revived/roam/roam_game.c), 0 for others.
extern "C" uint32_t roam_game_texture(uint32_t hash) __attribute__((weak));

namespace ug2 { namespace native { namespace texture {

const x86::reg32 ListHead = 0x830af0;       // doubly linked list of texture sets, the head slot is the sentinel
const x86::reg32 Bloom = 0x833784;          // one byte per (hash & 0x1fff)
const x86::reg32 DefaultCount = 0x82da80;
const x86::reg32 DefaultKeys = 0x82d530;
const x86::reg32 DefaultValues = 0x82d5c0;
const x86::reg32 Fallback = 0x82db08;

inline x86::reg32 nameHash(x86::reg32 string)
{
    x86::reg32 hash = 0xffffffffu;
    for (x86::reg32 c = rc::ld8(string); c != 0; c = rc::ld8(++string))
        hash = hash * 0x21 + c;
    return hash;
}

inline std::atomic<uint32_t> g_listTaken{0};

// Spin lock of the texture list: a hit relinks its node to the front and sets byte +0x53.
struct ListLock
{
    ListLock()
    {
        while (g_listTaken.exchange(1, std::memory_order_acquire))
            while (g_listTaken.load(std::memory_order_relaxed))
                ;
    }
    ~ListLock() { g_listTaken.store(0, std::memory_order_release); }
};

// First entry of the sorted table (stride 8, key at offset 0) whose key equals hash, 0 when there is none.
inline x86::reg32 search(x86::reg32 hash, x86::reg32 base, x86::sreg32 count)
{
    if (base == 0 || count <= 0)
        return 0;
    x86::sreg32 low = 0, high = count - 1;
    while (low <= high)
    {
        const x86::sreg32 sum = low + high;
        const x86::sreg32 mid = (sum - (sum >> 31)) >> 1;
        const x86::reg32 entry = base + x86::reg32(mid) * 8;
        const x86::reg32 key = rc::ld32(entry);
        if (hash < key)
            high = mid - 1;
        else if (hash > key)
            low = mid + 1;
        else
        {
            x86::reg32 found = entry;
            for (x86::sreg32 n = mid; n > 0 && rc::ld32(found - 8) == hash; --n)
                found -= 8;
            return found;
        }
    }
    return 0;
}

// Texture info of hash; orDefault: the fallback entry (and a trap when there is none) instead of 0.
inline x86::reg32 find(x86::reg32 hash, x86::reg32 orDefault, x86::reg32 byTable)
{
    using x86::reg32;
    const ListLock lock;
    if (hash != 0)
    {
        reg32 node = rc::ld32(ListHead);
        if (node != ListHead && rc::ld8(Bloom + (hash & 0x1fff)) != 0)
        {
            for (; node != ListHead; node = rc::ld32(node))
            {
                reg32 found = 0;
                const reg32 header = rc::ld32(node + 0x70);
                const reg32 table = header ? rc::ld32(header + 0x74) : 0;
                if (table != 0)
                {
                    const reg32 at = search(hash, table, x86::sreg32(rc::ld32(node + 0x74)));
                    found = at ? rc::ld32(at + 4) : 0;
                    if (found != 0 && !byTable)
                    {
                        if (rc::ld32(found + 0x70) == 0)
                            found = 0;
                        else if (rc::ld32(found + 0x3c) != 0 && rc::ld32(found + 0x74) == 0)
                            found = 0;
                    }
                }
                if (found == 0)
                    continue;
                const reg32 next = rc::ld32(node), previous = rc::ld32(node + 4);
                rc::st32(previous, next);
                rc::st32(next + 4, previous);
                const reg32 first = rc::ld32(ListHead);
                rc::st32(ListHead, node);
                rc::st32(first + 4, node);
                rc::st32(node + 4, ListHead);
                rc::st32(node, first);
                rc::st8(found + 0x53, 1);
                return found;
            }
        }
        if (roam_game_texture)
            if (const reg32 own = roam_game_texture(hash))
                return own;
        const reg32 count = rc::ld32(DefaultCount);
        for (reg32 i = 0; i < count; ++i)
            if (rc::ld32(DefaultKeys + 4 * i) == hash)
            {
                if (const reg32 value = rc::ld32(DefaultValues + 4 * i))
                    return value;
                break;
            }
    }
    if (!orDefault)
        return 0;
    if (const reg32 value = rc::ld32(Fallback))
        return value;
    x86::guestTrap(0x43bd50, "int3");
}

}}}

#endif /* !UG2_NATIVE_TEXTURE_LOOKUP_H_ */
