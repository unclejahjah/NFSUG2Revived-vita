#ifndef WINAPI_REVIVED_H_
#define WINAPI_REVIVED_H_

#include <x86.h>

namespace win32 { class WinApplication; }

// LAN play and Online through the NFSU2 Revived server (nfsu2.xyz): the game's Winsock and Netbios calls go to
// nfsu2-web's Winsock layer (src/lib/winapi/revived/winsock.c, net_lan.c), whose virtual LAN is carried to the
// server's switch over a TLS WebSocket (revived_link.cpp). Settings: network=revived (the default; direct: the
// Vita's own network and online_server, off: no network), revived_server (default nfsu2.xyz), revived_name (the
// name other players see; default the Vita's user name).
namespace win32 { namespace revived
{

// network=revived (read once).
bool enabled();
// One import of the game (dll "ws2_32" or "netapi32"), its arguments at cpu.esp + 4: the value for eax.
x86::reg32 call(WinApplication* app, x86::CPU& cpu, const char* dll, const char* name);

}}

#endif
