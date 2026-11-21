// Forwards to the translated post-processing pass (0x5c7470); its game-thread time counts as a direct-draw pass (offload_count).

#include <native/common.h>
#include <offload.h>

namespace ug2
{

void sub_5c7470_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_5c7470(win32::WinApplication* app, x86::CPU& cpu)
{
    const offload::Direct direct;
    sub_5c7470_guest(app, cpu);
}

}}
