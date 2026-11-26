// The texture packs' tables merged for a model's texture binding (0x489200, cdecl: packs, n; from 0x488F10), native.
// Each pack (+0x84: its table, entries of 8 bytes sorted by hash at +0, count a short at +8) gives its entries in
// order; the game merges them into one sorted list of entry pointers, the earlier pack first on equal hashes, taking
// each one with a pass over all the packs, then binds each pack's users with it (0x488FF0, thiscall on the pack:
// list, count). Translated, that merge was most of SetRideInfo's four calls of 0x488F10 when a friend's car comes in
// during free roam (10 to 13 ms a car on the Vita, 2026-11-23). Same result, same order, same guest allocation for the
// list; the counters stay on the host. Setting texture_merge_native=0: the game's own code.
#include <lib/winapp.h>
#include <platform/config.h>
#include <recomp.h>

#include <cstdint>
#include <vector>

namespace ug2
{

void sub_489200_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

const uint32_t Malloc = 0x575620;   // cdecl (size)
const uint32_t Free = 0x575630;     // cdecl (pointer)
const uint32_t Timer = 0x43bdf0;    // the game's clock read the original makes between the two allocations
const uint32_t Bind = 0x488ff0;     // thiscall on the pack (list, count), ret 8

// A game function from here; the CPU's registers as they were afterwards (memory keeps its changes).
uint32_t call(win32::WinApplication* app, x86::CPU& cpu, uint32_t fn, uint32_t ecx, int n, const uint32_t* args)
{
    const x86::CPU saved = cpu;
    x86::reg32 esp = cpu.esp;
    for (int i = n - 1; i >= 0; --i)
        rc::st32(esp -= 4, args[i]);
    esp -= 4;   // the return address' place
    cpu.esp = esp;
    cpu.ecx = ecx;
    app->dynamic_call(fn, cpu);
    const uint32_t result = cpu.eax;
    const bool terminate = cpu.terminate;
    cpu = saved;
    cpu.terminate = terminate;
    return result;
}

bool enabled()
{
    static const bool s_on = pf::Config::getBool("texture_merge_native", true);
    return s_on;
}

}

namespace native
{

void sub_489200(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!enabled())
    {
        sub_489200_guest(app, cpu);
        return;
    }
    const uint32_t ret = cpu.esp;
    const uint32_t packs = rc::ld32(ret + 4);
    const int32_t n = int32_t(rc::ld32(ret + 8));
    cpu.esp = ret + 4;   // the return address popped (cdecl: the caller pops the arguments)
    if (!packs || n < 0)
    {
        cpu.eax = 0;
        return;
    }
    if (n == 0)
    {
        cpu.eax = 1;
        return;
    }
    // working below the arguments, as the original's frame would
    cpu.esp = ret - 0x40;
    struct Pack { uint32_t table; int32_t count; int32_t at; };
    std::vector<Pack> p(static_cast<size_t>(n));
    int32_t total = 0;
    for (int32_t i = 0; i < n; ++i)
    {
        const uint32_t pack = rc::ld32(packs + 4u * uint32_t(i));
        const uint32_t info = pack ? rc::ld32(pack + 0x84u) : 0;
        if (!info)
        {
            rc::st32(packs + 4u * uint32_t(i), 0);   // (the original clears the caller's entry too)
            p[size_t(i)] = Pack{ 0, 0, 0 };
            continue;
        }
        const int32_t count = int16_t(rc::ld16(info + 8u));
        p[size_t(i)] = Pack{ rc::ld32(info), count, 0 };
        total += count;
    }
    {
        const uint32_t a[1] = { uint32_t(n) * 4u };
        const uint32_t counters = call(app, cpu, Malloc, 0, 1, a);   // (kept: the guest heap as the game leaves it)
        call(app, cpu, Timer, 0, 0, nullptr);
        const uint32_t b[1] = { uint32_t(total) * 4u };
        const uint32_t list = call(app, cpu, Malloc, 0, 1, b);
        if (cpu.terminate)
            return;
        int32_t made = 0;
        for (int32_t k = 0; k < total; ++k)
        {
            uint32_t best = 0, bestKey = 0;
            int32_t from = 0;
            for (int32_t i = 0; i < n; ++i)
            {
                Pack& q = p[size_t(i)];
                if (!q.table || q.at >= q.count)
                    continue;
                const uint32_t entry = q.table + 8u * uint32_t(q.at);
                const uint32_t key = rc::ld32(entry);
                if (!best || key < bestKey)
                {
                    best = entry;
                    bestKey = key;
                    from = i;
                }
            }
            if (!best)
                continue;
            rc::st32(list + 4u * uint32_t(made++), best);
            ++p[size_t(from)].at;
        }
        for (int32_t i = 0; i < n; ++i)
        {
            const uint32_t pack = rc::ld32(packs + 4u * uint32_t(i));
            if (!pack)
                continue;
            const uint32_t c[2] = { list, uint32_t(made) };
            call(app, cpu, Bind, pack, 2, c);
            if (cpu.terminate)
                return;
        }
        const uint32_t f1[1] = { counters };
        call(app, cpu, Free, 0, 1, f1);
        const uint32_t f2[1] = { list };
        call(app, cpu, Free, 0, 1, f2);
    }
    cpu.esp = ret + 4;
    cpu.eax = 1;
}

}}
