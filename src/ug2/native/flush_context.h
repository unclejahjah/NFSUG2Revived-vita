#ifndef UG2_NATIVE_FLUSH_CONTEXT_H_
#define UG2_NATIVE_FLUSH_CONTEXT_H_

// Worker context of a draw flush segment (cpu.og): own CPU and guest stack, private copies of the flush's
// globals, detached list heads, chained pool frees.

#include <native/common.h>
#include <winapi/d3d9_native.h>
#include <lib/memmap.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace ug2 { namespace native { namespace flush {

using x86::reg32;

// A guest range copied into the context; rw: the flush writes it.
struct Range
{
    reg32 base, size;
    bool rw;
};

constexpr Range Ranges[] = {
    {0x870910, 4, true},        // current group
    {0x87092c, 8, true},        // last stream, last indices
    {0x870940, 12, true},       // texture cache key, group, scroll flag
    {0x86ead8, 20, true},       // alpha, blend state words of the last texture bind
    {0x87684c, 8, true},        // matrix cache group, world
    {0x800534, 4, true},        // matrix cache view
    {0x872ed8, 60, true},       // statistic counters
    {0x8707cc, 4, false},
    {0x870848, 4, false},       // current view
    {0x870858, 4, false},
    {0x87088c, 12, false},
    {0x870924, 8, false},       // texture scroll divisors
    {0x870ce8, 4, false},
    {0x870cb8, 4, false},
    {0x86518c, 4, false},       // time
    {0x86514c, 4, false},       // frame counter
    {0x800504, 8, false},
    {0x80051c, 24, false},      // fog colour, headlight offsets
    {0x8654a4, 4, false},
    {0x86bd18, 4, false},
    {0x8900b4, 4, false},       // headlight owner
    {0x800b94, 4, false},
    {0x8a07a8, 32, false},      // ring textures
    {0x8a1c94, 12, false},      // ring index, fade
};
constexpr reg32 RangeCount = sizeof(Ranges) / sizeof(Ranges[0]);

constexpr reg32 rangeOffset(reg32 index)
{
    reg32 offset = 0;
    for (reg32 i = 0; i < index; ++i)
        offset += Ranges[i].size;
    return offset;
}

constexpr reg32 totalBytes()
{
    return rangeOffset(RangeCount);
}

// Context offset of n bytes at the address, or -1.
constexpr int find(reg32 address, reg32 n)
{
    for (reg32 i = 0; i < RangeCount; ++i)
        if (address >= Ranges[i].base && address + n <= Ranges[i].base + Ranges[i].size)
            return int(rangeOffset(i) + (address - Ranges[i].base));
    return -1;
}

// Frees to one pool linked as the game's free does; the pool header changes at giveBack. Skinned draws read stale
// bone slots that alias the header, so header() gives the words the guest would hold.
struct Chain
{
    reg32 pool = 0, head = 0, tail = 0, count = 0, free = 0;

    void open(reg32 poolAddress)
    {
        pool = poolAddress;
        head = rc::ld32(pool + 0x10);
        free = rc::ld32(pool + 0x18);
        tail = 0;
        count = 0;
    }

    void push(reg32 node)
    {
        rc::st32(node, head);
        head = node;
        tail = tail ? tail : node;
        ++count;
    }

    // The word at pool +0x10, +0x14 or +0x18 after the frees so far.
    reg32 header(reg32 offset) const
    {
        return offset == 0x10 ? head : offset == 0x18 ? free - count : rc::ld32(pool + offset);
    }

    void giveBack()
    {
        if (count != 0)
        {
            rc::st32(tail, rc::ld32(pool + 0x10));
            rc::st32(pool + 0x10, head);
            rc::st32(pool + 0x18, rc::ld32(pool + 0x18) - count);
        }
        count = 0;
    }
};

// What the flush reads of the view blocks and light contexts, copied at the marker.
const reg32 ViewBlocks = 0x832de0, ViewBlockSize = 0x70, ViewCount = 64;
const reg32 LightSlots = 64;
const reg32 PoisonBytes = 8 + 2 * (ViewBlockSize + 0xc0) + LightSlots * (0x10 + 0xa0);

struct ViewCopy
{
    bool    used = false;
    reg32   view = 0;
    reg32   matrices = 0;   // guest address of the three matrices its block points to
    float   m[48];
};

struct LightCopy
{
    reg32   address;
    float   vector[4];      // context +0xc0
    float   array[40];      // context +0x120
};

const reg32 ListCount = 16 * 23;
const reg32 StackBytes = 64 * 1024;
const reg32 HeadBytes = (ListCount * 8 + 15) & ~15u;

// Where a job's flush runs; NFSU_OFFLOAD_CORE: 0 the game thread, 1 the render thread at the splice, 2 a worker.
enum class Mode : uint8_t { Inline, RenderThread, Worker };
#if defined(NFSU_OFFLOAD_CORE) && NFSU_OFFLOAD_CORE == 1
constexpr Mode CoreMode = Mode::RenderThread;
#elif defined(NFSU_OFFLOAD_CORE) && NFSU_OFFLOAD_CORE == 2
constexpr Mode CoreMode = Mode::Worker;
#else
constexpr Mode CoreMode = Mode::Inline;
#endif

enum JobState : uint32_t { JobIdle, JobQueued, JobRunning, JobDone };

struct Context : win32::d3d9::bridge::SegmentRef
{
    uint8_t     mem[totalBytes()];
    reg32       counters[15];           // the counter range as snapshot
    Chain       entries, buckets, strips;
    uint16_t    lists[ListCount];       // detached lists in order
    reg32       listCount = 0;
    reg32       heads = 0;              // guest address of the detached heads
    reg32       stackTop = 0;
    reg32       viewObject = 0, viewKind = 0, viewIndex = 0;    // the current view object and its first two words
    ViewCopy    views[2];               // the current view's block and the headlight view's (9)
    bool        pending = false;        // a segment holds this job
    reg32       lightCount = 0;
    LightCopy   lights[LightSlots];
    uint8_t     lightSlot[128];         // by hash of the address: index + 1
    x86::CPU    cpu;
    win32::MemMap* block = nullptr;        // the detached heads in guest memory
    std::atomic<uint32_t> state{JobIdle};  // queued jobs: run by one thread, then done
    win32::WinApplication* app = nullptr;
    uint32_t    frame = 0;              // frame index at the marker
    bool        ok = true;              // the flush ran to its end
    uint32_t    fpscr = 0;              // the game thread's, set on the thread that runs the job
    void      (*run)(Context&) = nullptr;   // the flush on cpu, then the segment's publish

    // Marker: clears the light copies.
    void beginSnapshot();
    // Copies a light context; false: no room.
    bool addLight(reg32 address);
    // Snapshot of the globals, the parent's CPU, an empty stack.
    void open(const x86::CPU& parent);
    // The 64 bytes read at a guest address with the pool header words the frees so far changed.
    void overlayPools(reg32 at, uint8_t* bytes) const
    {
        for (const Chain* chain : {&entries, &buckets, &strips})
            for (reg32 offset = 0x10; chain->count != 0 && offset < 0x1c; offset += 4)
            {
                const reg32 word = chain->pool + offset - at;
                if (word <= 60)
                {
                    const reg32 value = chain->header(offset);
                    memcpy(bytes + word, &value, 4);
                }
            }
    }
    // Counter deltas added to the guest counters.
    void writeBack();
    // Chained frees returned to the pools.
    void giveBack();
    // Check mode: the guest ranges are overwritten, then restored.
    void poison();
    void unpoison();

    uint8_t saved[totalBytes()];
    uint8_t savedSources[PoisonBytes];
    const LightCopy* findLight(reg32 address) const
    {
        for (reg32 h = (address >> 6 ^ address >> 13) & 127; lightSlot[h]; h = (h + 1) & 127)
            if (lights[lightSlot[h] - 1].address == address)
                return &lights[lightSlot[h] - 1];
        return nullptr;
    }
};

// Moves a list's buckets under a new sentinel.
void detachList(reg32 head, reg32 detached);

// Running jobs: a worker thread (Mode::Worker), or whichever of the render thread's resolver and the game thread
// reaches the job first (Mode::RenderThread). Jobs run one at a time, in the order queued.
namespace queue
{
// Queued jobs run later; false: the game thread must not rely on them running by themselves (never with a worker).
void submit(Context& job);
// Returns when the job is done: runs it if nobody has (render thread mode), else waits. Any thread but the worker.
void await(void* job);
// Whether the job is done (acquire).
inline bool isDone(const Context& job) { return job.state.load(std::memory_order_acquire) == JobDone; }
// Microseconds waiting in await, jobs run and microseconds spent running them; read by the counters.
struct Totals { uint64_t waitedUs[2], waits[2], runs, busyUs; };
Totals totals();
}

// Jobs pending now and the most since the last call, game thread; microseconds the game thread waited at joins and
// at fences since the start.
struct Pending { uint32_t now, peak; uint64_t joinWaitUs, fenceWaitUs; };
Pending pendingStats();
// Closes done jobs without waiting, game thread.
void closeDone();
// Waits for every job and closes it, game thread; fence: waits are counted as fence waits.
void fence();
// Fence before guest data a pending flush reads is unloaded, game thread.
void unloadFence(const char* what);

inline Context* context(const x86::CPU& cpu)
{
    return static_cast<Context*>(cpu.og);
}

template< reg32 A, typename T > inline T load(const x86::CPU& cpu)
{
    constexpr int o = find(A, sizeof(T));
    if constexpr (o >= 0)
    {
        if (const Context* c = context(cpu))
        {
            T v;
            memcpy(&v, c->mem + o, sizeof(T));
            return v;
        }
    }
    return rc::load<T>(A);
}

template< reg32 A, typename T > inline void store(const x86::CPU& cpu, T value)
{
    constexpr int o = find(A, sizeof(T));
    if constexpr (o >= 0)
    {
        if (Context* c = context(cpu))
        {
            memcpy(c->mem + o, &value, sizeof(T));
            return;
        }
    }
    rc::store(A, value);
}

template< reg32 A > inline reg32 ld32(const x86::CPU& cpu) { return load<A, reg32>(cpu); }
template< reg32 A > inline x86::reg8 ld8(const x86::CPU& cpu) { return load<A, x86::reg8>(cpu); }
template< reg32 A > inline float ldf32(const x86::CPU& cpu) { return load<A, float>(cpu); }
template< reg32 A > inline void st32(const x86::CPU& cpu, reg32 value) { store<A, reg32>(cpu, value); }

// Address computed at run time.
inline reg32 ld32(const x86::CPU& cpu, reg32 address)
{
    if (const Context* c = context(cpu))
    {
        const int o = find(address, 4);
        if (o >= 0)
        {
            reg32 v;
            memcpy(&v, c->mem + o, 4);
            return v;
        }
    }
    return rc::ld32(address);
}

// The current view object's first two words.
inline reg32 viewKind(const x86::CPU& cpu)
{
    const Context* c = context(cpu);
    return c ? c->viewKind : rc::ld32(rc::ld32(0x870848));
}

inline reg32 viewIndex(const x86::CPU& cpu)
{
    const Context* c = context(cpu);
    return c ? c->viewIndex : rc::ld32(rc::ld32(0x870848) + 4);
}

// Matrices of the view block at an address.
inline const float* viewMatricesAt(const x86::CPU& cpu, reg32 block)
{
    if (const Context* c = context(cpu))
        for (const ViewCopy& v : c->views)
            if (v.used && ViewBlocks + v.view * ViewBlockSize == block)
                return v.m;
    return reinterpret_cast<const float*>(rc::host(rc::ld32(block)));
}

inline const float* viewMatrices(const x86::CPU& cpu, reg32 view)
{
    return viewMatricesAt(cpu, ViewBlocks + view * ViewBlockSize);
}

// A light context's vector and array.
inline const float* lightVector(const x86::CPU& cpu, reg32 light)
{
    if (const Context* c = context(cpu))
        if (const LightCopy* l = c->findLight(light))
            return l->vector;
    return reinterpret_cast<const float*>(rc::host(light + 0xc0));
}

inline const float* lightArray(const x86::CPU& cpu, reg32 light)
{
    if (const Context* c = context(cpu))
        if (const LightCopy* l = c->findLight(light))
            return l->array;
    return reinterpret_cast<const float*>(rc::host(light + 0x120));
}

// Host pointer of the words, in the context if held.
template< reg32 A > inline const void* hostPointer(const x86::CPU& cpu)
{
    constexpr int o = find(A, 4);
    if constexpr (o >= 0)
    {
        if (const Context* c = context(cpu))
            return c->mem + o;
    }
    return rc::host(A);
}

}}}

#endif /* !UG2_NATIVE_FLUSH_CONTEXT_H_ */
