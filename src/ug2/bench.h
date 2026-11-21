#ifndef UG2_BENCH_H_
#define UG2_BENCH_H_

// Boot race benchmark figures, a development tool: compiled in only with DEV_TOOLS, a no-op otherwise.

#ifdef DEV_TOOLS
#include "../dev/bench.h"
#else
#include <cstddef>

namespace ug2 { namespace bench {

typedef void (*WindowNote)(char* text, size_t size);

inline void configure(int, int) {}
inline void setWindowNote(WindowNote) {}
inline void raceStarted() {}
inline void raceEnded(const char*) {}

}}
#endif

#endif /* !UG2_BENCH_H_ */
