// Wrappers of the decompression dispatcher 0x574670 and the texture creation 0x5ce170: upload batching, hitch timing.

#include <hitch.h>
#include <textures.h>

#include <platform/config.h>
#include <platform/gxm.h>
#include <platform/perf.h>
#include <recomp.h>

namespace ug2 {

void sub_574670_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5ce170_guest(win32::WinApplication* app, x86::CPU& cpu);

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

}

}
