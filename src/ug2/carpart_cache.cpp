// CarPartDB::NewGetCarPart (0x61B980, thiscall: car type, slot, key, previous part, -1; ret 0x14) remembered while free
// roam builds a friend's car (roam_game.c build_ride: the stock parts, 0x637040, call it for every slot and walk the
// database each time: 21 to 27 ms a car on the Vita, 2026-11-23). The database is the game's own, so a part found
// stays where it is; the answers are forgotten when free roam says so (a new world, the shop packs in) and are only
// used while it asks for them, so the game's own garage and shops always walk the database themselves.
#include <lib/winapp.h>
#include <recomp.h>

#include <cstdint>
#include <cstring>

namespace
{

const int CacheN = 4096;   // a power of two
struct Entry
{
    uint32_t db, type, slot, key, previous, last, part;
    bool used;
};
Entry s_cache[CacheN];
bool s_on = false;
uint32_t s_hits = 0, s_misses = 0;

uint32_t hashOf(const uint32_t* a)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; ++i)
        h = (h ^ a[i]) * 16777619u;
    return h;
}

}

extern "C" void roam_partcache_on(int on)
{
    s_on = on != 0;
}

extern "C" void roam_partcache_forget(void)
{
    memset(s_cache, 0, sizeof(s_cache));
}

extern "C" void roam_partcache_counts(uint32_t* hits, uint32_t* misses)
{
    *hits = s_hits;
    *misses = s_misses;
}

namespace ug2
{

void sub_61b980_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_61b980(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_on)
    {
        sub_61b980_guest(app, cpu);
        return;
    }
    const uint32_t a[6] = { cpu.ecx, rc::ld32(cpu.esp + 4), rc::ld32(cpu.esp + 8), rc::ld32(cpu.esp + 12),
                            rc::ld32(cpu.esp + 16), rc::ld32(cpu.esp + 20) };
    const uint32_t h = hashOf(a);
    for (uint32_t probe = 0; probe < 8; ++probe)
    {
        const Entry& e = s_cache[(h + probe) & (CacheN - 1)];
        if (!e.used)
            break;
        if (e.db == a[0] && e.type == a[1] && e.slot == a[2] && e.key == a[3] && e.previous == a[4] && e.last == a[5])
        {
            ++s_hits;
            cpu.eax = e.part;
            cpu.esp += 4 + 0x14;   // the return address and the five arguments (ret 0x14)
            return;
        }
    }
    ++s_misses;
    sub_61b980_guest(app, cpu);
    for (uint32_t probe = 0; probe < 8; ++probe)
    {
        Entry& e = s_cache[(h + probe) & (CacheN - 1)];
        if (e.used && probe < 7)
            continue;
        e = Entry{ a[0], a[1], a[2], a[3], a[4], a[5], cpu.eax, true };
        break;
    }
}

}

}
