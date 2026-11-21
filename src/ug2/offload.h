#ifndef UG2_OFFLOAD_H_
#define UG2_OFFLOAD_H_

// Offload counters (offload_count=on), a development tool: draws and game-thread time per category. A no-op
// without DEV_TOOLS. Offload segments (NFSU_OFFLOAD_CORE) are in sub_5cf420.cpp.

#include <cstdint>

namespace ug2 { namespace offload {

enum Category : uint8_t { CatOther, CatWorld, CatCrowd, CatCars, CatCount };
enum { Parts = 6 };

}}

#ifdef DEV_TOOLS
#include "../dev/offload_count.h"
#else
namespace ug2 { namespace offload {

inline bool counting() { return false; }
inline void configure() {}

struct Pass
{
    Pass(Category, uint32_t) {}
};

struct Direct {};

inline void submitted(uint32_t) {}
inline void flushBegin() {}
inline void flushEnd() {}
inline void listBegin() {}
inline void listEnd() {}
inline void bucketBegin(uint32_t) {}
inline void bucketEnd() {}
inline void entryBegin(uint32_t) {}
inline void entryEnd() {}
inline void issued(uint32_t) {}
inline void created() {}
inline void textureStep(bool, uint32_t, uint32_t) {}
inline void matrixStep(bool, uint32_t, uint32_t, uint32_t) {}
inline void streamStep(bool, bool, uint32_t, uint32_t) {}
inline void boundary(bool) {}
inline void contextRun(bool) {}
inline uint64_t instructions() { return 0; }
inline void parts(const uint64_t*) {}
inline void logParts(uint64_t, uint64_t) {}

}}
#endif

#endif /* !UG2_OFFLOAD_H_ */
