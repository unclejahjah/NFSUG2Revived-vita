#include <platform/log.h>
#include <platform/crashlog.h>
#include <platform/perf.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace pf
{

// s_logPath is read and cleared under s_logMutex, which stays until the process exits so that no logger can use a
// deleted mutex. The path is copied: callers may pass a temporary.
static char        s_logPathBuffer[256];
static const char* s_logPath = nullptr;
static SceUID      s_logMutex = -1;
static bool        s_console = false;
static bool        s_info = true;

// Deferred lines (logSetDeferred): kept here under s_logMutex and written by the writer thread. s_fileMutex is held
// around file writes and taken before s_logMutex.
static const unsigned PendingBytes = 64 * 1024;
static char        s_pending[PendingBytes];
static char        s_writing[PendingBytes];
static unsigned    s_pendingBytes = 0;
static bool        s_deferred = false;
static SceUID      s_fileMutex = -1;
static SceUID      s_writerSema = -1;

// Appends with its own open and close.
static void appendFile(const char* text, unsigned length)
{
    if (!s_logPath || !length)
        return;
    int file = open(s_logPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (file >= 0)
    {
        write(file, text, length);
        close(file);
    }
}

// Writes the deferred lines; s_fileMutex held.
static void writePending()
{
    sceKernelLockMutex(s_logMutex, 1, nullptr);
    const unsigned length = s_pendingBytes;
    memcpy(s_writing, s_pending, length);
    s_pendingBytes = 0;
    sceKernelUnlockMutex(s_logMutex, 1);
    appendFile(s_writing, length);
}

static int writerThread(SceSize, void*)
{
    for (;;)
    {
        sceKernelWaitSema(s_writerSema, 1, nullptr);
        sceKernelLockMutex(s_fileMutex, 1, nullptr);
        writePending();
        sceKernelUnlockMutex(s_fileMutex, 1);
    }
    return 0;
}

// Queues text for the writer thread; false when full.
static bool defer(const char* text, unsigned length)
{
    sceKernelLockMutex(s_logMutex, 1, nullptr);
    const bool fits = s_pendingBytes + length <= PendingBytes;
    if (fits)
    {
        memcpy(s_pending + s_pendingBytes, text, length);
        s_pendingBytes += length;
    }
    sceKernelUnlockMutex(s_logMutex, 1);
    if (fits)
        sceKernelSignalSema(s_writerSema, 1);
    return fits;
}

// Writes or defers text; now: never deferred.
static void output(const char* text, unsigned length, bool now)
{
    const bool timed = perf::hitch();
    const uint32_t start = timed ? perf::now() : 0;
    if (s_console)
        sceClibPrintf("%.*s", int(length), text);
    if (s_deferred)
    {
        if (now || !defer(text, length))
        {
            sceKernelLockMutex(s_fileMutex, 1, nullptr);
            writePending();
            appendFile(text, length);
            sceKernelUnlockMutex(s_fileMutex, 1);
        }
    }
    else
    {
        if (s_logMutex >= 0)
            sceKernelLockMutex(s_logMutex, 1, nullptr);
        appendFile(text, length);
        if (s_logMutex >= 0)
            sceKernelUnlockMutex(s_logMutex, 1);
    }
    if (timed)
    {
        perf::event(perf::EventLogWrites, 1);
        perf::event(perf::EventLogWriteUs, perf::now() - start);
    }
}

// level null: an info line while log_info is off, kept only in the crash ring.
static void writeLine(const char* level, const char* fmt, va_list args, bool now)
{
    char buffer[1024];
    int prefix = snprintf(buffer, sizeof(buffer), "[%s] ", level ? level : "info");
    int len = vsnprintf(buffer + prefix, sizeof(buffer) - prefix - 1, fmt, args);
    if (len < 0)
        return;
    len += prefix;
    if (len > int(sizeof(buffer)) - 2)
        len = int(sizeof(buffer)) - 2;
    buffer[len++] = '\n';
    buffer[len] = 0;
    crashlog::record(buffer, unsigned(len));
    if (level)
        output(buffer, unsigned(len), now);
}

void logInit(const char* logFilePath)
{
    s_logMutex = sceKernelCreateMutex("log",0, 0, nullptr);
    if (logFilePath)
    {
        snprintf(s_logPathBuffer, sizeof(s_logPathBuffer), "%s", logFilePath);
        s_logPath = s_logPathBuffer;
    }
    if (s_logPath)
    {
        int file = open(s_logPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (file >= 0)
            close(file);
    }
}

void logSetInfo(bool enabled)
{
    s_info = enabled;
}

void logSetConsole(bool enabled)
{
    s_console = enabled;
}

void logSetDeferred(bool enabled)
{
    if (enabled && s_writerSema < 0)
    {
        s_fileMutex = sceKernelCreateMutex("log_file", 0, 0, nullptr);
        s_writerSema = sceKernelCreateSema("log_writer", 0, 0, 0x7fffffff, nullptr);
        const SceUID thread = sceKernelCreateThread("log_writer", writerThread, 0x10000100, 0x4000, 0, 0, nullptr);
        if (s_fileMutex < 0 || s_writerSema < 0 || thread < 0 || sceKernelStartThread(thread, 0, nullptr) < 0)
        {
            s_writerSema = -1;
            return;
        }
    }
    if (s_writerSema < 0)
        return;
    if (!enabled)
        logFlush();
    s_deferred = enabled;
}

void logFlush()
{
    if (!s_deferred)
        return;
    sceKernelLockMutex(s_fileMutex, 1, nullptr);
    writePending();
    sceKernelUnlockMutex(s_fileMutex, 1);
}

void logShutdown()
{
    logFlush();
    if (s_logMutex >= 0)
        sceKernelLockMutex(s_logMutex, 1, nullptr);
    s_logPath = nullptr;
    if (s_logMutex >= 0)
        sceKernelUnlockMutex(s_logMutex, 1);
}

void logDebug(const char* fmt, ...)
{
#ifdef NFSU_LOG_DEBUG
    va_list args;
    va_start(args, fmt);
    writeLine("debug", fmt, args, false);
    va_end(args);
#else
    (void)fmt;
#endif
}

void logInfo(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine(s_info ? "info" : nullptr, fmt, args, false);
    va_end(args);
}

void logBlock(const char* text, unsigned length)
{
    crashlog::record(text, length);
    if (length)
        output(text, length, false);
}

void logBlockUnlocked(const char* text, unsigned length)
{
    if (!length || !s_logPathBuffer[0])
        return;
    crashlog::record(text, length);
    const SceUID file = sceIoOpen(s_logPathBuffer, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (file >= 0)
    {
        sceIoWrite(file, text, length);
        sceIoClose(file);
    }
}

int logMutexId()
{
    return s_logMutex;
}

const char* logFilePath()
{
    return s_logPathBuffer;
}

void logError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("error", fmt, args, true);
    va_end(args);
}

void fatal(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("fatal", fmt, args, true);
    va_end(args);
    char reason[256];
    va_start(args, fmt);
    vsnprintf(reason, sizeof(reason), fmt, args);
    va_end(args);
    crashlog::report(reason);
    logShutdown();
    sceKernelExitProcess(1);
    for (;;) {}
}

}
