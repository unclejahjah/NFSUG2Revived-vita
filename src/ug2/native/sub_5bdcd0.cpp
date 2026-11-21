// Native sub_5bdcd0: copies the 16 bone matrices of a strip and uploads them as the effect's matrix array (cdecl: palette, strip).

#include <native/common.h>
#include <native/flush_context.h>
#include <platform/switches.h>
#include <winapi/d3d9_native.h>
#include <winapi/d3d9_probe.h>

namespace ug2
{

void sub_5bdcd0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_5bdcd0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("sub_5bdcd0_native")) [[unlikely]]
    {
        sub_5bdcd0_guest(app, cpu);
        return;
    }
    const x86::reg32 palette = argument(cpu, 0);
    const x86::reg32 indices = argument(cpu, 1) + 0x21;
    float matrices[16 * 16];
    for (x86::reg32 i = 0; i < 16; ++i)
        memcpy(matrices + 16 * i, rc::host(palette + (x86::reg32(rc::ld8(indices + i)) << 6)), 64);
    if (const flush::Context* context = flush::context(cpu))
        for (x86::reg32 i = 0; i < 16; ++i)
            context->overlayPools(palette + (x86::reg32(rc::ld8(indices + i)) << 6), reinterpret_cast<uint8_t*>(matrices) + 64 * i);
#ifdef DEV_TOOLS
    if (win32::d3d9::probe::on())
    {
        bool alias = false;
        for (x86::reg32 pool : {rc::ld32(0x870918), rc::ld32(0x87091c), rc::ld32(0x870954)})
            for (x86::reg32 i = 0; i < 16; ++i)
            {
                const x86::reg32 at = palette + (x86::reg32(rc::ld8(indices + i)) << 6);
                alias = alias || (at < pool + 0x1c && at + 64 > pool + 0x10);
            }
        win32::d3d9::probe::count(win32::d3d9::probe::SkinnedCalls);
        win32::d3d9::probe::count(win32::d3d9::probe::SkinnedPoolAlias, alias);
    }
#endif
    const x86::reg32 group = flush::ld32<0x870910>(cpu);
    cpu.eax = win32::d3d9::bridge::effectSetMatrixArray(app, cpu, rc::ld32(group + 0x20),
                                                        rc::ld32(rc::ld32(group + 0x1c) + 0x114), matrices, 16);
    cpu.esp += 4;
}

}}
