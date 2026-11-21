// Native sub_5d6ad0: GrandSceneryCullInfo::StuffScenery, one view's culled scenery list filtered by pass flags and
// handed to the view's Render (thiscall; view, pass bits; ret 8).

#include <native/common.h>
#include <native/shadow.h>
#include <offload.h>
#include <platform/switches.h>
#include <platform/cached.h>
#include <platform/config.h>

namespace ug2
{

void sub_5d6ad0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5c5930(win32::WinApplication* app, x86::CPU& cpu);
void sub_48f600(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

const x86::reg32 CurrentModel = 0x883e00;
const x86::reg32 MatrixCursor = 0x82da5c;
const x86::reg32 MatrixEnd = 0x82da60;
const x86::reg32 MatrixOverflow = 0x82da6c;
const x86::reg32 MatrixMissed = 0x82da70;

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("scenery_stuff_native"))
    {
        sub_5d6ad0_guest(app, cpu);
        return;
    }
    const x86::reg32 entry = cpu.esp;
    const x86::reg32 self = cpu.ecx;
    const x86::reg32 view = rc::ld32(entry + 4);
    const x86::reg32 pass = rc::ld8(entry + 8);
    const x86::reg32 eye = rc::ld32(view + 0x40) + 0x40;

    const x86::reg32 base = ((pass & 1) ? 0x1000u : 0u) | ((pass & 0x80) ? 0x100u : 0u);
    x86::reg32 wanted = 0, unwanted = 0;
    if (pass & 0x10)
        wanted = 0x2000;
    else if (pass & 8)
        unwanted = 0x2000;
    if (pass & 2)
        wanted = 0x2000;

    const x86::sreg32 count = x86::sreg32(rc::ld32(self + 0x6c0));
    cpu.edx = x86::reg32(count);
    cpu.esp = entry + 12;
    if (count <= 0)
    {
        cpu.eax = 0;
        return;
    }
    x86::reg32 list = self;
    x86::sreg32 index = 0;
    while (rc::ld32(list + 0x40) != view)
    {
        list += 0x90;
        if (++index >= count)
        {
            cpu.eax = x86::reg32(count);
            cpu.ecx = list;
            return;
        }
    }
    cpu.ecx = list;
    x86::reg32 end = rc::ld32(list + 0x4c);
    x86::reg32 item = rc::ld32(list + 0x48);
    cpu.eax = end;
    if (item >= end)
        return;

    const x86::reg32 savedEbx = cpu.ebx, savedEsi = cpu.esi, savedEdi = cpu.edi, savedEbp = cpu.ebp;
    const x86::reg32 frame = entry - 0x20;
    const auto render = [&](x86::reg32 model, x86::reg32 instance, x86::reg32 matrix, x86::reg32 flags) {
        rc::st32(frame - 0x14, model);
        rc::st32(frame - 0x10, instance);
        rc::st32(frame - 0xc, matrix);
        rc::st32(frame - 8, flags);
        rc::st32(frame - 4, 0);
        cpu.ecx = view;
        cpu.esi = flags;
        cpu.esp = frame - 0x18;
        sub_5c5930(app, cpu);
    };

    // The filters only ever hold bit 0x2000, which is record bit 0x400.
    const bool keepSet = wanted != 0 || unwanted == 0;
    const bool keepClear = unwanted != 0 || wanted == 0;
    const x86::reg32 rejectMask = keepSet && keepClear ? 0u : 0x400u;
    const x86::reg32 rejectValue = keepClear ? 0x400u : 0u;
    x86::reg32 handled = 0;
    do
    {
        const x86::reg32 record = rc::ld32(item + 8);
        const x86::reg32 bits = rc::ld16(record + 0x1a);
        if ((bits & rejectMask) == rejectValue)
        {
            item += 0xc;
            continue;
        }
        const x86::reg32 word = rc::ld32(item);
        x86::reg32 flags = base;
        if (bits & 0x8000)
            flags |= 0x200;
        if (bits & 0x400)
            flags |= 0x2000;
        if ((word & 3) == 2)
            flags |= 4;

        handled = item;
        const x86::reg32 model = word & ~x86::reg32(3);
        rc::st32(CurrentModel, model);
        const x86::reg32 instance = rc::ld32(item + 4);
        cpu.edx = (record & 0xffff0000u) | bits;
        cpu.ebx = view;
        cpu.edi = item;
        cpu.ebp = entry - 4;
        if (instance == 0)
        {
            cpu.eax = 0;
            render(model, 0x872e90, 0, flags);
        }
        else
        {
            const x86::reg32 solid = rc::ld32(model + 0xc);
            const x86::reg32 marked = solid ? rc::ld8(solid + 0xe) & 0x40u : 0u;
            if (marked == 0)
            {
                cpu.edx = instance;
                cpu.eax = model;
                render(model, instance, 0, flags);
            }
            else
            {
                const x86::reg32 kind = rc::ld32(view + 4);
                if (x86::sreg32(kind) < 0x10 || x86::sreg32(kind) > 0x15)
                {
                    const Real dx = ld(instance + 0x30) - ld(eye);
                    const Real dy = ld(instance + 0x34) - ld(eye + 4);
                    const Real dz = ld(instance + 0x38) - ld(eye + 8);
                    Real distance = dz * dz;
                    distance += dy * dy;
                    distance += dx * dx;
                    if (!(distance < ld(0x784500)))
                    {
                        cpu.eax = rc::ld32(view + 0x54);
                        render(model, instance, cpu.eax, flags);
                    }
                    else
                    {
                        const x86::reg32 matrix = rc::ld32(MatrixCursor);
                        const x86::reg32 limit = rc::ld32(MatrixEnd);
                        cpu.edx = limit;
                        if (matrix + 0x1c0 >= limit)
                        {
                            rc::st32(MatrixOverflow, 1);
                            rc::st32(MatrixMissed, rc::ld32(MatrixMissed) + 0x1c0);
                        }
                        else
                        {
                            rc::st32(MatrixCursor, matrix + 0x1c0);
                            if (matrix != 0)
                            {
                                const x86::reg32 camera = rc::ld32(view + 0x40);
                                rc::st32(frame - 0x18, matrix);
                                rc::st32(frame - 0x14, 0x7f3ff0);
                                rc::st32(frame - 0x10, instance);
                                rc::st32(frame - 0xc, camera);
                                rc::st32(frame - 8, eye);
                                rc::st32(frame - 4, view);
                                cpu.eax = instance;
                                cpu.ecx = eye;
                                cpu.edx = camera;
                                cpu.ebx = matrix;
                                cpu.esi = flags;
                                cpu.esp = frame - 0x1c;
                                sub_48f600(app, cpu);
                                if (cpu.terminate)
                                    return;
                                cpu.edx = rc::ld32(CurrentModel);
                                render(cpu.edx, rc::ld32(item + 4), matrix, flags & 0xfffffdffu);
                            }
                        }
                    }
                }
            }
        }
        if (cpu.terminate)
            return;
        const x86::reg32 owner = rc::ld32(item + 8);
        rc::st8(owner + 0x1b, rc::ld8(owner + 0x1b) & 0x7f);
        rc::st32(CurrentModel, 0);
        app->allowPreemption(cpu);
        end = rc::ld32(list + 0x4c);
        item += 0xc;
    } while (item < end);

    const x86::reg32 last = item - 0xc;
    if (handled != last)
        cpu.edx = (rc::ld32(last + 8) & 0xffff0000u) | rc::ld16(rc::ld32(last + 8) + 0x1a);
    cpu.eax = end;
    cpu.ecx = list;
    cpu.ebx = savedEbx;
    cpu.esi = savedEsi;
    cpu.edi = savedEdi;
    cpu.ebp = savedEbp;
    cpu.esp = entry + 12;
}

void declare(x86::CPU& cpu, shadow::Spans& spans)
{
    (void)cpu;
    spans.push_back(shadow::Span{CurrentModel, 4, false});
    spans.push_back(shadow::Span{MatrixCursor, 4, false});
    spans.push_back(shadow::Span{MatrixOverflow, 8, false});
}

const shadow::Spec spec = {"sub_5d6ad0", shadow::Ecx | shadow::Ebx | shadow::Esp | shadow::Ebp | shadow::Esi | shadow::Edi, 0, false, false};

}

void sub_5d6ad0(win32::WinApplication* app, x86::CPU& cpu)
{
    const offload::Pass pass(offload::CatWorld, rc::ld8(cpu.esp + 8));
    shadow::call(spec, app, cpu, body, sub_5d6ad0_guest, declare);
}

}}
