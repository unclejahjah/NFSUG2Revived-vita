// Wrappers of the decompression dispatcher 0x574670 and the texture creation 0x5ce170 (upload batching, hitch
// timing), and of the unloads that must not overtake a pending draw flush.

#include <hitch.h>
#include <native/common.h>
#include <native/flush_context.h>
#include <textures.h>

#include <platform/config.h>
#include <platform/switches.h>
#include <platform/gxm.h>
#include <platform/perf.h>
#include <recomp.h>

namespace ug2 {

void sub_574670_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5ce170_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_577b10_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_493f20_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_494dd0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_494b10_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

// Decompression dispatcher (cdecl; source, destination): COMP, HUFF, RAWW, JDLZ.
void sub_574670(win32::WinApplication* app, x86::CPU& cpu)
{
    hitch::timed<pf::perf::EventJdlz>([&] { sub_574670_guest(app, cpu); });
}

// Texture creation of a texture record (thiscall).
void sub_5ce170(win32::WinApplication* app, x86::CPU& cpu)
{
    static const bool batch = NFSU_DEV_BOOL("texture_batch", true);
    textures::creating();
    if (batch)
        pf::gxm::Device::get().beginUploads();
    hitch::timed<pf::perf::EventTextureCreates>([&] { sub_5ce170_guest(app, cpu); });
    if (batch)
        pf::gxm::Device::get().endUploads();
}

// Chunk unload (cdecl; chunks, size).
void sub_577b10(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 from = argument(cpu, 0), size = argument(cpu, 1);
    flush::unloadNotice("chunks", from, size, 8);
    sub_577b10_guest(app, cpu);
    flush::unloadPoison(8, from, size);
}

// Solid unload (cdecl; chunk, pack): clears each solid's mesh.
void sub_493f20(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 chunk = argument(cpu, 0), size = rc::ld32(chunk + 4);
    flush::unloadNotice("solid", chunk, size + 8, 1);
    sub_493f20_guest(app, cpu);
    flush::unloadPoison(1, chunk + 8, size);
}

// Texture pack unload (cdecl; chunk).
void sub_494dd0(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 chunk = argument(cpu, 0), size = rc::ld32(chunk + 4);
    flush::unloadNotice("texture pack", chunk, size + 8, 2);
    sub_494dd0_guest(app, cpu);
    flush::unloadPoison(2, chunk + 8, size);
}

// Streamed texture unload (thiscall; pack offset, size): the pack's texture records with image or palette there.
void sub_494b10(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 pack = cpu.ecx, from = argument(cpu, 0), size = argument(cpu, 1);
    const x86::reg32 count = rc::ld32(pack + 0x74), owner = rc::ld32(pack + 0x70);
    const x86::reg32 table = owner >= 0x10000 ? rc::ld32(owner + 0x74) : 0;
    for (x86::reg32 i = 0; table >= 0x10000 && x86::sreg32(i) < x86::sreg32(count); ++i)
    {
        const x86::reg32 record = rc::ld32(table + 8 * i + 4);
        if (record == 0)
            continue;
        const bool image = rc::ld32(record + 0x38) != 0 && rc::ld32(record + 0x30) - from < size;
        const bool palette = rc::ld32(record + 0x3c) != 0 && rc::ld32(record + 0x34) - from < size;
        if (image || palette)
            flush::unloadNotice("streamed texture", record, 0x7c, 4, false);
    }
    flush::unloadSettle();
    sub_494b10_guest(app, cpu);
}

}

}
