// Forwards to the translated crowd pass (0x61d4b0); its submissions count as crowd (offload_count).

#include <native/common.h>
#include <offload.h>

namespace ug2
{

void sub_61d4b0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_61d4b0(win32::WinApplication* app, x86::CPU& cpu)
{
    const offload::Pass pass(offload::CatCrowd, 0);
    sub_61d4b0_guest(app, cpu);
}

}}
