// Stall reporter (stall.h): a low-priority thread checks the present count every 500 ms; after stall_seconds without
// one it reports, then every 30 s while stalled, three times at most.
#include <platform/stall.h>

#include <platform/config.h>
#include <platform/crashlog.h>
#include <platform/log.h>
#include <platform/sampler.h>
#include <platform/sync.h>

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>

#include <algorithm>
#include <csignal>
#include <cstdarg>
#include <exception>
#include <new>
#include <cstdio>
#include <cstring>
#include <malloc.h>

extern "C"
{
int sceKernelSuspendThreadForVM(SceUID thid);
int sceKernelResumeThreadForVM(SceUID thid);
int sceKernelGetCondInfo(SceUID condId, SceKernelCondInfo* info);
int sceKernelGetLwMutexInfoById(SceUID lwMutexId, void* info);
int sceKernelGetLwCondInfoById(SceUID lwCondId, void* info);
}

namespace pf { namespace stall
{

Gxm g_gxm;

namespace
{

const uint32_t CheckMs = 500;
const uint64_t GapUs = 5000000;         // a longer check interval: the process was paused, not stalled
const uint64_t RepeatUs = 30000000;
const uint32_t MaxReports = 3;
const int MaxThreads = 64;
const int Claimed = -1;
const uint32_t StackScanBytes = 32 * 1024;
const uint32_t StackAddresses = 24;

// SceKernelLwMutexInfo and SceKernelLwCondInfo of psp2kern.
struct LwMutexInfo
{
    SceSize     size;
    SceUID      id;
    char        name[32];
    SceUInt32   attr;
    void*       work;
    SceInt32    initCount;
    SceInt32    currentCount;
    SceUID      currentOwnerId;
    SceUInt32   numWaitThreads;
};

struct LwCondInfo
{
    SceSize     size;
    SceUID      id;
    char        name[32];
    SceUInt32   attr;
    void*       work;
    void*       lwMutex;
    SceInt32    numWaitThreads;
};

template< size_t N >
struct Text
{
    char    data[N];
    size_t  used;

    void clear()
    {
        used = 0;
        data[0] = 0;
    }

    __attribute__((format(printf, 2, 3))) void add(const char* fmt, ...)
    {
        if (used + 1 >= N)
            return;
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(data + used, N - used, fmt, args);
        va_end(args);
        if (n > 0)
            used = std::min(N - 1, used + size_t(n));
    }
};

struct Entry
{
    std::atomic<int>    id;         // 0: free
    char                name[32];
};
Entry s_registry[MaxThreads];
std::atomic<uint32_t> s_unregistered{0};

struct Snapshot
{
    SceUID              id;
    char                name[32];
    int                 infoResult;
    SceKernelThreadInfo info;
    Text<256>           wait;
    bool                read;       // registers tried: result is the context call's
    int                 result;
    bool                broken;     // its wait was broken to read the registers
    uint32_t            pc, lr, sp;
    uint32_t            stack[StackAddresses];
    uint32_t            stackCount;
};
const int MaxSnapshots = MaxThreads + 2;
Snapshot s_snapshots[MaxSnapshots];
int s_snapshotCount = 0;

const uint32_t BufferSize = 32 * 1024;
char s_buffer[BufferSize];
uint32_t s_used = 0;

SceUID s_main = -1;
SceUID s_self = -1;
Thread* s_thread = nullptr;
Semaphore* s_wake = nullptr;
std::atomic<bool> s_run{false};
uint64_t s_stallUs = 8000000;
bool s_contexts = false;
uint32_t s_codeLo = 0, s_codeSize = 0;
int s_priority = Thread::PriorityLow;
bool s_powerTick = false;        // stall_power_tick: keeps the system from idling into standby
uint64_t s_crashUs = 0;         // stall_crash: crash for a dump after this long without a present
bool s_direct = false;          // stall_direct: reports bypass the log mutex
uint64_t s_heartbeatUs = 0;     // stall_heartbeat: progress lines while running

uint64_t nowUs()
{
    return sceKernelGetProcessTimeWide();
}

SceUID s_logFd = -1;            // stall_direct: the log, open from the start
int s_probe = 0;                // the last test open's result
uint32_t s_probeFailures = 0;

void flush()
{
    if (s_direct && s_logFd >= 0)
    {
        sceIoWrite(s_logFd, s_buffer, s_used);
        s_used = 0;
        return;
    }
    if (s_direct)
        logBlockUnlocked(s_buffer, s_used);
    else
        logBlock(s_buffer, s_used);
    s_used = 0;
}

// Appends a report line; never while a thread is suspended.
__attribute__((format(printf, 1, 2))) void line(const char* fmt, ...)
{
    if (s_used > BufferSize - 2048)
        flush();
    static const char Prefix[] = "[info] stall: ";
    memcpy(s_buffer + s_used, Prefix, sizeof(Prefix) - 1);
    s_used += sizeof(Prefix) - 1;
    const uint32_t room = BufferSize - s_used - 1;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(s_buffer + s_used, room, fmt, args);
    va_end(args);
    if (n > 0)
        s_used += std::min(uint32_t(n), room - 1);
    s_buffer[s_used++] = '\n';
}

// The code segment holding this function.
void findCode()
{
    const uint32_t self = uint32_t(reinterpret_cast<uintptr_t>(&findCode));
    SceKernelModuleInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    const SceUID module = sceKernelGetModuleIdByAddr(reinterpret_cast<void*>(&findCode));
    if (module < 0 || sceKernelGetModuleInfo(module, &info) < 0)
        return;
    for (const SceKernelSegmentInfo& segment : info.segments)
    {
        const uint32_t lo = uint32_t(reinterpret_cast<uintptr_t>(segment.vaddr));
        if (self - lo < segment.memsz)
        {
            s_codeLo = lo;
            s_codeSize = segment.memsz;
        }
    }
}

// Thumb return address: odd, in code, after BL or BLX.
bool returnAddress(uint32_t word)
{
    const uint32_t at = word & ~1u;
    if (!(word & 1) || at - s_codeLo < 4 || at - s_codeLo >= s_codeSize)
        return false;
    const uint16_t* code = reinterpret_cast<const uint16_t*>(uintptr_t(at));
    const uint16_t first = code[-2], second = code[-1];
    const bool longCall = (first & 0xf800) == 0xf000
                       && ((second & 0xd000) == 0xd000 || (second & 0xd001) == 0xc000);
    return longCall || (second & 0xff87) == 0x4780;
}

// Return addresses above sp in its memory block, innermost first.
void scanStack(Snapshot& s)
{
    s.stackCount = 0;
    SceKernelMemBlockInfo block;
    memset(&block, 0, sizeof(block));
    block.size = sizeof(block);
    if ((s.sp & 3) || sceKernelGetMemBlockInfoByAddr(reinterpret_cast<void*>(uintptr_t(s.sp)), &block) < 0)
        return;
    const uint32_t blockLo = uint32_t(reinterpret_cast<uintptr_t>(block.mappedBase));
    if (s.sp - blockLo >= block.mappedSize)
        return;
    uint32_t top = blockLo + block.mappedSize;
    const uint32_t base = uint32_t(reinterpret_cast<uintptr_t>(s.info.stack));
    if (s.sp - base < uint32_t(s.info.stackSize))
        top = std::min(top, base + uint32_t(s.info.stackSize));
    const uint32_t end = top - s.sp > StackScanBytes ? s.sp + StackScanBytes : top;
    for (uint32_t at = s.sp; at < end && s.stackCount < StackAddresses; at += 4)
    {
        const uint32_t word = *reinterpret_cast<const uint32_t*>(uintptr_t(at));
        if (returnAddress(word))
            s.stack[s.stackCount++] = word;
    }
}

void addSnapshot(SceUID id, const char* name)
{
    if (id <= 0 || id == s_self || s_snapshotCount == MaxSnapshots)
        return;
    for (int i = 0; i < s_snapshotCount; ++i)
        if (s_snapshots[i].id == id)
            return;
    Snapshot& s = s_snapshots[s_snapshotCount++];
    memset(&s, 0, sizeof(s));
    s.id = id;
    strncpy(s.name, name, sizeof(s.name) - 1);
    s.info.size = sizeof(s.info);
    s.infoResult = sceKernelGetThreadInfo(id, &s.info);
    s.wait.clear();
}

const char* threadName(SceUID id)
{
    for (int i = 0; i < s_snapshotCount; ++i)
        if (s_snapshots[i].id == id)
            return s_snapshots[i].name;
    return id == s_self ? "stall_report" : "?";
}

template< size_t N >
void addOwner(Text<N>& t, SceUID owner)
{
    if (owner > 0)
        t.add(" owner 0x%08x \"%s\"", unsigned(owner), threadName(owner));
    else
        t.add(" no owner");
}

// Describes a wait object, trying each kernel object kind.
template< size_t N >
void describeObject(Text<N>& t, SceUID id)
{
    SceKernelMutexInfo mutex;
    mutex.size = sizeof(mutex);
    if (sceKernelGetMutexInfo(id, &mutex) >= 0)
    {
        t.add(" mutex \"%.31s\" count %d", mutex.name, mutex.currentCount);
        addOwner(t, mutex.currentOwnerId);
        t.add(" waiters %d", mutex.numWaitThreads);
        return;
    }
    SceKernelSemaInfo sema;
    sema.size = sizeof(sema);
    if (sceKernelGetSemaInfo(id, &sema) >= 0)
    {
        t.add(" semaphore \"%.31s\" count %d of %d waiters %d", sema.name, sema.currentCount, sema.maxCount,
              sema.numWaitThreads);
        return;
    }
    SceKernelCondInfo cond;
    cond.size = sizeof(cond);
    if (sceKernelGetCondInfo(id, &cond) >= 0)
    {
        t.add(" cond \"%.31s\" waiters %d, mutex 0x%08x", cond.name, cond.numWaitThreads, unsigned(cond.mutexId));
        mutex.size = sizeof(mutex);
        if (sceKernelGetMutexInfo(cond.mutexId, &mutex) >= 0)
        {
            t.add(" \"%.31s\"", mutex.name);
            addOwner(t, mutex.currentOwnerId);
        }
        return;
    }
    SceKernelEventFlagInfo flag;
    flag.size = sizeof(flag);
    if (sceKernelGetEventFlagInfo(id, &flag) >= 0)
    {
        t.add(" event flag \"%.31s\" pattern 0x%x waiters %d", flag.name, unsigned(flag.currentPattern),
              flag.numWaitThreads);
        return;
    }
    LwMutexInfo lwMutex;
    memset(&lwMutex, 0, sizeof(lwMutex));
    lwMutex.size = sizeof(lwMutex);
    if (sceKernelGetLwMutexInfoById(id, &lwMutex) >= 0)
    {
        t.add(" lw mutex \"%.31s\" count %d", lwMutex.name, int(lwMutex.currentCount));
        addOwner(t, lwMutex.currentOwnerId);
        t.add(" waiters %u", unsigned(lwMutex.numWaitThreads));
        return;
    }
    LwCondInfo lwCond;
    memset(&lwCond, 0, sizeof(lwCond));
    lwCond.size = sizeof(lwCond);
    if (sceKernelGetLwCondInfoById(id, &lwCond) >= 0)
    {
        t.add(" lw cond \"%.31s\" waiters %d", lwCond.name, int(lwCond.numWaitThreads));
        return;
    }
    if (id > 0)
        t.add(" (no object info)");
}

void readRegisters(Snapshot& s, bool breakWait)
{
    s.read = true;
    s.result = sceKernelSuspendThreadForVM(s.id);
    if (s.result < 0)
        return;
    s.broken = breakWait;
    s.result = sampler::registers(s.id, &s.pc, &s.lr, &s.sp);
    if (s.result >= 0)
        scanStack(s);
    sceKernelResumeThreadForVM(s.id);
}

const char* stage(const std::atomic<const char*>& marker)
{
    const char* name = marker.load(std::memory_order_relaxed);
    return name ? name : "-";
}

void gxmLines()
{
    const Gxm& g = g_gxm;
    Text<256> slots;
    slots.clear();
    uint32_t gpuDone = 0;
    for (int i = 0; i < MaxSlots; ++i)
    {
        const volatile unsigned int* done = g.slotDone[i].load(std::memory_order_relaxed);
        if (!done)
            continue;
        const uint32_t value = *done, wanted = g.slotValue[i].load(std::memory_order_relaxed);
        gpuDone = std::max(gpuDone, value);
        slots.add(" %u/%u%s", unsigned(value), unsigned(wanted), value == wanted ? "" : " pending");
    }
    Text<128> layers;
    layers.clear();
    for (int i = 0; i < MaxLayers; ++i)
    {
        const volatile unsigned int* done = g.layerDone[i].load(std::memory_order_relaxed);
        if (done)
            layers.add(" %u/%u", unsigned(*done), unsigned(g.layerValue[i].load(std::memory_order_relaxed)));
    }
    line("gxm frames: begun %u submitted %u completed %u, gpu done %u, slots (done/wanted)%s | stage \"%s\" | "
         "uploads queued %u done %u",
         unsigned(g.begun.load()), unsigned(g.submitted.load()), unsigned(g.completed.load()), unsigned(gpuDone),
         slots.data, stage(g.frames), unsigned(g.uploadsQueued.load()), unsigned(g.uploadsDone.load()));
    line("gxm render: stage \"%s\" frame %u scene %u of %u | presents %u, display callbacks %u (present %u) | "
         "layers (done/wanted)%s",
         stage(g.render), unsigned(g.drawing.load()), unsigned(g.scene.load()), unsigned(g.scenes.load()),
         unsigned(g.presents.load()), unsigned(g.flips.load()), unsigned(g.flipped.load()), layers.data);
    Text<512> recorders;
    recorders.clear();
    for (int i = 0; i < MaxRecorders; ++i)
        if (g.recorder[i].load(std::memory_order_relaxed))
            recorders.add(" | %d \"%s\" job %u", i, stage(g.recorder[i]), unsigned(g.recorderJob[i].load()));
    line("gxm lists: generation %u jobs %u done %u%s", unsigned(g.jobGeneration.load()), unsigned(g.jobs.load()),
         unsigned(g.jobsDone.load()), recorders.data);
}

template< size_t N >
void addStatus(Text<N>& t, uint32_t status)
{
    static const char* const Names[] = { "running", "ready", "standby", "waiting", "dormant", "deleted", "dead",
                                         "stagnant", "suspended" };
    bool any = false;
    for (uint32_t i = 0; i < sizeof(Names) / sizeof(Names[0]); ++i)
    {
        if (status & (1u << i))
        {
            t.add("%s%s", any ? "+" : "", Names[i]);
            any = true;
        }
    }
    t.add("%s(0x%x)", any ? " " : "", unsigned(status));
}

void threadLines(const Snapshot& s)
{
    Text<512> t;
    t.clear();
    t.add("thread 0x%08x \"%s\"", unsigned(s.id), s.name);
    if (s.infoResult < 0)
    {
        line("%s: no thread info (0x%08x)", t.data, unsigned(s.infoResult));
        return;
    }
    const SceKernelThreadInfo& i = s.info;
    if (strncmp(i.name, s.name, sizeof(s.name)))
        t.add(" kernel \"%.31s\"", i.name);
    t.add(" ");
    addStatus(t, i.status);
    t.add(" priority %d cpu %d mask 0x%x run %llu ms", int(i.currentPriority), int(i.currentCpuId),
          unsigned(i.currentCpuAffinityMask), (unsigned long long)(i.runClocks / 1000));
    if (s.wait.used)
        t.add(" | %s", s.wait.data);
    line("%s", t.data);
    if (!s.read)
        return;
    if (s.result < 0)
    {
        line("  regs not read (0x%08x)", unsigned(s.result));
        return;
    }
    line("  regs pc 0x%08x lr 0x%08x sp 0x%08x%s", unsigned(s.pc), unsigned(s.lr), unsigned(s.sp),
         s.broken ? " (wait broken to read them)" : "");
    if (!s.stackCount)
        return;
    Text<512> stack;
    stack.clear();
    for (uint32_t k = 0; k < s.stackCount; ++k)
        stack.add(" 0x%08x", unsigned(s.stack[k]));
    line("  stack%s", stack.data);
}

// The log mutex's owner, 0 when free.
SceUID logOwner(SceKernelMutexInfo* info)
{
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    if (logMutexId() < 0 || sceKernelGetMutexInfo(logMutexId(), info) < 0 || info->currentCount <= 0)
        return 0;
    return info->currentOwnerId;
}

void logMutexLine()
{
    SceKernelMutexInfo info;
    const SceUID owner = logOwner(&info);
    line("log mutex 0x%08x: count %d owner 0x%08x \"%s\" waiters %d", unsigned(logMutexId()), int(info.currentCount),
         unsigned(owner), owner ? threadName(owner) : "-", int(info.numWaitThreads));
}

// One line through the unlocked path; for failure hooks.
__attribute__((format(printf, 1, 2))) void urgent(const char* fmt, ...)
{
    char text[512];
    static const char Prefix[] = "[info] stall: ";
    memcpy(text, Prefix, sizeof(Prefix) - 1);
    size_t used = sizeof(Prefix) - 1;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(text + used, sizeof(text) - used - 1, fmt, args);
    va_end(args);
    used = n > 0 ? std::min(sizeof(text) - 2, used + size_t(n)) : used;
    text[used++] = '\n';
    logBlockUnlocked(text, unsigned(used));
}

void heapLine(const char* what)
{
    const struct mallinfo m = mallinfo();
    urgent("%s in thread 0x%08x: newlib heap arena %u used %u free %u, lr %p", what, unsigned(sceKernelGetThreadId()),
           unsigned(m.arena), unsigned(m.uordblks), unsigned(m.fordblks), __builtin_return_address(0));
}

void onNoMemory()
{
    heapLine("operator new failed");
    std::set_new_handler(nullptr);
}

void onTerminate()
{
    heapLine("std::terminate");
    *static_cast<volatile int*>(nullptr) = 0x7e57;
    for (;;) {}
}

void onAbort(int)
{
    heapLine("abort()");
    *static_cast<volatile int*>(nullptr) = 0xab07;
}

void heartbeat(uint64_t t)
{
    const Gxm& g = g_gxm;
    SceKernelMutexInfo info;
    const SceUID owner = logOwner(&info);
    const struct mallinfo m = mallinfo();
    s_probe = sceIoOpen(logFilePath(), SCE_O_RDONLY, 0);
    if (s_probe >= 0)
        sceIoClose(s_probe);
    else
        ++s_probeFailures;
    s_used = 0;
    line("open probe 0x%08x (%u failed)", unsigned(s_probe), unsigned(s_probeFailures));
    line("heartbeat %u ms: presents %u callbacks %u | frames begun %u submitted %u completed %u \"%s\" | render "
         "frame %u (%u draws) \"%s\" | lists %u/%u | uploads %u/%u | log owner 0x%08x waiters %d | heap used %u free %u",
         unsigned(t / 1000), unsigned(g.presents.load()), unsigned(g.flips.load()), unsigned(g.begun.load()),
         unsigned(g.submitted.load()), unsigned(g.completed.load()), stage(g.frames), unsigned(g.drawing.load()),
         unsigned(g.draws.load()), stage(g.render), unsigned(g.jobsDone.load()), unsigned(g.jobs.load()), unsigned(g.uploadsDone.load()),
         unsigned(g.uploadsQueued.load()), unsigned(owner), int(info.numWaitThreads), unsigned(m.uordblks),
         unsigned(m.fordblks));
    flush();
}

void report(uint64_t stalledUs, uint32_t number)
{
    const uint64_t began = nowUs();
    const bool last = number == MaxReports;
    s_used = 0;
    line("no present for %u.%u s (presents %u), report %u of %u; pf::stall::start at %p, code 0x%08x size 0x%x",
         unsigned(stalledUs / 1000000), unsigned(stalledUs / 100000 % 10), unsigned(g_gxm.presents.load()),
         unsigned(number), unsigned(MaxReports), reinterpret_cast<void*>(&start), unsigned(s_codeLo),
         unsigned(s_codeSize));
    gxmLines();

    Thread::setCurrentPriority(Thread::PriorityHigh);
    s_snapshotCount = 0;
    addSnapshot(s_main, "main");
    addSnapshot(g_gxm.displayThread.load(std::memory_order_relaxed), "display queue");
    for (Entry& e : s_registry)
    {
        const int id = e.id.load(std::memory_order_acquire);
        if (id > 0)
            addSnapshot(id, e.name);
    }
    for (int k = 0; k < s_snapshotCount; ++k)
    {
        Snapshot& s = s_snapshots[k];
        if (s.infoResult >= 0 && (s.info.status & SCE_THREAD_WAITING))
        {
            s.wait.add("wait type 0x%x on 0x%08x", unsigned(s.info.waitType), unsigned(s.info.waitId));
            describeObject(s.wait, s.info.waitId);
        }
    }
    // Threads in a wait are only suspended in the last report: suspending one breaks its wait.
    for (int k = 0; k < s_snapshotCount && s_contexts; ++k)
    {
        Snapshot& s = s_snapshots[k];
        const uint32_t status = s.infoResult >= 0 ? s.info.status : 0;
        if (status & (SCE_THREAD_RUNNING | SCE_THREAD_READY))
            readRegisters(s, false);
    }
    for (int k = 0; k < s_snapshotCount && s_contexts && last; ++k)
    {
        Snapshot& s = s_snapshots[k];
        if (s.infoResult >= 0 && (s.info.status & SCE_THREAD_WAITING))
            readRegisters(s, true);
    }
    Thread::setCurrentPriority(s_priority);
    logMutexLine();

    for (int k = 0; k < s_snapshotCount; ++k)
        threadLines(s_snapshots[k]);
    if (const uint32_t missing = s_unregistered.load())
        line("%u threads not in the registry (full)", unsigned(missing));
    line("end of report %u (%u us)%s", unsigned(number), unsigned(nowUs() - began),
         s_contexts ? (last ? "" : "; threads in a wait get registers in the last report")
                    : "; no thread registers (no VM thread functions)");
    flush();
    char what[96];
    snprintf(what, sizeof(what), "freeze: no frame for %u s (stall report %u of %u)", unsigned(stalledUs / 1000000),
             unsigned(number), unsigned(MaxReports));
    crashlog::freeze(what);
}

int reporterEntry(void*)
{
    s_self = sceKernelGetThreadId();
    if (s_direct)
        s_logFd = sceIoOpen(logFilePath(), SCE_O_WRONLY | SCE_O_APPEND, 0);
    logInfo("stall: a report after %u s without a present (stall_seconds)%s; pf::stall::start at %p, code 0x%08x "
            "size 0x%x", unsigned(s_stallUs / 1000000), s_contexts ? "" : ", without thread registers",
            reinterpret_cast<void*>(&start), unsigned(s_codeLo), unsigned(s_codeSize));
    uint32_t seen = 0, reports = 0;
    uint64_t lastChange = nowUs(), lastCheck = lastChange, lastReport = 0, lastBeat = lastChange;
    SceUID heldBy = 0;
    uint64_t heldSince = 0;
    bool heldReported = false;
    while (true)
    {
        s_wake->waitFor(CheckMs);
        if (!s_run.load(std::memory_order_relaxed))
            break;
        if (s_powerTick)
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        const uint64_t t = nowUs();
        const uint32_t presents = g_gxm.presents.load(std::memory_order_relaxed);
        // Deliberate: a crash dump holds every thread's state (stall_crash).
        if (s_crashUs && presents && presents == seen && t - lastChange >= s_crashUs && t - lastCheck <= GapUs)
            *static_cast<volatile uint32_t*>(nullptr) = presents;
        if (s_heartbeatUs && t - lastBeat >= s_heartbeatUs)
        {
            heartbeat(t);
            lastBeat = t;
        }
        SceKernelMutexInfo info;
        const SceUID owner = s_direct ? logOwner(&info) : 0;
        if (owner != heldBy)
        {
            heldBy = owner;
            heldSince = t;
            heldReported = false;
        }
        // A log mutex held this long blocks every logger, presents included (stall_direct only).
        if (heldBy && !heldReported && !reports && t - heldSince >= s_stallUs && t - lastCheck <= GapUs)
        {
            heldReported = true;
            s_used = 0;
            line("log mutex held by 0x%08x for %u ms", unsigned(heldBy), unsigned((t - heldSince) / 1000));
            flush();
            report(t - lastChange, ++reports);
            lastReport = t;
        }
        else if (presents != seen)
        {
            if (reports)
            {
                s_used = 0;
                line("presents again after %u ms", unsigned((t - lastChange) / 1000));
                flush();
            }
            seen = presents;
            lastChange = t;
            reports = 0;
        }
        else if (!reports && t - lastCheck > GapUs)
        {
            lastChange = t;
        }
        else if (presents && (reports ? reports < MaxReports && t - lastReport >= RepeatUs
                                      : t - lastChange >= s_stallUs))
        {
            report(t - lastChange, ++reports);
            lastReport = t;
        }
        lastCheck = nowUs();
    }
    return 0;
}

}

void start()
{
    if (s_thread || !Config::getBool("stall_report", true))
        return;
    const int seconds = std::max(1, Config::getInt("stall_seconds", 8));
    s_stallUs = uint64_t(seconds) * 1000000u;
    s_main = sceKernelGetThreadId();
    s_contexts = sampler::calibrate();
    findCode();
    s_direct = Config::getBool("stall_direct", false);
    if (s_direct)
    {
        std::set_new_handler(&onNoMemory);
        std::set_terminate(&onTerminate);
        signal(SIGABRT, &onAbort);
    }
    s_heartbeatUs = uint64_t(std::max(0, Config::getInt("stall_heartbeat", 0))) * 1000000u;
    s_crashUs = uint64_t(std::max(0, Config::getInt("stall_crash", 0))) * 1000000u;
    s_powerTick = Config::getBool("stall_power_tick", false);
    const int cpus = Config::getInt("stall_cpu", 0) & 15;
    s_priority = std::min(int(Thread::PriorityLow), std::max(int(Thread::PriorityHigh),
                                                            Config::getInt("stall_priority", Thread::PriorityLow)));
    s_wake = new Semaphore("stall_wake", 0);
    s_run = true;
    s_thread = new Thread("stall_report", &reporterEntry, nullptr, s_priority, 32 * 1024,
                          cpus ? cpus << 16 : Thread::CpuAll);
    s_thread->start();
}

void stop()
{
    if (!s_thread)
        return;
    s_run = false;
    s_wake->post();
    s_thread->join();
    delete s_thread;
    s_thread = nullptr;
    delete s_wake;
    s_wake = nullptr;
}

void addThread(int id, const char* name)
{
    for (Entry& e : s_registry)
    {
        int expected = 0;
        if (e.id.load(std::memory_order_relaxed) == 0
            && e.id.compare_exchange_strong(expected, Claimed, std::memory_order_acquire))
        {
            strncpy(e.name, name, sizeof(e.name) - 1);
            e.name[sizeof(e.name) - 1] = 0;
            e.id.store(id, std::memory_order_release);
            return;
        }
    }
    s_unregistered.fetch_add(1, std::memory_order_relaxed);
}

void removeThread(int id)
{
    for (Entry& e : s_registry)
    {
        if (e.id.load(std::memory_order_relaxed) == id)
        {
            e.id.store(0, std::memory_order_release);
            return;
        }
    }
}

}}
