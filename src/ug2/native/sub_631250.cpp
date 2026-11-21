// Forwards to the translated car pass of view 4 (0x631250); its submissions count as cars (offload_count).

#include <native/common.h>
#include <offload.h>

namespace ug2
{

void sub_631250_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_631250(win32::WinApplication* app, x86::CPU& cpu)
{
    const offload::Pass pass(offload::CatCars, 0);
    sub_631250_guest(app, cpu);
}

}}
