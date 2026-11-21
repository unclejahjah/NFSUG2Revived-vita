// Forwards to the translated flare pass (0x5c5580); its game-thread time counts as a direct-draw pass (offload_count).

#include <native/common.h>
#include <offload.h>

namespace ug2
{

void sub_5c5580_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_5c5580(win32::WinApplication* app, x86::CPU& cpu)
{
    const offload::Direct direct;
    sub_5c5580_guest(app, cpu);
}

}}
