// Forwards to the translated SubmitMesh (0x5ceb30) and tags the entry it queued with the current offload category.

#include <native/common.h>
#include <offload.h>

namespace ug2
{

void sub_5ceb30_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_5ceb30(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 info = offload::counting() ? argument(cpu, 4) : 0;
    sub_5ceb30_guest(app, cpu);
    if (info)
        offload::submitted(info);
}

}}
