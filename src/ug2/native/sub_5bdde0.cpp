// Native replacement of sub_5bdde0: per-mesh world, view and projection upload with a three-way cache (cdecl: world, view).

#include <native/common.h>
#include <native/flush_context.h>
#include <offload.h>
#include <native/shadow.h>
#include <platform/switches.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <winapi/d3d9_native.h>

namespace ug2
{

void sub_5bdde0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

const float* matrixAt(x86::reg32 address)
{
    return reinterpret_cast<const float*>(rc::host(address));
}

// Bridge calls never write guest memory: reads stay valid.
void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("world_upload_native"))
    {
        sub_5bdde0_guest(app, cpu);
        return;
    }
    const x86::reg32 world = argument(cpu, 0);
    const x86::reg32 view = argument(cpu, 1);
    flush::st32<0x872ef4>(cpu, flush::ld32<0x872ef4>(cpu) + 1);
    if (world == 0x872e90)
        flush::st32<0x872f00>(cpu, flush::ld32<0x872f00>(cpu) + 1);
    const x86::reg32 group = flush::ld32<0x870910>(cpu);
    const x86::reg32 cachedGroup = flush::ld32<0x87684c>(cpu);
    const x86::reg32 cachedView = flush::ld32<0x800534>(cpu);
    const bool hit = world == flush::ld32<0x876850>(cpu) && group == cachedGroup && cachedView == view;
    offload::matrixStep(hit, world, group, view);
    if (hit)
    {
        cpu.eax = group;
        cpu.esp += 4;
        return;
    }
    const float* const views = flush::viewMatrices(cpu, view);
    const x86::reg32 device = rc::ld32(0x870974);
    const bool viewChanged = view != cachedView;
    if ((viewChanged || group != cachedGroup) && rc::ld8(group + 0x18) == 0)
    {
        win32::d3d9::bridge::setTransform(app, cpu, device, 2, views);
        win32::d3d9::bridge::setTransform(app, cpu, device, 3, views + 16);
    }
    if (viewChanged)
    {
        flush::st32<0x872efc>(cpu, flush::ld32<0x872efc>(cpu) + 1);
        flush::st32<0x800534>(cpu, view);
    }
    flush::st32<0x872ef8>(cpu, flush::ld32<0x872ef8>(cpu) + 1);
    flush::st32<0x87684c>(cpu, group);
    flush::st32<0x876850>(cpu, world);
    const float* const worldMatrix = matrixAt(world);
    const x86::reg32 desc = rc::ld32(group + 0x1c);
    const x86::reg32 wvHandle = rc::ld32(desc + 0x934);
    float product[16];
    if (wvHandle != 0)
    {
        win32::d3d9::bridge::matrixMultiply(worldMatrix, views, product);
        win32::d3d9::bridge::effectSetMatrix(app, cpu, rc::ld32(group + 0x20), wvHandle, product);
    }
    if (rc::ld8(group + 0x18) == 0)
    {
        win32::d3d9::bridge::setTransform(app, cpu, device, 0x100, worldMatrix);
        cpu.eax = group;
        cpu.esp += 4;
        return;
    }
    win32::d3d9::bridge::matrixMultiply(worldMatrix, views + 32, product);
    cpu.eax = win32::d3d9::bridge::effectSetMatrix(app, cpu, rc::ld32(group + 0x20), rc::ld32(desc + 0x95c), product);
    cpu.esp += 4;
}

void declare(x86::CPU&, shadow::Spans& spans)
{
    spans.push_back(shadow::Span{0x872ef4, 16, false});
    spans.push_back(shadow::Span{0x876850, 4, false});
    spans.push_back(shadow::Span{0x87684c, 4, false});
    spans.push_back(shadow::Span{0x800534, 4, false});
}

// ecx and edx are not compared: D3D call leftovers.
const shadow::Spec spec = {"sub_5bdde0", shadow::Eax | shadow::Ebx | shadow::Esp | shadow::Ebp | shadow::Esi | shadow::Edi, 0, false, true};

}

void sub_5bdde0(win32::WinApplication* app, x86::CPU& cpu)
{
    shadow::call(spec, app, cpu, body, sub_5bdde0_guest, declare);
}

}}
