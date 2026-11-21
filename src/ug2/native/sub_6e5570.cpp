// Native sub_6e5570: samples an animation at a time (thiscall, ret 12: time, output, flag). Copies the track values at an exact key, else calls sub_6efe40 to interpolate.

#include <native/anim.h>
#include <native/common.h>
#include <native/fast.h>

namespace ug2
{

void sub_6e5570_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6e4d90(win32::WinApplication* app, x86::CPU& cpu);
void sub_6efa20(win32::WinApplication* app, x86::CPU& cpu);
void sub_6efe40(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

using x86::reg32;

signed char s_on;

struct Frame
{
    win32::WinApplication* app;
    x86::CPU& cpu;
    reg32 below;
};

// Thiscall to guest, arguments below entry esp.
bool call(Frame& f, void (*function)(win32::WinApplication*, x86::CPU&), reg32 object, std::initializer_list<reg32> args)
{
    reg32 at = f.below + 4;
    for (reg32 a : args)
    {
        rc::st32(at, a);
        at += 4;
    }
    f.cpu.ecx = object;
    f.cpu.esp = f.below;
    function(f.app, f.cpu);
    return !f.cpu.terminate;
}

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!fast::enabled(s_on, "anim_sample_native")) [[unlikely]]
    {
        sub_6e5570_guest(app, cpu);
        return;
    }
    const reg32 entryEsp = cpu.esp;
    const reg32 self = cpu.ecx;
    const reg32 timeBits = argument(cpu, 0), output = argument(cpu, 1), flag = argument(cpu, 2);
    Frame frame = {app, cpu, entryEsp - 64};
    const reg32 savedEbx = cpu.ebx, savedEsi = cpu.esi, savedEdi = cpu.edi, savedEbp = cpu.ebp;
    cpu.esi = self;
    if (flag != 0)
    {
        if (!call(frame, sub_6e4d90, self, {timeBits, output, flag}))
            return;
        cpu.esi = savedEsi;
        cpu.esp = entryEsp + 16;
        return;
    }
    if (rc::ld32(self + 0x20) != 0)
    {
        rc::st32(self + 0x10, 0xffffffff);
        rc::st32(self + 0x20, 0);
    }
    const reg32 object = rc::ld32(self + 0xc);
    const reg32 count = rc::ld16(object + 8);
    const reg32 header = rc::ld32(object + 4);
    const reg32 words = rc::ld16(header);
    const reg32 indices = object + 0xa;
    float time;
    memcpy(&time, &timeBits, 4);
    const reg32 key = anim::timeIndex(cpu, time);
    const reg32 objectAgain = rc::ld32(self + 0xc);
    const reg32 headerAgain = rc::ld32(objectAgain + 4);
    const reg32 countAgain = rc::ld16(objectAgain + 8);
    reg32 clamped = key;
    if (int32_t(key) >= int32_t(countAgain))
        clamped = countAgain - 1;
    else if (int32_t(key) < 0)
        clamped = 0;
    cpu.ebx = clamped;
    cpu.edi = key;
    cpu.ebp = header;
    const reg32 data = rc::ld32(self + 0x14);
    if (!call(frame, sub_6efa20, headerAgain, {0, rc::ld16(headerAgain), rc::ld32(self + 0x10), clamped, data, data}))
        return;
    rc::st32(self + 0x10, clamped);
    const float keyFloat = float(int32_t(key));
    cpu.fpu.compare(double(Real(time)), double(Real(keyFloat)));
    const bool exact = __builtin_parity((cpu.fpu.status.word >> 8) & 0x44) != 0;
    reg32 eax;
    if (!exact && int32_t(key + 1) < int32_t(count))
    {
        Real fraction = Real(time);
        fraction -= Real(keyFloat);
        const reg32 fractionBits = bits(float(fraction));
        if (!call(frame, sub_6efe40, header, {0, words, key, key + 1, rc::ld32(self + 0x14), output, 4, indices, fractionBits}))
            return;
        eax = (cpu.eax & ~0xffu) | 1;
    }
    else
    {
        reg32 j = 0;
        if (int32_t(words) > 0)
        {
            reg32 index = indices;
            do
            {
                const reg32 destination = output + reg32(rc::ld16(index)) * 4;
                const reg32 source = rc::ld32(self + 0x14) + j * 4;
                for (reg32 k = 0; k < 16; k += 4)
                    rc::st32(destination + k, rc::ld32(source + k));
                j += 4;
                index += 2;
            } while (int32_t(j) < int32_t(words));
        }
        eax = (j & ~0xffu) | 1;
    }
    cpu.eax = eax;
    cpu.ebx = savedEbx;
    cpu.esi = savedEsi;
    cpu.edi = savedEdi;
    cpu.ebp = savedEbp;
    cpu.esp = entryEsp + 16;
}

}

void sub_6e5570(win32::WinApplication* app, x86::CPU& cpu)
{
    body(app, cpu); // not shadow-checked: its callees update animation state, a second run would repeat that
}

}}
