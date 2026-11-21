// The launcher's log: a few lines in launcher.log beside the game's own log, written at once.

#include <platform/log.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace pf
{

namespace
{
FILE* s_file = nullptr;

void write(const char* level, const char* fmt, va_list args)
{
    if (!s_file)
        return;
    fputs(level, s_file);
    vfprintf(s_file, fmt, args);
    fputc('\n', s_file);
    fflush(s_file);
}
}

void logInit(const char* logFilePath)
{
    s_file = fopen(logFilePath, "w");
}

void logShutdown()
{
    if (s_file)
        fclose(s_file);
    s_file = nullptr;
}

void logInfo(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    write("[info] ", fmt, args);
    va_end(args);
}

void logError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    write("[error] ", fmt, args);
    va_end(args);
}

void logDebug(const char*, ...)
{
}

}
