#ifndef PLATFORM_CRASHLOG_H_
#define PLATFORM_CRASHLOG_H_

// Crash reports (crash_report): what is needed to find a crash afterwards, in <data folder>/crashes.
// - Every log line, info lines included when log_info is off, also goes to a 64 KB ring in memory. The system crash
//   dump (ux0:data/psp2core-*.psp2dmp) holds it; tools/vita/crashdump.py finds and prints it.
// - With kubridge.skprx (ur0:tai/config.txt *KERNEL), a CPU exception in any thread writes
//   crashes/crash-<date>-<time>.txt at once: what failed, registers, the code addresses on the stack, the last log
//   lines. The system then ends the process as before, with its dump. fatal() and std::terminate write one too.
// - A freeze (stall_report) writes crashes/freeze-<date>-<time>.txt with the stall reports and the last log lines.
// - Each start keeps the last run's log as NeedForSpeedUnderground2.previous.log. When the last run crashed (a report
//   or a system dump newer than its start), its log is also copied beside the report and a notice says so.

#include <cstdint>
#include <string>
#include <vector>

namespace pf { namespace crashlog
{

// Before logInit: keeps the last run's log, looks at how the last run ended and marks this one as running.
void begin(const char* dataFolder, const char* logPath, const char* version);
// After Config::load: the exception handlers, when crash_report is on. False without kubridge.
bool install();
// The notice lines about the last run (empty when it did not crash); begin() fills them.
const std::vector<std::string>& notices();
// Every log line, as written.
void record(const char* text, unsigned length);
// The reason of the next trap (guestTrap), shown in its report.
void noteTrap(uint32_t guestAddress, const char* reason);
// Writes a report now, for an end that is no CPU exception (fatal, std::terminate).
void report(const char* reason);
// The stall reporter found no frame for a while: writes (and on later calls rewrites) crashes/freeze-<stamp>.txt
// with the last log lines, its report among them.
void freeze(const char* what);
// The run ended through the normal exit.
void end();

}}

#endif
