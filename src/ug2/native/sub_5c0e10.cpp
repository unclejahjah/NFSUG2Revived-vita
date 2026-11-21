// Native sub_5c0e10: one particle into the batch (cdecl: particle record); the first of a batch binds matrices and
// texture and locks the buffer. The quad of sub_5bb430 is built inline.

#include <native/common.h>
#include <native/particle_batch.h>
#include <platform/switches.h>
#include <platform/config.h>

namespace ug2
{

void sub_5c0e10_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bdde0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bcc30(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bb320(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

// Batch start: the guest's frame and registers, for its three callees.
[[gnu::noinline]] bool particleBatchBegin(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 p)
{
    const x86::reg32 entry = cpu.esp;
    const x86::reg32 ebx = cpu.ebx, ebp = cpu.ebp, esi = cpu.esi, edi = cpu.edi;
    rc::st32(entry - 4, ebp);
    const x86::reg32 frame = ((entry - 4) & 0xfffffff0u) - 0x64 - 12;
    cpu.ebp = entry - 4;
    cpu.esi = p;
    cpu.edi = rc::ld32(p + 8);
    cpu.ebx = rc::ld32(p + 4);
    rc::st8(0x872b98, 1);
    rc::st32(frame - 4, rc::ld32(rc::ld32(p) + 4));
    rc::st32(frame - 8, 0x872e90);
    cpu.esp = frame - 12;
    sub_5bdde0(app, cpu);
    if (cpu.terminate)
        return false;
    rc::st32(frame - 12, cpu.ebx);
    cpu.esp = frame - 16;
    sub_5bcc30(app, cpu);
    if (cpu.terminate)
        return false;
    rc::st32(frame + 0xc, cpu.edi);
    rc::st32(frame - 4, frame + 0xc);
    cpu.ecx = 0x86b7a4;
    cpu.esp = frame - 8;
    sub_5bb320(app, cpu);
    if (cpu.terminate)
        return false;
    rc::st32(0x87074c, 1);
    cpu.ebx = ebx;
    cpu.ebp = ebp;
    cpu.esi = esi;
    cpu.edi = edi;
    cpu.esp = entry;
    return true;
}

namespace
{

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("particles_native"))
    {
        sub_5c0e10_guest(app, cpu);
        return;
    }
    const x86::reg32 p = argument(cpu, 0);
    if (rc::ld32(0x87074c) == 0 && !particleBatchBegin(app, cpu, p))
        return;

    batch::Quad q;
    q.x = ld(p + 0x10);
    q.y = ld(p + 0x14);
    q.z = ld(p + 0x18);
    q.width = ld(p + 0x30);
    q.height = ld(p + 0x34);
    q.alpha = ld(p + 0x2c);
    q.red = ld(p + 0x20);
    q.green = ld(p + 0x24);
    q.blue = ld(p + 0x28);
    q.s0 = rc::ld32(p + 0x48);
    q.t0 = rc::ld32(p + 0x4c);
    q.s1 = rc::ld32(p + 0x40);
    q.t1 = rc::ld32(p + 0x44);
    // The record is on 0x48C290's stack; its next node is at record - 0x230 + 0xE8.
    cpu.ecx = batch::append(q, batch::thinning(), [p] { return rc::ld32(p - 0x230 + 0xe8); });
    cpu.eax = 1;
    cpu.esp += 4;
}

}

// No shadow compare: the batch start locks the vertex buffer: a second run would lock it again.
void sub_5c0e10(win32::WinApplication* app, x86::CPU& cpu)
{
    body(app, cpu);
}

}}
