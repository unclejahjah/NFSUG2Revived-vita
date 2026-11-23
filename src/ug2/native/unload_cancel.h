#ifndef UG2_NATIVE_UNLOAD_CANCEL_H_
#define UG2_NATIVE_UNLOAD_CANCEL_H_

// Guest ranges an unload is freeing, for the flush worker: queued draws that point into one are skipped, and the
// unloading thread waits out the one draw step in progress. No game or platform dependencies.

#include <atomic>
#include <cstdint>

namespace ug2 { namespace native { namespace unload {

const uint32_t MaxRanges = 64;
const uint32_t JoinGap = 256;   // ranges of one notice this close are one range
const uint32_t LowestPointer = 0x10000;     // below it: a null or an offset, never unloaded memory

struct Stats
{
    uint32_t notices, joined, full, retired, steps, skipped;
};

// Which pointer of a step is unloaded, and by which notice (tag: the caller's kind).
struct Hit
{
    uint32_t index, base, size, tag;
};

// Any thread; serial: the last job queued. False: no room.
bool publish(uint32_t base, uint32_t size, uint32_t serial, uint32_t tag = 0);
// Drops notices older than every pending job; oldest: ~0u when none pend.
void retire(uint32_t oldestPending);
// Ranges live now (relaxed).
uint32_t count();
// Worker: a draw step begins; true: a pointer is unloaded for this job.
bool begin(uint32_t jobSerial, const uint32_t* pointers, uint32_t n, Hit* hit = nullptr);
// Worker, inside a step: the same test for pointers read later.
bool dead(uint32_t jobSerial, const uint32_t* pointers, uint32_t n, Hit* hit = nullptr);
// Worker: the step ends; skipped counts it as a cancelled draw.
void end(bool skipped = false);
// Odd while the worker is inside a step.
uint32_t step();
Stats stats();
// Tests: forgets everything.
void reset();

}}}

#endif /* !UG2_NATIVE_UNLOAD_CANCEL_H_ */
