// Worker context of a draw flush segment: snapshot of the guest globals, write-back, chain return, check-mode poison.

#include <native/flush_context.h>
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <platform/time.h>

namespace ug2 { namespace native { namespace flush {

namespace
{

const reg32 CounterBase = 0x872ed8;
const reg32 Poison = 0xa5a5a5a5u;
const int CounterOffset = find(CounterBase, 4);

}

void detachList(reg32 head, reg32 detached)
{
    const reg32 first = rc::ld32(head), last = rc::ld32(head + 4);
    rc::st32(detached, first);
    rc::st32(detached + 4, last);
    rc::st32(first + 4, detached);
    rc::st32(last, detached);
    rc::st32(head, head);
    rc::st32(head + 4, head);
}

void Context::beginSnapshot()
{
    lightCount = 0;
    memset(lightSlot, 0, sizeof(lightSlot));
}

bool Context::addLight(reg32 address)
{
    if (findLight(address))
        return true;
    if (lightCount == LightSlots)
        return false;
    LightCopy& l = lights[lightCount];
    l.address = address;
    memcpy(l.vector, rc::host(address + 0xc0), sizeof(l.vector));
    memcpy(l.array, rc::host(address + 0x120), sizeof(l.array));
    reg32 h = (address >> 6 ^ address >> 13) & 127;
    while (lightSlot[h])
        h = (h + 1) & 127;
    lightSlot[h] = uint8_t(++lightCount);
    return true;
}

void Context::open(const x86::CPU& parent)
{
    viewObject = rc::ld32(0x870848);
    viewKind = rc::ld32(viewObject);
    viewIndex = rc::ld32(viewObject + 4);
    views[0].used = viewIndex < ViewCount;
    views[0].view = viewIndex;
    views[1].used = viewIndex != 9;
    views[1].view = 9;
    for (ViewCopy& v : views)
    {
        if (!v.used)
            continue;
        v.matrices = rc::ld32(ViewBlocks + v.view * ViewBlockSize);
        v.used = v.matrices >= 0x10000;
        if (v.used)
            memcpy(v.m, rc::host(v.matrices), sizeof(v.m));
    }
    for (reg32 i = 0; i < RangeCount; ++i)
        memcpy(mem + rangeOffset(i), rc::host(Ranges[i].base), Ranges[i].size);
    memcpy(counters, mem + CounterOffset, sizeof(counters));
    if (!block)
    {
        block = new win32::MemMap(HeadBytes, win32::MemMap::High, pf::perf::AllocThread);
        heads = block->getBlockStart();
    }
    // One runs at a time, so the guest stack is shared.
    static win32::MemMap* const stack = new win32::MemMap(StackBytes, win32::MemMap::High, pf::perf::AllocThread);
    stackTop = stack->getBlockStart() + StackBytes - 16;
    entries.open(rc::ld32(0x87091c));
    buckets.open(rc::ld32(0x870918));
    strips.open(rc::ld32(0x870954));
    cpu = parent;
    cpu.og = this;
    cpu.esp = stackTop;
}

void Context::writeBack()
{
    for (reg32 i = 0; i < 15; ++i)
    {
        reg32 now;
        memcpy(&now, mem + CounterOffset + 4 * i, 4);
        rc::st32(CounterBase + 4 * i, rc::ld32(CounterBase + 4 * i) + (now - counters[i]));
    }
}

void Context::giveBack()
{
    entries.giveBack();
    buckets.giveBack();
    strips.giveBack();
}

namespace
{

// Guest ranges the snapshots replace.
template< typename F > void sources(const Context& c, F&& each)
{
    if (c.viewObject)
        each(c.viewObject, 8);
    for (const ViewCopy& v : c.views)
    {
        if (!v.used)
            continue;
        each(ViewBlocks + v.view * ViewBlockSize, ViewBlockSize);
        each(v.matrices, 0xc0);
    }
    for (reg32 i = 0; i < c.lightCount; ++i)
    {
        each(c.lights[i].address + 0xc0, 0x10);
        each(c.lights[i].address + 0x120, 0xa0);
    }
}

}

void Context::poison()
{
    reg32 at = 0;
    sources(*this, [&](reg32 base, reg32 size)
    {
        memcpy(savedSources + at, rc::host(base), size);
        at += size;
    });
    sources(*this, [&](reg32 base, reg32 size)
    {
        for (reg32 b = 0; b < size; b += 4)
            rc::st32(base + b, Poison);
    });
    for (reg32 i = 0; i < RangeCount; ++i)
    {
        memcpy(saved + rangeOffset(i), rc::host(Ranges[i].base), Ranges[i].size);
        for (reg32 b = 0; b < Ranges[i].size; b += 4)
            rc::st32(Ranges[i].base + b, Poison);
    }
}

void Context::unpoison()
{
    reg32 at = 0;
    sources(*this, [&](reg32 base, reg32 size)
    {
        for (reg32 b = 0; b < size; b += 4)
            if (rc::ld32(base + b) != Poison)
                pf::logError("flush context: the flush wrote 0x%06x outside its context", unsigned(base + b));
        memcpy(rc::host(base), savedSources + at, size);
        at += size;
    });
    for (reg32 i = 0; i < RangeCount; ++i)
    {
        for (reg32 b = 0; b < Ranges[i].size; b += 4)
            if (rc::ld32(Ranges[i].base + b) != Poison)
                pf::logError("flush context: the flush wrote 0x%06x outside its context", unsigned(Ranges[i].base + b));
        memcpy(rc::host(Ranges[i].base), saved + rangeOffset(i), Ranges[i].size);
    }
}


namespace queue
{

namespace
{

const uint32_t RingSize = 64;
const int WorkerPriority = pf::Thread::PriorityHigh + 8;    // below the audio and render threads (64)
const uint32_t WorkerStack = 256 * 1024;

Context* s_ring[RingSize];
std::atomic<uint32_t> s_first{0}, s_last{0};
pf::Semaphore* s_wake = nullptr;
pf::Thread* s_thread = nullptr;
pf::Mutex* s_runLock = nullptr;
pf::Mutex* s_doneLock = nullptr;
pf::CondVar* s_doneCond = nullptr;
std::atomic<uint32_t> s_waiters{0};
std::atomic<int> s_gameThread{0};
std::atomic<uint64_t> s_waitedUs[2], s_waits[2], s_runs, s_busyUs;

#ifdef __vita__
inline uint32_t readFpscr()
{
    uint32_t value;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(value));
    return value;
}

inline void writeFpscr(uint32_t value)
{
    __asm__ volatile("vmsr fpscr, %0" : : "r"(value) : "memory");
}
#else
inline uint32_t readFpscr() { return 0; }
inline void writeFpscr(uint32_t) {}
#endif

void finished(Context& job)
{
    job.state.store(JobDone, std::memory_order_seq_cst);
    if (s_waiters.load(std::memory_order_seq_cst))
    {
        pf::LockGuard guard(*s_doneLock);
        s_doneCond->broadcast();
    }
}

void runClaimed(Context& job)
{
    pf::LockGuard guard(*s_runLock);
    const uint32_t fpscr = readFpscr();
    if (fpscr != job.fpscr)
        writeFpscr(job.fpscr);
    const uint32_t start = pf::perf::now();
    job.run(job);
    s_busyUs.fetch_add(pf::perf::now() - start, std::memory_order_relaxed);
    s_runs.fetch_add(1, std::memory_order_relaxed);
    if (fpscr != job.fpscr)
        writeFpscr(fpscr);
    finished(job);
}

bool claim(Context& job)
{
    uint32_t queued = JobQueued;
    return job.state.compare_exchange_strong(queued, JobRunning, std::memory_order_acq_rel);
}

int workerEntry(void*)
{
    pf::perf::trackThread("flush_worker");
    for (;;)
    {
        s_wake->wait();
        const uint32_t first = s_first.load(std::memory_order_relaxed);
        Context* job = s_ring[first % RingSize];
        s_first.store(first + 1, std::memory_order_release);
#ifdef DEV_SETTINGS
        static const int delay = pf::Config::getInt("offload_worker_delay", 0);
        if (delay > 0)
            pf::sleepMs(uint32_t(delay));
#endif
        if (claim(*job))
            runClaimed(*job);
    }
    return 0;
}

void start()
{
    s_runLock = new pf::Mutex("flush_run");
    s_doneLock = new pf::Mutex("flush_done");
    s_doneCond = new pf::CondVar("flush_done", *s_doneLock);
    if (CoreMode == Mode::Worker)
    {
        s_wake = new pf::Semaphore("flush_wake", 0);
        s_thread = new pf::Thread("flush_worker", &workerEntry, nullptr, WorkerPriority, WorkerStack,
                                  pf::cores::producerMask());
        s_thread->start();
        pf::logInfo("offload: flush worker on core %d", pf::cores::producerCore());
    }
    else
        pf::logInfo("offload: flushes run by the render thread at the splice");
}

}

void submit(Context& job)
{
    static const bool started = (start(), true);
    (void)started;
    s_gameThread.store(pf::Thread::currentId(), std::memory_order_relaxed);
    job.fpscr = readFpscr();
    job.state.store(JobQueued, std::memory_order_release);
    if (CoreMode == Mode::Worker)
    {
        const uint32_t last = s_last.load(std::memory_order_relaxed);
        s_ring[last % RingSize] = &job;
        s_last.store(last + 1, std::memory_order_release);
        s_wake->post();
    }
}

void await(void* pointer)
{
    Context& job = *static_cast<Context*>(pointer);
    if (isDone(job))
        return;
    if (CoreMode == Mode::RenderThread && claim(job))
    {
        runClaimed(job);
        return;
    }
    const uint32_t start = pf::perf::now();
    s_waiters.fetch_add(1, std::memory_order_seq_cst);
    {
        pf::LockGuard guard(*s_doneLock);
        while (job.state.load(std::memory_order_seq_cst) != JobDone)
            s_doneCond->wait();
    }
    s_waiters.fetch_sub(1, std::memory_order_seq_cst);
    const int game = pf::Thread::currentId() == s_gameThread.load(std::memory_order_relaxed);
    s_waitedUs[game].fetch_add(pf::perf::now() - start, std::memory_order_relaxed);
    s_waits[game].fetch_add(1, std::memory_order_relaxed);
}

int gameThread()
{
    return s_gameThread.load(std::memory_order_relaxed);
}

Totals totals()
{
    return Totals{ { s_waitedUs[0].load(), s_waitedUs[1].load() }, { s_waits[0].load(), s_waits[1].load() },
                   s_runs.load(), s_busyUs.load() };
}

}

}}}
