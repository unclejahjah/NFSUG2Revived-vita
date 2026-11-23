#ifndef PLATFORM_TLS_H_
#define PLATFORM_TLS_H_

#include <cstddef>
#include <cstdint>
#include <string>

// A TLS client connection over a SceNet TCP socket (Mbed TLS), for the Revived server's WebSocket
// (src/lib/winapi/revived/revived_link.cpp). The server's certificate is checked against Mozilla's CA list
// (third_party/cacert) and its host name. One thread uses a stream at a time. SceNet must be up (ws2_32 netUp).
namespace pf { namespace tls
{

class Stream
{
public:
    Stream();
    ~Stream();

    // DNS, TCP and the TLS handshake, at most timeoutMs in all. False with why set on failure.
    bool open(const char* host, uint16_t port, uint32_t timeoutMs, std::string& why);
    // Every byte, waiting at most timeoutMs for room; false: the link is gone (then close it).
    bool write(const void* data, size_t size, uint32_t timeoutMs = 5000);
    // What has arrived, without waiting: > 0 bytes read, 0 nothing yet, < 0 the link is gone.
    int read(void* data, size_t size);
    // Waits at most timeoutMs for something to read (true) or not (false).
    bool waitReadable(uint32_t timeoutMs);
    void close();
    bool isOpen() const { return m_socket >= 0; }

private:
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    bool waitFor(unsigned events, uint32_t timeoutMs);
    struct Impl;
    Impl* m_impl;
    int m_socket;
    int m_epoll;
};

}}

#endif
