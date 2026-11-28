#include <lib/scheduler.h>
#include <lib/timer.h>
#include <platform/config.h>
#include <platform/switches.h>

// The guest threads of SPEED2.EXE, started through the CRT's _beginthreadex thunk or the EA thread trampoline. The
// DNS, network pump, TAPI and serial threads start only for network or modem play.
namespace win32 { namespace scheduler
{

void configureGameThreads()
{
    setStartThunk(0x0075F617, 0x4C, 0x50);
    setStartThunk(0x006F57C4, 0x04, 0x08);
    setThreadRole(0x007263C7, pf::cores::Audio,  "audio mixer");
    setThreadRole(0x006F5AD7, pf::cores::Helper, "EA timer");
    setThreadRole(0x006F7195, pf::cores::Io,     "EA file device");
    setThreadRole(0x004C5D10, pf::cores::Io,     "memory card");
    setThreadRole(0x0074A9B0, pf::cores::Helper, "DNS lookup");
    setThreadRole(0x0074BA50, pf::cores::Helper, "network pump");
    setThreadRole(0x00750900, pf::cores::Helper, "TAPI");
    setThreadRole(0x00757390, pf::cores::Helper, "serial");
    if (NFSU_DEV_BOOL("ea_timer_unlocked", true))
        Timer::setUnlockedCallback(0x006F5B4D);
}

}}
