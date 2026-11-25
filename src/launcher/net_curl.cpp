// libcurl transfers, from the parked updater: mbedTLS and a bundled CA file on the Vita. Options follow PS Vita
// Alive's HTTP client (MIT, THIRD_PARTY.md); peers are verified.

#include "net.h"
#include <curl/curl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#ifdef __vita__
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <cerrno>

// Strict IPv4 parse: newlib's fails on host names under Vita3K.
extern "C" int inet_pton(int af, const char* src, void* dst)
{
    if (af != AF_INET)
    {
        if (af == AF_INET6)
            return 0;
        errno = EAFNOSUPPORT;
        return -1;
    }
    unsigned char out[4];
    for (int i = 0; i < 4; ++i)
    {
        unsigned value = 0, digits = 0;
        for (; *src >= '0' && *src <= '9' && digits < 4; ++src, ++digits)
            value = value * 10 + unsigned(*src - '0');
        if (!digits || digits > 3 || value > 255 || *src != (i < 3 ? '.' : '\0'))
            return 0;
        out[i] = static_cast<unsigned char>(value);
        ++src;
    }
    memcpy(dst, out, 4);
    return 1;
}
#endif

namespace ug2 { namespace launcher { namespace net {

namespace
{

const char UserAgent[] = "NFSU2VITA/" NFSU_VERSION;
const int ReceiveBytes = 512 * 1024;   // doubled the Vita's throughput (88 to 169 KB/s)
std::string s_caFile;
bool s_curl = false;
#ifdef __vita__
const int NetMemoryBytes = 256 * 1024;
void* s_netMemory = nullptr;
bool s_netModule = false, s_netInit = false, s_netCtl = false;
#endif

struct Transfer
{
    const std::atomic<bool>* abort = nullptr;
    FILE* file = nullptr;
    uint64_t offset = 0;
    uint64_t limit = 0;
    std::atomic<uint64_t>* done = nullptr;
    bool overflow = false;
};

size_t toFile(char* data, size_t size, size_t count, void* user)
{
    Transfer& t = *static_cast<Transfer*>(user);
    const size_t n = size * count;
    if (t.offset + n > t.limit)
    {
        t.overflow = true;
        return 0;
    }
    if (fwrite(data, 1, n, t.file) != n)
        return 0;
    t.offset += n;
    if (t.done)
        t.done->store(t.offset);
    return n;
}

int progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const Transfer& t = *static_cast<const Transfer*>(user);
    return t.abort && t.abort->load() ? 1 : 0;
}

int socketOptions(void*, curl_socket_t socket, curlsocktype)
{
    const int bytes = ReceiveBytes;
    setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    return CURL_SOCKOPT_OK;
}

CURL* handle(const std::string& url, Transfer& t, char* errorText)
{
    CURL* curl = curl_easy_init();
    if (!curl)
        return nullptr;
    errorText[0] = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorText);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, UserAgent);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, url.compare(0, 8, "https://") == 0 ? "https" : "http,https");
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!s_caFile.empty())
        curl_easy_setopt(curl, CURLOPT_CAINFO, s_caFile.c_str());
    curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, socketOptions);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, long(ReceiveBytes));
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    return curl;
}

Result finish(CURL* curl, CURLcode code, const Transfer& t, const char* errorText, std::string* error)
{
    long status = 0, redirects = 0;
    char* last = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_REDIRECT_COUNT, &redirects);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &last);
    std::string where = last ? last : "";
    where = where.substr(0, where.find('/', where.find("://") == std::string::npos ? 0 : where.find("://") + 3));
    where = " [" + where + " after " + std::to_string(redirects) + " redirects]";
    curl_easy_cleanup(curl);
    if (code == CURLE_OK)
        return Result::Ok;
    if (code == CURLE_ABORTED_BY_CALLBACK && t.abort && t.abort->load())
    {
        *error = "cancelled";
        return Result::Cancelled;
    }
    if (t.overflow)
        *error = "the server sent more than expected" + where;
    else
        *error = std::string(errorText[0] ? errorText : curl_easy_strerror(code)) +
                 (status ? " (HTTP " + std::to_string(status) + ")" : std::string()) + where;
    return Result::Failed;
}

}

bool start(const char* caFile, std::string* error)
{
    s_caFile = caFile ? caFile : "";
#ifdef __vita__
    if (!s_netModule)
    {
        const int r = sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) == SCE_SYSMODULE_LOADED ? 0 : sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
        if (r < 0)
        {
            *error = "network module: " + std::to_string(r);
            return false;
        }
        s_netModule = true;
    }
    if (!s_netInit)
    {
        s_netMemory = malloc(NetMemoryBytes);
        SceNetInitParam param = { s_netMemory, NetMemoryBytes, 0 };
        if (!s_netMemory || sceNetInit(&param) < 0)
        {
            free(s_netMemory);
            s_netMemory = nullptr;
            *error = "sceNetInit failed";
            return false;
        }
        s_netInit = true;
    }
    if (!s_netCtl)
    {
        if (sceNetCtlInit() < 0)
        {
            *error = "sceNetCtlInit failed";
            return false;
        }
        s_netCtl = true;
    }
    int state = 0;
    if (sceNetCtlInetGetState(&state) < 0 || state != SCE_NETCTL_STATE_CONNECTED)
    {
        *error = "not connected to a network";
        return false;
    }
#endif
    if (!s_curl)
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        {
            *error = "curl_global_init failed";
            return false;
        }
        s_curl = true;
    }
    return true;
}

void stop()
{
    if (s_curl)
        curl_global_cleanup();
    s_curl = false;
#ifdef __vita__
    if (s_netCtl)
        sceNetCtlTerm();
    if (s_netInit)
        sceNetTerm();
    if (s_netModule)
        sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    free(s_netMemory);
    s_netMemory = nullptr;
    s_netCtl = s_netInit = s_netModule = false;
#endif
}

Result download(const std::string& url, const std::string& path, uint64_t* have, uint64_t limit,
                const std::atomic<bool>* abort, std::atomic<uint64_t>* done, std::string* error)
{
    for (int attempt = 0;; ++attempt)
    {
        Transfer t;
        t.abort = abort;
        t.offset = *have;
        t.limit = limit;
        t.done = done;
        t.file = fopen(path.c_str(), *have ? "ab" : "wb");
        if (!t.file)
        {
            *error = "cannot write " + path;
            return Result::Failed;
        }
        char errorText[CURL_ERROR_SIZE];
        CURL* curl = handle(url, t, errorText);
        if (!curl)
        {
            fclose(t.file);
            *error = "curl_easy_init failed";
            return Result::Failed;
        }
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, toFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
        if (*have)
            curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, curl_off_t(*have));
        const CURLcode code = curl_easy_perform(curl);
        const bool closed = fclose(t.file) == 0;
        // As PS Vita Alive: a server that ignores Range gets one full download.
        if (code == CURLE_RANGE_ERROR && *have && !attempt)
        {
            curl_easy_cleanup(curl);
            *have = 0;
            continue;
        }
        *have = t.offset;
        const Result result = finish(curl, code, t, errorText, error);
        if (result == Result::Ok && !closed)
        {
            *error = "writing " + path + " failed";
            return Result::Failed;
        }
        return result;
    }
}

}}}
