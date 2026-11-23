// Free roam's car loads, timed inside (the Vita, 2026-11-23): each friend's car costs three frames of 20 to 50 ms in
// the game's SetRideInfo (0x601110) and CarLoader::Load (0x63BC70); these natives time their callees while a slow
// step is being timed (roam_game.c step_begin/step_end), so its log line says which callee took the time.
#include <lib/winapp.h>
#include <platform/time.h>
#include <recomp.h>

#include <cstdint>
#include <cstdio>

namespace
{

const uint32_t Probed[] = {0x413440, 0x5b5350, 0x639ce0, 0x5aec20, 0x63a440, 0x63a360, 0x63a610, 0x63a5a0, 0x5f5010, 0x5f46a0, 0x63ae70, 0x57ef60, 0x63a1b0, 0x637840, 0x633b00, 0x633c30, 0x633b90, 0x57cac0, 0x43cd10};
const int ProbeN = sizeof(Probed) / sizeof(Probed[0]);
bool s_on = false;
uint64_t s_us[ProbeN];
uint32_t s_calls[ProbeN];
int s_depth[ProbeN];

struct Probe
{
    int i;
    bool active;
    uint64_t t0;
    explicit Probe(int index) : i(index), active(s_on), t0(0)
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

}

}
