// Native sub_4901d0: texture info of a name hash from the texture sets or the default table (cdecl: hash, or-default, by-table).

#include <native/common.h>
#include <native/texture_lookup.h>
#include <platform/switches.h>

namespace ug2
{

void sub_4901d0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_4901d0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("sub_4901d0_native")) [[unlikely]]
    {
        sub_4901d0_guest(app, cpu);
        return;
    }
    cpu.eax = texture::find(argument(cpu, 0), argument(cpu, 1), argument(cpu, 2));
    cpu.esp += 4;
}

}}
