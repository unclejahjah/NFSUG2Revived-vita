// Winsock 2 for SPEED2.EXE on the Vita's network stack (SceNet).
//
// The game's network code (EA DirtySock: LAN discovery, the Online lobby client, the race links) is translated x86,
// so the bytes it puts on the wire are the PC game's own. What decides whether a Vita plays with PCs (on the same
// Wi-Fi, or through a community server for the Online menu) is this layer: it answers every call the way Winsock 2 on
// Windows does (structure layouts, error codes, the connect, select and close semantics DirtySock relies on). The
// behaviour follows th0mk/nfsu2-web's runtime/winsock.c (MIT), which does the same over BSD sockets.
//
// Online: EA's servers are gone. As the PC "eanationhub" mod and the NFSU2 Revived ports do, every *.ea.com name
// resolves to the community server named by online_server (default eahub.eu), and the lobby host "*pcnfs05.ea.com"
// in the game's data (0x7FBDBC) loses its '*', which asked DirtySock for SSL 2.0 on port 20921: the lobby link is
// then plain TCP to port 20920.
//
// Threads: every call comes with the execution lock held. Native sockets are always non-blocking; a call that has to
// wait (a blocking connect, accept, send or recv, select with a timeout, a name lookup) waits in slices of at most
// 50 ms with the lock released, so the other guest threads and the renderer keep running, and checks after each
// slice that the socket was not closed by another thread meanwhile (WSAEINTR, as Windows says).
#include <winapi/ws2_32.h>
#include <winapi/support.h>
#include <lib/memmap.h>
#include <lib/teb.h>
#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/time.h>
#include <recomp.h>

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <strings.h>
#include <cstdio>
#include <string>
#include <vector>

namespace win32 { namespace ws2_32
{

namespace
{

const int SOCKET_ERROR = -1;
const SOCKET INVALID_SOCKET = 0xffffffff;

// Winsock error codes: WSABASEERR + the BSD errno for the socket errors, which the Vita's stack numbers the same.
enum : int
{
    WSABASEERR = 10000,
    WSAEINTR = 10004,
    WSAEFAULT = 10014,
    WSAEINVAL = 10022,
    WSAEMFILE = 10024,
    WSAEWOULDBLOCK = 10035,
    WSAEALREADY = 10037,
    WSAENOTSOCK = 10038,
    WSAEMSGSIZE = 10040,
    WSAENOPROTOOPT = 10042,
    WSAEAFNOSUPPORT = 10047,
    WSAEOPNOTSUPP = 10045,
    WSAENETDOWN = 10050,
    WSAEISCONN = 10056,
    WSAENOTCONN = 10057,
    WSAETIMEDOUT = 10060,
    WSASYSNOTREADY = 10091,
    WSAVERNOTSUPPORTED = 10092,
    WSANOTINITIALISED = 10093,
    WSAHOST_NOT_FOUND = 11001,
};

const x86::reg32 OnlineHostAddress = 0x7FBDBC;      // "*pcnfs05.ea.com", SPEED2.EXE v1.2 NA
const char OnlineHostSecure[16] = "*pcnfs05.ea.com";
const char OnlineHostPlain[16] = "pcnfs05.ea.com";
const int NetPoolSize = 512 * 1024;
const uint32_t SliceUs = 50 * 1000;

enum State { Fresh, Listening, Connecting, Connected, Failed };

struct Sock
{
    int id = -1;                // native SceNet socket
    int type = 0;               // 1 stream, 2 datagram
    State state = Fresh;
    bool nonBlocking = false;   // FIONBIO; the native socket is always non-blocking
    bool bound = false;
    int error = 0;              // SO_ERROR: why a non-blocking connect failed, until read
    x86::reg32 recvTimeoutMs = 0;
    x86::reg32 sendTimeoutMs = 0;
};

// The table is only touched with the execution lock held.
std::map<SOCKET, Sock> s_sockets;
SOCKET s_nextHandle = 0x100;
int s_startups = 0;
int s_netState = 0;             // 0 not tried, 1 SceNet up, -1 off or failed
uint32_t s_localIp = 0;         // network order
uint32_t s_netmask = 0;
unsigned char s_mac[6];
MemMap* s_hostEntry = nullptr;  // gethostbyname's HOSTENT (one thread asks: the game's DNS thread)

x86::reg8* guest(x86::reg32 address)
{
    return rc::host(address);
}

x86::reg32 ld32(x86::reg32 address) { x86::reg32 v; memcpy(&v, guest(address), 4); return v; }
void st32(x86::reg32 address, x86::reg32 v) { memcpy(guest(address), &v, 4); }
uint16_t ld16(x86::reg32 address) { uint16_t v; memcpy(&v, guest(address), 2); return v; }
void st16(x86::reg32 address, uint16_t v) { memcpy(guest(address), &v, 2); }

bool guestRange(x86::reg32 address, x86::reg32 size)
{
    return address && MemMap::isGuestRange(address, size);
}

int fail(WinApplication* app, x86::CPU& cpu, int error)
{
    setLastError(app, cpu, x86::reg32(error));
    return SOCKET_ERROR;
}

// A failed SceNet call (0x804101xx) as its Winsock error.
int wsaError(int result)
{
    const int e = result & 0xff;
    return e ? WSABASEERR + e : WSAENETDOWN;
}

bool wouldBlock(int result)
{
    return (result & 0xff) == SCE_NET_EAGAIN;
}

const char* ipText(uint32_t ip, char* text)
{
    const unsigned char* b = reinterpret_cast<const unsigned char*>(&ip);
    snprintf(text, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return text;
}

uint32_t parseIp(const char* text)
{
    SceNetInAddr address;
    return text && *text && sceNetInetPton(SCE_NET_AF_INET, text, &address) == 1 ? address.s_addr : 0;
}

void readLocalAddress()
{
    SceNetCtlInfo info;
    s_localIp = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0 ? parseIp(info.ip_address) : 0;
    s_netmask = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_NETMASK, &info) >= 0 ? parseIp(info.netmask) : 0;
    if (!s_netmask)
        s_netmask = 0x00ffffff;   // 255.255.255.0
}

// SceNet once, on the first use. network=off keeps the port offline, as before.
bool netUp()
{
    if (s_netState)
        return s_netState > 0;
    s_netState = -1;
    if (!pf::Config::getBool("network", true))
    {
        pf::logInfo("ws2_32: network=off, the game stays offline");
        return false;
    }
    int result = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (result < 0)
    {
        pf::logError("ws2_32: the network module did not load (0x%08X), the game stays offline", unsigned(result));
        return false;
    }
    static void* pool = malloc(NetPoolSize);
    SceNetInitParam param = { pool, NetPoolSize, 0 };
    result = sceNetInit(&param);
    if (result < 0 && unsigned(result) != SCE_NET_ERROR_EBUSY)
    {
        pf::logError("ws2_32: sceNetInit failed (0x%08X), the game stays offline", unsigned(result));
        return false;
    }
    result = sceNetCtlInit();
    if (result < 0 && unsigned(result) != 0x80412102)   // SCE_NETCTL_ERROR_NOT_TERMINATED: already up
    {
        pf::logError("ws2_32: sceNetCtlInit failed (0x%08X), the game stays offline", unsigned(result));
        return false;
    }
    SceNetEtherAddr mac;
    memset(&mac, 0, sizeof(mac));
    sceNetGetMacAddress(&mac, 0);
    memcpy(s_mac, mac.data, 6);
    s_netState = 1;
    return true;
}

// A call or option the layer does not know: logged once per kind, and the game goes on (unsupported() would trap).
void note(const char* api, const char* format, unsigned value)
{
    static std::set<std::string> s_noted;
    char text[96];
    snprintf(text, sizeof(text), format, value);
    if (s_noted.insert(std::string(api) + text).second)
        pf::logInfo("ws2_32: %s: %s not supported, answered as Windows would without it", api, text);
}

bool connected()
{
    int state = SCE_NETCTL_STATE_DISCONNECTED;
    return sceNetCtlInetGetState(&state) >= 0 && state == SCE_NETCTL_STATE_CONNECTED;
}

// The game's lobby host without its '*': plain TCP to port 20920, which the community servers speak.
void onlinePatch()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    if (!MemMap::isGuestRange(OnlineHostAddress, 16))
        return;
    x86::reg8* text = guest(OnlineHostAddress);
    if (!memcmp(text, OnlineHostPlain, 16))
        return;
    if (memcmp(text, OnlineHostSecure, 16))
    {
        pf::logInfo("ws2_32: the lobby host at 0x%08X is not \"%s\" (another game version?), left as it is",
                    unsigned(OnlineHostAddress), OnlineHostSecure);
        return;
    }
    memcpy(text, OnlineHostPlain, 16);
    pf::logInfo("ws2_32: lobby host \"%s\": plain TCP to port 20920", OnlineHostPlain);
}

std::string onlineServer()
{
    std::string server = pf::Config::getString("online_server", "eahub.eu");
    return server == "off" ? std::string() : server;
}

Sock* find(SOCKET handle)
{
    auto it = s_sockets.find(handle);
    return it == s_sockets.end() ? nullptr : &it->second;
}

// Every socket call needs a started Winsock and a known socket; null with the error set otherwise.
Sock* use(WinApplication* app, x86::CPU& cpu, SOCKET handle)
{
    if (!s_startups)
    {
        setLastError(app, cpu, WSANOTINITIALISED);
        return nullptr;
    }
    Sock* s = find(handle);
    if (!s)
        setLastError(app, cpu, WSAENOTSOCK);
    return s;
}

// Guest sockaddr_in {u16 family, u16 port (network order), u32 address, 8 zero bytes} to the native one.
int guestAddress(x86::reg32 name, int length, SceNetSockaddrIn* out)
{
    if (!guestRange(name, 16) || length < 16)
        return WSAEFAULT;
    if (ld16(name) != SCE_NET_AF_INET)
        return WSAEAFNOSUPPORT;
    memset(out, 0, sizeof(*out));
    out->sin_len = sizeof(*out);
    out->sin_family = SCE_NET_AF_INET;
    memcpy(&out->sin_port, guest(name + 2), 2);
    memcpy(&out->sin_addr.s_addr, guest(name + 4), 4);
    // 255.255.255.255 goes out as this network's broadcast, which the Vita sends.
    if (out->sin_addr.s_addr == 0xffffffff && s_localIp)
        out->sin_addr.s_addr = s_localIp | ~s_netmask;
    return 0;
}

void putGuestAddress(const SceNetSockaddrIn& in, x86::reg32 name, x86::reg32 length)
{
    if (!guestRange(name, 16))
        return;
    memset(guest(name), 0, 16);
    st16(name, SCE_NET_AF_INET);
    memcpy(guest(name + 2), &in.sin_port, 2);
    memcpy(guest(name + 4), &in.sin_addr.s_addr, 4);
    if (guestRange(length, 4))
        st32(length, 16);
}

// Releases the execution lock while it waits, as Sleep does.
class Unlocked
{
public:
    Unlocked(WinApplication* app, x86::CPU& cpu) : m_app(app), m_cpu(cpu) { m_app->unlockContext(m_cpu); }
    ~Unlocked() { m_app->lockContext(m_cpu); }
private:
    WinApplication* m_app;
    x86::CPU& m_cpu;
};

// One epoll set per thread, filled for each wait and emptied after it.
int threadEpoll()
{
    static thread_local int epoll = -1;
    if (epoll < 0)
        epoll = sceNetEpollCreate("ws2_32", 0);
    return epoll;
}

struct Watch
{
    int id;
    unsigned events;    // wanted in, returned out
};

// Waits up to timeoutUs (0: a look) for events on the native sockets; without the execution lock when it waits.
void waitEvents(WinApplication* app, x86::CPU& cpu, std::vector<Watch>& watches, uint32_t timeoutUs)
{
    const int epoll = threadEpoll();
    for (Watch& w : watches)
    {
        SceNetEpollEvent event;
        memset(&event, 0, sizeof(event));
        event.events = w.events | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP;
        event.data.u32 = unsigned(&w - watches.data());
        sceNetEpollControl(epoll, SCE_NET_EPOLL_CTL_ADD, w.id, &event);
        w.events = 0;
    }
    SceNetEpollEvent events[64];
    const int count = std::min<int>(int(watches.size()), 64);
    int got;
    if (timeoutUs)
    {
        Unlocked unlocked(app, cpu);
        got = sceNetEpollWait(epoll, events, count, int(timeoutUs));
    }
    else
    {
        got = sceNetEpollWait(epoll, events, count, 0);
    }
    for (int i = 0; i < got; ++i)
        if (events[i].data.u32 < watches.size())
            watches[events[i].data.u32].events = events[i].events;
    for (Watch& w : watches)
        sceNetEpollControl(epoll, SCE_NET_EPOLL_CTL_DEL, w.id, nullptr);
}

// A non-blocking connect that finished: Connected, or Failed with its error.
void updateConnect(Sock& s, unsigned events)
{
    if (s.state != Connecting || !(events & (SCE_NET_EPOLLOUT | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP)))
        return;
    int error = 0;
    unsigned int length = sizeof(error);
    sceNetGetsockopt(s.id, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR, &error, &length);
    if (!error && (events & (SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP)))
        error = SCE_NET_ECONNREFUSED;
    if (error)
    {
        s.state = Failed;
        s.error = WSABASEERR + error;
    }
    else
    {
        s.state = Connected;
    }
}

void pollConnect(WinApplication* app, x86::CPU& cpu, Sock& s)
{
    if (s.state != Connecting)
        return;
    std::vector<Watch> watch = { { s.id, SCE_NET_EPOLLOUT } };
    waitEvents(app, cpu, watch, 0);
    updateConnect(s, watch[0].events);
}

uint64_t deadline(x86::reg32 timeoutMs)
{
    return timeoutMs ? pf::ticksUs() + uint64_t(timeoutMs) * 1000 : 0;
}

// One slice of a blocking call's wait for events on the socket; false (the error set) when the socket was closed
// meanwhile or the time ran out.
bool waitSlice(WinApplication* app, x86::CPU& cpu, SOCKET handle, unsigned events, uint64_t until, Sock*& s)
{
    uint32_t slice = SliceUs;
    if (until)
    {
        const uint64_t now = pf::ticksUs();
        if (now >= until)
        {
            setLastError(app, cpu, WSAETIMEDOUT);
            return false;
        }
        slice = uint32_t(std::min<uint64_t>(until - now, SliceUs));
    }
    std::vector<Watch> watch = { { s->id, events } };
    waitEvents(app, cpu, watch, slice);
    s = find(handle);
    if (!s || cpu.terminate)
    {
        setLastError(app, cpu, WSAEINTR);
        return false;
    }
    updateConnect(*s, watch[0].events);
    return true;
}

int nativeFlags(int flags)
{
    return (flags & 2 ? SCE_NET_MSG_PEEK : 0) | SCE_NET_MSG_DONTWAIT;   // MSG_PEEK; MSG_OOB and MSG_DONTROUTE dropped
}

int doSend(WinApplication* app, x86::CPU& cpu, SOCKET handle, x86::reg32 buf, int len, int flags, x86::reg32 to,
           int tolen)
{
    Sock* s = use(app, cpu, handle);
    if (!s)
        return SOCKET_ERROR;
    if (len < 0 || (len && !guestRange(buf, x86::reg32(len))))
        return fail(app, cpu, WSAEFAULT);
    SceNetSockaddrIn address;
    if (to && s->type == SCE_NET_SOCK_DGRAM)
    {
        const int error = guestAddress(to, tolen, &address);
        if (error)
            return fail(app, cpu, error);
    }
    else if (s->type == SCE_NET_SOCK_STREAM)
    {
        pollConnect(app, cpu, *s);
        if (s->state == Connecting && s->nonBlocking)
            return fail(app, cpu, WSAEWOULDBLOCK);
        if (s->state != Connected && s->state != Connecting)
            return fail(app, cpu, WSAENOTCONN);
        to = 0;
    }
    const uint64_t until = deadline(s->sendTimeoutMs);
    int sent = 0;
    for (;;)
    {
        int result;
        if (s->state == Connecting)
            result = SCE_NET_ERROR_EAGAIN;
        else if (to)
            result = sceNetSendto(s->id, guest(buf + sent), unsigned(len - sent), nativeFlags(flags),
                                  reinterpret_cast<const SceNetSockaddr*>(&address), sizeof(address));
        else
            result = sceNetSend(s->id, guest(buf + sent), unsigned(len - sent), nativeFlags(flags));
        if (result >= 0)
        {
            s->bound = true;
            sent += result;
            // A blocking stream send returns when everything went; a datagram is one send.
            if (s->type != SCE_NET_SOCK_STREAM || s->nonBlocking || sent >= len)
                return sent;
            continue;
        }
        if (!wouldBlock(result))
            return sent ? sent : fail(app, cpu, wsaError(result));
        if (s->nonBlocking)
            return sent ? sent : fail(app, cpu, WSAEWOULDBLOCK);
        if (!waitSlice(app, cpu, handle, SCE_NET_EPOLLOUT, until, s))
            return sent ? sent : SOCKET_ERROR;
        if (s->state == Failed)
            return fail(app, cpu, s->error);
    }
}

int doRecv(WinApplication* app, x86::CPU& cpu, SOCKET handle, x86::reg32 buf, int len, int flags, x86::reg32 from,
           x86::reg32 fromlen)
{
    Sock* s = use(app, cpu, handle);
    if (!s)
        return SOCKET_ERROR;
    if (len < 0 || (len && !guestRange(buf, x86::reg32(len))))
        return fail(app, cpu, WSAEFAULT);
    if (from && (!guestRange(fromlen, 4) || int(ld32(fromlen)) < 16 || !guestRange(from, 16)))
        return fail(app, cpu, WSAEFAULT);
    if (s->type == SCE_NET_SOCK_STREAM)
    {
        pollConnect(app, cpu, *s);
        if (s->state != Connected && !(s->state == Connecting && !s->nonBlocking))
            return fail(app, cpu, s->state == Connecting ? WSAEWOULDBLOCK : WSAENOTCONN);
    }
    else if (!s->bound)
    {
        return fail(app, cpu, WSAEINVAL);   // Windows: recvfrom on an unbound datagram socket
    }
    const uint64_t until = deadline(s->recvTimeoutMs);
    for (;;)
    {
        SceNetSockaddrIn address;
        unsigned int length = sizeof(address);
        memset(&address, 0, sizeof(address));
        const int result = s->state == Connecting ? int(SCE_NET_ERROR_EAGAIN)
                         : sceNetRecvfrom(s->id, len ? guest(buf) : nullptr, unsigned(len), nativeFlags(flags),
                                          reinterpret_cast<SceNetSockaddr*>(&address), &length);
        if (result >= 0)
        {
            if (from)
            {
                if (s->type == SCE_NET_SOCK_STREAM)
                    sceNetGetpeername(s->id, reinterpret_cast<SceNetSockaddr*>(&address), &length);
                putGuestAddress(address, from, fromlen);
            }
            return result;
        }
        if (!wouldBlock(result))
            return fail(app, cpu, wsaError(result));
        if (s->nonBlocking)
            return fail(app, cpu, WSAEWOULDBLOCK);
        if (!waitSlice(app, cpu, handle, SCE_NET_EPOLLIN, until, s))
            return SOCKET_ERROR;
        if (s->state == Failed)
            return fail(app, cpu, s->error);
    }
}

// Bytes waiting (FIONREAD): a stream's queue, a datagram socket's next datagram.
x86::reg32 bytesWaiting(Sock& s)
{
    static std::vector<unsigned char> scratch(65536);
    const int result = sceNetRecv(s.id, scratch.data(), unsigned(scratch.size()), SCE_NET_MSG_PEEK | SCE_NET_MSG_DONTWAIT);
    return result > 0 ? x86::reg32(result) : 0;
}

void putHostEntry(const char* name, uint32_t ip)
{
    if (!s_hostEntry)
        s_hostEntry = new MemMap(4096);
    // HOSTENT {char* h_name; char** h_aliases; short h_addrtype; short h_length; char** h_addr_list}, then the
    // aliases' end, the address list {address, 0}, the address and the name.
    const x86::reg32 base = s_hostEntry->getBlockStart();
    const x86::reg32 aliases = base + 16, list = base + 20, address = base + 28, text = base + 32;
    memset(guest(base), 0, 4096);
    strncpy(reinterpret_cast<char*>(guest(text)), name, 255);
    st32(base, text);
    st32(base + 4, aliases);
    st16(base + 8, SCE_NET_AF_INET);
    st16(base + 10, 4);
    st32(base + 12, list);
    st32(list, address);
    memcpy(guest(address), &ip, 4);
}

bool endsWith(const std::string& text, const char* end)
{
    const size_t n = strlen(end);
    return text.size() >= n && !strcasecmp(text.c_str() + text.size() - n, end);
}

std::string hostName()
{
    char name[32];
    snprintf(name, sizeof(name), "VITA-%02X%02X%02X", s_mac[3], s_mac[4], s_mac[5]);
    return name;
}

// A DNS lookup through SceNet's resolver, without the execution lock; 0 when the name is not found.
uint32_t resolve(WinApplication* app, x86::CPU& cpu, const std::string& name)
{
    if (uint32_t ip = parseIp(name.c_str()))
        return ip;
    Unlocked unlocked(app, cpu);
    const int resolver = sceNetResolverCreate("ws2_32", nullptr, 0);
    if (resolver < 0)
        return 0;
    SceNetInAddr address;
    address.s_addr = 0;
    const int result = sceNetResolverStartNtoa(resolver, name.c_str(), &address, 0, 0, 0);
    sceNetResolverDestroy(resolver);
    return result >= 0 ? address.s_addr : 0;
}

}

bool adapterAddress(unsigned char mac[6])
{
    if (!netUp())
        return false;
    memcpy(mac, s_mac, 6);
    return true;
}

int WSAStartup(WinApplication* app, x86::CPU& cpu, WORD wVersionRequested, x86::reg32 lpWSAData)
{
    RC_USE(cpu);
    if (!netUp())
    {
        offline(app, cpu, "WSAStartup");
        return WSASYSNOTREADY;
    }
    if (!connected())
    {
        pf::logInfo("ws2_32: WSAStartup: the Vita is not connected to a network (Wi-Fi off or not set up)");
        return WSASYSNOTREADY;
    }
    const unsigned major = wVersionRequested & 0xff, minor = wVersionRequested >> 8;
    if (!major)
        return WSAVERNOTSUPPORTED;
    if (!guestRange(lpWSAData, 400))
        return WSAEFAULT;
    // WSADATA: wVersion, wHighVersion, szDescription[257], szSystemStatus[129], iMaxSockets, iMaxUdpDg, lpVendorInfo.
    const WORD version = (major > 2 || (major == 2 && minor >= 2)) ? 0x0202 : wVersionRequested;
    memset(guest(lpWSAData), 0, 400);
    st16(lpWSAData, version);
    st16(lpWSAData + 2, 0x0202);
    strcpy(reinterpret_cast<char*>(guest(lpWSAData + 4)), "WinSock 2.0");
    strcpy(reinterpret_cast<char*>(guest(lpWSAData + 4 + 257)), "Running");
    if (!s_startups++)
    {
        readLocalAddress();
        onlinePatch();
        char ip[16], mask[16];
        const std::string server = onlineServer();
        pf::logInfo("ws2_32: WSAStartup(%u.%u): address %s, netmask %s, EA names resolve to %s", major, minor,
                    ipText(s_localIp, ip), ipText(s_netmask, mask), server.empty() ? "nothing (online_server=off)"
                                                                                   : server.c_str());
    }
    return 0;
}

int WSACleanup(WinApplication* app, x86::CPU& cpu)
{
    if (!s_startups)
        return fail(app, cpu, WSANOTINITIALISED);
    if (!--s_startups)
    {
        for (auto& entry : s_sockets)
            sceNetSocketClose(entry.second.id);
        s_sockets.clear();
    }
    return 0;
}

// The calling thread's last error, as GetLastError.
int WSAGetLastError(WinApplication* app, x86::CPU& cpu)
{
    return int(app->getMemory<x86::reg32>(cpu.efs + teb::LastError));
}

int WSAIoctl(WinApplication* app, x86::CPU& cpu, SOCKET s, DWORD dwIoControlCode, x86::reg32 lpvInBuffer,
             DWORD cbInBuffer, x86::reg32 lpvOutBuffer, DWORD cbOutBuffer, x86::reg32 lpcbBytesReturned,
             x86::reg32 lpOverlapped, x86::reg32 lpCompletionRoutine)
{
    RC_USE(lpOverlapped);
    RC_USE(lpCompletionRoutine);
    Sock* sock = use(app, cpu, s);
    if (!sock)
        return SOCKET_ERROR;
    auto returned = [&](x86::reg32 bytes) { if (guestRange(lpcbBytesReturned, 4)) st32(lpcbBytesReturned, bytes); };
    switch (dwIoControlCode)
    {
    case 0x8004667E:    // FIONBIO
        if (!guestRange(lpvInBuffer, 4) || cbInBuffer < 4)
            return fail(app, cpu, WSAEFAULT);
        sock->nonBlocking = ld32(lpvInBuffer) != 0;
        returned(0);
        return 0;
    case 0x4004667F:    // FIONREAD
        if (!guestRange(lpvOutBuffer, 4) || cbOutBuffer < 4)
            return fail(app, cpu, WSAEFAULT);
        st32(lpvOutBuffer, bytesWaiting(*sock));
        returned(4);
        return 0;
    case 0xC8000014:    // SIO_ROUTING_INTERFACE_QUERY: the local address that reaches a destination
        if (!guestRange(lpvOutBuffer, 16) || cbOutBuffer < 16)
        {
            returned(16);
            return fail(app, cpu, WSAEFAULT);
        }
        memset(guest(lpvOutBuffer), 0, 16);
        st16(lpvOutBuffer, SCE_NET_AF_INET);
        memcpy(guest(lpvOutBuffer + 4), &s_localIp, 4);
        returned(16);
        return 0;
    case 0x4004747F:    // SIO_GET_INTERFACE_LIST: INTERFACE_INFO {flags, address, broadcast, netmask} (76 bytes)
    {
        if (!guestRange(lpvOutBuffer, 76) || cbOutBuffer < 76)
            return fail(app, cpu, WSAEFAULT);
        memset(guest(lpvOutBuffer), 0, 76);
        st32(lpvOutBuffer, 1 | 2);  // IFF_UP | IFF_BROADCAST
        const uint32_t broadcast = s_localIp | ~s_netmask;
        const uint32_t values[3] = { s_localIp, broadcast, s_netmask };
        for (int i = 0; i < 3; ++i)
        {
            st16(lpvOutBuffer + 4 + 24 * i, SCE_NET_AF_INET);
            memcpy(guest(lpvOutBuffer + 8 + 24 * i), &values[i], 4);
        }
        returned(76);
        return 0;
    }
    case 0x9800000C:    // SIO_UDP_CONNRESET: the Vita never reports a reset on a datagram socket
        returned(0);
        return 0;
    default:
        note("WSAIoctl", "code 0x%08X", unsigned(dwIoControlCode));
        return fail(app, cpu, WSAEOPNOTSUPP);
    }
}

SOCKET socket(WinApplication* app, x86::CPU& cpu, int af, int type, int protocol)
{
    if (!s_startups)
    {
        setLastError(app, cpu, WSANOTINITIALISED);
        return INVALID_SOCKET;
    }
    if (af != SCE_NET_AF_INET)
    {
        setLastError(app, cpu, WSAEAFNOSUPPORT);
        return INVALID_SOCKET;
    }
    if (type != SCE_NET_SOCK_STREAM && type != SCE_NET_SOCK_DGRAM)
    {
        note("socket", "type %u (raw sockets, the LAN ping)", unsigned(type));
        RC_USE(protocol);
        setLastError(app, cpu, WSAEOPNOTSUPP);
        return INVALID_SOCKET;
    }
    const int id = sceNetSocket("nfsu2", SCE_NET_AF_INET, type, protocol);
    if (id < 0)
    {
        setLastError(app, cpu, x86::reg32(wsaError(id)));
        return INVALID_SOCKET;
    }
    const int on = 1;
    sceNetSetsockopt(id, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof(on));
    const SOCKET handle = s_nextHandle;
    s_nextHandle += 4;
    Sock& s = s_sockets[handle];
    s.id = id;
    s.type = type;
    return handle;
}

int closesocket(WinApplication* app, x86::CPU& cpu, SOCKET s)
{
    if (!use(app, cpu, s))
        return SOCKET_ERROR;
    // A thread waiting on it sees the handle gone after its slice (WSAEINTR).
    sceNetSocketClose(s_sockets[s].id);
    s_sockets.erase(s);
    return 0;
}

int shutdown(WinApplication* app, x86::CPU& cpu, SOCKET s, int how)
{
    Sock* sock = use(app, cpu, s);
    if (!sock)
        return SOCKET_ERROR;
    if (how < 0 || how > 2)
        return fail(app, cpu, WSAEINVAL);
    if (sock->type == SCE_NET_SOCK_STREAM && sock->state != Connected)
        return fail(app, cpu, WSAENOTCONN);
    const int result = sceNetShutdown(sock->id, how);
    return result < 0 ? fail(app, cpu, wsaError(result)) : 0;
}

int bind(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, int namelen)
{
    Sock* sock = use(app, cpu, s);
    if (!sock)
        return SOCKET_ERROR;
    SceNetSockaddrIn address;
    const int error = guestAddress(name, namelen, &address);
    if (error)
        return fail(app, cpu, error);
    if (sock->bound)
        return fail(app, cpu, WSAEINVAL);
    const int result = sceNetBind(sock->id, reinterpret_cast<const SceNetSockaddr*>(&address), sizeof(address));
    if (result < 0)
    {
        char ip[16];
        pf::logInfo("ws2_32: bind %s:%u failed (0x%08X)", ipText(address.sin_addr.s_addr, ip),
                    unsigned(((address.sin_port & 0xff) << 8) | (address.sin_port >> 8)), unsigned(result));
        return fail(app, cpu, wsaError(result));
    }
    sock->bound = true;
    return 0;
}

int connect(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, int namelen)
{
    Sock* sock = use(app, cpu, s);
    if (!sock)
        return SOCKET_ERROR;
    SceNetSockaddrIn address;
    const int error = guestAddress(name, namelen, &address);
    if (error)
        return fail(app, cpu, error);
    if (sock->type == SCE_NET_SOCK_STREAM)
    {
        pollConnect(app, cpu, *sock);
        if (sock->state == Connecting)
            return fail(app, cpu, WSAEALREADY);
        if (sock->state == Connected)
            return fail(app, cpu, WSAEISCONN);
        if (sock->state == Listening)
            return fail(app, cpu, WSAEINVAL);
    }
    const int result = sceNetConnect(sock->id, reinterpret_cast<const SceNetSockaddr*>(&address), sizeof(address));
    sock->bound = true;
    if (sock->type != SCE_NET_SOCK_STREAM)
        return result < 0 ? fail(app, cpu, wsaError(result)) : 0;
    if (result >= 0)
    {
        sock->state = Connected;
        return 0;
    }
    if ((result & 0xff) != SCE_NET_EINPROGRESS && !wouldBlock(result))
    {
        sock->state = Fresh;
        return fail(app, cpu, wsaError(result));
    }
    sock->state = Connecting;
    sock->error = 0;
    if (sock->nonBlocking)
        return fail(app, cpu, WSAEWOULDBLOCK);   // Windows says WSAEWOULDBLOCK, not WSAEINPROGRESS
    const uint64_t until = deadline(75 * 1000);  // about the time Windows gives a connect
    while (sock->state == Connecting)
        if (!waitSlice(app, cpu, s, SCE_NET_EPOLLOUT, until, sock))
            return SOCKET_ERROR;
    if (sock->state == Failed)
    {
        const int failed = sock->error;
        sock->state = Fresh;
        sock->error = 0;
        return fail(app, cpu, failed);
    }
    return 0;
}

int listen(WinApplication* app, x86::CPU& cpu, SOCKET s, int backlog)
{
    Sock* sock = use(app, cpu, s);
    if (!sock)
        return SOCKET_ERROR;
    if (sock->type != SCE_NET_SOCK_STREAM)
        return fail(app, cpu, WSAEOPNOTSUPP);
    if (!sock->bound)
        return fail(app, cpu, WSAEINVAL);
    const int result = sceNetListen(sock->id, std::max(1, std::min(backlog, 16)));
    if (result < 0)
        return fail(app, cpu, wsaError(result));
    sock->state = Listening;
    return 0;
}

SOCKET accept(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 addr, x86::reg32 addrlen)
{
    Sock* sock = use(app, cpu, s);
    if (!sock)
        return INVALID_SOCKET;
    if (sock->state != Listening)
    {
        setLastError(app, cpu, WSAEINVAL);
        return INVALID_SOCKET;
    }
    if (addr && (!guestRange(addrlen, 4) || int(ld32(addrlen)) < 16 || !guestRange(addr, 16)))
    {
        setLastError(app, cpu, WSAEFAULT);
        return INVALID_SOCKET;
    }
    for (;;)
    {
        SceNetSockaddrIn address;
        unsigned int length = sizeof(address);
        const int id = sceNetAccept(sock->id, reinterpret_cast<SceNetSockaddr*>(&address), &length);
        if (id >= 0)
        {
            const int on = 1;
            sceNetSetsockopt(id, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof(on));
            const SOCKET handle = s_nextHandle;
            s_nextHandle += 4;
            const bool nonBlocking = sock->nonBlocking;   // the accepted socket inherits it, as on Windows
            Sock& accepted = s_sockets[handle];
            accepted.id = id;
            accepted.type = SCE_NET_SOCK_STREAM;
            accepted.state = Connected;
            accepted.bound = true;
            accepted.nonBlocking = nonBlocking;
            if (addr)
                putGuestAddress(address, addr, addrlen);
            return handle;
        }
        if (!wouldBlock(id))
        {
            setLastError(app, cpu, x86::reg32(wsaError(id)));
            return INVALID_SOCKET;
        }
        if (sock->nonBlocking)
        {
            setLastError(app, cpu, WSAEWOULDBLOCK);
            return INVALID_SOCKET;
        }
        if (!waitSlice(app, cpu, s, SCE_NET_EPOLLIN, 0, sock))
            return INVALID_SOCKET;
    }
}

int ioctlsocket(WinApplication* app, x86::CPU& cpu, SOCKET s, LONG cmd, x86::reg32 argp)
{
    Sock* sock = use(app, cpu, s);
    if (!sock)
        return SOCKET_ERROR;
    if (!guestRange(argp, 4))
        return fail(app, cpu, WSAEFAULT);
    switch (x86::reg32(cmd))
    {
    case 0x8004667E:    // FIONBIO
        sock->nonBlocking = ld32(argp) != 0;
        return 0;
    case 0x4004667F:    // FIONREAD
        st32(argp, bytesWaiting(*sock));
        return 0;
    case 0x40047307:    // SIOCATMARK: no out-of-band data
        st32(argp, 1);
        return 0;
    default:
        note("ioctlsocket", "command 0x%08X", unsigned(cmd));
        return fail(app, cpu, WSAEINVAL);
    }
}

int setsockopt(WinApplication* app, x86::CPU& cpu, SOCKET s, int level, int optname, x86::reg32 optval,
               int optlen)
{
    Sock* sock = use(app, cpu, s);
    if (!sock)
        return SOCKET_ERROR;
    if (optlen < 1 || !guestRange(optval, x86::reg32(optlen)))
        return fail(app, cpu, WSAEFAULT);
    // A BOOL may come as one byte.
    const int value = optlen >= 4 ? int(ld32(optval)) : int(*guest(optval));
    int result = 0;
    if (level == SCE_NET_SOL_SOCKET)
    {
        switch (x86::reg32(optname))
        {
        case SCE_NET_SO_REUSEADDR:
        case SCE_NET_SO_KEEPALIVE:
        case SCE_NET_SO_BROADCAST:
        case SCE_NET_SO_OOBINLINE:
        case SCE_NET_SO_SNDBUF:
        case SCE_NET_SO_RCVBUF:
            result = sceNetSetsockopt(sock->id, level, optname, &value, sizeof(value));
            break;
        case SCE_NET_SO_LINGER:     // struct linger {u_short l_onoff, l_linger}
        {
            if (optlen < 4)
                return fail(app, cpu, WSAEFAULT);
            SceNetLinger linger = { ld16(optval), ld16(optval + 2) };
            result = sceNetSetsockopt(sock->id, level, optname, &linger, sizeof(linger));
            break;
        }
        case 0xFFFFFF7F:            // SO_DONTLINGER
        {
            SceNetLinger linger = { value ? 0 : 1, 0 };
            result = sceNetSetsockopt(sock->id, level, SCE_NET_SO_LINGER, &linger, sizeof(linger));
            break;
        }
        case SCE_NET_SO_SNDTIMEO:   // DWORD ms, kept here: the native socket never blocks
            sock->sendTimeoutMs = x86::reg32(value);
            return 0;
        case SCE_NET_SO_RCVTIMEO:
            sock->recvTimeoutMs = x86::reg32(value);
            return 0;
        case 0xFFFFFFFB:            // SO_EXCLUSIVEADDRUSE: nothing else on the Vita shares the game's ports
            return 0;
        default:
            note("setsockopt", "SOL_SOCKET option 0x%X", unsigned(optname));
            return 0;
        }
    }
    else if (level == SCE_NET_IPPROTO_TCP && optname == SCE_NET_TCP_NODELAY)
    {
        result = sceNetSetsockopt(sock->id, level, optname, &value, sizeof(value));
    }
    else if (level == SCE_NET_IPPROTO_IP && (optname == SCE_NET_IP_TTL || optname == SCE_NET_IP_TOS))
    {
        result = sceNetSetsockopt(sock->id, level, optname, &value, sizeof(value));
    }
    else
    {
        note("setsockopt", "option 0x%X", unsigned(level << 16 | optname));
        return 0;
    }
    if (result < 0)
        pf::logInfo("ws2_32: setsockopt level 0x%X option 0x%X = %d failed (0x%08X), ignored", unsigned(level),
                    unsigned(optname), value, unsigned(result));
    return 0;
}

int getsockopt(WinApplication* app, x86::CPU& cpu, SOCKET s, int level, int optname, x86::reg32 optval,
               x86::reg32 optlen)
{
    Sock* sock = use(app, cpu, s);
    if (!sock)
        return SOCKET_ERROR;
    if (!guestRange(optlen, 4) || int(ld32(optlen)) < 4 || !guestRange(optval, 4))
        return fail(app, cpu, WSAEFAULT);
    int value = 0;
    if (level == SCE_NET_SOL_SOCKET)
    {
        switch (x86::reg32(optname))
        {
        case 0x0002:                // SO_ACCEPTCONN
            value = sock->state == Listening;
            break;
        case SCE_NET_SO_ERROR:      // then cleared
            pollConnect(app, cpu, *sock);
            value = sock->error;
            sock->error = 0;
            if (sock->state == Failed)
                sock->state = Fresh;
            break;
        case SCE_NET_SO_TYPE:
            value = sock->type;
            break;
        case SCE_NET_SO_SNDTIMEO:
            value = int(sock->sendTimeoutMs);
            break;
        case SCE_NET_SO_RCVTIMEO:
            value = int(sock->recvTimeoutMs);
            break;
        default:
        {
            unsigned int length = sizeof(value);
            const int result = sceNetGetsockopt(sock->id, level, optname, &value, &length);
            if (result < 0)
                return fail(app, cpu, WSAENOPROTOOPT);
        }
        }
    }
    else
    {
        unsigned int length = sizeof(value);
        const int result = sceNetGetsockopt(sock->id, level, optname, &value, &length);
        if (result < 0)
            return fail(app, cpu, WSAENOPROTOOPT);
    }
    st32(optval, x86::reg32(value));
    st32(optlen, 4);
    return 0;
}

int send(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags)
{
    return doSend(app, cpu, s, buf, len, flags, 0, 0);
}

int sendto(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags, x86::reg32 to,
           int tolen)
{
    return doSend(app, cpu, s, buf, len, flags, to, tolen);
}

int recv(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags)
{
    return doRecv(app, cpu, s, buf, len, flags, 0, 0);
}

int recvfrom(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 buf, int len, int flags, x86::reg32 from,
             x86::reg32 fromlen)
{
    return doRecv(app, cpu, s, buf, len, flags, from, fromlen);
}

// fd_set: {u_int fd_count; SOCKET fd_array[fd_count]}; each set comes back holding its ready sockets.
int select(WinApplication* app, x86::CPU& cpu, int nfds, x86::reg32 readfds, x86::reg32 writefds,
           x86::reg32 exceptfds, x86::reg32 timeout)
{
    RC_USE(nfds);
    if (!s_startups)
        return fail(app, cpu, WSANOTINITIALISED);
    const x86::reg32 sets[3] = { readfds, writefds, exceptfds };
    std::vector<SOCKET> handles[3];
    size_t total = 0;
    for (int k = 0; k < 3; ++k)
    {
        if (!sets[k])
            continue;
        if (!guestRange(sets[k], 4))
            return fail(app, cpu, WSAEFAULT);
        const x86::reg32 count = ld32(sets[k]);
        if (count > 1024 || (count && !guestRange(sets[k] + 4, 4 * count)))
            return fail(app, cpu, WSAEINVAL);
        for (x86::reg32 i = 0; i < count; ++i)
        {
            const SOCKET handle = ld32(sets[k] + 4 + 4 * i);
            if (!find(handle))
                return fail(app, cpu, WSAENOTSOCK);
            handles[k].push_back(handle);
        }
        total += count;
    }
    if (!total)
        return fail(app, cpu, WSAEINVAL);
    uint64_t until = 0;
    bool forever = true;
    if (timeout)
    {
        if (!guestRange(timeout, 8))
            return fail(app, cpu, WSAEFAULT);
        const int64_t seconds = std::max<int32_t>(0, int32_t(ld32(timeout)));
        const int64_t micros = std::max<int32_t>(0, int32_t(ld32(timeout + 4)));
        until = pf::ticksUs() + uint64_t(seconds * 1000000 + micros);
        forever = false;
    }
    bool waited = false;
    for (;;)
    {
        // One watch per distinct socket, with what any set asks of it.
        std::vector<Watch> watches;
        std::vector<SOCKET> watched;
        for (int k = 0; k < 3; ++k)
            for (SOCKET handle : handles[k])
            {
                Sock* s = find(handle);
                if (!s)
                    return fail(app, cpu, waited ? WSAEINTR : WSAENOTSOCK);
                const size_t at = std::find(watched.begin(), watched.end(), handle) - watched.begin();
                if (at == watched.size())
                {
                    watched.push_back(handle);
                    watches.push_back({ s->id, 0 });
                }
                watches[at].events |= k == 0 ? SCE_NET_EPOLLIN : SCE_NET_EPOLLOUT;
            }
        const uint64_t now = pf::ticksUs();
        const uint32_t slice = !waited ? 0 : forever ? SliceUs : uint32_t(std::min<uint64_t>(until > now ? until - now : 0, SliceUs));
        waitEvents(app, cpu, watches, slice);
        waited = true;
        std::vector<SOCKET> ready[3];
        int count = 0;
        for (int k = 0; k < 3; ++k)
            for (SOCKET handle : handles[k])
            {
                Sock* s = find(handle);
                if (!s)
                    return fail(app, cpu, WSAEINTR);
                const unsigned events = watches[std::find(watched.begin(), watched.end(), handle) - watched.begin()].events;
                updateConnect(*s, events);
                bool yes = false;
                if (k == 0)         // data, a connection to accept, or the end or a reset to read
                    yes = (events & (SCE_NET_EPOLLIN | SCE_NET_EPOLLHUP | SCE_NET_EPOLLERR)) && s->state != Connecting
                          && !(s->type == SCE_NET_SOCK_STREAM && s->state == Fresh);
                else if (k == 1)    // connected and room to send
                    yes = (events & SCE_NET_EPOLLOUT) && (s->type != SCE_NET_SOCK_STREAM || s->state == Connected);
                else                // a non-blocking connect that failed
                    yes = s->state == Failed;
                if (yes)
                {
                    ready[k].push_back(handle);
                    ++count;
                }
            }
        const bool timedOut = !forever && pf::ticksUs() >= until;
        if (count || timedOut || cpu.terminate)
        {
            for (int k = 0; k < 3; ++k)
            {
                if (!sets[k])
                    continue;
                st32(sets[k], x86::reg32(ready[k].size()));
                for (size_t i = 0; i < ready[k].size(); ++i)
                    st32(sets[k] + 4 + 4 * x86::reg32(i), ready[k][i]);
            }
            return count;
        }
    }
}

int getpeername(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, x86::reg32 namelen)
{
    Sock* sock = use(app, cpu, s);
    if (!sock)
        return SOCKET_ERROR;
    if (!guestRange(namelen, 4) || int(ld32(namelen)) < 16 || !guestRange(name, 16))
        return fail(app, cpu, WSAEFAULT);
    // DirtySock watches its connections with getpeername: WSAENOTCONN until connected, and after a reset.
    pollConnect(app, cpu, *sock);
    if (sock->type == SCE_NET_SOCK_STREAM && sock->state != Connected)
        return fail(app, cpu, WSAENOTCONN);
    SceNetSockaddrIn address;
    unsigned int length = sizeof(address);
    const int result = sceNetGetpeername(sock->id, reinterpret_cast<SceNetSockaddr*>(&address), &length);
    if (result < 0)
        return fail(app, cpu, WSAENOTCONN);
    putGuestAddress(address, name, namelen);
    return 0;
}

int getsockname(WinApplication* app, x86::CPU& cpu, SOCKET s, x86::reg32 name, x86::reg32 namelen)
{
    Sock* sock = use(app, cpu, s);
    if (!sock)
        return SOCKET_ERROR;
    if (!guestRange(namelen, 4) || int(ld32(namelen)) < 16 || !guestRange(name, 16))
        return fail(app, cpu, WSAEFAULT);
    if (!sock->bound)
        return fail(app, cpu, WSAEINVAL);
    SceNetSockaddrIn address;
    unsigned int length = sizeof(address);
    const int result = sceNetGetsockname(sock->id, reinterpret_cast<SceNetSockaddr*>(&address), &length);
    if (result < 0)
        return fail(app, cpu, wsaError(result));
    putGuestAddress(address, name, namelen);
    return 0;
}

// *.ea.com goes to the community server; this Vita's own name to its address; dotted addresses as they are.
x86::reg32 gethostbyname(WinApplication* app, x86::CPU& cpu, LPCSTR name)
{
    if (!s_startups)
    {
        setLastError(app, cpu, WSANOTINITIALISED);
        return 0;
    }
    if (!name)
    {
        setLastError(app, cpu, WSAEFAULT);
        return 0;
    }
    const std::string requested = name;
    std::string lookup = requested;
    if (!strcasecmp(requested.c_str(), "ea.com") || endsWith(requested, ".ea.com"))
        lookup = onlineServer();
    uint32_t ip = 0;
    if (!strcasecmp(requested.c_str(), hostName().c_str()) || requested.empty())
        ip = s_localIp;
    else if (!lookup.empty())
        ip = resolve(app, cpu, lookup);
    char text[16];
    pf::logInfo("ws2_32: gethostbyname %s%s%s -> %s", requested.c_str(), lookup != requested ? " via " : "",
                lookup != requested ? (lookup.empty() ? "(online_server=off)" : lookup.c_str()) : "",
                ip ? ipText(ip, text) : "not found");
    if (!ip)
    {
        setLastError(app, cpu, WSAHOST_NOT_FOUND);
        return 0;
    }
    putHostEntry(requested.c_str(), ip);
    return s_hostEntry->getBlockStart();
}

int gethostname(WinApplication* app, x86::CPU& cpu, x86::reg32 name, int namelen)
{
    if (!s_startups)
        return fail(app, cpu, WSANOTINITIALISED);
    const std::string host = hostName();
    if (namelen <= int(host.size()) || !guestRange(name, x86::reg32(namelen)))
        return fail(app, cpu, WSAEFAULT);
    memcpy(guest(name), host.c_str(), host.size() + 1);
    return 0;
}

}}
