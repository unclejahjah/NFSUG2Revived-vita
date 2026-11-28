#include <winapi/waitprobe.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <platform/switches.h>
#include <platform/log.h>
#include <platform/time.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

namespace waitprobe
{

namespace
{

struct Entry
{
    Kind kind;
    uint32_t thread, caller, handle;
    uint32_t count, timeouts;
    uint64_t totalUs;
    uint32_t maxUs;
    char host[24];
};

std::vector<std::pair<int, std::string>> s_names;

std::string hostName()
{
#ifdef __vita__
    const int id = sceKernelGetThreadId();
    for (const auto& n : s_names)
        if (n.first == id)
            return n.second;
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    std::string name = sceKernelGetThreadInfo(id, &info) >= 0 ? info.name : "?";
    s_names.push_back({id, name});
    return name;
#else
    return "host";
#endif
}

std::mutex s_mutex;
std::vector<Entry> s_entries;
uint64_t s_last = 0;

void report(uint64_t now)
{
    for (Kind kind : {Wait, Signal})
    {
        std::vector<Entry> rows;
        for (const Entry& e : s_entries)
            if (e.kind == kind)
                rows.push_back(e);
        std::sort(rows.begin(), rows.end(), [kind](const Entry& a, const Entry& b) {
            return kind == Wait ? a.totalUs > b.totalUs : a.count > b.count;
        });
        for (size_t i = 0; i < rows.size() && i < 8; ++i)
        {
            const Entry& e = rows[i];
            pf::logInfo("waitprobe %s host %s thread %u from 0x%08x handle 0x%x: %u calls, %u timeouts, %.2f ms total, max %.2f ms",
                        kind == Wait ? "wait  " : "signal", e.host, e.thread, e.caller, e.handle, e.count, e.timeouts,
                        double(e.totalUs) / 1000.0, double(e.maxUs) / 1000.0);
        }
    }
    s_entries.clear();
    s_last = now;
}

}

bool enabled()
{
    return NFSU_DEV_BOOL_CACHED("wait_probe", false);
}

void record(Kind kind, uint32_t thread, uint32_t caller, uint32_t handle, uint32_t us, bool timedOut)
{
    const uint64_t now = pf::ticksUs();
    std::lock_guard<std::mutex> guard(s_mutex);
    const std::string host = hostName();
    if (!s_last)
        s_last = now;
    Entry* found = nullptr;
    for (Entry& e : s_entries)
        if (e.kind == kind && e.thread == thread && e.caller == caller && e.handle == handle && host == e.host)
        {
            found = &e;
            break;
        }
    if (!found)
    {
        s_entries.push_back({kind, thread, caller, handle, 0, 0, 0, 0, {}});
        found = &s_entries.back();
        strncpy(found->host, host.c_str(), sizeof(found->host) - 1);
    }
    ++found->count;
    found->timeouts += timedOut;
    found->totalUs += us;
    found->maxUs = std::max(found->maxUs, us);
    if (now - s_last >= 1000000)
        report(now);
}

}
