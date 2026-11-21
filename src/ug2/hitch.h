#ifndef UG2_HITCH_H_
#define UG2_HITCH_H_

// Hitch log (hitch_log=on), a development tool: compiled in only with DEV_TOOLS, a no-op otherwise.

#ifdef DEV_TOOLS
#include "../dev/hitch.h"
#else
#include <lib/winapp.h>
#include <platform/perf.h>

namespace ug2 { namespace hitch {

inline bool enabled() { return false; }
inline void configure(win32::WinApplication&) {}

template <pf::perf::Event Calls, typename Body>
inline void timed(Body body)
{
    body();
}

}}
#endif

#endif /* !UG2_HITCH_H_ */
