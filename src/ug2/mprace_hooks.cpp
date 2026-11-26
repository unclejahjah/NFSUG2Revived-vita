// Free roam's multiplayer races (nfsu2-web's roam_game.c mp_* and mprace.c, src/lib/winapi/revived/roam) patch two of
// the game's message handlers at run time (dispatch_patch, which on the Vita only remembers the handler:
// revived/roam/mp/mp_vita.c). These natives run the remembered handler in their place, with a CPU of the registers
// it reads and writes (revived/roam/mp/cpu.h), and the handler runs the game's own through sub_00XXXXXX:
//   0x4C2170  UI_Pause's message handler (thiscall msg, obj, a3, a4; ret 0x10; only from its vtable 0x797CB8): the
//             race's Quit, for its fee (mprace.c pause_msg)
//   0x499C00  EngageRaceDialog's message handler (same shape): the race card's Solo or Multiplayer question
//             (roam_game.c mp_card)
#include <freeroam.h>
#include <lib/winapp.h>
#include <recomp.h>

extern "C"
{
#include "../lib/winapi/revived/roam/mp/cpu_regs.h"
// the handler patched over a guest function, none: nullptr (mp_vita.c)
typedef void (*RoamPatch)(CPU*);
RoamPatch roam_patch_of(uint32_t address);
void sub_004c2170(CPU* c);
void sub_00499c00(CPU* c);
}

namespace ug2
{

void sub_4c2170_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_499c00_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

typedef void (*GuestFunction)(win32::WinApplication*, x86::CPU&);

// The translated code's state of the call being patched: its CPU, and the app, while the handler runs.
struct Call
{
    win32::WinApplication* app;
    x86::CPU* cpu;
};
Call s_call;

void toShim(const x86::CPU& cpu, CPU* c)
{
    c->eax = cpu.eax; c->ecx = cpu.ecx; c->edx = cpu.edx; c->ebx = cpu.ebx;
    c->esp = cpu.esp; c->ebp = cpu.ebp; c->esi = cpu.esi; c->edi = cpu.edi;
    c->eip = 0;
}

void fromShim(const CPU* c, x86::CPU& cpu)
{
    cpu.eax = c->eax; cpu.ecx = c->ecx; cpu.edx = c->edx; cpu.ebx = c->ebx;
    cpu.esp = c->esp; cpu.ebp = c->ebp; cpu.esi = c->esi; cpu.edi = c->edi;
}

// The patched handler for address in place of the guest's, else the guest's.
void run(win32::WinApplication* app, x86::CPU& cpu, uint32_t address, GuestFunction guest)
{
    const RoamPatch patch = roam_patch_of(address);
    if (!patch || s_call.cpu)   // (a handler of ours calling into the game, which sends this message again: the game's)
    {
        guest(app, cpu);
        return;
    }
    freeroam::GuestScope scope(app, cpu);   // (the handler's own guest calls: roam_win_call on this CPU)
    CPU c;
    toShim(cpu, &c);
    s_call.app = app;
    s_call.cpu = &cpu;
    patch(&c);
    s_call.app = nullptr;
    s_call.cpu = nullptr;
    fromShim(&c, cpu);
}

// The game's own handler, from inside ours: its registers from the handler's CPU, and back.
void guestFrom(CPU* c, GuestFunction guest)
{
    win32::WinApplication* const app = s_call.app;
    x86::CPU* const cpu = s_call.cpu;
    if (!app || !cpu)
        return;
    fromShim(c, *cpu);
    s_call.cpu = nullptr;   // (as above: the game's own while it runs)
    guest(app, *cpu);
    s_call.cpu = cpu;
    toShim(*cpu, c);
}

}

namespace native
{

void sub_4c2170(win32::WinApplication* app, x86::CPU& cpu)
{
    run(app, cpu, 0x004C2170, sub_4c2170_guest);
}

void sub_499c00(win32::WinApplication* app, x86::CPU& cpu)
{
    run(app, cpu, 0x00499C00, sub_499c00_guest);
}

}

}

extern "C" void sub_004c2170(CPU* c)
{
    ug2::guestFrom(c, ug2::sub_4c2170_guest);
}

extern "C" void sub_00499c00(CPU* c)
{
    ug2::guestFrom(c, ug2::sub_499c00_guest);
}
