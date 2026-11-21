// Wrappers of the decompression dispatcher 0x574670 and the texture creation 0x5ce170 (upload batching, hitch
// timing), and of the unloads that must not overtake a pending draw flush.

#include <hitch.h>
#include <native/flush_context.h>
#include <textures.h>

#include <platform/config.h>
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
    static const bool batch = pf::Config::getBool("texture_batch", true);
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
    flush::unloadFence("chunks");
    sub_577b10_guest(app, cpu);
}

// Solid unload (cdecl; chunk, pack): clears each solid's mesh.
void sub_493f20(win32::WinApplication* app, x86::CPU& cpu)
{
    flush::unloadFence("solid");
    sub_493f20_guest(app, cpu);
}

// Texture pack unload (cdecl; chunk).
void sub_494dd0(win32::WinApplication* app, x86::CPU& cpu)
{
    flush::unloadFence("texture pack");
    sub_494dd0_guest(app, cpu);
}

// Streamed texture unload (thiscall).
void sub_494b10(win32::WinApplication* app, x86::CPU& cpu)
{
    flush::unloadFence("streamed textures");
    sub_494b10_guest(app, cpu);
}

}

}
