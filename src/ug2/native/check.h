#ifndef UG2_NATIVE_CHECK_H_
#define UG2_NATIVE_CHECK_H_

// Native-versus-guest check in any build (phys_native_check=on): both bodies run from one state, the guest outcome is
// kept, and differences in registers, x87 depth and declared spans are counted.

#include <native/shadow.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <platform/switches.h>

#include <atomic>

namespace ug2 { namespace native { namespace check {

struct Stats
{
    const char* name;
    std::atomic<uint32_t> compared{0};
    std::atomic<uint32_t> mismatches{0};
    std::atomic<uint32_t> details{0};
};

// True with phys_native_check=on.
inline bool enabled()
{
    return NFSU_DEV_BOOL_CACHED("phys_native_check", false);
}

void compare(Stats& stats, x86::reg8 registers, win32::WinApplication* app, x86::CPU& cpu, shadow::Body native,
             shadow::Body guest, shadow::Declare declare);

// Logs the totals of every check that ran.
void report();

}}}

#endif /* !UG2_NATIVE_CHECK_H_ */
