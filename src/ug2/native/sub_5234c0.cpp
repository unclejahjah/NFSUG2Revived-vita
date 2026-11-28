// Native sub_5234c0 (thiscall, ret 4): the visual rating as a fixed value per game stage when the mod is on.

#include "../mods_runtime.h"
#include "../novisrep.h"
#include <native/common.h>

namespace ug2
{

void sub_5234c0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_5234c0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!modrt::visualRepRemoved)
    {
        sub_5234c0_guest(app, cpu);
        return;
    }
    // NFSU/NFSU2 Visual Reputation Remover 1.1 [NFSU2] RemoveVisualRep: 0x5234C0 jumps to CalculateVisualRep
    const int32_t stage = int32_t(rc::ld32(novisrep::StageAddress));
    cpu.fpu.count += 1;
    cpu.fpu.regs[cpu.fpu.count & 7] = Real(novisrep::rating(stage));
    cpu.esp += 8;
}

}

}
