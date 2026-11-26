// Line width with wrapping (sub_50c080): marks its token skip as one the icons leave alone.

#include "../psprompt.h"
#include <native/common.h>

namespace ug2
{

void sub_50c080_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_50c080(win32::WinApplication* app, x86::CPU& cpu)
{
    const psprompt::Scope saved = psprompt::enterText(psprompt::Other, 0);
    sub_50c080_guest(app, cpu);
    psprompt::leaveText(saved);
}

}

}
