// Analog menu scanner (sub_571d50; cdecl: node, pad buffer, -, config, port): menu up/down ignore the pedal byte
// while the right stick is off centre.

#include "../garage_stick.h"
#include <native/common.h>
#include <lib/gamepad.h>

namespace ug2
{

void sub_571d50_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_571d50(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 config = argument(cpu, 3);
    const x86::reg32 event = rc::ld32(config + 8);
    if (event != garage_stick::MenuUpAlternate && event != garage_stick::MenuDownAlternate)
    {
        sub_571d50_guest(app, cpu);
        return;
    }
    float x, y;
    win32::Gamepad::getRightStick(&x, &y);
    const x86::reg32 pedal = argument(cpu, 1) + (rc::ld8(config + 0x12) >> 3);
    const x86::reg8 real = rc::ld8(pedal);
    rc::st8(pedal, garage_stick::menuPedalByte(real, y));
    sub_571d50_guest(app, cpu);
    rc::st8(pedal, real);
}

}

}
