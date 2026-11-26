// The Vita's file lock: the PS button locked through the shell and a thread that keeps the auto-suspend timer reset.

#include "file_lock.h"
#include <platform/log.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/shellutil.h>
#include <atomic>
#endif

namespace ug2 { namespace launcher {

namespace
{

#ifdef __vita__
const int TickEveryUs = 1000000;
const int SliceUs = 100000;

class VitaLock : public FileLock
{
public:
    void take() override
    {
        if (m_thread >= 0)
            return;
        if (!m_events)
            m_events = sceShellUtilInitEvents(0) >= 0;
        const int locked = sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN);
        if (locked < 0)
            pf::logError("launcher: sceShellUtilLock returned 0x%08x", unsigned(locked));
        m_running = true;
        m_thread = sceKernelCreateThread("mods_power_tick", ticks, 0x10000100, 0x4000, 0, 0, nullptr);
        if (m_thread < 0 || sceKernelStartThread(m_thread, sizeof(this), &m_self) < 0)
            pf::logError("launcher: the power tick thread did not start (0x%08x)", unsigned(m_thread));
        pf::logInfo("launcher: file lock taken");
    }

    void release() override
    {
        if (m_thread < 0)
            return;
        m_running = false;
        sceKernelWaitThreadEnd(m_thread, nullptr, nullptr);
        sceKernelDeleteThread(m_thread);
        m_thread = -1;
        sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN);
        pf::logInfo("launcher: file lock released");
    }

private:
    static int ticks(SceSize, void* argument)
    {
        const VitaLock& lock = **static_cast<VitaLock* const*>(argument);
        for (int waited = TickEveryUs; lock.m_running.load(); waited += SliceUs)
        {
            if (waited >= TickEveryUs)
            {
                sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
                waited = 0;
            }
            sceKernelDelayThread(SliceUs);
        }
        return 0;
    }

    VitaLock* m_self = this;
    SceUID m_thread = -1;
    std::atomic<bool> m_running{ false };
    bool m_events = false;
};

VitaLock s_lock;
#else
class HostLock : public FileLock
{
public:
    void take() override {}
    void release() override {}
};

HostLock s_lock;
#endif

}

FileLock& systemLock()
{
    return s_lock;
}

}}
