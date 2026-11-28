// Text entry. The game's line editor (state at 0x83A7B0) reads WM_CHAR; this polls it and gives its field to the
// on-screen keyboard through win32::Gamepad::editText, which types the result back as WM_CHAR.

#include "textentry.h"

#include <lib/gamepad.h>
#include <platform/config.h>
#include <platform/switches.h>
#include <platform/log.h>
#include <recomp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ug2 { namespace textentry {

namespace {

using x86::reg32;

const reg32 Editor = 0x0083A7B0;
const reg32 EditText = Editor + 0x100;      // text being edited, 256 bytes
const reg32 EditType = Editor + 0x204;      // character set: 0, 3 text, 4 login, 5 masked, 6 name
const reg32 EditMax = Editor + 0x20C;       // longest text
const reg32 EditActive = Editor + 0x210;    // byte, set once the screen accepts keys
const reg32 EditOwner = Editor + 0x214;     // the edit session, zero when none
const reg32 PeekMessageSlot = 0x00783330;   // import slot of PeekMessageA, called every frame by 0x5BF1C0
const reg32 MaxLength = 255;
const reg32 FramesRendered = 0x0086514C;
const reg32 DebugFrame = 600;               // frame at which ime_test_field starts its editor
const reg32 NewObject = 0x00575620;         // the game's allocator, cdecl (size)
const reg32 BeginEdit = 0x004FF3B0;         // thiscall on Editor (owner, type, text, max), ret 0x10
const reg32 ScreenHandler = 0x00FF0000;     // address of the stand-in screen's message handler

win32::MethodPtr s_peekMessage = nullptr;
bool s_editing = false;
char s_last[MaxLength + 1];

bool s_debugDone = false;

reg32 callGuest(win32::WinApplication* app, x86::CPU& cpu, reg32 function, reg32 ecx, const reg32* args, int count)
{
    x86::CPU saved = cpu;
    reg32 esp = cpu.esp;
    for (int i = count - 1; i >= 0; --i)
        rc::st32(esp -= 4, args[i]);
    esp -= 4;
    cpu.esp = esp;
    cpu.ecx = ecx;
    app->dynamic_call(function, cpu);
    const reg32 result = cpu.eax;
    cpu = saved;
    return result;
}

void screenHandler(win32::WinApplication*, x86::CPU& cpu)
{
    pf::logInfo("textentry: test screen got message 0x%08X, editor text \"%s\"", unsigned(rc::ld32(cpu.esp + 4)),
                s_last);
    cpu.esp += 20;
}

// ime_test_field=type,max,text: opens the editor on a stand-in screen, as the game's screens do.
void debugField(win32::WinApplication* app, x86::CPU& cpu)
{
    const char* spec = NFSU_DEV_STRING("ime_test_field", "");
    if (s_debugDone || !*spec || rc::ld32(FramesRendered) < DebugFrame)
        return;
    s_debugDone = true;
    const unsigned type = unsigned(atoi(spec));
    const char* comma = strchr(spec, ',');
    const unsigned maxLength = comma ? unsigned(atoi(comma + 1)) : 12;
    const char* text = comma && strchr(comma + 1, ',') ? strchr(comma + 1, ',') + 1 : "";
    reg32 size = 0x40;
    const reg32 session = callGuest(app, cpu, NewObject, 0, &size, 1);
    const reg32 screen = callGuest(app, cpu, NewObject, 0, &size, 1);
    const reg32 table = callGuest(app, cpu, NewObject, 0, &size, 1);
    const reg32 string = callGuest(app, cpu, NewObject, 0, &size, 1);
    rc::st32(session, 0);
    rc::st32(session + 4, screen);
    rc::st32(session + 8, 0);
    rc::st32(session + 0xC, 0);
    rc::st32(session + 0x10, 0);
    rc::st32(screen, table);
    rc::st32(table + 8, ScreenHandler);
    for (reg32 i = 0; i < 0x40; ++i)
        rc::st8(string + i, 0);
    for (reg32 i = 0; i < 0x3F && text[i]; ++i)
        rc::st8(string + i, x86::reg8(text[i]));
    const reg32 args[4] = { session, type, string, maxLength };
    callGuest(app, cpu, BeginEdit, Editor, args, 4);
    pf::logInfo("textentry: test editor started, type %u, at most %u characters, text \"%s\"", type, maxLength, text);
}

void poll()
{
    const bool active = rc::ld8(EditActive) != 0 && rc::ld32(EditOwner) != 0;
    if (!active)
    {
        if (s_editing)
        {
            win32::Gamepad::endTextEdit();
            pf::logInfo("textentry: editor closed, last text \"%s\"", s_last);
            s_editing = false;
        }
        return;
    }
    const reg32 type = rc::ld32(EditType);
    const reg32 maxLength = rc::ld32(EditMax) < MaxLength ? rc::ld32(EditMax) : MaxLength;
    reg32 length = 0;
    for (; length < maxLength && rc::ld8(EditText + length); ++length)
        s_last[length] = char(rc::ld8(EditText + length));
    s_last[length] = 0;
    if (!s_editing)
        pf::logInfo("textentry: editor active, type %u, at most %u characters, text \"%s\"", unsigned(type),
                    unsigned(maxLength), s_last);
    win32::Gamepad::editText(title(type), s_last, maxLength, extraChars(type), true);
    s_editing = true;
}

void peekMessageHook(win32::WinApplication* app, x86::CPU& cpu)
{
    s_peekMessage(app, cpu);
    debugField(app, cpu);
    poll();
}

}

void configure(win32::WinApplication& app)
{
    win32::Gamepad::setTextAppendOnly(true);
    if (*NFSU_DEV_STRING("ime_test_field", ""))
        app.registerMethod(ScreenHandler, { "test screen handler", screenHandler });
    s_peekMessage = app.replaceMethod(rc::ld32(PeekMessageSlot), peekMessageHook);
    pf::logInfo("textentry: line editor 0x%08X polled at PeekMessageA, typed back as WM_CHAR", unsigned(Editor));
}

}}
