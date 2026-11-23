// Crash reports (crashlog.h).
#include <platform/crashlog.h>

#include <platform/config.h>
#include <platform/log.h>

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>

extern "C"
{
// kubridge.h (github.com/bythos14/kubridge); weak imports in kubridge_stub.cpp.
typedef struct KuKernelExceptionContext
{
    SceUInt32 r[13];
    SceUInt32 sp;
    SceUInt32 lr;
    SceUInt32 pc;
    SceUInt64 vfpRegisters[32];
    SceUInt32 SPSR;
    SceUInt32 FPSCR;
    SceUInt32 FPEXC;
    SceUInt32 FSR;
    SceUInt32 FAR;
    SceUInt32 exceptionType;
} KuKernelExceptionContext;
typedef void (*KuKernelExceptionHandler)(KuKernelExceptionContext*);
int kuKernelRegisterExceptionHandler(SceUInt32 exceptionType, KuKernelExceptionHandler pHandler,
                                     KuKernelExceptionHandler* pOldHandler, void* pOpt);
int kuKernelRegisterAbortHandler(KuKernelExceptionHandler pHandler, KuKernelExceptionHandler* pOldHandler,
                                 void* pOpt);
}

#ifndef NFSU_COMMIT
#define NFSU_COMMIT "unknown"
#endif

namespace pf { namespace crashlog
{

namespace
{

const uint32_t RingBytes = 64 * 1024;
const uint32_t ReportRingBytes = 24 * 1024;     // the newest part of the ring goes in a report
const uint32_t StackWords = 16 * 1024;          // at most 64 KB of stack scanned
const uint32_t MaxReturns = 64;
const char* const Types[] = {"data abort", "prefetch abort", "undefined instruction"};

// Found in a dump by its magic (crashdump.py): magic, size, write position, wrapped, then the text.
struct Ring
{
    char                    magic[16];
    uint32_t                size;
    uint32_t                used;
    uint32_t                wrapped;
    uint32_t                reserved;
    char                    text[RingBytes];
};
Ring s_ring = {{'N', 'F', 'S', 'U', '2', 'C', 'R', 'A', 'S', 'H', 'R', 'I', 'N', 'G', '0', '1'}, RingBytes, 0, 0, 0, {}};
std::atomic_flag s_ringLock = ATOMIC_FLAG_INIT;

char s_folder[256];             // <data>/crashes
char s_runPath[288];            // crashes/run.txt: this run's start, and its report when it has one
const char* s_version = "";
std::vector<std::string> s_notices;
bool s_installed = false;
KuKernelExceptionHandler s_old[3] = {};
std::atomic<int> s_reporting{0};
std::atomic<uint32_t> s_trapAddress{0};
std::atomic<const char*> s_trapReason{nullptr};
uint32_t s_codeLo = 0, s_codeSize = 0;
char s_codeName[32];

// The report: static, an exception handler has little stack.
char s_text[48 * 1024];
uint32_t s_used = 0;
SceKernelModuleInfo s_module;
SceKernelThreadInfo s_thread;

__attribute__((format(printf, 1, 2))) void add(const char* fmt, ...)
{
    if (s_used + 1 >= sizeof(s_text))
        return;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(s_text + s_used, sizeof(s_text) - s_used, fmt, args);
    va_end(args);
    if (n > 0)
        s_used = std::min(uint32_t(sizeof(s_text) - 1), s_used + uint32_t(n));
}

void addBytes(const char* text, uint32_t length)
{
    length = std::min(length, uint32_t(sizeof(s_text) - 1 - s_used));
    memcpy(s_text + s_used, text, length);
    s_used += length;
}

bool writeFile(const char* path, const char* text, uint32_t length, bool append)
{
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | (append ? SCE_O_APPEND : SCE_O_TRUNC), 0666);
    if (fd < 0)
        return false;
    sceIoWrite(fd, text, length);
    sceIoClose(fd);
    return true;
}

bool exists(const char* path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

void copyFile(const char* from, const char* to)
{
    const SceUID in = sceIoOpen(from, SCE_O_RDONLY, 0);
    if (in < 0)
        return;
    const SceUID out = sceIoOpen(to, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out >= 0)
    {
        static char buffer[16 * 1024];
        int n;
        while ((n = sceIoRead(in, buffer, sizeof(buffer))) > 0)
            sceIoWrite(out, buffer, n);
        sceIoClose(out);
    }
    sceIoClose(in);
}

std::string readText(const char* path)
{
    std::string text;
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return text;
    char buffer[1024];
    int n;
    while ((n = sceIoRead(fd, buffer, sizeof(buffer))) > 0 && text.size() < 16 * 1024)
        text.append(buffer, n);
    sceIoClose(fd);
    return text;
}

std::string field(const std::string& text, const char* key)
{
    const std::string k = std::string(key) + "=";
    size_t at = text.rfind("\n" + k);
    at = at == std::string::npos ? (text.compare(0, k.size(), k) == 0 ? 0 : std::string::npos) : at + 1;
    if (at == std::string::npos)
        return std::string();
    const size_t end = text.find('\n', at);
    return text.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size());
}

// "20261121-233122", local time.
void stamp(char* out, size_t size)
{
    SceDateTime t;
    memset(&t, 0, sizeof(t));
    sceRtcGetCurrentClockLocalTime(&t);
    snprintf(out, size, "%04u%02u%02u-%02u%02u%02u", unsigned(t.year), unsigned(t.month), unsigned(t.day),
             unsigned(t.hour), unsigned(t.minute), unsigned(t.second));
}

// The code segment of the game module (this one).
void findCode()
{
    memset(&s_module, 0, sizeof(s_module));
    s_module.size = sizeof(s_module);
    const SceUID id = sceKernelGetModuleIdByAddr(reinterpret_cast<void*>(&findCode));
    if (id < 0 || sceKernelGetModuleInfo(id, &s_module) < 0)
        return;
    snprintf(s_codeName, sizeof(s_codeName), "%s", s_module.module_name);
    const uint32_t self = uint32_t(reinterpret_cast<uintptr_t>(&findCode));
    for (const SceKernelSegmentInfo& segment : s_module.segments)
    {
        const uint32_t lo = uint32_t(reinterpret_cast<uintptr_t>(segment.vaddr));
        if (self - lo < segment.memsz)
        {
            s_codeLo = lo;
            s_codeSize = segment.memsz;
        }
    }
}

// "0x81234567 (NFSU2VITA.elf@1+0x1234)"; crashdump.py names it with the build's .elf.
void where(const char* label, uint32_t address)
{
    if (s_codeSize && address - s_codeLo < s_codeSize)
    {
        add("%s 0x%08x (%s@1+0x%x)\n", label, unsigned(address), s_codeName, unsigned(address - s_codeLo));
        return;
    }
    memset(&s_module, 0, sizeof(s_module));
    s_module.size = sizeof(s_module);
    const SceUID id = sceKernelGetModuleIdByAddr(reinterpret_cast<void*>(address));
    if (id >= 0 && sceKernelGetModuleInfo(id, &s_module) >= 0)
    {
        for (int k = 0; k < 4; ++k)
        {
            const uint32_t lo = uint32_t(reinterpret_cast<uintptr_t>(s_module.segments[k].vaddr));
            if (address - lo < s_module.segments[k].memsz)
            {
                add("%s 0x%08x (%s@%d+0x%x)\n", label, unsigned(address), s_module.module_name, k + 1,
                    unsigned(address - lo));
                return;
            }
        }
    }
    add("%s 0x%08x\n", label, unsigned(address));
}

void header(const char* what)
{
    char when[32];
    stamp(when, sizeof(when));
    add("NeedForSpeedUnderground2 crash report\n");
    add("version %s, commit %s, built %s %s\n", s_version, NFSU_COMMIT, __DATE__, __TIME__);
    add("time %s\n", when);
    memset(&s_thread, 0, sizeof(s_thread));
    s_thread.size = sizeof(s_thread);
    const SceUID self = sceKernelGetThreadId();
    sceKernelGetThreadInfo(self, &s_thread);
    add("thread 0x%08x \"%s\"\n", unsigned(self), s_thread.name);
    add("what %s\n", what);
    if (const char* reason = s_trapReason.load())
        add("guest trap at 0x%08x: %s\n", unsigned(s_trapAddress.load()), reason);
}

// Return addresses into the game code between sp and the top of the thread's stack (header() read its bounds).
void stackScan(uint32_t sp)
{
    const uint32_t top = uint32_t(reinterpret_cast<uintptr_t>(s_thread.stack)) + uint32_t(s_thread.stackSize);
    add("\nreturn addresses on the stack (sp 0x%08x, stack 0x%08x size 0x%x):\n", unsigned(sp),
        unsigned(uintptr_t(s_thread.stack)), unsigned(s_thread.stackSize));
    uint32_t found = 0;
    if (sp < top && top - sp <= 16 * 1024 * 1024)
    {
        const uint32_t words = std::min(StackWords, (top - sp) / 4);
        const uint32_t* stack = reinterpret_cast<const uint32_t*>(uintptr_t(sp));
        for (uint32_t k = 0; k < words && found < MaxReturns; ++k)
        {
            const uint32_t v = stack[k];
            if ((v & 1) && v - s_codeLo < s_codeSize)
            {
                add("  sp+0x%04x 0x%08x (%s@1+0x%x)\n", unsigned(k * 4), unsigned(v), s_codeName,
                    unsigned(v - s_codeLo));
                ++found;
            }
        }
    }
    if (!found)
        add("  none found\n");
}

void ringText()
{
    add("\n--- last log lines (info lines included) ---\n");
    // A thread that crashed while it held the lock leaves it held: give up after a while and read it anyway.
    bool locked = false;
    for (int k = 0; k < 1000 && !(locked = !s_ringLock.test_and_set(std::memory_order_acquire)); ++k)
        sceKernelDelayThread(100);
    const uint32_t used = std::min(s_ring.used, RingBytes);
    const uint32_t take = std::min(ReportRingBytes, s_ring.wrapped ? RingBytes : used);
    const uint32_t start = (used + RingBytes - take) % RingBytes;
    if (start + take <= RingBytes)
        addBytes(s_ring.text + start, take);
    else
    {
        addBytes(s_ring.text + start, RingBytes - start);
        addBytes(s_ring.text, take - (RingBytes - start));
    }
    if (locked)
        s_ringLock.clear(std::memory_order_release);
    add("\n--- end of report ---\n");
}

char s_freezeName[64];

// Writes s_text to crashes/crash-<stamp>.txt and names it in run.txt.
void save()
{
    char name[64], path[352];
    char when[32];
    stamp(when, sizeof(when));
    snprintf(name, sizeof(name), "crash-%s.txt", when);
    s_freezeName[0] = 0;
    snprintf(path, sizeof(path), "%s/%s", s_folder, name);
    writeFile(path, s_text, s_used, false);
    char line[96];
    const int n = snprintf(line, sizeof(line), "report=%s\n", name);
    writeFile(s_runPath, line, uint32_t(n), true);
    // Also in the log, which the next start keeps as the previous log.
    static const char Note[] = "[error] crash: report written to crashes/";
    char tail[160];
    const int m = snprintf(tail, sizeof(tail), "%s%s\n", Note, name);
    logBlockUnlocked(tail, unsigned(m));
}

void onException(KuKernelExceptionContext* c)
{
    const uint32_t type = c->exceptionType < 3 ? c->exceptionType : 0;
    if (s_reporting.exchange(1) == 0)
    {
        s_used = 0;
        header(Types[type]);
        if (type == 0)
            add("data address 0x%08x, fault status 0x%08x\n", unsigned(c->FAR), unsigned(c->FSR));
        else if (type == 1)
            add("instruction address 0x%08x, fault status 0x%08x\n", unsigned(c->FAR), unsigned(c->FSR));
        add("\n");
        where("pc ", c->pc);
        where("lr ", c->lr);
        for (int k = 0; k < 13; k += 4)
        {
            for (int j = k; j < k + 4 && j < 13; ++j)
                add("r%-2d %08x  ", j, unsigned(c->r[j]));
            add("\n");
        }
        add("sp  %08x  cpsr %08x  fpscr %08x\n", unsigned(c->sp), unsigned(c->SPSR), unsigned(c->FPSCR));
        stackScan(c->sp);
        ringText();
        save();     // no logFlush: the crashed thread may hold the log's file lock; the ring has the lines
    }
    // The system's own handling: the process ends with its crash dump.
    if (KuKernelExceptionHandler old = s_old[type])
        old(c);
    sceKernelExitProcess(1);
}

void onTerminate()
{
    report("std::terminate (an exception nobody caught)");
    __builtin_trap();
}

void onAbort(int)
{
    report("abort()");
    __builtin_trap();
}

void scanDumps(time_t since, std::string& newest)
{
    const SceUID dir = sceIoDopen("ux0:data");
    if (dir < 0)
        return;
    SceIoDirent entry;
    long long best = 0;
    while (sceIoDread(dir, &entry) > 0)
    {
        const char* n = entry.d_name;
        if (strncmp(n, "psp2core-", 9) != 0 || !strstr(n, "-eboot.bin.psp2dmp"))
            continue;
        const long long t = strtoll(n + 9, nullptr, 10);
        if (t + 5 >= (long long)since && t > best)
        {
            best = t;
            newest = n;
        }
    }
    sceIoDclose(dir);
}

}

void begin(const char* dataFolder, const char* logPath, const char* version)
{
    s_version = version;
    findCode();
    snprintf(s_folder, sizeof(s_folder), "%s/crashes", dataFolder);
    snprintf(s_runPath, sizeof(s_runPath), "%s/run.txt", s_folder);
    sceIoMkdir(s_folder, 0777);

    // The last run's log, before logInit empties it.
    std::string previous;
    if (logPath && exists(logPath))
    {
        previous = logPath;
        const size_t dot = previous.rfind(".log");
        previous = (dot == std::string::npos ? previous : previous.substr(0, dot)) + ".previous.log";
        sceIoRemove(previous.c_str());
        if (sceIoRename(logPath, previous.c_str()) < 0)
            previous.clear();
    }

    // run.txt still there: the last run did not end through the normal exit (a crash, or closed from the home screen).
    const std::string run = readText(s_runPath);
    if (!run.empty())
    {
        const std::string report = field(run, "report");
        const time_t started = time_t(strtoll(field(run, "start").c_str(), nullptr, 10));
        std::string dump;
        if (started > 0)
            scanDumps(started, dump);
        std::string base;
        if (!report.empty())
            base = report.substr(0, report.rfind('.'));
        else if (!dump.empty())
            base = "crash-" + dump.substr(9, dump.find('-', 9) - 9);
        if (!base.empty())
        {
            const std::string log = std::string(s_folder) + "/" + base + ".log";
            if (!previous.empty())
                copyFile(previous.c_str(), log.c_str());
            std::string line = report.compare(0, 7, "freeze-") == 0 ? "The last run froze." : "The last run crashed.";
            if (!report.empty())
                line += " Report: " + std::string(s_folder) + "/" + report + ".";
            else
                line += " No crash report (kubridge.skprx is needed for one).";
            if (!dump.empty())
                line += " System dump: ux0:data/" + dump + ".";
            if (!previous.empty())
                line += " Its log: " + log + ".";
            s_notices.push_back(line);
        }
        sceIoRemove(s_runPath);
    }
    char text[160];
    const int n = snprintf(text, sizeof(text), "start=%lld\nversion=%s\ncommit=%s\n", (long long)time(nullptr),
                           version, NFSU_COMMIT);
    writeFile(s_runPath, text, uint32_t(n), false);
}

bool install()
{
    if (!Config::getBool("crash_report", true))
        return false;
    std::set_terminate(&onTerminate);
    signal(SIGABRT, &onAbort);
    int result = 0;
    for (uint32_t type = 0; type < 3 && result >= 0; ++type)
        result = kuKernelRegisterExceptionHandler(type, &onException, &s_old[type], nullptr);
    if (result >= 0)
        s_installed = true;
    else if (kuKernelRegisterAbortHandler(&onException, &s_old[0], nullptr) >= 0)
    {
        // An older kubridge: data and prefetch aborts only, both through one handler.
        s_old[1] = s_old[0];
        s_installed = true;
        logInfo("crash: kubridge without undefined instruction handlers (an older version): guest traps get no report");
    }
    if (s_installed)
        logInfo("crash: reports go to %s (code %s 0x%08x size 0x%x)", s_folder, s_codeName, unsigned(s_codeLo),
                unsigned(s_codeSize));
    else
        logInfo("crash: kubridge.skprx is not loaded (0x%08x): no crash reports, only the system dump", unsigned(result));
    return s_installed;
}

const std::vector<std::string>& notices()
{
    return s_notices;
}

void record(const char* text, unsigned length)
{
    if (length >= RingBytes)
    {
        text += length - (RingBytes - 1);
        length = RingBytes - 1;
    }
    while (s_ringLock.test_and_set(std::memory_order_acquire))
    {
    }
    uint32_t used = s_ring.used;
    const uint32_t first = std::min(uint32_t(length), RingBytes - used);
    memcpy(s_ring.text + used, text, first);
    if (first < length)
    {
        memcpy(s_ring.text, text + first, length - first);
        s_ring.wrapped = 1;
    }
    used += length;
    if (used >= RingBytes)
    {
        used -= RingBytes;
        s_ring.wrapped = 1;
    }
    s_ring.used = used;
    s_ringLock.clear(std::memory_order_release);
}

void noteTrap(uint32_t guestAddress, const char* reason)
{
    s_trapAddress = guestAddress;
    s_trapReason = reason;
    // Without the handlers the trap itself writes nothing: report it here, without registers.
    if (!s_installed)
        report("guest trap");
}

void report(const char* reason)
{
    if (!s_runPath[0] || s_reporting.exchange(1) != 0)
        return;
    s_used = 0;
    header(reason);
    stackScan(uint32_t(reinterpret_cast<uintptr_t>(__builtin_frame_address(0))));
    ringText();
    save();
    logFlush();
}

void freeze(const char* what)
{
    if (!s_runPath[0] || s_reporting.load() != 0)
        return;
    const bool first = !s_freezeName[0];
    if (first)
    {
        char when[32];
        stamp(when, sizeof(when));
        snprintf(s_freezeName, sizeof(s_freezeName), "freeze-%s.txt", when);
        char line[96];
        const int n = snprintf(line, sizeof(line), "report=%s\n", s_freezeName);
        writeFile(s_runPath, line, uint32_t(n), true);
    }
    s_used = 0;
    header(what);
    ringText();
    char path[352];
    snprintf(path, sizeof(path), "%s/%s", s_folder, s_freezeName);
    writeFile(path, s_text, s_used, false);
    if (first)
    {
        char tail[160];
        const int m = snprintf(tail, sizeof(tail), "[error] crash: freeze report written to crashes/%s\n", s_freezeName);
        logBlockUnlocked(tail, unsigned(m));
    }
}

void end()
{
    if (s_runPath[0])
        sceIoRemove(s_runPath);
}

}}
