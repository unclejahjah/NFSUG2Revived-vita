// TLS client streams on SceNet with Mbed TLS (include/platform/tls.h).
//
// The socket is non-blocking from the start: the connect, the handshake and every write wait on an epoll set with a
// deadline, so a server that does not answer costs at most the caller's timeout. Mbed TLS reads and writes through
// the two callbacks below, which turn SceNet's EAGAIN into its WANT_READ / WANT_WRITE.
#include <platform/tls.h>
#include <platform/log.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/net/net.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <cstdio>
#include <cstring>

// Mozilla's CA certificates (third_party/cacert/cacert.pem, embedded by CMake).
extern "C" const unsigned char tls_cacert_pem[];
extern "C" const size_t tls_cacert_pem_size;

// MBEDTLS_ENTROPY_HARDWARE_ALT: the kernel's random number generator.
extern "C" int mbedtls_hardware_poll(void*, unsigned char* output, size_t len, size_t* olen)
{
    size_t done = 0;
    while (done < len)
    {
        const size_t n = len - done < 64 ? len - done : 64;
        if (sceKernelGetRandomNumber(output + done, n) < 0)
            break;
        done += n;
    }
    *olen = done;
    return 0;
}

// MBEDTLS_PLATFORM_MS_TIME_ALT
extern "C" mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return mbedtls_ms_time_t(sceKernelGetProcessTimeWide() / 1000);
}

namespace pf { namespace tls
{

namespace
{

uint64_t nowMs()
{
    return sceKernelGetProcessTimeWide() / 1000;
}

// The shared parts, made once: the random generator and the CA list (parsing it takes a while).
struct Shared
{
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;
    bool ok = false;
};

Shared* makeShared()
{
        Shared* s = new Shared;
        mbedtls_entropy_init(&s->entropy);
        mbedtls_ctr_drbg_init(&s->drbg);
        mbedtls_x509_crt_init(&s->ca);
        const uint64_t t0 = nowMs();
        const psa_status_t psa = psa_crypto_init();   // TLS 1.3 needs PSA crypto
        static const char personal[] = "nfsu2vita-tls";
        int r = mbedtls_ctr_drbg_seed(&s->drbg, mbedtls_entropy_func, &s->entropy,
                                      reinterpret_cast<const unsigned char*>(personal), sizeof(personal) - 1);
        // the PEM parser needs the text to end with a 0 byte
        std::string pem(reinterpret_cast<const char*>(tls_cacert_pem), tls_cacert_pem_size);
        const int bad = mbedtls_x509_crt_parse(&s->ca, reinterpret_cast<const unsigned char*>(pem.c_str()),
                                               pem.size() + 1);
        int count = 0;
        for (const mbedtls_x509_crt* c = &s->ca; c && c->raw.len; c = c->next)
            ++count;
        s->ok = psa == PSA_SUCCESS && r == 0 && count > 0;
        pf::logInfo("tls: %d CA certificates (%d not taken), random generator %s, %u ms", count, bad < 0 ? -1 : bad,
                    r == 0 && psa == PSA_SUCCESS ? "ready" : "FAILED", unsigned(nowMs() - t0));
        return s;
}

Shared* shared()
{
    static Shared* s_shared = makeShared();   // one thread makes it, the others wait (a guarded static)
    return s_shared;
}

std::string tlsError(int code)
{
    char text[128];
    mbedtls_strerror(code, text, sizeof(text));
    char line[160];
    snprintf(line, sizeof(line), "%s (-0x%04X)", text, unsigned(-code));
    return line;
}

int bioSend(void* context, const unsigned char* data, size_t size)
{
    const int socket = *static_cast<int*>(context);
    const int r = sceNetSend(socket, data, size, 0);
    if (r >= 0)
        return r;
    if ((r & 0xff) == SCE_NET_EAGAIN)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

int bioRecv(void* context, unsigned char* data, size_t size)
{
    const int socket = *static_cast<int*>(context);
    const int r = sceNetRecv(socket, data, size, 0);
    if (r >= 0)
        return r;   // 0: the server closed the TCP connection
    if ((r & 0xff) == SCE_NET_EAGAIN)
        return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

}

struct Stream::Impl
{
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
};

Stream::Stream() : m_impl(nullptr), m_socket(-1), m_epoll(-1)
{
}

Stream::~Stream()
{
    close();
}

bool Stream::waitFor(unsigned events, uint32_t timeoutMs)
{
    if (m_epoll < 0)
    {
        m_epoll = sceNetEpollCreate("tls", 0);
        if (m_epoll < 0)
            return false;
        SceNetEpollEvent event;
        memset(&event, 0, sizeof(event));
        event.events = SCE_NET_EPOLLIN | SCE_NET_EPOLLOUT | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP;
        event.data.fd = m_socket;
        sceNetEpollControl(m_epoll, SCE_NET_EPOLL_CTL_ADD, m_socket, &event);
    }
    SceNetEpollEvent event;
    memset(&event, 0, sizeof(event));
    event.events = events | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP;
    event.data.fd = m_socket;
    sceNetEpollControl(m_epoll, SCE_NET_EPOLL_CTL_MOD, m_socket, &event);
    SceNetEpollEvent out;
    const int n = sceNetEpollWait(m_epoll, &out, 1, int(timeoutMs) * 1000);
    return n > 0;
}

bool Stream::open(const char* host, uint16_t port, uint32_t timeoutMs, std::string& why)
{
    close();
    const uint64_t deadline = nowMs() + timeoutMs;
    Shared* s = shared();
    if (!s->ok)
    {
        why = "TLS is not available (no random generator or CA list)";
        return false;
    }
    // DNS
    SceNetInAddr address;
    address.s_addr = 0;
    if (sceNetInetPton(SCE_NET_AF_INET, host, &address) != 1)
    {
        const int resolver = sceNetResolverCreate("tls", nullptr, 0);
        if (resolver < 0)
        {
            why = "no DNS resolver";
            return false;
        }
        const int r = sceNetResolverStartNtoa(resolver, host, &address, int(timeoutMs) * 1000 / 2, 2, 0);
        sceNetResolverDestroy(resolver);
        if (r < 0 || !address.s_addr)
        {
            char text[96];
            snprintf(text, sizeof(text), "the name %s did not resolve (0x%08X)", host, unsigned(r));
            why = text;
            return false;
        }
    }
    // TCP
    m_socket = sceNetSocket("tls", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (m_socket < 0)
    {
        why = "no socket";
        m_socket = -1;
        return false;
    }
    int on = 1;
    sceNetSetsockopt(m_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof(on));
    sceNetSetsockopt(m_socket, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &on, sizeof(on));
    SceNetSockaddrIn to;
    memset(&to, 0, sizeof(to));
    to.sin_len = sizeof(to);
    to.sin_family = SCE_NET_AF_INET;
    to.sin_port = sceNetHtons(port);
    to.sin_addr = address;
    int r = sceNetConnect(m_socket, reinterpret_cast<SceNetSockaddr*>(&to), sizeof(to));
    if (r < 0 && (r & 0xff) != SCE_NET_EINPROGRESS && (r & 0xff) != SCE_NET_EAGAIN)
    {
        char text[64];
        snprintf(text, sizeof(text), "connect failed (0x%08X)", unsigned(r));
        why = text;
        close();
        return false;
    }
    if (r < 0)
    {
        const uint64_t now = nowMs();
        if (now >= deadline || !waitFor(SCE_NET_EPOLLOUT, uint32_t(deadline - now)))
        {
            why = "the server did not answer";
            close();
            return false;
        }
        int error = 0;
        unsigned length = sizeof(error);
        sceNetGetsockopt(m_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR, &error, &length);
        if (error)
        {
            char text[64];
            snprintf(text, sizeof(text), "connect failed (error %d)", error);
            why = text;
            close();
            return false;
        }
    }
    // TLS
    m_impl = new Impl;
    mbedtls_ssl_init(&m_impl->ssl);
    mbedtls_ssl_config_init(&m_impl->config);
    r = mbedtls_ssl_config_defaults(&m_impl->config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r == 0)
    {
        mbedtls_ssl_conf_authmode(&m_impl->config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&m_impl->config, &s->ca, nullptr);
        mbedtls_ssl_conf_rng(&m_impl->config, mbedtls_ctr_drbg_random, &s->drbg);
        r = mbedtls_ssl_setup(&m_impl->ssl, &m_impl->config);
    }
    if (r == 0)
        r = mbedtls_ssl_set_hostname(&m_impl->ssl, host);
    if (r != 0)
    {
        why = "TLS setup failed: " + tlsError(r);
        close();
        return false;
    }
    mbedtls_ssl_set_bio(&m_impl->ssl, &m_socket, bioSend, bioRecv, nullptr);
    for (;;)
    {
        r = mbedtls_ssl_handshake(&m_impl->ssl);
        if (r == 0)
            break;
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            const uint32_t flags = mbedtls_ssl_get_verify_result(&m_impl->ssl);
            why = "TLS handshake failed: " + tlsError(r);
            if (flags && flags != uint32_t(-1))
            {
                char text[512];
                mbedtls_x509_crt_verify_info(text, sizeof(text), "", flags);
                why += std::string("; certificate: ") + text;
            }
            close();
            return false;
        }
        const uint64_t now = nowMs();
        if (now >= deadline || !waitFor(r == MBEDTLS_ERR_SSL_WANT_READ ? SCE_NET_EPOLLIN : SCE_NET_EPOLLOUT,
                                         uint32_t(deadline - now)))
        {
            why = "the TLS handshake timed out";
            close();
            return false;
        }
    }
    return true;
}

bool Stream::write(const void* data, size_t size, uint32_t timeoutMs)
{
    if (!m_impl)
        return false;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    const uint64_t deadline = nowMs() + timeoutMs;
    while (size)
    {
        const int r = mbedtls_ssl_write(&m_impl->ssl, p, size);
        if (r > 0)
        {
            p += r;
            size -= size_t(r);
            continue;
        }
        if (r != MBEDTLS_ERR_SSL_WANT_WRITE && r != MBEDTLS_ERR_SSL_WANT_READ)
            return false;
        const uint64_t now = nowMs();
        if (now >= deadline || !waitFor(r == MBEDTLS_ERR_SSL_WANT_READ ? SCE_NET_EPOLLIN : SCE_NET_EPOLLOUT,
                                         uint32_t(deadline - now)))
            return false;
    }
    return true;
}

int Stream::read(void* data, size_t size)
{
    if (!m_impl)
        return -1;
    for (int tries = 0; tries < 4; ++tries)
    {
        const int r = mbedtls_ssl_read(&m_impl->ssl, static_cast<unsigned char*>(data), size);
        if (r > 0)
            return r;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            return 0;
        // TLS 1.3: a session ticket after the handshake is not data; read again
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            continue;
        return -1;   // 0 (the TCP connection closed), a close_notify or an error
    }
    return 0;
}

bool Stream::waitReadable(uint32_t timeoutMs)
{
    if (!m_impl)
        return false;
    if (mbedtls_ssl_get_bytes_avail(&m_impl->ssl) || mbedtls_ssl_check_pending(&m_impl->ssl))
        return true;
    return waitFor(SCE_NET_EPOLLIN, timeoutMs);
}

void Stream::close()
{
    if (m_impl)
    {
        if (m_socket >= 0)
            mbedtls_ssl_close_notify(&m_impl->ssl);
        mbedtls_ssl_free(&m_impl->ssl);
        mbedtls_ssl_config_free(&m_impl->config);
        delete m_impl;
        m_impl = nullptr;
    }
    if (m_epoll >= 0)
    {
        sceNetEpollDestroy(m_epoll);
        m_epoll = -1;
    }
    if (m_socket >= 0)
    {
        sceNetSocketClose(m_socket);
        m_socket = -1;
    }
}

}}
