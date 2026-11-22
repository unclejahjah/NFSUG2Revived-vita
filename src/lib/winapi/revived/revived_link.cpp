// The "page" of nfsu2-web's virtual LAN on the Vita: the link between the net mailboxes (net_proto.h) and the NFSU2
// Revived server's switch, a WebSocket over TLS (wss://<revived_server>/lan).
//
// nfsu2-web's page does this in TypeScript (web/src/net/index.ts); this follows it:
//   - connect while a mailbox wants the switch (LAN play: from the game's WSAStartup to its last WSACleanup), let it
//     go IdleMs after none does (NET_PAGE_IDLE: the server frees the place at once);
//   - the hello names the player and says whether LAN play is open ("lan") and which UDP ports have a socket
//     ("udp"), and {"t":"lan"} says it again when that changes;
//   - welcome: the address on the switch (vip, netmask) into every mailbox, NET_PAGE_ONLINE;
//   - peers / delta: the players on the switch, as peer_bits; one that left resets its TCP connections
//     (NET_LAN_TCP_GONE into the LAN mailbox);
//   - C's TX ring: each datagram as one binary frame (big-endian: kind, 0, 0, 0, source vip, destination vip,
//     source port, destination port, payload); kinds 16 and up are for the page only and are dropped here;
//   - binary frames from the switch into the RX ring of the mailbox their kind is for; LAN TCP frames wait while the
//     ring is full (a stream must not lose a segment), the others are dropped as UDP would be; a SYN with no LAN
//     mailbox is refused (RST);
//   - a ping every 10 s (switch_rtt_ms); the server is full: in line ("queue"), with the ticket kept for reconnects;
//     the link lost: reconnect with the address's token (resume), backing off from 250 ms to 5 s.
// Plain frames only: the hello asks for no bundles or deltas.
//
// One thread runs the link. C's side wakes it through tx_bell (vlan_atomic_notify); it also looks at the TX rings
// every few milliseconds while the switch is connected, as the server gathers what it sends a player for 4 ms.
#include <platform/config.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/tls.h>
#include <winapi/ws2_32.h>

#include "cpu.h"
#include "host_abi.h"
#include "net_proto.h"

#include <psp2/apputil.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/system_param.h>

#include <mbedtls/base64.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha1.h>

#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

extern "C" uint64_t ws_now_us(void);
// free roam's friends on the Vita (roam_vita.cpp): what the server said ({"t":"friend",...}); weak, for builds without
extern "C" void roam_host_friend(const char* op, const char* name, const char* st, const char* done)
    __attribute__((weak));

namespace
{
// control messages from other threads (revived_link_control_send), sent by the link thread
pthread_mutex_t s_controlLock = PTHREAD_MUTEX_INITIALIZER;
std::vector<std::string> s_controlOut;
}

namespace
{

const uint32_t ConnectTimeoutMs = 10000;
const uint64_t IdleMs = 10000;          // no mailbox wants the switch this long: the place goes back
const uint64_t PingMs = 10000;
const uint64_t PeersWaitMs = 1000;      // after the welcome, for the players on the switch
const uint32_t PollMs = 4;              // while connected: TX rings looked at this often
const size_t MaxMessage = 1 << 20;      // a WebSocket message larger than this ends the link
const uint16_t CloseKicked = 4001, CloseBanned = 4003, CloseRename = 4004, CloseFull = 1013;

const size_t FrameHeader = 16;          // kind, 0, 0, 0, src vip, dst vip, src port, dst port (big-endian)

uint64_t nowMs()
{
    return sceKernelGetProcessTimeWide() / 1000;
}

uint32_t ld(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
void st(uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
void putBe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
void putBe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }

// ---- the wake-ups: vlan_atomic_wait32 / notify (memory.atomic.wait32 / notify of the browser build) -----------------
// One mutex and one condition for every word: a notify wakes all waiters, each looks at its word again.
pf::Mutex& waitMutex()
{
    static pf::Mutex s_mutex("revived_wait", false);
    return s_mutex;
}

pf::CondVar& waitCond()
{
    static pf::CondVar s_cond("revived_wait", waitMutex());
    return s_cond;
}

// ---- a small JSON reader, enough for the switch's control messages ---------------------------------------------------
struct Json
{
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    double number = 0;
    bool boolean = false;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* get(const char* key) const
    {
        for (const auto& f : fields)
            if (f.first == key)
                return &f.second;
        return nullptr;
    }
    std::string str(const char* key) const
    {
        const Json* v = get(key);
        return v && v->type == String ? v->text : std::string();
    }
    bool num(const char* key, double& out) const
    {
        const Json* v = get(key);
        if (!v || v->type != Number)
            return false;
        out = v->number;
        return true;
    }
    bool truthy(const char* key) const
    {
        const Json* v = get(key);
        return v && ((v->type == Bool && v->boolean) || (v->type == Number && v->number != 0));
    }
};

class JsonParser
{
public:
    JsonParser(const char* p, const char* end) : m_p(p), m_end(end) {}
    bool parse(Json& out, int depth = 0)
    {
        if (depth > 16)
            return false;
        space();
        if (m_p >= m_end)
            return false;
        const char c = *m_p;
        if (c == '{')
        {
            out.type = Json::Object;
            ++m_p;
            space();
            if (m_p < m_end && *m_p == '}')
                return ++m_p, true;
            for (;;)
            {
                space();
                Json key;
                if (m_p >= m_end || *m_p != '"' || !string(key.text))
                    return false;
                space();
                if (m_p >= m_end || *m_p++ != ':')
                    return false;
                out.fields.emplace_back(key.text, Json());
                if (!parse(out.fields.back().second, depth + 1))
                    return false;
                space();
                if (m_p < m_end && *m_p == ',')
                {
                    ++m_p;
                    continue;
                }
                return m_p < m_end && *m_p++ == '}';
            }
        }
        if (c == '[')
        {
            out.type = Json::Array;
            ++m_p;
            space();
            if (m_p < m_end && *m_p == ']')
                return ++m_p, true;
            for (;;)
            {
                out.items.emplace_back();
                if (!parse(out.items.back(), depth + 1))
                    return false;
                space();
                if (m_p < m_end && *m_p == ',')
                {
                    ++m_p;
                    continue;
                }
                return m_p < m_end && *m_p++ == ']';
            }
        }
        if (c == '"')
        {
            out.type = Json::String;
            return string(out.text);
        }
        if (word("true"))
        {
            out.type = Json::Bool;
            out.boolean = true;
            return true;
        }
        if (word("false"))
        {
            out.type = Json::Bool;
            return true;
        }
        if (word("null"))
            return true;
        // a number
        char buffer[48];
        size_t n = 0;
        while (m_p < m_end && n < sizeof(buffer) - 1 && strchr("+-0123456789.eE", *m_p))
            buffer[n++] = *m_p++;
        if (!n)
            return false;
        buffer[n] = 0;
        out.type = Json::Number;
        out.number = strtod(buffer, nullptr);
        return true;
    }

private:
    void space()
    {
        while (m_p < m_end && (*m_p == ' ' || *m_p == '\t' || *m_p == '\r' || *m_p == '\n'))
            ++m_p;
    }
    bool word(const char* w)
    {
        const size_t n = strlen(w);
        if (size_t(m_end - m_p) < n || memcmp(m_p, w, n))
            return false;
        m_p += n;
        return true;
    }
    static void utf8(std::string& out, uint32_t c)
    {
        if (c < 0x80)
            out += char(c);
        else if (c < 0x800)
            out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
        else if (c < 0x10000)
            out += char(0xE0 | c >> 12), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
        else
            out += char(0xF0 | c >> 18), out += char(0x80 | ((c >> 12) & 0x3F)), out += char(0x80 | ((c >> 6) & 0x3F)),
                out += char(0x80 | (c & 0x3F));
    }
    bool hex4(uint32_t& v)
    {
        if (m_end - m_p < 4)
            return false;
        v = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char h = *m_p++;
            v <<= 4;
            if (h >= '0' && h <= '9') v |= uint32_t(h - '0');
            else if (h >= 'a' && h <= 'f') v |= uint32_t(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= uint32_t(h - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out)
    {
        ++m_p;   // the opening quote
        while (m_p < m_end)
        {
            const char c = *m_p++;
            if (c == '"')
                return true;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (m_p >= m_end)
                return false;
            const char e = *m_p++;
            switch (e)
            {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u':
            {
                uint32_t v;
                if (!hex4(v))
                    return false;
                if (v >= 0xD800 && v < 0xDC00 && m_end - m_p >= 6 && m_p[0] == '\\' && m_p[1] == 'u')
                {
                    m_p += 2;
                    uint32_t low;
                    if (!hex4(low))
                        return false;
                    v = 0x10000 + ((v - 0xD800) << 10) + (low - 0xDC00);
                }
                utf8(out, v);
                break;
            }
            default: out += e; break;
            }
        }
        return false;
    }
    const char* m_p;
    const char* m_end;
};

std::string jsonText(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\', out += char(c);
        else if (c < 0x20)
        {
            char e[8];
            snprintf(e, sizeof(e), "\\u%04x", c);
            out += e;
        }
        else
            out += char(c);
    }
    return out + "\"";
}

std::string ipText(uint32_t ip)
{
    char t[16];
    snprintf(t, sizeof(t), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return t;
}

uint32_t ipNumber(const std::string& s, uint32_t fallback)
{
    unsigned a, b, c, d;
    if (sscanf(s.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return fallback;
    return a << 24 | b << 16 | c << 8 | d;
}

// ---- one mailbox -----------------------------------------------------------------------------------------------------
struct Endpoint
{
    const char* name;           // "lan", "roam"
    net_mailbox* mb = nullptr;
    std::deque<std::vector<uint8_t>> waiting;   // LAN TCP frames for a full RX ring, in order

    net_slot* slot(uint32_t offset, uint32_t index, uint32_t slots)
    {
        return reinterpret_cast<net_slot*>(reinterpret_cast<uint8_t*>(mb) + offset + (index % slots) * NET_SLOT_BYTES);
    }
    bool wants() const { return mb && ld(&mb->want) != 0; }
    void ring()
    {
        __atomic_add_fetch(&mb->rx_bell, 1, __ATOMIC_SEQ_CST);
        vlan_atomic_notify(reinterpret_cast<int*>(&mb->rx_bell), 0xFFFFFFFFu);
    }
    // a frame from the switch into the RX ring; false: it is full
    bool put(const uint8_t* f, size_t size)
    {
        const uint32_t head = ld(&mb->rx_head), tail = ld(&mb->rx_tail);
        if (head - tail >= NET_RX_SLOTS)
            return false;
        net_slot* s = slot(mb->rx_off, head, NET_RX_SLOTS);
        const uint32_t len = uint32_t(size - FrameHeader);
        s->len = len;
        s->kind = f[0];
        s->src_ip = be32(f + 4);
        s->dst_ip = be32(f + 8);
        s->src_port = be16(f + 12);
        s->dst_port = be16(f + 14);
        s->t_ms = double(ws_now_us()) / 1000.0;
        memcpy(s->data, f + FrameHeader, len);
        st(&mb->rx_head, head + 1);
        return true;
    }
};

// ---- the link --------------------------------------------------------------------------------------------------------
class Link
{
public:
    // the claimed name's token, from revived_passphrase (as the switch's hello and /vault take it); "" without one
    static std::string token() { return claimToken(); }

    void registerMailbox(int kind, void* ptr, uint32_t bytes)
    {
        net_mailbox* mb = static_cast<net_mailbox*>(ptr);
        if (bytes < NET_MAILBOX_BYTES || mb->magic != NET_MAGIC || mb->version != NET_VERSION)
        {
            pf::logError("revived: a mailbox (kind %d) does not match this link", kind);
            return;
        }
        Endpoint& ep = kind == HOST_KIND_LAN ? m_lan : m_roam;
        if (ep.mb)
            return;
        // the page is there: idle until a mailbox wants the switch
        st(&mb->page_state, m_online ? NET_PAGE_ONLINE : NET_PAGE_IDLE);
        ep.mb = mb;
        if (m_online)
            publishOnline(ep);
        pf::logInfo("revived: %s mailbox attached", ep.name);
        vlan_atomic_notify(reinterpret_cast<int*>(&mb->tx_bell), 1);
    }

    void run()
    {
        m_server = pf::Config::getString("revived_server", "nfsu2.xyz");
        m_name = playerName();
        m_claim = claimToken();
        pf::logInfo("revived: the server is %s, the player %s%s", m_server.c_str(), m_name.c_str(),
                    m_claim.empty() ? "" : ", with the passphrase of its claimed name");
        for (;;)
        {
            const bool want = m_lan.wants() || m_roam.wants();
            if (!m_stream.isOpen())
            {
                dropTx(m_lan);
                dropTx(m_roam);
                const uint64_t now = nowMs();
                if (!want || m_stayOff || now < m_nextTry)
                {
                    if (!want && !m_stayOff)
                        setPage(NET_PAGE_IDLE);
                    idleWait(want && !m_stayOff ? uint32_t(m_nextTry - now) : 500);
                    continue;
                }
                connect();
                continue;
            }
            if (want)
                m_unwantedSince = 0;
            else if (!m_unwantedSince)
                m_unwantedSince = nowMs();
            else if (nowMs() - m_unwantedSince >= IdleMs)
            {
                release("idle");
                continue;
            }
            sendLanState();
            drainTx(m_lan);
            drainTx(m_roam);
            flushWaiting();
            if (m_opened)
            {
                std::vector<std::string> out;
                pthread_mutex_lock(&s_controlLock);
                out.swap(s_controlOut);
                pthread_mutex_unlock(&s_controlLock);
                for (const std::string& m : out)
                    sendFrame(0x1, m.data(), m.size());
            }
            if (m_publishAt && nowMs() >= m_publishAt)
                publish();
            if (m_opened && nowMs() >= m_nextPing)
                ping();
            if (!m_stream.isOpen())
                continue;
            if (m_stream.waitReadable(m_lan.waiting.empty() ? PollMs : 1))
                receive();
        }
    }

private:
    static std::string playerName()
    {
        std::string name = pf::Config::getString("revived_name", "");
        if (name.empty())
        {
            char user[SCE_SYSTEM_PARAM_USERNAME_MAXSIZE];
            memset(user, 0, sizeof(user));
            if (sceAppUtilSystemParamGetString(SCE_SYSTEM_PARAM_ID_USERNAME, reinterpret_cast<SceChar8*>(user),
                                               sizeof(user)) >= 0)
                name = user;
        }
        if (name.empty())
            name = "Vita";
        if (name.size() > 24)
            name.resize(24);
        return name;
    }

    // The token of the player's nfsu2.xyz passphrase (revived_passphrase, the one of its cloud saves), as the browser
    // makes it (web/src/app/vault.ts readPhrase and keysOf): the words in lower case after "nfsu2-xyz", PBKDF2 with
    // SHA-256 (100,000 rounds, its salt), the second 32 bytes of 64 in base64url. The hello names it ("claim"), so
    // the server lets this Vita play under the names that passphrase claimed. The passphrase never leaves the Vita.
    static std::string claimToken()
    {
        const std::string text = pf::Config::getString("revived_passphrase", "");
        std::vector<std::string> words;
        std::string w;
        for (size_t i = 0; i <= text.size(); ++i)
        {
            const char c = i < text.size() ? char(tolower(static_cast<unsigned char>(text[i]))) : 0;
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                w += c;
            else if (!w.empty())
            {
                words.push_back(w);
                w.clear();
            }
        }
        size_t first = words.size() >= 2 && words[0] == "nfsu2" && words[1] == "xyz" ? 2 : 0;
        if (words.size() - first != 8)
        {
            if (!text.empty())
                pf::logError("revived: revived_passphrase is not a passphrase (8 words after nfsu2-xyz): not used");
            return {};
        }
        std::string phrase = "nfsu2-xyz";
        for (size_t i = first; i < words.size(); ++i)
            phrase += "-" + words[i];
        static const char Salt[] = "nfsu2-xyz keyphrase saves v1";
        uint8_t bits[64];
        if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const uint8_t*>(phrase.data()),
                                          phrase.size(), reinterpret_cast<const uint8_t*>(Salt), sizeof(Salt) - 1,
                                          100000, sizeof(bits), bits) != 0)
        {
            pf::logError("revived: the passphrase's token could not be made");
            return {};
        }
        uint8_t b64[64];
        size_t n = 0;
        mbedtls_base64_encode(b64, sizeof(b64), &n, bits + 32, 32);
        std::string token;
        for (size_t i = 0; i < n; ++i)
            if (b64[i] == '+')
                token += '-';
            else if (b64[i] == '/')
                token += '_';
            else if (b64[i] != '=')
                token += char(b64[i]);
        memset(bits, 0, sizeof(bits));
        return token;
    }

    void setPage(uint32_t state)
    {
        for (Endpoint* ep : { &m_lan, &m_roam })
            if (ep->mb && ld(&ep->mb->page_state) != state)
            {
                st(&ep->mb->page_state, state);
                ep->ring();
            }
    }

    void publishPeers(Endpoint& ep)
    {
        if (!ep.mb)
            return;
        uint32_t bits[NET_PEER_BITS / 32];
        memset(bits, 0, sizeof(bits));
        int players = 0;
        for (const auto& p : m_peers)
        {
            if (!p.second)
                ++players;
            if (m_vip && ((p.first ^ m_vip) & m_netmask))
                continue;
            const uint32_t h = p.first & ~m_netmask;
            if (h < NET_PEER_BITS)
                bits[h >> 5] |= 1u << (h & 31);
        }
        for (unsigned i = 0; i < NET_PEER_BITS / 32; ++i)
            st(&ep.mb->peer_bits[i], bits[i]);
        st(&ep.mb->switch_peers, uint32_t(players));
    }

    void publishOnline(Endpoint& ep)
    {
        if (!ep.mb)
            return;
        st(&ep.mb->vip, m_vip);
        st(&ep.mb->netmask, m_netmask);
        st(&ep.mb->generation, m_generation);
        st(&ep.mb->page_state, NET_PAGE_ONLINE);
        ep.ring();
    }

    // a mailbox that wants the switch ends the wait early (set_want rings tx_bell)
    // welcomed: the address and the players into every mailbox, NET_PAGE_ONLINE
    void publish()
    {
        m_publishAt = 0;
        m_online = true;
        for (Endpoint* ep : { &m_lan, &m_roam })
        {
            publishPeers(*ep);
            publishOnline(*ep);
        }
    }

    void idleWait(uint32_t ms)
    {
        if (!ms)
            return;
        waitMutex().lock();
        waitCond().waitFor(ms > 500 ? 500 : ms);
        waitMutex().unlock();
    }

    bool lanOpen() const { return m_lan.wants(); }
    uint32_t lanPorts() const
    {
        if (!m_lan.mb)
            return 0;
        const uint32_t p = ld(&m_lan.mb->udp_ports);
        return p & NET_UDP_PORTS_SET ? p : 0;
    }

    void connect()
    {
        setPage(NET_PAGE_CONNECTING);
        if (!win32::ws2_32::netStart())
        {
            failed("the Vita's network is off");
            return;
        }
        std::string why;
        if (!m_stream.open(m_server.c_str(), 443, ConnectTimeoutMs, why))
        {
            failed(why);
            return;
        }
        // the WebSocket handshake
        uint8_t nonce[16];
        sceKernelGetRandomNumber(nonce, sizeof(nonce));
        unsigned char key[32];
        size_t keyLength = 0;
        mbedtls_base64_encode(key, sizeof(key), &keyLength, nonce, sizeof(nonce));
        const bool resume = (m_vip && !m_token.empty()) || !m_ticket.empty();
        std::string request = "GET /lan?queue" + std::string(resume ? "&resume" : "") + " HTTP/1.1\r\n"
                              "Host: " + m_server + "\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              "Sec-WebSocket-Key: " + std::string(reinterpret_cast<char*>(key), keyLength) + "\r\n"
                              "Sec-WebSocket-Version: 13\r\n"
                              "User-Agent: NFSU2VITA/" NFSU_VERSION "\r\n"
                              "\r\n";
        if (!m_stream.write(request.data(), request.size()))
        {
            failed("the server closed the connection");
            return;
        }
        std::string answer;
        const uint64_t deadline = nowMs() + ConnectTimeoutMs;
        size_t end = std::string::npos;
        while ((end = answer.find("\r\n\r\n")) == std::string::npos)
        {
            if (answer.size() > 8192 || nowMs() >= deadline)
            {
                failed("no WebSocket answer from the server");
                return;
            }
            char buffer[1024];
            const int n = m_stream.read(buffer, sizeof(buffer));
            if (n < 0)
            {
                failed("the server closed the connection during the WebSocket handshake");
                return;
            }
            if (n == 0)
                m_stream.waitReadable(100);
            else
                answer.append(buffer, size_t(n));
        }
        if (answer.compare(0, 12, "HTTP/1.1 101") != 0)
        {
            const size_t line = answer.find("\r\n");
            failed("the server refused the WebSocket: " + answer.substr(0, line));
            return;
        }
        // Sec-WebSocket-Accept: base64(SHA-1(key + the protocol's GUID))
        {
            const std::string text = std::string(reinterpret_cast<char*>(key), keyLength) +
                                     "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            unsigned char digest[20];
            mbedtls_sha1(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest);
            unsigned char expected[32];
            size_t expectedLength = 0;
            mbedtls_base64_encode(expected, sizeof(expected), &expectedLength, digest, sizeof(digest));
            std::string lower = answer.substr(0, end);
            for (char& c : lower)
                if (c >= 'A' && c <= 'Z')
                    c = char(c - 'A' + 'a');
            const size_t at = lower.find("sec-websocket-accept:");
            std::string got;
            if (at != std::string::npos)
            {
                size_t p = at + 21;
                while (p < end && answer[p] == ' ')
                    ++p;
                const size_t q = answer.find("\r\n", p);
                got = answer.substr(p, q - p);
                while (!got.empty() && got.back() == ' ')
                    got.pop_back();
            }
            if (got != std::string(reinterpret_cast<char*>(expected), expectedLength))
            {
                failed("the server's WebSocket answer does not match");
                return;
            }
        }
        m_rx.assign(answer.begin() + long(end + 4), answer.end());
        m_message.clear();
        m_opened = true;
        // the hello
        m_lanSent = lanOpen();
        m_udpSent = m_lanSent ? lanPorts() : 0;
        std::string hello = "{\"t\":\"hello\",\"name\":" + jsonText(m_name) + ",\"lan\":" + (m_lanSent ? "1" : "0");
        if (m_udpSent)
            hello += ",\"udp\":" + std::to_string(m_udpSent);
        if (m_vip && !m_token.empty())
            hello += ",\"vip\":" + jsonText(ipText(m_vip)) + ",\"token\":" + jsonText(m_token);
        if (!m_ticket.empty())
            hello += ",\"ticket\":" + jsonText(m_ticket);
        if (!m_claim.empty())
            hello += ",\"claim\":" + jsonText(m_claim);
        hello += "}";
        sendFrame(0x1, hello.data(), hello.size());
        m_nextPing = nowMs() + PingMs;
        m_unwantedSince = 0;
        pf::logInfo("revived: connected to %s, waiting for an address on the switch", m_server.c_str());
    }

    void failed(const std::string& why)
    {
        m_stream.close();
        ++m_failures;
        if (m_failures == 1 || m_failures % 10 == 0)
            pf::logInfo("revived: no link to %s (%s); trying again in %u ms (%u attempts)", m_server.c_str(),
                        why.c_str(), unsigned(m_backoff), m_failures);
        setPage(NET_PAGE_OFFLINE);
        m_nextTry = nowMs() + m_backoff;
        m_backoff = m_backoff * 2 > 5000 ? 5000 : m_backoff * 2;
    }

    // the link lost (or closed by the server with code)
    void lost(uint16_t code, const std::string& reason)
    {
        const bool wasOnline = m_online;
        m_stream.close();
        m_online = false;
        m_opened = false;
        m_publishAt = 0;
        m_lanSent = false;
        m_udpSent = 0;
        if (code == CloseKicked || code == CloseBanned || code == CloseRename)
        {
            m_stayOff = true;
            setPage(NET_PAGE_IDLE);
            pf::logInfo("revived: the server's operator %s (%s%s%s): no online play until the game starts again",
                        code == CloseKicked ? "removed this player" : code == CloseBanned ? "banned this player"
                                                                    : "took this player's name away",
                        m_removal.c_str(), m_removal.empty() ? "" : ", ", reason.c_str());
            return;
        }
        if (code == CloseFull)
            pf::logInfo("revived: the server is full; trying again until a place frees up");
        else if (wasOnline)
            pf::logInfo("revived: lost the switch (%u%s%s); reconnecting", unsigned(code), reason.empty() ? "" : " ",
                        reason.c_str());
        setPage(NET_PAGE_OFFLINE);
        m_nextTry = nowMs() + m_backoff;
        m_backoff = m_backoff * 2 > 5000 ? 5000 : m_backoff * 2;
    }

    // nobody wants the switch: the socket goes with 1000, the server frees the address at once
    void release(const char* why)
    {
        drainTx(m_lan);
        drainTx(m_roam);
        uint8_t payload[2 + 8];
        putBe16(payload, 1000);
        const size_t n = strlen(why) < 8 ? strlen(why) : 8;
        memcpy(payload + 2, why, n);
        sendFrame(0x8, payload, 2 + n);
        m_stream.close();
        m_online = false;
        m_opened = false;
        m_publishAt = 0;
        m_vip = 0;
        m_token.clear();
        m_ticket.clear();
        m_peers.clear();
        m_unwantedSince = 0;
        for (Endpoint* ep : { &m_lan, &m_roam })
            ep->waiting.clear();
        setPage(NET_PAGE_IDLE);
        pf::logInfo("revived: no network feature in use: the place on the server goes back until one is");
    }

    void sendLanState()
    {
        if (!m_opened)
            return;
        const bool lan = lanOpen();
        const uint32_t udp = lan ? lanPorts() : 0;
        if (lan == m_lanSent && udp == m_udpSent)
            return;
        std::string m = std::string("{\"t\":\"lan\",\"on\":") + (lan ? "1" : "0");
        if (udp)
            m += ",\"udp\":" + std::to_string(udp);
        m += "}";
        sendFrame(0x1, m.data(), m.size());
        m_lanSent = lan;
        m_udpSent = udp;
    }

    void ping()
    {
        m_nextPing = nowMs() + PingMs;
        const uint32_t id = ++m_pingId;
        m_pingAt[id % 16] = { id, nowMs() };
        const std::string m = "{\"t\":\"ping\",\"id\":" + std::to_string(id) + "}";
        sendFrame(0x1, m.data(), m.size());
    }

    // one WebSocket frame, masked as a client's must be
    void sendFrame(uint8_t opcode, const void* data, size_t size)
    {
        if (!m_stream.isOpen())
            return;
        m_out.clear();
        m_out.push_back(uint8_t(0x80 | opcode));
        if (size < 126)
            m_out.push_back(uint8_t(0x80 | size));
        else if (size < 65536)
        {
            m_out.push_back(0x80 | 126);
            m_out.push_back(uint8_t(size >> 8));
            m_out.push_back(uint8_t(size));
        }
        else
        {
            m_out.push_back(0x80 | 127);
            for (int i = 7; i >= 0; --i)
                m_out.push_back(uint8_t(uint64_t(size) >> (8 * i)));
        }
        uint8_t mask[4];
        sceKernelGetRandomNumber(mask, 4);
        m_out.insert(m_out.end(), mask, mask + 4);
        const size_t at = m_out.size();
        m_out.resize(at + size);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i)
            m_out[at + i] = p[i] ^ mask[i & 3];
        if (!m_stream.write(m_out.data(), m_out.size()))
            lost(1006, "the link broke while sending");
    }

    // C's TX ring without a switch: the datagrams are lost, as UDP's would be
    void dropTx(Endpoint& ep)
    {
        if (!ep.mb)
            return;
        const uint32_t head = ld(&ep.mb->tx_head), tail = ld(&ep.mb->tx_tail);
        if (head == tail)
            return;
        __atomic_add_fetch(&ep.mb->tx_offline_drop, head - tail, __ATOMIC_SEQ_CST);
        st(&ep.mb->tx_tail, head);
    }

    void drainTx(Endpoint& ep)
    {
        if (!ep.mb)
            return;
        if (!m_online || !m_stream.isOpen())
        {
            dropTx(ep);
            return;
        }
        uint32_t tail = ld(&ep.mb->tx_tail);
        const uint32_t head = ld(&ep.mb->tx_head);
        uint8_t frame[FrameHeader + NET_MAX_PAYLOAD];
        while (tail != head && m_stream.isOpen())
        {
            const net_slot* s = ep.slot(ep.mb->tx_off, tail, NET_TX_SLOTS);
            const uint32_t kind = s->kind, len = s->len;
            if (kind < NET_KIND_NOTICE && len <= NET_MAX_PAYLOAD)
            {
                frame[0] = uint8_t(kind);
                frame[1] = frame[2] = frame[3] = 0;
                putBe32(frame + 4, m_vip);   // the switch writes the real one
                putBe32(frame + 8, s->dst_ip);
                putBe16(frame + 12, s->src_port);
                putBe16(frame + 14, s->dst_port);
                memcpy(frame + FrameHeader, s->data, len);
                sendFrame(0x2, frame, FrameHeader + len);
            }
            else if (kind == NET_KIND_NOTICE && len <= NET_MAX_PAYLOAD)
            {
                // a line the page would show over the game (free roam: a friend joined, left...): the log here,
                // the friend's name first, ended by 0x1F
                std::string text(reinterpret_cast<const char*>(s->data), len);
                const size_t k = text.find('\x1f');
                if (k != std::string::npos)
                    text.erase(k, 1);
                if (!text.empty() && text[0] != '\x1e')
                    pf::logInfo("revived: %s", text.c_str());
            }
            ++tail;
            st(&ep.mb->tx_tail, tail);
        }
    }

    // ---- C <- switch ----
    void receive()
    {
        uint8_t buffer[4096];
        for (int rounds = 0; rounds < 64; ++rounds)
        {
            const int n = m_stream.read(buffer, sizeof(buffer));
            if (n < 0)
            {
                lost(1006, "the server closed the connection");
                return;
            }
            if (n == 0)
                break;
            m_rx.insert(m_rx.end(), buffer, buffer + n);
        }
        size_t at = 0;
        while (m_stream.isOpen())
        {
            // a frame: header, then the payload (the server's frames are not masked)
            if (m_rx.size() - at < 2)
                break;
            const uint8_t b0 = m_rx[at], b1 = m_rx[at + 1];
            size_t header = 2;
            uint64_t length = b1 & 0x7F;
            if (length == 126)
            {
                if (m_rx.size() - at < 4)
                    break;
                length = be16(&m_rx[at + 2]);
                header = 4;
            }
            else if (length == 127)
            {
                if (m_rx.size() - at < 10)
                    break;
                length = 0;
                for (int i = 0; i < 8; ++i)
                    length = length << 8 | m_rx[at + 2 + i];
                header = 10;
            }
            const bool masked = b1 & 0x80;
            if (masked)
                header += 4;
            if (length > MaxMessage)
            {
                lost(1009, "a message too large from the server");
                return;
            }
            if (m_rx.size() - at < header + length)
                break;
            uint8_t* payload = &m_rx[at + header];
            if (masked)
                for (uint64_t i = 0; i < length; ++i)
                    payload[i] ^= m_rx[at + header - 4 + (i & 3)];
            const uint8_t opcode = b0 & 0x0F;
            const bool fin = b0 & 0x80;
            at += header + size_t(length);
            if (opcode == 0x8)
            {
                const uint16_t code = length >= 2 ? be16(payload) : 1005;
                const std::string reason = length > 2 ? std::string(reinterpret_cast<char*>(payload + 2),
                                                                    size_t(length - 2)) : std::string();
                // answer the close, then the link goes
                uint8_t echo[2];
                putBe16(echo, code);
                sendFrame(0x8, echo, 2);
                lost(code, reason);
                return;
            }
            if (opcode == 0x9)
            {
                sendFrame(0xA, payload, size_t(length));
                continue;
            }
            if (opcode == 0xA)
                continue;
            if (opcode == 0x1 || opcode == 0x2)
            {
                m_messageOpcode = opcode;
                m_message.assign(payload, payload + length);
            }
            else if (opcode == 0x0)
            {
                if (m_message.size() + length > MaxMessage)
                {
                    lost(1009, "a message too large from the server");
                    return;
                }
                m_message.insert(m_message.end(), payload, payload + length);
            }
            else
                continue;
            if (!fin)
                continue;
            if (m_messageOpcode == 0x1)
                control(reinterpret_cast<const char*>(m_message.data()), m_message.size());
            else
                binary(m_message.data(), m_message.size());
            m_message.clear();
        }
        if (at)
            m_rx.erase(m_rx.begin(), m_rx.begin() + long(at));
    }

    void control(const char* text, size_t size)
    {
        Json m;
        JsonParser parser(text, text + size);
        if (!parser.parse(m) || m.type != Json::Object)
            return;
        const std::string t = m.str("t");
        double v = 0;
        if (t == "welcome" && m.num("vip", v))
        {
            const uint32_t vip = uint32_t(int64_t(v));
            const bool again = vip == m_vip;
            m_vip = vip;
            m_netmask = ipNumber(m.str("mask").empty() ? "255.255.255.0" : m.str("mask"), 0xFFFFFF00u);
            ++m_generation;
            m_token = m.str("token");
            m_ticket.clear();
            m_backoff = 250;
            m_failures = 0;
            ++m_connections;
            // the address goes to C with the players on the switch, which follow the welcome: a connect made at once
            // would find nobody there (PeersWaitMs at most)
            m_publishAt = nowMs() + PeersWaitMs;
            pf::logInfo("revived: on the virtual LAN as %s/%d%s", ipText(m_vip).c_str(),
                        __builtin_popcount(m_netmask),
                        m_connections > 1 ? (again ? " (the same address again)" : " (a new address)") : "");
        }
        else if (t == "queue")
        {
            m_ticket = m.str("ticket");
            double pos = 0, of = 0;
            m.num("pos", pos);
            m.num("of", of);
            setPage(NET_PAGE_OFFLINE);
            m_backoff = 250;
            pf::logInfo("revived: the server is full: in line for a place, %d of %d", int(pos), int(of));
        }
        else if (t == "peers" || t == "delta")
        {
            const Json* peers = m.get("peers");
            if (!peers || peers->type != Json::Array)
                return;
            std::map<uint32_t, bool> before;
            if (t == "peers")
                before.swap(m_peers);
            else if (const Json* gone = m.get("gone"))
                for (const Json& g : gone->items)
                    if (g.type == Json::Number)
                    {
                        const uint32_t vip = uint32_t(int64_t(g.number));
                        if (m_peers.erase(vip))
                            peerLeft(vip);
                    }
            for (const Json& p : peers->items)
            {
                const Json* vip = p.get("vip");
                if (vip && vip->type == Json::Number)
                    m_peers[uint32_t(int64_t(vip->number))] = p.truthy("server");
            }
            for (const auto& b : before)
                if (!m_peers.count(b.first))
                    peerLeft(b.first);
            publishPeers(m_lan);
            publishPeers(m_roam);
            if (m_publishAt)
                publish();
        }
        else if (t == "pong" && m.num("id", v))
        {
            const uint32_t id = uint32_t(v);
            const PingAt& p = m_pingAt[id % 16];
            if (p.id == id)
            {
                const double rtt = double(nowMs() - p.at);
                for (Endpoint* ep : { &m_lan, &m_roam })
                    if (ep->mb)
                        ep->mb->switch_rtt_ms = rtt;
            }
        }
        else if (t == "friend")
        {
            // free roam's friends (nfsu2-web server-rs src/app/friends.rs)
            const std::string op = m.str("op"), name = m.str("name"), st = m.str("st"), done = m.str("done");
            pf::logInfo("revived: friends: %s %s%s%s%s%s", op.c_str(), name.c_str(), st.empty() ? "" : ", ", st.c_str(),
                        done.empty() ? "" : ", ", done.c_str());
            if (roam_host_friend)
                roam_host_friend(op.c_str(), name.c_str(), st.c_str(), done.c_str());
        }
        else if (t == "kick" || t == "ban" || t == "rename")
        {
            m_removal = m.str("msg");   // its close follows
        }
        else if (t == "races")
        {
            if (m.truthy("lifted"))
                pf::logInfo("revived: the race ban was lifted");
            else
                pf::logInfo("revived: the server's operator keeps this player out of races (%s)", m.str("msg").c_str());
        }
    }

    // a player that left the switch: its TCP connections to us are reset
    void peerLeft(uint32_t vip)
    {
        if (vip == m_vip || !m_lan.mb || ld(&m_lan.mb->mode) != NET_MODE_LAN)
            return;
        uint8_t f[FrameHeader + NET_LAN_HDR];
        memset(f, 0, sizeof(f));
        f[0] = NET_KIND_LAN_TCP;
        putBe32(f + 4, vip);
        putBe32(f + 8, m_vip);
        f[FrameHeader] = NET_LAN_TCP_GONE;
        lanIn(f, sizeof(f), true);
    }

    void binary(const uint8_t* f, size_t size)
    {
        if (size < FrameHeader)
            return;
        const uint8_t kind = f[0];
        if (size - FrameHeader > NET_MAX_PAYLOAD)
        {
            for (Endpoint* ep : { &m_lan, &m_roam })
                if (ep->mb)
                    __atomic_add_fetch(&ep->mb->rx_oversize, 1, __ATOMIC_SEQ_CST);
            return;
        }
        if (kind == NET_KIND_LAN_UDP || kind == NET_KIND_LAN_TCP || kind == NET_KIND_LAN_ICMP)
        {
            if (!m_lan.mb || ld(&m_lan.mb->mode) != NET_MODE_LAN)
            {
                // no LAN play in this game yet: a connect is refused, as by a host with no listener
                if (kind == NET_KIND_LAN_TCP && size >= FrameHeader + NET_LAN_HDR && f[FrameHeader] == NET_LAN_TCP_SYN)
                    refuse(f);
                return;
            }
            lanIn(f, size, kind == NET_KIND_LAN_TCP);
            return;
        }
        // free roam: a player's endpoint takes the virtual LAN's UDP
        Endpoint& ep = m_roam;
        if (!ep.mb || kind != NET_KIND_UDP || ld(&ep.mb->mode) != NET_MODE_VLAN)
            return;
        if (!ep.put(f, size))
        {
            __atomic_add_fetch(&ep.mb->rx_full_drop, 1, __ATOMIC_SEQ_CST);
            return;
        }
        ep.ring();
    }

    // into the LAN RX ring; TCP frames wait (in order) while it is full, the others are dropped as UDP would be
    void lanIn(const uint8_t* f, size_t size, bool keep)
    {
        Endpoint& ep = m_lan;
        if (keep && !ep.waiting.empty())
        {
            ep.waiting.emplace_back(f, f + size);
            return;
        }
        if (ep.put(f, size))
        {
            ep.ring();
            return;
        }
        if (keep)
            ep.waiting.emplace_back(f, f + size);
        else
            __atomic_add_fetch(&ep.mb->rx_full_drop, 1, __ATOMIC_SEQ_CST);
    }

    void flushWaiting()
    {
        Endpoint& ep = m_lan;
        int n = 0;
        while (!ep.waiting.empty() && ep.put(ep.waiting.front().data(), ep.waiting.front().size()))
        {
            ep.waiting.pop_front();
            ++n;
        }
        if (n)
            ep.ring();
    }

    void refuse(const uint8_t* f)
    {
        uint8_t r[FrameHeader + NET_LAN_HDR];
        memset(r, 0, sizeof(r));
        r[0] = NET_KIND_LAN_TCP;
        putBe32(r + 4, m_vip);
        memcpy(r + 8, f + 4, 4);          // back to its source
        memcpy(r + 12, f + 14, 2);        // our port
        memcpy(r + 14, f + 12, 2);        // its port
        r[FrameHeader] = NET_LAN_TCP_RST;
        r[FrameHeader + 1] = 64;
        memcpy(r + FrameHeader + 4, f + FrameHeader + 4, 4);   // the connection id
        sendFrame(0x2, r, sizeof(r));
    }

    struct PingAt
    {
        uint32_t id;
        uint64_t at;
    };

    Endpoint m_lan { "lan", nullptr, {} };
    Endpoint m_roam { "roam", nullptr, {} };
    pf::tls::Stream m_stream;
    std::string m_server, m_name, m_claim;
    std::vector<uint8_t> m_rx, m_message, m_out;
    uint8_t m_messageOpcode = 0;
    bool m_opened = false;          // the WebSocket is up (the hello sent)
    bool m_online = false;          // welcomed: an address on the switch
    bool m_stayOff = false;         // removed by the server's operator
    bool m_lanSent = false;
    uint32_t m_udpSent = 0;
    uint32_t m_vip = 0, m_netmask = 0xFFFFFF00u, m_generation = 0;
    std::string m_token, m_ticket, m_removal;
    std::map<uint32_t, bool> m_peers;   // vip -> a server's own host
    uint64_t m_nextTry = 0, m_backoff = 250, m_unwantedSince = 0, m_nextPing = 0, m_publishAt = 0;
    unsigned m_failures = 0, m_connections = 0;
    uint32_t m_pingId = 0;
    PingAt m_pingAt[16] = {};
};

// Mailboxes can be registered before the link thread runs: the Link exists from the first call.
Link& link()
{
    static Link* s_link = new Link;
    return *s_link;
}

void* linkMain(void*)
{
    link().run();
    return nullptr;
}

}

extern "C"
{

int vlan_atomic_wait32(int* address, int expected, long long timeoutNs)
{
    waitMutex().lock();
    if (__atomic_load_n(address, __ATOMIC_SEQ_CST) != expected)
    {
        waitMutex().unlock();
        return 1;   // "not-equal"
    }
    long long ms = timeoutNs < 0 ? 1000 : (timeoutNs + 999999) / 1000000;
    if (ms < 1)
        ms = 1;
    const bool woken = waitCond().waitFor(uint32_t(ms));
    waitMutex().unlock();
    return woken ? 0 : 2;   // "ok" / "timed-out"
}

int vlan_atomic_notify(int*, unsigned)
{
    waitMutex().lock();
    waitCond().broadcast();
    waitMutex().unlock();
    return 0;
}

void nfsu2_register(int kind, void* ptr, uint32_t bytes)
{
    link().registerMailbox(kind, ptr, bytes);
}

}

// A control message ({"t":...}) to the server, from any thread: the link sends it while on the switch (else it waits).
extern "C" void revived_link_control_send(const char* json)
{
    pthread_mutex_lock(&s_controlLock);
    if (s_controlOut.size() < 64)
        s_controlOut.push_back(json);
    pthread_mutex_unlock(&s_controlLock);
}

// Vita Settings' "Add friend by name": nfsu2-web's POST /friends {"op":"ask","name":...} with the claimed name's token
// (server-rs src/app/friends.rs), on a thread of its own; its answer as a line for the settings row.
namespace
{
pthread_mutex_t s_askLock = PTHREAD_MUTEX_INITIALIZER;
std::string s_askStatus;
unsigned s_askVersion = 0;
bool s_asking = false;

void askStatus(const std::string& text)
{
    pthread_mutex_lock(&s_askLock);
    s_askStatus = text;
    ++s_askVersion;
    pthread_mutex_unlock(&s_askLock);
    pf::logInfo("revived: friends: %s", text.c_str());
}

void* askMain(void* arg)
{
    std::string* name = static_cast<std::string*>(arg);
    const std::string who = *name;
    delete name;
    const std::string token = Link::token();
    std::string result;
    if (token.empty())
        result = "Needs your passphrase (Multiplayer)";
    else if (!win32::ws2_32::netStart())
        result = "No network";
    else
    {
        const std::string host = pf::Config::getString("revived_server", "nfsu2.xyz");
        tls::Stream stream;
        std::string why;
        if (!stream.open(host.c_str(), 443, 15000, why))
            result = "No connection";
        else
        {
            std::string body = "{\"op\":\"ask\",\"name\":\"";
            for (char ch : who)
                if (ch != '"' && ch != '\\' && uint8_t(ch) >= 0x20)
                    body += ch;
            body += "\"}";
            const std::string head = "POST /friends HTTP/1.1\r\nHost: " + host + "\r\nAuthorization: Bearer " + token
                                   + "\r\nUser-Agent: NFSU2VITA\r\nContent-Type: application/json\r\nContent-Length: "
                                   + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            std::string raw;
            if (stream.write(head.data(), head.size()) && stream.write(body.data(), body.size()))
            {
                const uint64_t start = nowMs();
                char buffer[2048];
                while (nowMs() - start < 15000)
                {
                    if (!stream.waitReadable(500))
                        continue;
                    const int n = stream.read(buffer, sizeof(buffer));
                    if (n < 0)
                        break;
                    raw.append(buffer, size_t(n));
                    if (raw.size() > 65536)
                        break;
                }
            }
            stream.close();
            const size_t at = raw.find("\r\n\r\n");
            std::string done, error;
            if (at != std::string::npos)
            {
                // the body: chunked or not, the JSON object from its first '{'
                const size_t open = raw.find('{', at), close = raw.rfind('}');
                if (open != std::string::npos && close != std::string::npos && close > open)
                {
                    Json m;
                    JsonParser parser(raw.data() + open, raw.data() + close + 1);
                    if (parser.parse(m) && m.type == Json::Object)
                    {
                        done = m.str("done");
                        error = m.str("error");
                    }
                }
            }
            if (raw.empty())
                result = "No answer";
            else if (error == "keyphrase")
                result = "The server doesn't know your passphrase";
            else if (done == "asked" || done == "sent")
                result = "Request sent to " + who;
            else if (done == "friends")
                result = "You and " + who + " are friends";
            else if (done == "claim" || done == "none")
                result = who + " has no claimed name";
            else if (done == "rate")
                result = "Too many requests, try later";
            else if (done == "full")
                result = "Friends list full";
            else
                result = done.empty() ? "No answer" : who + ": " + done;
        }
    }
    askStatus(result);
    pthread_mutex_lock(&s_askLock);
    s_asking = false;
    pthread_mutex_unlock(&s_askLock);
    return nullptr;
}
}

void revived_friend_ask(const std::string& name)
{
    if (name.empty())
        return;
    pthread_mutex_lock(&s_askLock);
    const bool busy = s_asking;
    s_asking = true;
    pthread_mutex_unlock(&s_askLock);
    if (busy)
        return;
    askStatus("Asking " + name + "...");
    pthread_t thread;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 256 * 1024);   // the TLS handshake's stack
    std::string* arg = new std::string(name);
    if (pthread_create(&thread, &attributes, askMain, arg) == 0)
        pthread_detach(thread);
    else
    {
        delete arg;
        pthread_mutex_lock(&s_askLock);
        s_asking = false;
        pthread_mutex_unlock(&s_askLock);
        askStatus("Could not ask");
    }
    pthread_attr_destroy(&attributes);
}

std::string revived_friend_status(unsigned* version)
{
    pthread_mutex_lock(&s_askLock);
    const std::string s = s_askStatus;
    if (version)
        *version = s_askVersion;
    pthread_mutex_unlock(&s_askLock);
    return s;
}

// revived_bridge.cpp: SceNet up and the link thread started, once.
bool revived_link_start()
{
    waitMutex();
    waitCond();
    link();
    pthread_t thread;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 256 * 1024);   // the TLS handshake's stack
    const bool ok = pthread_create(&thread, &attributes, linkMain, nullptr) == 0;
    pthread_attr_destroy(&attributes);
    if (ok)
        pthread_detach(thread);
    else
        pf::logError("revived: the link thread did not start");
    return ok;
}
