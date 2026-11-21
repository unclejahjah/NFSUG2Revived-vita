// What nfsu2-web's Winsock layer (winsock.c, net_lan.c) asks of its runtime (cpu.h, host_abi.h), on the Vita port:
// guest memory, the calling thread's last error, the execution lock while a call waits, and the log.
#include <winapi/revived.h>
#include <winapi/support.h>
#include <lib/memmap.h>
#include <lib/teb.h>
#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>

#include "cpu.h"

#include <cstring>
#include <strings.h>
#include <string>

extern "C" int winsock_call(CPU* c, const char* dll, const char* n, uint32_t* out);
// revived_link.cpp: SceNet up, the link thread started
bool revived_link_start();

namespace
{

// The guest thread inside a call (one host thread per guest thread).
thread_local win32::WinApplication* t_app = nullptr;
thread_local x86::CPU* t_cpu = nullptr;
thread_local int t_unlocked = 0;

}

extern "C"
{

uint8_t* nfsu2_guest_base = nullptr;

int mem_is_committed(uint32_t address)
{
    return address && win32::MemMap::isGuestRange(address, 1);
}

uint32_t nfsu2_guest_alloc(uint32_t bytes)
{
    win32::MemMap* block = new win32::MemMap((bytes + 4095) & ~4095u);
    const uint32_t start = block->getBlockStart();
    memset(win32::g_guestBase + start, 0, bytes);
    return start;
}

void nfsu2_log(const char* line)
{
    pf::logInfo("revived: %s", line);
}

void guest_wait_begin(void)
{
    if (t_app && t_cpu && !t_unlocked++)
        t_app->unlockContext(*t_cpu);
}

void guest_wait_end(void)
{
    if (t_app && t_cpu && t_unlocked && !--t_unlocked)
        t_app->lockContext(*t_cpu);
}

void win32_set_last_error(CPU*, uint32_t e)
{
    if (t_app && t_cpu)
        win32::setLastError(t_app, *t_cpu, e);
}

uint32_t win32_get_last_error(CPU*)
{
    if (!t_app || !t_cpu)
        return 0;
    return t_app->getMemory<x86::reg32>(t_cpu->efs + win32::teb::LastError);
}

// nfsu2-web's Online extras (the persona line over the game, the lobby search filter): not on the Vita.
void online_lobby_read(uint32_t, const uint8_t*, int) {}
void olfilter_lobby_mark(uint32_t, uint8_t*, int) {}
void olfilter_lobby_sent(uint32_t, uint8_t*, int) {}

}

namespace win32 { namespace revived
{

bool enabled()
{
    static const bool s_enabled = [] {
        // revived, on (as before the Revived link) or unset; direct and off are the others
        const char* mode = pf::Config::getString("network", "revived");
        return strcasecmp(mode, "direct") && strcasecmp(mode, "off") && strcasecmp(mode, "0") &&
               strcasecmp(mode, "false") && strcasecmp(mode, "no");
    }();
    return s_enabled;
}

x86::reg32 call(WinApplication* app, x86::CPU& cpu, const char* dll, const char* name)
{
    if (!nfsu2_guest_base)
        nfsu2_guest_base = g_guestBase;
    // Every Winsock call starts the link once (SceNet, the link thread); without a network the calls see the
    // virtual LAN down, as the browser does without its server.
    static const bool s_link = revived_link_start();
    (void)s_link;
    win32::WinApplication* const outerApp = t_app;
    x86::CPU* const outerCpu = t_cpu;
    t_app = app;
    t_cpu = &cpu;
    CPU c;
    c.esp = cpu.esp + 4;   // winsock.c reads the first argument at esp
    c.fs_base = cpu.efs;
    c.eax = cpu.eax;
    c.eip = 0;
    uint32_t out = 0;
    if (!winsock_call(&c, dll, name, &out))
    {
        pf::logError("revived: %s!%s is not in the Winsock layer", dll, name);
        out = uint32_t(-1);
    }
    t_app = outerApp;
    t_cpu = outerCpu;
    return out;
}

}}
