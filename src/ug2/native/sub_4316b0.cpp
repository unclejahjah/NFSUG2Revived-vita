// Native sub_4316b0: per-frame update of an animated object. Calls three vtable updates, builds the pose (sub_6e3b10) and multiplies bone matrices (thiscall, ret 0).

#include <lib/guest.h>
#include <native/common.h>
#include <native/fast.h>
#include <native/matrix4.h>

namespace ug2
{

void sub_4316b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6e3b10(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

using x86::reg32;

signed char s_on, s_matrix;

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!fast::enabled(s_on, "anim_update_native") || !fast::enabled(s_matrix, "matrix4_native")) [[unlikely]]
    {
        sub_4316b0_guest(app, cpu);
        return;
    }
    const reg32 entryEsp = cpu.esp;
    const reg32 self = cpu.ecx;
    const reg32 below = entryEsp - 24;
    const reg32 skeleton = rc::ld32(rc::ld32(self + 0x20) + 0x24);
    const reg32 keyframes = rc::ld32(self + 0x2c);
    const reg32 time = bits(float(ld(self + 0x64)));
    const reg32 source = rc::ld32(self + 0x24);
    const reg32 savedEbx = cpu.ebx, savedEsi = cpu.esi, savedEdi = cpu.edi, savedEbp = cpu.ebp;
    cpu.ebx = time;
    cpu.esi = self;
    cpu.edi = source;
    cpu.ebp = skeleton;
    static const reg32 children[3] = {0x48, 0x4c, 0x44};
    for (reg32 offset : children)
    {
        const reg32 object = rc::ld32(self + offset);
        if (object == 0)
            continue;
        rc::st32(below + 12, 0);
        rc::st32(below + 8, source);
        rc::st32(below + 4, time);
        cpu.ecx = object;
        cpu.esp = below + 4;
        ::native::callGuest(app, cpu, rc::ld32(rc::ld32(object) + 0x18));
        if (cpu.terminate)
            return;
    }
    rc::st32(below + 12, 0);
    rc::st32(below + 8, keyframes);
    rc::st32(below + 4, source);
    cpu.ecx = skeleton;
    cpu.esp = below;
    sub_6e3b10(app, cpu);
    if (cpu.terminate)
        return;
    if (rc::ld8(self + 0x34) & 1)
    {
        const reg32 bones = rc::ld32(rc::ld32(self + 0x20) + 0x24);
        const int32_t count = int32_t(rc::ld32(bones + 8));
        reg32 destination = rc::ld32(self + 0x2c);
        reg32 pose = bones + 0x40;
        for (int32_t i = 0; i < count; ++i)
        {
            matrix4::multiply(rc::host(pose), rc::host(destination), rc::host(destination));
            destination += 0x40;
            pose += 0x70;
        }
    }
    cpu.eax = 0;
    cpu.ebx = savedEbx;
    cpu.esi = savedEsi;
    cpu.edi = savedEdi;
    cpu.ebp = savedEbp;
    cpu.esp = entryEsp + 4;
}

}

void sub_4316b0(win32::WinApplication* app, x86::CPU& cpu)
{
    body(app, cpu); // not shadow-checked: its callees update game state, a second run would repeat that
}

}}
