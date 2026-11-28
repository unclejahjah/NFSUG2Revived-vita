// Extra Options 5.1.0.1340's code caves, called where MOD_SWITCHES (tools/recompiler/games/ug2.py) redirects a call.

#include "../mods_runtime.h"
#include <native/common.h>

namespace ug2
{

void sub_578060(win32::WinApplication* app, x86::CPU& cpu);
void sub_545920(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

typedef x86::reg32 reg32;

void push(x86::CPU& cpu, reg32 value)
{
    cpu.esp -= 4;
    rc::st32(cpu.esp, value);
}

// A cdecl call: the caller pops the arguments.
template <typename... Args> bool callCdecl(win32::WinApplication* app, x86::CPU& cpu,
                                       void (*callee)(win32::WinApplication*, x86::CPU&), Args... args)
{
    const reg32 words[] = { reg32(args)... };
    for (int i = int(sizeof...(Args)) - 1; i >= 0; --i)
        push(cpu, words[i]);
    cpu.esp -= 4;
    callee(app, cpu);
    cpu.esp += 4 * sizeof...(Args);
    return !cpu.terminate;
}

const reg32 PlayerBarriersLetter = 0x7a078b, BarriersLetter = 0x7a0798;
const reg32 CurrentRace = 0x883dac;

}

// Track availability (stdcall: track info, direction).
void exoptsTrackAvailable(win32::WinApplication*, x86::CPU& cpu)
{
    // ExtraOptions 5.1.0.1340 [Menu] AnyTrackInAnyRaceMode: UIQRTrackSelect_IsAvailable, all but 13 crashing tracks
    static const int Crashing[] = { 1001, 1002, 1003, 1099, 1102, 3001, 4200, 4300, 4400, 4600, 4700, 4800, 4900 };
    const reg32 block = argument(cpu, 0);
    bool available = block != 0;
    if (available)
    {
        const int id = int16_t(rc::ld16(block + 138));
        for (int crashing : Crashing)
            available &= id != crashing;
    }
    cpu.eax = available ? 1 : 0;
    cpu.esp += 12;
}

// The last vinyl category, then the hidden one (thiscall, three arguments).
void exoptsVinylCategories(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 screen = cpu.ecx;
    sub_545920(app, cpu);
    if (cpu.terminate)
        return;
    // ExtraOptions 5.1.0.1340 [Menu] ShowSpecialVinyls: VinylCategoryCodeCave, AddCustomElementToMenu(0x1C, ...)
    push(cpu, 0xe79a53f8);
    push(cpu, 0x93ec1ce9);
    push(cpu, 0x1c);
    cpu.esp -= 4;
    cpu.ecx = screen;
    sub_545920(app, cpu);
}

// The value 0x53290D stores as a new career's cash.
void exoptsStartingCash(win32::WinApplication*, x86::CPU& cpu)
{
    // ExtraOptions 5.1.0.1340 [Gameplay] StartingCash: cave's value via 0x576090's eax, dead after 0x53290D (al only)
    cpu.ecx = rc::ld32(0x865130);
    cpu.eax = reg32(modrt::exoStartingCashValue);
    cpu.esp += 4;
}

// Point of view of a player camera (cdecl: camera).
void exoptsPovType(win32::WinApplication*, x86::CPU& cpu)
{
    // ExtraOptions 5.1.0.1340 [Gameplay] EnableHiddenCameraModes: Player.h GetPOVTypeFromPlayerCamera
    static const reg32 Pov[6] = { 2, 4, 1, 0, 5, 3 };
    const reg32 camera = argument(cpu, 0);
    cpu.eax = camera < 6 ? Pov[camera] : 0;
    cpu.esp += 4;
}

// The race barrier set-up 0x578060 (thiscall).
void exoptsRaceBarriers(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 race = rc::ld32(CurrentRace);
    if (race)
    {
        // ExtraOptions 5.1.0.1340 [Gameplay] RemoveRaceBarriers: BarrierCodeCave first, the code before reads no letter
        const int id = int16_t(rc::ld16(race + 0x8a));
        const bool keep = id >= 4600 || (id >= 4300 && id <= 4399);
        rc::st8(PlayerBarriersLetter, keep ? 'I' : 'M');
        rc::st8(BarriersLetter, keep ? 'I' : 'M');
    }
    sub_578060(app, cpu);
}

}

}
