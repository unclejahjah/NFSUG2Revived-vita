#ifndef UG2_AUDIO_STATS_H_
#define UG2_AUDIO_STATS_H_

#include <lib/winapp.h>
#include <x86.h>

// Mixer thread counters (dev_mixer_stats), a development tool: compiled in only with DEV_TOOLS, no-ops otherwise.
namespace ug2 { namespace mixstats
{

enum Scope
{
    Loop, Tick, Callbacks, Notifications, Voices, Aux, Bank, Mix, Convert, Resample, Eaxa, Modules,
};

}}

#ifdef DEV_TOOLS
#include "../dev/audio_stats.h"
#else
namespace ug2 { namespace mixstats
{

inline bool on() { return false; }
inline void configure(win32::WinApplication&) {}
inline void enter(const x86::CPU&, int) {}
inline void leave(const x86::CPU&) {}
inline void loopEnter(const x86::CPU&, x86::reg32) {}
inline void loopLeave(const x86::CPU&) {}
inline void voicesEnter(const x86::CPU&, x86::reg32) {}
inline void voicesLeave(const x86::CPU&) {}
inline void mixCall(const x86::CPU&, x86::reg32, float, x86::reg32) {}
inline void work(const x86::CPU&, int, uint32_t, uint32_t = 0) {}
inline void bankCall(const x86::CPU&, x86::reg32) {}
inline void bankRun(const x86::CPU&, const char*, bool) {}

class Scoped
{
public:
    Scoped(const x86::CPU&, int) {}
};

}}
#endif

#endif /* !UG2_AUDIO_STATS_H_ */
