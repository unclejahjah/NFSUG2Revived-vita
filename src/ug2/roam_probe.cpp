// Free roam's car loads, timed inside (the Vita, 2026-11-23): each friend's car costs three frames of 20 to 50 ms in
// the game's SetRideInfo (0x601110) and CarLoader::Load (0x63BC70); these natives time their callees while a slow
// step is being timed (roam_game.c step_begin/step_end), so its log line says which callee took the time.
#include <lib/winapp.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <recomp.h>

#include <cstdint>
#include <cstdio>

namespace
{

const uint32_t Probed[] = {0x413440, 0x5b5350, 0x639ce0, 0x5aec20, 0x63a440, 0x63a360, 0x63a610, 0x63a5a0, 0x5f5010, 0x5f46a0, 0x63ae70, 0x57ef60, 0x63a1b0, 0x637840, 0x633b00, 0x633c30, 0x633b90, 0x57cac0, 0x43cd10, 0x638190, 0x631e20, 0x634800, 0x61ff40, 0x615280, 0x636840, 0x6204d0, 0x61fe10, 0x60c280, 0x61f700, 0x48f6a0, 0x61c9d0, 0x61c570, 0x57ca10, 0x579b90, 0x610e00, 0x488f10, 0x48b8b0, 0x57c5b0, 0x57b5d0, 0x57c4a0, 0x5798f0, 0x440e50, 0x488ff0};
const int ProbeN = sizeof(Probed) / sizeof(Probed[0]);
bool s_on = false;
int s_thread = 0;   // the thread probing (roam's, the game's main thread): calls on others are not counted
uint64_t s_us[ProbeN];
uint32_t s_calls[ProbeN];
int s_depth[ProbeN];

struct Probe
{
    int i;
    bool active;
    uint64_t t0;
    explicit Probe(int index) : i(index), active(s_on && pf::Thread::currentId() == s_thread), t0(0)
    {
        if (active && !s_depth[i]++)
            t0 = pf::ticksUs();
    }
    ~Probe()
    {
        if (active && s_depth[i] > 0 && !--s_depth[i])
        {
            s_us[i] += pf::ticksUs() - t0;
            ++s_calls[i];
        }
    }
};

}

extern "C" void roam_probe_begin(void)
{
    for (int i = 0; i < ProbeN; ++i)
    {
        s_us[i] = 0;
        s_calls[i] = 0;
        s_depth[i] = 0;
    }
    s_thread = pf::Thread::currentId();
    s_on = true;
}

// the callees that took 1 ms or more since roam_probe_begin, as " 0x601110 12.3 ms (2),"; probing stops
extern "C" void roam_probe_end(char* out, int n)
{
    s_on = false;
    int at = 0;
    out[0] = 0;
    for (int i = 0; i < ProbeN && at < n - 40; ++i)
        if (s_us[i] >= 1000)
            at += snprintf(out + at, size_t(n - at), " %x %.1f ms (%u),", unsigned(Probed[i]), s_us[i] / 1000.0, unsigned(s_calls[i]));
    if (at)
        out[at - 1] = 0;
}

namespace ug2
{
void sub_413440_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5b5350_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_639ce0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5aec20_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_63a440_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_63a360_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_63a610_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_63a5a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5f5010_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5f46a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_63ae70_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_57ef60_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_63a1b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_637840_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_633b00_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_633c30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_633b90_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_57cac0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_43cd10_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_638190_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_631e20_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_634800_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_61ff40_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_615280_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_636840_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6204d0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_61fe10_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_60c280_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_61f700_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_48f6a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_61c9d0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_61c570_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_57ca10_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_579b90_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_610e00_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_488f10_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_48b8b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_57c5b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_57b5d0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_57c4a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5798f0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_440e50_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_488ff0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_413440(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(0);
    sub_413440_guest(app, cpu);
}

void sub_5b5350(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(1);
    sub_5b5350_guest(app, cpu);
}

void sub_639ce0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(2);
    sub_639ce0_guest(app, cpu);
}

void sub_5aec20(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(3);
    sub_5aec20_guest(app, cpu);
}

void sub_63a440(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(4);
    sub_63a440_guest(app, cpu);
}

void sub_63a360(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(5);
    sub_63a360_guest(app, cpu);
}

void sub_63a610(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(6);
    sub_63a610_guest(app, cpu);
}

void sub_63a5a0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(7);
    sub_63a5a0_guest(app, cpu);
}

void sub_5f5010(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(8);
    sub_5f5010_guest(app, cpu);
}

void sub_5f46a0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(9);
    sub_5f46a0_guest(app, cpu);
}

void sub_63ae70(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(10);
    sub_63ae70_guest(app, cpu);
}

void sub_57ef60(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(11);
    sub_57ef60_guest(app, cpu);
}

void sub_63a1b0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(12);
    sub_63a1b0_guest(app, cpu);
}

void sub_637840(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(13);
    sub_637840_guest(app, cpu);
}

void sub_633b00(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(14);
    sub_633b00_guest(app, cpu);
}

void sub_633c30(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(15);
    sub_633c30_guest(app, cpu);
}

void sub_633b90(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(16);
    sub_633b90_guest(app, cpu);
}

void sub_57cac0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(17);
    sub_57cac0_guest(app, cpu);
}

void sub_43cd10(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(18);
    sub_43cd10_guest(app, cpu);
}

void sub_638190(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(19);
    sub_638190_guest(app, cpu);
}

void sub_631e20(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(20);
    sub_631e20_guest(app, cpu);
}

void sub_634800(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(21);
    sub_634800_guest(app, cpu);
}

void sub_61ff40(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(22);
    sub_61ff40_guest(app, cpu);
}

void sub_615280(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(23);
    sub_615280_guest(app, cpu);
}

void sub_636840(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(24);
    sub_636840_guest(app, cpu);
}

void sub_6204d0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(25);
    sub_6204d0_guest(app, cpu);
}

void sub_61fe10(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(26);
    sub_61fe10_guest(app, cpu);
}

void sub_60c280(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(27);
    sub_60c280_guest(app, cpu);
}

void sub_61f700(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(28);
    sub_61f700_guest(app, cpu);
}

void sub_48f6a0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(29);
    sub_48f6a0_guest(app, cpu);
}

void sub_61c9d0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(30);
    sub_61c9d0_guest(app, cpu);
}

void sub_61c570(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(31);
    sub_61c570_guest(app, cpu);
}

void sub_57ca10(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(32);
    sub_57ca10_guest(app, cpu);
}

void sub_579b90(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(33);
    sub_579b90_guest(app, cpu);
}

void sub_610e00(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(34);
    sub_610e00_guest(app, cpu);
}

void sub_488f10(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(35);
    sub_488f10_guest(app, cpu);
}

void sub_48b8b0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(36);
    sub_48b8b0_guest(app, cpu);
}

void sub_57c5b0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(37);
    sub_57c5b0_guest(app, cpu);
}

void sub_57b5d0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(38);
    sub_57b5d0_guest(app, cpu);
}

void sub_57c4a0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(39);
    sub_57c4a0_guest(app, cpu);
}

void sub_5798f0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(40);
    sub_5798f0_guest(app, cpu);
}

void sub_440e50(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(41);
    sub_440e50_guest(app, cpu);
}

void sub_488ff0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Probe probe(42);
    sub_488ff0_guest(app, cpu);
}

}

}
