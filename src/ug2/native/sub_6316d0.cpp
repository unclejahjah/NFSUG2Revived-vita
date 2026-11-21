// Forwards to the translated car pass of the main and mirror views (0x6316d0); its submissions count as cars (offload_count).

#include <native/common.h>
#include <offload.h>

namespace ug2
{

void sub_6316d0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_6316d0(win32::WinApplication* app, x86::CPU& cpu)
{
    const offload::Pass pass(offload::CatCars, 0);
    sub_6316d0_guest(app, cpu);
}

}}
