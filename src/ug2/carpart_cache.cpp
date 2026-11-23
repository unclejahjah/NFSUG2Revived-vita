// CarPartDB::NewGetCarPart (0x61B980, thiscall: car type, slot, key, previous part, -1; ret 0x14) remembered while free
// roam builds a friend's car (roam_game.c build_ride: the stock parts, 0x637040, call it for every slot and walk the
// database each time: 21 to 27 ms a car on the Vita, 2026-11-23). The database is the game's own, so a part found
// stays where it is; the answers are forgotten when free roam says so (a new world, the shop packs in) and are only
// used while it asks for them, so the game's own garage and shops always walk the database themselves.
#include <lib/winapp.h>
#include <recomp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

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

// CarPartDB's walk (0x61B670) indexed: for each pack and car type, its records by slot, in the pack's order (built
// with one pass over the pack the first time that car is asked for). See native::sub_61b670.
struct PackKey
{
    uint32_t recs, n, table, tableN, car, pad = 0;
    bool operator==(const PackKey& o) const
    {
        return recs == o.recs && n == o.n && table == o.table && tableN == o.tableN && car == o.car;
    }
};
struct PackKeyHash
{
    size_t operator()(const PackKey& k) const
    {
        return hashOf(reinterpret_cast<const uint32_t*>(&k));
    }
};
struct CarIndex
{
    std::unordered_map<int, std::vector<uint32_t>> bySlot;
};
std::unordered_map<PackKey, CarIndex, PackKeyHash>* s_index = nullptr;
uint32_t s_walks = 0, s_built = 0;

extern "C" void roam_partcache_forget(void)
{
    memset(s_cache, 0, sizeof(s_cache));
    if (s_index)
        s_index->clear();
}

// " parts: H hits, M misses, W walks, B indexed" since the last call (build_ride's breakdown, roam_game.c).
extern "C" void roam_partcache_note(char* out, int n)
{
    static uint32_t h0, m0, w0, b0;
    snprintf(out, size_t(n), " db %u/%u/%u/%u", unsigned(s_hits - h0), unsigned(s_misses - m0), unsigned(s_walks - w0),
             unsigned(s_built - b0));
    h0 = s_hits; m0 = s_misses; w0 = s_walks; b0 = s_built;
}

extern "C" void roam_partcache_counts(uint32_t* hits, uint32_t* misses)
{
    *hits = s_hits;
    *misses = s_misses;
}

namespace ug2
{

void sub_61b980_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_61bcd0_guest(win32::WinApplication* app, x86::CPU& cpu);

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

namespace
{

bool inList(uint32_t v, const uint32_t* list, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (list[i] == v)
            return true;
    return false;
}

uint32_t stringHash(const char* s)   // bStringHash (0x43DB50)
{
    uint32_t h = 0xffffffffu;
    for (; *s; ++s)
        h = h * 33u + uint8_t(*s);
    return h;
}

// The part's attribute `hash` (0x61B79F..: the attribute list u16 at +0xA into the tables at [0x8A1CC8]), 0 if none.
uint32_t attribute(uint32_t rec, uint32_t hash)
{
    const uint16_t list = rc::ld16(rec + 0xa);
    if (list == 0xffff)
        return 0;
    const uint32_t t = rc::ld32(0x8a1cc8);
    const uint32_t lists = rc::ld32(t + 0x14), entries = rc::ld32(t + 0x1c);
    const int count = int16_t(rc::ld16(lists + list * 2u));
    for (int i = 0; i < count; ++i)
    {
        const uint32_t e = entries + uint32_t(int32_t(int16_t(rc::ld16(lists + list * 2u + 2u + uint32_t(i) * 2u)))) * 8u;
        if (hash == 0 || rc::ld32(e) == hash)
            return e ? rc::ld32(e + 4) : 0;
    }
    return 0;
}

}

}

// CarPartDB::FindPart (0x61B670, thiscall: car type, slot, part key or 0, previous part or 0, upgrade level or -1;
// ret 0x14): for each pack (a ring from [db]) of 14-byte records sorted by their car index (byte 7), from the car's
// first record (or after the previous part) to the pack's end, the first with the car's type, the slot, the key, not
// excluded for this car (EXCLUDE_UG1/UG2/SUV) and the level. The game scans to the pack's end every time, which the
// stock parts (0x637040) do about 200 times a car: while free roam builds a friend's car the same answer comes from an
// index of the car's records by slot instead.
// (called first by native::sub_61b670, src/ug2/native/sub_61b670.cpp: true when it answered)
bool roamPartIndexFind(x86::CPU& cpu)
{
    if (!s_on)
        return false;
    ++s_walks;
    if (!s_index)
        s_index = new std::unordered_map<PackKey, CarIndex, PackKeyHash>();
    const uint32_t db = cpu.ecx;
    const uint32_t car = rc::ld32(cpu.esp + 4), key = rc::ld32(cpu.esp + 12), previous = rc::ld32(cpu.esp + 16);
    const int slot = int32_t(rc::ld32(cpu.esp + 8)), level = int32_t(rc::ld32(cpu.esp + 20));
    static const uint32_t ug1[] = { 0xdb7c, 0xdc00, 0xac6d1, 0x163087, 0x150fb80, 0x290c04d, 0x2ca04df, 0x3455d6b,
                                    0x3b8c48a, 0x4fd0522, 0x545aec60, 0x72b2e513, 0x7502da84, 0x79aebfec, 0xbe48375e };
    static const uint32_t ug2[] = { 0x453, 0x6e7, 0xa86e, 0xacc9, 0xdc01, 0x293fad7, 0x30236ee, 0x9d2eff6, 0x2d7c9b5d,
                                    0x35165819, 0x4b10070a, 0xcf4458f8, 0xf77c13eb };
    static const uint32_t suv[] = { 0x219c94a, 0x6126dd4d, 0xcf704771 };
    static const uint32_t hUg1 = native::stringHash("EXCLUDE_UG1"), hUg2 = native::stringHash("EXCLUDE_UG2"),
                          hSuv = native::stringHash("EXCLUDE_SUV");
    const bool isUg1 = native::inList(car, ug1, sizeof ug1 / 4), isUg2 = native::inList(car, ug2, sizeof ug2 / 4),
               isSuv = native::inList(car, suv, sizeof suv / 4);
    const uint32_t table = rc::ld32(0x8a1cb8), tableN = rc::ld32(0x8a1cbc);
    uint32_t found = 0;
    for (uint32_t pack = rc::ld32(db); pack != db && !found; pack = rc::ld32(pack))
    {
        const uint32_t recs = rc::ld32(pack + 0x34), n = rc::ld32(pack + 0x38), end = recs + n * 14u;
        uint32_t start;
        if (previous)
        {
            if (previous < recs || previous >= end)
                continue;
            start = previous + 14u;
        }
        else
        {
            uint8_t index = 0xff;   // the car's index (0x61B707), the first record with it (0x488C50)
            for (uint32_t i = 0; i < tableN; ++i)
                if (rc::ld32(table + i * 4u) == car)
                {
                    index = uint8_t(i);
                    break;
                }
            if (!recs || int32_t(n) <= 0)
                continue;
            int lo = 0, hi = int(n) - 1, at = -1;
            while (lo <= hi)
            {
                const int mid = (lo + hi) / 2;
                const uint8_t b = rc::ld8(recs + uint32_t(mid) * 14u + 7u);
                if (index < b)
                    hi = mid - 1;
                else if (index > b)
                    lo = mid + 1;
                else
                {
                    at = mid;
                    break;
                }
            }
            if (at < 0)
                continue;
            while (at > 0 && rc::ld8(recs + uint32_t(at - 1) * 14u + 7u) == index)
                --at;
            start = recs + uint32_t(at) * 14u;
        }
        const PackKey k{ recs, n, table, tableN, car };
        auto it = s_index->find(k);
        if (it == s_index->end())
        {
            ++s_built;
            it = s_index->emplace(k, CarIndex()).first;
            for (uint32_t r = recs; r < end; r += 14u)
                if (rc::ld32(table + uint32_t(rc::ld8(r + 7u)) * 4u) == car)
                    it->second.bySlot[int(int8_t(rc::ld8(r + 4u)))].push_back(r);
        }
        const auto list = it->second.bySlot.find(slot);
        if (list == it->second.bySlot.end())
            continue;
        const std::vector<uint32_t>& v = list->second;
        for (auto r = std::lower_bound(v.begin(), v.end(), start); r != v.end() && !found; ++r)
        {
            const uint32_t rec = *r;
            if (rec >= end)
                break;
            if (key && ((uint32_t(rc::ld16(rec + 2)) << 16) | rc::ld16(rec)) != key)
                continue;
            if (isUg1 && native::attribute(rec, hUg1))
                continue;
            if (isUg2 && native::attribute(rec, hUg2))
                continue;
            if (isSuv && native::attribute(rec, hSuv))
                continue;
            const int l = rc::ld8(rec + 5u) >> 5;
            if (l == level || level == -1)
                found = rec;
        }
    }
    cpu.eax = found;
    cpu.esp += 4 + 0x14;
    return true;
}

namespace native
{

// RideInfo::UpdatePartsEnabled (0x61BCD0, thiscall, no arguments). The stock parts (0x637040) call it after each of
// its 170 slots, about 0.15 ms each on the Vita: while free roam builds a friend's car it is skipped there, and
// build_ride calls it once when the parts are on (the calls at 0x6370DE and 0x63716B write their return addresses:
// RETURN_ADDRESS_SITES in tools/recompiler/games/ug2.py).
void sub_61bcd0(win32::WinApplication* app, x86::CPU& cpu)
{
    const uint32_t back = rc::ld32(cpu.esp);
    if (back == 0x006370E3 || back == 0x00637170)
    {
        rc::st32(cpu.esp, 0);
        if (s_on)
        {
            cpu.esp += 4;   // ret
            return;
        }
    }
    sub_61bcd0_guest(app, cpu);
}

}

}
