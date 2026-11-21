// Dead ranges of unloads in progress: a short list under a spin flag, an atomic count for the worker's fast path.

#include <native/unload_cancel.h>

#include <initializer_list>

namespace ug2 { namespace native { namespace unload {

namespace
{

struct Range
{
    uint32_t base, size, serial, tag;
};

Range s_ranges[MaxRanges];
uint32_t s_used = 0;
std::atomic<uint32_t> s_count{0};
std::atomic<uint32_t> s_step{0};
std::atomic_flag s_lock = ATOMIC_FLAG_INIT;
std::atomic<uint32_t> s_notices{0}, s_joined{0}, s_full{0}, s_retired{0}, s_steps{0}, s_skipped{0};

struct Guard
{
    Guard() { while (s_lock.test_and_set(std::memory_order_acquire)) {} }
    ~Guard() { s_lock.clear(std::memory_order_release); }
};

inline bool inside(const Range& r, uint32_t pointer)
{
    return pointer - r.base < r.size;
}

bool scan(uint32_t jobSerial, const uint32_t* pointers, uint32_t n, Hit* hit)
{
    const Guard guard;
    for (uint32_t i = 0; i < s_used; ++i)
    {
        const Range& r = s_ranges[i];
        if (r.serial < jobSerial)
            continue;
        for (uint32_t p = 0; p < n; ++p)
            if (pointers[p] >= LowestPointer && inside(r, pointers[p]))
            {
                if (hit)
                    *hit = Hit{p, r.base, r.size, r.tag};
                return true;
            }
    }
    return false;
}

}

bool publish(uint32_t base, uint32_t size, uint32_t serial, uint32_t tag)
{
    if (size == 0)
        return true;
    if (base < LowestPointer)
        return false;
    const Guard guard;
    s_notices.fetch_add(1, std::memory_order_relaxed);
    const uint64_t end = uint64_t(base) + size;
    for (uint32_t i = 0; i < s_used; ++i)
    {
        Range& r = s_ranges[i];
        const uint64_t rEnd = uint64_t(r.base) + r.size;
        if (r.serial != serial || r.tag != tag || base > rEnd + JoinGap || end + JoinGap < r.base)
            continue;
        const uint32_t low = base < r.base ? base : r.base;
        const uint64_t high = end > rEnd ? end : rEnd;
        r.base = low;
        r.size = uint32_t(high - low);
        s_joined.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (s_used == MaxRanges)
    {
        s_full.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    s_ranges[s_used++] = Range{base, size, serial, tag};
    s_count.store(s_used, std::memory_order_seq_cst);
    return true;
}

void retire(uint32_t oldestPending)
{
    if (s_count.load(std::memory_order_relaxed) == 0)
        return;
    const Guard guard;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < s_used; ++i)
        if (s_ranges[i].serial >= oldestPending)
            s_ranges[kept++] = s_ranges[i];
    s_retired.fetch_add(s_used - kept, std::memory_order_relaxed);
    s_used = kept;
    s_count.store(kept, std::memory_order_seq_cst);
}

uint32_t count()
{
    return s_count.load(std::memory_order_relaxed);
}

bool begin(uint32_t jobSerial, const uint32_t* pointers, uint32_t n, Hit* hit)
{
    s_step.store(s_step.load(std::memory_order_relaxed) + 1, std::memory_order_seq_cst);
    if (__builtin_expect(s_count.load(std::memory_order_seq_cst) == 0, 1))
        return false;
    s_steps.fetch_add(1, std::memory_order_relaxed);
    return scan(jobSerial, pointers, n, hit);
}

bool dead(uint32_t jobSerial, const uint32_t* pointers, uint32_t n, Hit* hit)
{
    if (__builtin_expect(s_count.load(std::memory_order_seq_cst) == 0, 1))
        return false;
    return scan(jobSerial, pointers, n, hit);
}

void end(bool skipped)
{
    if (skipped)
        s_skipped.fetch_add(1, std::memory_order_relaxed);
    s_step.store(s_step.load(std::memory_order_relaxed) + 1, std::memory_order_seq_cst);
}

uint32_t step()
{
    return s_step.load(std::memory_order_seq_cst);
}

Stats stats()
{
    return Stats{ s_notices.load(), s_joined.load(), s_full.load(), s_retired.load(), s_steps.load(), s_skipped.load() };
}

void reset()
{
    const Guard guard;
    s_used = 0;
    s_count.store(0);
    s_step.store(0);
    for (std::atomic<uint32_t>* a : {&s_notices, &s_joined, &s_full, &s_retired, &s_steps, &s_skipped})
        a->store(0);
}

}}}
