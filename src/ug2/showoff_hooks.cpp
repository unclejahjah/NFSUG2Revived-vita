// Free roam's show-off and stance (nfsu2-web's showoff.c and stance_vita.c, src/lib/winapi/revived/roam) in the
// game's own functions. Each native fills a CPU of the registers from the translated code's (revived/roam/mp/cpu.h),
// runs showoff.c's override of the function, and the override runs the game's own through sub_00XXXXXX_orig:
//   0x620AE0  the car's drawing (thiscall render info, 12 arguments, ret 0x30): a car's open parts on its type's list
//             of part animations while it is drawn, and its stance (camber, offset, tires' width) for the call
//   0x615A10  the car's lights (thiscall, ret 0xC): the hood's and the trunk's parts, open as when drawn
//   0x438140  the part animation banks thrown away (cdecl, a car type's hash): ours first, and safely
//   0x6E2310  EAGL's animation heap made at the boot (size, ret 4): big enough for every car type's part animations
//   0x6E2290  ...an object of a size from it, 0x6E2240 one of a kind (ret 4): the process heap when it is full
// The game's drawing and the lights make no guest calls of ours; the heap's and the banks' do (roam_win_call, on this
// CPU through freeroam::GuestScope).
#include <freeroam.h>
#include <lib/memmap.h>
#include <lib/winapp.h>
#include <recomp.h>

extern "C"
{
#include "../lib/winapi/revived/roam/mp/cpu_regs.h"
extern uint8_t* nfsu2_guest_base;
int roam_host_ini_value(const char* key, char* out, int n);   // roam_vita.cpp
void sub_00620ae0(CPU* c);   // showoff.c's
void sub_00615a10(CPU* c);
void sub_00438140(CPU* c);
void showoff_eagl_init(CPU* c);
void showoff_eagl_new(CPU* c);
void showoff_eagl_kind(CPU* c);
}

namespace ug2
{

void sub_620ae0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_615a10_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_438140_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6e2310_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6e2290_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6e2240_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

typedef void (*GuestFunction)(win32::WinApplication*, x86::CPU&);
typedef void (*Override)(CPU*);

// The translated code's call an override runs in: its app and CPU. Overrides nest (the drawing's game code can throw
// banks away, a bank's load can make EAGL objects), so each call's is kept on the stack and linked to the one before.
struct Call
{
    win32::WinApplication* app;
    x86::CPU* cpu;
    Call* outer;
};
Call* s_call = nullptr;

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

void invoke(win32::WinApplication* app, x86::CPU& cpu, Override over)
{
    if (!nfsu2_guest_base)   // (EAGL's heap is made at the boot, before free roam's first frame)
        nfsu2_guest_base = win32::g_guestBase;
    CPU c;
    toShim(cpu, &c);
    Call call = { app, &cpu, s_call };
    s_call = &call;
    over(&c);
    s_call = call.outer;
    fromShim(&c, cpu);
}

// calls: the override makes guest calls of its own (roam_win_call on this CPU while it runs)
void run(win32::WinApplication* app, x86::CPU& cpu, Override over, bool calls)
{
    if (!calls)
    {
        invoke(app, cpu, over);
        return;
    }
    freeroam::GuestScope scope(app, cpu);
    invoke(app, cpu, over);
}

// The game's own function, from inside an override: its registers from the override's CPU, and back.
void guestFrom(CPU* c, GuestFunction guest)
{
    Call* const call = s_call;
    if (!call)
        return;
    fromShim(c, *call->cpu);
    s_call = call->outer;   // (a native the game's code reaches meanwhile is a call of its own)
    guest(call->app, *call->cpu);
    s_call = call;
    toShim(*call->cpu, c);
}

}

namespace native
{

void sub_620ae0(win32::WinApplication* app, x86::CPU& cpu)
{
    run(app, cpu, sub_00620ae0, false);
}

void sub_615a10(win32::WinApplication* app, x86::CPU& cpu)
{
    run(app, cpu, sub_00615a10, false);
}

void sub_438140(win32::WinApplication* app, x86::CPU& cpu)
{
    run(app, cpu, sub_00438140, true);
}

// (ShowOff = 0 in scripts\\NFSU2FreeRoam.ini: the game's heap as it makes it, and no show-off)
static bool showoffOn()
{
    char v[8] = "";
    return !roam_host_ini_value("ShowOff", v, int(sizeof(v))) || v[0] != '0';
}

void sub_6e2310(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!showoffOn())
    {
        sub_6e2310_guest(app, cpu);
        return;
    }
    run(app, cpu, showoff_eagl_init, true);
}

void sub_6e2290(win32::WinApplication* app, x86::CPU& cpu)
{
    run(app, cpu, showoff_eagl_new, true);
}

void sub_6e2240(win32::WinApplication* app, x86::CPU& cpu)
{
    run(app, cpu, showoff_eagl_kind, true);
}

}

}

extern "C"
{

void sub_00620ae0_orig(CPU* c) { ug2::guestFrom(c, ug2::sub_620ae0_guest); }
void sub_00615a10_orig(CPU* c) { ug2::guestFrom(c, ug2::sub_615a10_guest); }
void sub_00438140_orig(CPU* c) { ug2::guestFrom(c, ug2::sub_438140_guest); }
void sub_006e2310(CPU* c) { ug2::guestFrom(c, ug2::sub_6e2310_guest); }
void sub_006e2290(CPU* c) { ug2::guestFrom(c, ug2::sub_6e2290_guest); }
void sub_006e2240(CPU* c) { ug2::guestFrom(c, ug2::sub_6e2240_guest); }

}
