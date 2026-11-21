#ifndef UG2_DRIVE_H_
#define UG2_DRIVE_H_

// Automated driver for test runs, a development tool: compiled in only with DEV_TOOLS, a no-op otherwise.

#ifdef DEV_TOOLS
#include "../dev/drive.h"
#else
#include <cstddef>
#include <cstdint>

namespace ug2 { namespace drive {

const bool available = false;

inline void configure(uint32_t) {}
inline void start() {}
inline void stop() {}
inline void describe(char*, size_t) {}

}}
#endif

#endif /* !UG2_DRIVE_H_ */
