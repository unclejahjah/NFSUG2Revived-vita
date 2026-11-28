// Options > Controls on three own screens (copies of UI_Options and UI_OptionsController): stick shape rows, Control
// Type, and Customize rebinding through the PC capture, Square removing.

#include "controlsshell.h"
#include "fepackage.h"
#include "controlsbake.h"
#include "controlslink.h"
#include "controlspack.h"
#include "controlsrows.h"
#include "customize.h"
#include "diagram.h"
#include "ownscreens.h"
#include "psprompt.h"
#include "options_menu.h"
#include "translations.h"

#include <lib/gamepad.h>
#include <lib/memmap.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>

#include <cstring>
#include <string>
#include <vector>

namespace ug2 { namespace controlsshell {

namespace {

using x86::reg32;
using fepackage::nameHash;

const reg32 NewObject = 0x575620;           // cdecl (bytes)
const reg32 ScreenBase = 0x552780;          // thiscall (screen data): the options screen's base constructor
const reg32 OptionsVtable = 0x795e28;       // 0 destructor, 2 message handler 0x49C690, 4 row builder 0x4B8470
const reg32 OptionsMessage = 0x49c690;      // thiscall (message, 3 more), ret 0x10
const reg32 OptionsDestructor = 0x4cfaf0;
const reg32 ScreenSize = 0x164;
const reg32 RowConstructor = 0x51e230;      // thiscall (row groups x4 of the screen, enabled)
const reg32 AddRow = 0x54ef30;              // thiscall on the screen (row, 1)
const reg32 FinishRows = 0x539650;          // thiscall on the screen (0)
const reg32 HideObject = 0x50ca00;          // cdecl (object)
const reg32 ShowObject = 0x50ca50;          // cdecl (object)
const reg32 FontOf = 0x51bd70;              // cdecl (text object's font handle +0x24): the font, loaded when +8 is set
const reg32 FindObject = 0x5379c0;          // cdecl (package name, hash)
const reg32 SetText = 0x50c900;             // cdecl (text object, text hash)
const reg32 SetFormatted = 0x537b30;        // cdecl (text object, format, argument list): the object's own string
const reg32 FormatString = 0x786e70;        // the game's "%s"
const reg32 SwitchScreen = 0x537980;        // cdecl (source name, target name, 0)
const reg32 MeasureText = 0x50bec0;         // thiscall on a font (wide text, 0): the width on the x87 stack
const reg32 TextTable = 0x8383d8, TextCount = 0x8383d0;
const reg32 TwoValueRow = 0x794138;         // vtable of a two-value row; 17 slots
const reg32 GameMode = 0x850078;
const reg32 PcControllerName = 0x793dcc;    // "UI_Options_PC_Controller.fng", pushed by Options > Controls (0x4B61A5)
const reg32 PcControllerEntryName = 0x7f7e54;   // the screen table's pointer to the same string
const reg32 LoadChunks = 0x577ab0;          // cdecl (data, bytes)
const reg32 SetImage = 0x50c990;            // cdecl (image object, texture hash)
const reg32 TextureByHash = 0x4901d0;       // cdecl (hash, default, unloaded): texture info; [info] + 0x18 the D3D texture
const reg32 SlotLockRect = 19, SlotUnlockRect = 20;     // IDirect3DTexture9 vtable
const char* const PictureTexture = "CTRL_PICTURE";
const reg32 RowCount = 0x12c, VisibleRows = 0x134;     // the list's rows + 1 (the scroll bar takes rows), and lines shown
const reg32 SlotMessage = 1, SlotUpdate = 3, SlotBase = 16;
const reg32 MessageLeft = 0x9120409e, MessageRight = 0xb5971bf1, MessageAccept = 0xc407210;
const reg32 MessageBack = 0x911ab364, MessageOutroDone = 0xe1fde1d1, ScreenLeaving = 0x138;
const reg32 LabelObject = 0x2c, ValueObject = 0x30, ArrowLeft = 0x4c, ArrowRight = 0x50, HighlightedRow = 0x54;
const reg32 ObjectFlags = 0x1c, ObjectFont = 0x24, ObjectTextHash = 0x60, DataOffset = 0x2c, DataX = 0x1c, DataScale = 0x38,
            DirtyFlag = 0x400000;
const reg32 TextAlign = 0x6c, TextAlignCentre = 1, TextAlignRight = 2;
const reg32 TitleObject = 0x42adb44c;
const reg32 TextControls = 0x04dc6db5, TextJoypad = 0xf91fba4d, TextCustomize = 0x8d5abf58, TextControlType = 0xba7306aa,
            TextNone = 0x001a076f;
const reg32 ArmCapture = 0x5c2100;          // thiscall on a pad slot (action, flag), ret 8
const reg32 SelectMap = 0x5cf320;           // thiscall on a pad slot (map), ret 4
const reg32 SetPadMask = 0x537950;          // cdecl (package name, mask): its four ports' pad masks
const reg32 HelpBarName = 0x790000;         // "UI_PC_Help_Bar.fng", from 0x50CEA0
const reg32 SetScript = 0x51cf70;           // cdecl (object, script name, 1)
const reg32 ScriptPulse = 0x78f6bc, ScriptHighlight = 0x78f228;
const reg32 CaptureAction = 0x33c, CaptureFlag = 0x870768;      // slot field and global the PC capture runs on
const reg32 EscapeFlag = 0x87075c;          // set only by a buffered keyboard Escape (0x5C19BF); our GetDeviceData sends none
const reg32 PadWords = 0x870760, PadIndex = 0x230, PadStateBytes = 0x110, PovOffset = 0x20;   // DIJOYSTATE2 copies, slot's pad
const reg32 MessageButton1 = 0xc519bfc0;
const reg32 PadMessages[] = { 0x406415e3, MessageAccept, 0x911ab364, 0x9120409e, 0xb5971bf1, 0x72619778, 0x911c0a4b, 0xc519bfbf,
                              MessageButton1, 0xc519bfc1, 0xc519bfc2, 0xb5af2461, 0x5073ef13, 0xd9feec59, 0x911e0fdc };
const double RowLabelWidth = 225.0;
const float ValueCentreShift = 48.0f;

// Fake guest addresses of our native methods.
const reg32 FakeBase = 0x2000;
const reg32 FakeConstructor = FakeBase, FakeDestructor = FakeBase + 0x40, FakeMessage = FakeBase + 0x50, FakeRows = FakeBase + 0x60,
            FakeRowMessage = FakeBase + 0x70, FakeRowUpdate = FakeBase + 0x80, FakeArrowless = FakeBase + 0x100;

enum Screen { ScreenNone, ScreenTop, ScreenJoypad, ScreenCustomize, ScreenCount };
const char* const ScreenNames[ScreenCount] = { "", "UI_Controls.fng", "UI_ControlsJoypad.fng", "UI_ControlsCustomize.fng" };
const char* const ScreenSources[ScreenCount] = { "", "UI_Options.fng", "UI_OptionsController.fng", "UI_Options.fng" };
const reg32 ScreenTitles[ScreenCount] = { 0, TextControls, TextJoypad, TextCustomize };
const Screen ScreenParents[ScreenCount] = { ScreenNone, ScreenNone, ScreenTop, ScreenJoypad };

enum Kind { KindJoypad, KindStick, KindControlType, KindCustomize, KindEntry, KindVitaButton, KindSetting };

// Customize's rows after the game's actions: free roam's held buttons (src/lib/controls.cpp), cfg settings that
// Left and Right step through as Vita Settings does (options_menu.cpp), not binds in registry.txt.
struct VitaButton
{
    const char* label;
    const char* key;
};
const VitaButton VitaButtons[] = {
    { "Horn (free roam)", "horn_button" },
    { "Drift Mode (free roam)", "drift_button" },
};
const int VitaButtonCount = int(sizeof(VitaButtons) / sizeof(VitaButtons[0]));

// The port's own controller settings, on the Controls screen after the stick rows (they were Vita Settings'
// Controller page): Left and Right step them as Vita Settings did, labels and values from options_menu.cpp.
// The game's UI_Options list has 13 row slots (Customize's copy grows its own): Joypad, 6 stick rows and these 6.
const char* const ControlsSettings[] = {
    "steering", "rear_touch", "free_look_ease_back", "free_look_swing", "garage_stick", "garage_stick_deadzone",
};
const int ControlsSettingCount = int(sizeof(ControlsSettings) / sizeof(ControlsSettings[0]));
const int TopRowCount = 1 + 6 + ControlsSettingCount;     // Joypad, the stick rows, the settings
static_assert(TopRowCount <= 13, "the Controls list shows 13 rows");

struct Text
{
    reg32 hash;
    const char* plain;
};

const Text StickLabels[6] = { { 0, "Left Dead Zone" }, { 0, "Left Outer Limit" }, { 0, "Left Sensitivity" },
                              { 0, "Right Dead Zone" }, { 0, "Right Outer Limit" }, { 0, "Right Sensitivity" } };

// Customize entry labels, in controlsdata::Entry order.
const Text Entries[] =
{
    { 0, "Steer Left" },
    { 0, "Steer Right" },
    { 0, "Free Look" },
    { 0x619b23b0, nullptr },
    { 0, "Brake" },     // the game only has "Brake / Reverse"; ours until translated
    { 0xb70ff4a4, nullptr },
    { 0x5e5c5f2e, nullptr },
    { 0xdc1cd97d, nullptr },
    { 0x56afe190, nullptr },
    { 0x13121940, nullptr },
    { 0xd38df194, nullptr },
    { 0x4a1341b3, nullptr },
    { 0x6026c91f, nullptr },
    { 0x6ef09d0d, nullptr },
    { 0x70a30056, nullptr },
    { 0xeac74624, nullptr },
    { 0x64730aac, nullptr },
    { 0x3dc6a111, nullptr },
};
const int EntryCount = int(sizeof(Entries) / sizeof(Entries[0]));
static_assert(EntryCount == controlsdata::EntryCount, "one row per controlsdata::Entry");
const int CustomizeRowCount = EntryCount + VitaButtonCount;

struct Row
{
    reg32 row;
    Kind kind;
    int index;      // the stick row or the Customize entry
};

// Customize shows a window of the entries in its rows.

Screen s_screen = ScreenNone;
reg32 s_screenObject = 0, s_package = 0;
std::vector<Row> s_rows;
int s_slots[ScreenCount] = {};      // row slots of each copy (LEFT_ARROW_n objects)
int s_settle = 0;
int s_checkIn = 0;                  // frames until the baked edits are checked (log)
reg32 s_vtable = 0, s_rowTable = 0, s_arrowlessTable = 0, s_text = 0, s_textArgument = 0;
float s_valueReference = 0.0f;       // row 1's value x after the game's first layout, as controls-overhaul took it
bool s_haveReference = false;
int s_resetLogs = 0;

// The Joypad picture and labels change together: composed aside, then swapped in one frame once every label is fitted.
struct Picture
{
    controlstore::Record shown = {}, record = {};
    bool haveShown = false;
    bool pending = false;
    diagram::Plan plan = {};
    int waited = 0;         // frames the pending swap waited for the label font
};
Picture s_picture;
diagram::Composer s_composer;   // the one host buffer every plan is composed into
reg32 s_pack = 0, s_packBytes = 0, s_image = 0;     // the one texture pack, loaded once, and its DXT3 bytes in it
reg32 s_lockedRect = 0;
const int FontWait = 30;

enum WaitPhase { WaitNone, WaitArmed, WaitGuard };

// The bind wait: armed on the PC capture, then held until every control is released.
struct Wait
{
    WaitPhase phase = WaitNone;
    customize::Wait data;
    reg32 slot = 0, value = 0;                  // the pad slot, the row's value object
    int frames = 0, rearms = 0;
    int finds[3] = {};                          // our capture natives reached since the last arm: button, axis, hat
};
Wait s_wait;

reg32 guestMemory(size_t bytes)
{
    return (new win32::MemMap(reg32(bytes)))->getBlockStart();
}

// Guest call restoring only esp, keeping the x87 result.
reg32 call(win32::WinApplication* app, x86::CPU& cpu, reg32 address, reg32 self, std::initializer_list<reg32> args)
{
    const reg32 saved = cpu.esp;
    reg32 sp = saved;
    for (const reg32* it = args.end(); it != args.begin();)
        rc::st32(sp -= 4, *--it);
    rc::st32(sp -= 4, 0);
    cpu.esp = sp;
    cpu.ecx = self;
    app->dynamic_call(address, cpu);
    cpu.esp = saved;
    return cpu.eax;
}

std::string readString(reg32 address)
{
    std::string text;
    for (char c; address && (c = char(rc::ld8(address))) != 0 && text.size() < 200; ++address)
        text += c;
    return text;
}

std::string tableText(reg32 hash)
{
    const reg32 table = rc::ld32(TextTable), count = rc::ld32(TextCount);
    for (reg32 i = 0; table && i < count; ++i)
    {
        if (rc::ld32(table + 8 * i) == hash)
            return readString(rc::ld32(table + 8 * i + 4));
    }
    return std::string();
}

reg32 findObject(win32::WinApplication* app, x86::CPU& cpu, reg32 hash)
{
    return s_package ? call(app, cpu, FindObject, 0, { s_package, hash }) : 0;
}

void setHash(win32::WinApplication* app, x86::CPU& cpu, reg32 object, reg32 hash)
{
    if (object)
        call(app, cpu, SetText, 0, { object, hash });
}

// Sets the object's own string, not a language table text.
void setPlain(win32::WinApplication* app, x86::CPU& cpu, reg32 object, const std::string& text)
{
    if (!object)
        return;
    if (!s_text)
    {
        s_text = guestMemory(256);
        s_textArgument = guestMemory(4);
    }
    const size_t n = text.size() < 255 ? text.size() : 255;
    for (size_t i = 0; i < n; ++i)
        rc::st8(s_text + reg32(i), uint8_t(text[i]));
    rc::st8(s_text + reg32(n), 0);
    rc::st32(s_textArgument, s_text);
    call(app, cpu, SetFormatted, 0, { object, FormatString, s_textArgument });
    rc::st32(object + ObjectFlags, rc::ld32(object + ObjectFlags) | DirtyFlag);
}

void setText(win32::WinApplication* app, x86::CPU& cpu, reg32 object, const Text& text)
{
    if (text.plain)
        setPlain(app, cpu, object, translations::translate(text.plain));
    else
        setHash(app, cpu, object, text.hash);
}

double measure(win32::WinApplication* app, x86::CPU& cpu, reg32 font, const std::string& text)
{
    static reg32 s_buffer = 0;
    if (!s_buffer)
        s_buffer = guestMemory(512);
    const size_t n = text.size() < 250 ? text.size() : 250;
    for (size_t i = 0; i < n; ++i)
        rc::st16(s_buffer + reg32(2 * i), uint8_t(text[i]));
    rc::st16(s_buffer + reg32(2 * n), 0);
    call(app, cpu, MeasureText, font, { s_buffer, 0 });
    return double(cpu.fpu.pop());
}

void fitLabel(win32::WinApplication* app, x86::CPU& cpu, reg32 label, const Text& text)
{
    const reg32 font = psprompt::lastFont();
    if (!font || !label)
        return;
    const std::string plain = text.plain ? translations::translate(text.plain) : tableText(text.hash);
    if (plain.empty())
        return;
    const std::string fitted = diagram::fitLine(plain, RowLabelWidth, [&](const std::string& t) { return measure(app, cpu, font, t); });
    if (fitted != plain)
        setPlain(app, cpu, label, fitted);
}

bool hasArrows(Kind kind)
{
    return kind == KindStick || kind == KindControlType || kind == KindVitaButton || kind == KindSetting;
}

void hideArrows(win32::WinApplication* app, x86::CPU& cpu, reg32 row)
{
    for (reg32 offset : { ArrowLeft, ArrowRight })
    {
        if (const reg32 arrow = rc::ld32(row + offset))
            call(app, cpu, HideObject, 0, { arrow });
    }
}

void rowTexts(win32::WinApplication* app, x86::CPU& cpu, const Row& r)
{
    const reg32 label = rc::ld32(r.row + LabelObject), value = rc::ld32(r.row + ValueObject);
    switch (r.kind)
    {
    case KindJoypad:
        setHash(app, cpu, label, TextJoypad);
        setPlain(app, cpu, value, "");
        break;
    case KindStick:
        setText(app, cpu, label, StickLabels[r.index]);
        setPlain(app, cpu, value, std::to_string(controlsrows::stickPercent(win32::controls::stickShape(r.index >= 3),
                                                                            controlsrows::StickPart(r.index % 3))) + "%");
        break;
    case KindControlType:
    {
        const int type = controlstore::type(controlslink::backend());
        setHash(app, cpu, label, TextControlType);
        setPlain(app, cpu, value, translations::translate(controlsdata::controlTypeName(type + 1)));
        break;
    }
    case KindCustomize:
        setHash(app, cpu, label, TextCustomize);
        setPlain(app, cpu, value, "");
        break;
    case KindEntry:
    {
        const std::string shown = customize::entryValue(controlstore::active(controlslink::backend()), controlsdata::Entry(r.index));
        setText(app, cpu, label, Entries[r.index]);
        fitLabel(app, cpu, label, Entries[r.index]);
        if (!shown.empty())
            setPlain(app, cpu, value, translations::translate(shown.c_str()));
        else
            setHash(app, cpu, value, TextNone);
        break;
    }
    case KindVitaButton:
    {
        const Text text = { 0, VitaButtons[r.index].label };
        setText(app, cpu, label, text);
        fitLabel(app, cpu, label, text);
        setPlain(app, cpu, value, options::cfgValue(VitaButtons[r.index].key));
        break;
    }
    case KindSetting:
    {
        const std::string plain = options::cfgLabel(ControlsSettings[r.index]);
        const Text text = { 0, plain.c_str() };
        setText(app, cpu, label, text);
        fitLabel(app, cpu, label, text);
        setPlain(app, cpu, value, options::cfgValue(ControlsSettings[r.index]));
        break;
    }
    }
}

const Row* rowOf(reg32 row)
{
    for (const Row& r : s_rows)
    {
        if (r.row == row)
            return &r;
    }
    return nullptr;
}

int rowIndex(reg32 row)
{
    for (size_t i = 0; i < s_rows.size(); ++i)
    {
        if (s_rows[i].row == row)
            return int(i);
    }
    return -1;
}

void refreshRows(win32::WinApplication* app, x86::CPU& cpu)
{
    for (const Row& r : s_rows)
        rowTexts(app, cpu, r);
}

#ifdef DEV_TOOLS
#define BIND_LOG(...) pf::logInfo(__VA_ARGS__)
#else
#define BIND_LOG(...) do {} while (0)
#endif

const char* const ControlNames[controlsdata::ControlCount] = { "Triangle", "Circle", "Cross", "Square", "L", "R", "d-pad up",
    "d-pad down", "d-pad left", "d-pad right", "left stick", "right stick", "rear L2", "rear R2", "rear L3", "rear R3" };

std::string entryName(int entry)
{
    const Text& t = Entries[entry];
    return t.plain ? std::string(t.plain) : tableText(t.hash);
}

void setInstruction(win32::WinApplication* app, x86::CPU& cpu, reg32 text)
{
    setHash(app, cpu, findObject(app, cpu, controlsbake::instructionHash()), text);
}

// The PC's input block: our screen and the help bar.
void padMasks(win32::WinApplication* app, x86::CPU& cpu, reg32 mask)
{
    if (s_package)
        call(app, cpu, SetPadMask, 0, { s_package, mask });
    call(app, cpu, SetPadMask, 0, { HelpBarName, mask });
}

void armCapture(win32::WinApplication* app, x86::CPU& cpu)
{
    call(app, cpu, ArmCapture, s_wait.slot, { customize::armAction(s_wait.data.entry), 0 });
    for (int& n : s_wait.finds)
        n = 0;
}

// As the PC capture ends one (0x5B853A).
void endCapture(reg32 slot)
{
    rc::st32(slot + CaptureAction, 0);
    rc::st8(CaptureFlag, 0);
}

// X on an entry: the PC screen's wait (0x4F93CC).
void startWait(win32::WinApplication* app, x86::CPU& cpu, const Row& r)
{
    const reg32 slot = controlslink::padSlot();
    if (!slot)
    {
        pf::logError("controlsshell: no pad slot, no bind wait for %s", entryName(r.index).c_str());
        return;
    }
    uint32_t inputs;
    float sticks[4];
    win32::Gamepad::getPad(&inputs, sticks);
    s_wait = Wait();
    s_wait.phase = WaitArmed;
    customize::beginWait(&s_wait.data, controlsdata::Entry(r.index), inputs);
    s_wait.slot = slot;
    s_wait.value = rc::ld32(r.row + ValueObject);
    padMasks(app, cpu, 0);
    armCapture(app, cpu);
    if (s_wait.value)
        call(app, cpu, SetScript, 0, { s_wait.value, ScriptPulse, 1 });
    setInstruction(app, cpu, controlsbake::TextPressButton);
    BIND_LOG("controlsshell: bind wait for %s starts: slot %08x armed with action %u%s; held now, not presses: %s", entryName(r.index).c_str(),
             unsigned(slot), unsigned(customize::armAction(s_wait.data.entry)),
             customize::watchesSticks(s_wait.data.entry) ? ", sticks watched by us" : ", takes no stick", customize::inputNames(inputs).c_str());
}

// The capture's result written, the flash stopped, then the release guard.
void finishWait(win32::WinApplication* app, x86::CPU& cpu)
{
    const customize::Wait& w = s_wait.data;
    const std::string name = entryName(w.entry);
    controlstore::Backend& store = controlslink::backend();
    if (w.result == customize::Cancel)
    {
        for (reg32 map : { 0u, 2u, 3u, 1u })
            call(app, cpu, SelectMap, s_wait.slot, { map });
        BIND_LOG("controlsshell: bind wait for %s cancelled by %s after %d frames", name.c_str(), w.found.c_str(), s_wait.frames);
    }
    else
    {
        const int typeBefore = controlstore::type(store);
        const bool changed = controlstore::bindEntry(store, w.entry, w.control);
        BIND_LOG("controlsshell: bind %s to %s (%s, frame %d): %s", name.c_str(), ControlNames[w.control], w.found.c_str(), s_wait.frames,
                 changed ? "written" : "no change");
        if (changed)
        {
            BIND_LOG("controlsshell: registry.txt saved: Controls_Custom, Controls_Type %d%s", controlstore::type(store),
                     typeBefore != controlstore::type(store) ? " (was a Control Type, now Custom)" : "");
            controlslink::storeChanged("Customize");
        }
    }
    if (s_wait.value)
        call(app, cpu, SetScript, 0, { s_wait.value, ScriptHighlight, 1 });
    setInstruction(app, cpu, controlsbake::TextConfigure);
    refreshRows(app, cpu);
    s_wait.phase = WaitGuard;
    BIND_LOG("controlsshell: bind wait for %s ends; input back once every control is released", name.c_str());
}

void waitFrame(win32::WinApplication* app, x86::CPU& cpu)
{
    uint32_t inputs;
    float sticks[4];
    win32::Gamepad::getPad(&inputs, sticks);
    if (s_wait.phase == WaitGuard)
    {
        if (customize::released(inputs, sticks))
        {
            padMasks(app, cpu, 0xff);
            s_wait.phase = WaitNone;
            BIND_LOG("controlsshell: every control released, input back");
        }
        return;
    }
    ++s_wait.frames;
    std::string why;
    customize::waitPad(&s_wait.data, inputs, sticks, &why);
    if (!why.empty())
        BIND_LOG("controlsshell: bind wait frame %d: %s", s_wait.frames, why.c_str());
    if (s_wait.data.result != customize::Ignore)
    {
        if (s_wait.phase == WaitArmed && rc::ld32(s_wait.slot + CaptureAction))
            endCapture(s_wait.slot);
        finishWait(app, cpu);
        return;
    }
    const reg32 action = rc::ld32(s_wait.slot + CaptureAction);
    if (s_wait.frames % 60 == 0)
        BIND_LOG("controlsshell: bind wait frame %d: still waiting, capture action %u, flag %u, inputs %s, sticks %.2f %.2f %.2f %.2f, "
                 "finds since arming: %d buttons, %d axes, %d hats", s_wait.frames, unsigned(action), unsigned(rc::ld8(CaptureFlag)),
                 customize::inputNames(inputs).c_str(), double(sticks[0]), double(sticks[1]), double(sticks[2]), double(sticks[3]),
                 s_wait.finds[0], s_wait.finds[1], s_wait.finds[2]);
    if (!action)
    {
        const reg32 esc = rc::ld8(EscapeFlag);
        ++s_wait.rearms;
        pf::logError("controlsshell: bind wait frame %d: the PC capture ended without a verdict (Escape flag %u, finds since arming: %d "
                     "buttons, %d axes, %d hats%s); armed again (%d)", s_wait.frames, unsigned(esc), s_wait.finds[0], s_wait.finds[1],
                     s_wait.finds[2], s_wait.finds[0] + s_wait.finds[1] + s_wait.finds[2] ? "" :
                     ": our natives 0x5B8460/0x5B8250/0x5C1720 were never reached, is src/ug2/generated older than NATIVE_OVERRIDES?",
                     s_wait.rearms);
        if (esc)
            rc::st8(EscapeFlag, 0);
        armCapture(app, cpu);
    }
}

// Left/right on Control Type: saved at once, the slot follows.
void changeType(int direction)
{
    controlstore::Backend& store = controlslink::backend();
    const int before = controlstore::type(store), after = controlsrows::nextType(before, direction);
    controlstore::setType(store, after);
    controlslink::storeChanged("the Control Type row");
    BIND_LOG("controlsshell: Control Type %s -> %s, registry.txt saved (Controls_Type %d)", controlsdata::controlTypeName(before + 1),
             controlsdata::controlTypeName(after + 1), after);
}

// Left/right on a stick row: one step, saved to registry.txt.
void changeStick(int index, int direction)
{
    namespace in = win32::controls;
    const bool right = index >= 3;
    const controlsrows::StickPart part = controlsrows::StickPart(index % 3);
    const in::StickShape before = in::stickShape(right), after = controlsrows::stepStick(before, part, direction);
    if (controlsrows::stickPercent(after, part) == controlsrows::stickPercent(before, part))
        return;
    in::setStickShape(right, after);
    controlslink::saveShape(right, part);
    BIND_LOG("controlsshell: %s %d%% -> %d%%, saved to registry.txt", StickLabels[index].plain, controlsrows::stickPercent(before, part),
             controlsrows::stickPercent(after, part));
}

// Square on an entry: Remove.
void removeHighlighted(win32::WinApplication* app, x86::CPU& cpu)
{
    const Row* r = rowOf(rc::ld32(s_screenObject + HighlightedRow));
    if (!r || r->kind != KindEntry)
        return;
    controlstore::Backend& store = controlslink::backend();
    const int typeBefore = controlstore::type(store);
    const bool changed = controlstore::removeEntry(store, controlsdata::Entry(r->index));
    BIND_LOG("controlsshell: remove %s: %s", entryName(r->index).c_str(), changed ? "written" : "nothing bound, no change");
    if (!changed)
        return;
    BIND_LOG("controlsshell: registry.txt saved: Controls_Custom, Controls_Type %d%s", controlstore::type(store),
             typeBefore != controlstore::type(store) ? " (was a Control Type, now Custom)" : "");
    controlslink::storeChanged("Customize Remove");
    refreshRows(app, cpu);
}

bool padMessage(reg32 message)
{
    for (reg32 m : PadMessages)
    {
        if (m == message)
            return true;
    }
    return false;
}

// Centre alignment; true when the flag changed.
bool centreAlign(reg32 value)
{
    const reg32 align = rc::ld32(value + TextAlign);
    if ((align & (TextAlignCentre | TextAlignRight)) == TextAlignCentre)
        return false;
    rc::st32(value + TextAlign, (align & ~TextAlignRight) | TextAlignCentre);
    return true;
}

// Every value slot centred before any row text is set.
void alignAllValues(win32::WinApplication* app, x86::CPU& cpu)
{
    for (int n = 1; n <= s_slots[ScreenCustomize]; ++n)
    {
        if (const reg32 value = findObject(app, cpu, nameHash("OPTION_DATA_" + std::to_string(n))))
            centreAlign(value);
    }
}

// The value x controls-overhaul used: row 1's after the first layout.
void takeValueReference()
{
    s_haveReference = false;
    for (size_t i = 0; i < s_rows.size() && i < 2; ++i)
    {
        const reg32 value = rc::ld32(s_rows[i].row + ValueObject);
        if (s_rows[i].kind == KindEntry && value)
        {
            s_valueReference = rc::ldf32a(rc::ld32(value + DataOffset) + DataX);
            s_haveReference = true;
        }
    }
    pf::logInfo("controlsshell: value column reference x %.1f, values centred at %.1f", s_valueReference,
                s_valueReference - ValueCentreShift);
}

// Entry values centred on controls-overhaul's column.
void centreValues(win32::WinApplication* app, x86::CPU& cpu)
{
    if (s_screen != ScreenCustomize || !s_haveReference)
        return;
    const float target = s_valueReference - ValueCentreShift;
    for (const Row& r : s_rows)
    {
        const reg32 value = rc::ld32(r.row + ValueObject);
        if (r.kind != KindEntry || !value)
            continue;
        const reg32 data = rc::ld32(value + DataOffset);
        const float x = rc::ldf32a(data + DataX);
        if (x != target)
        {
#ifdef DEV_SETTINGS
            if (s_resetLogs < 40 && x != s_valueReference)
            {
                ++s_resetLogs;
                pf::logInfo("controlsshell: value %08x of row %d at x %.1f, set to %.1f", unsigned(value), rowIndex(r.row), x, target);
            }
#endif
            rc::stf32a(data + DataX, target);
            rc::st32(value + ObjectFlags, rc::ld32(value + ObjectFlags) | DirtyFlag);
        }
        if (centreAlign(value))
        {
            rc::st32(value + ObjectFlags, rc::ld32(value + ObjectFlags) | DirtyFlag);
            setPlain(app, cpu, value, " ");
            rowTexts(app, cpu, r);
        }
    }
}

void preparePicture(const controlstore::Record& record)
{
    s_picture.record = record;
    s_picture.plan = diagram::planFor(record);
    const std::vector<uint8_t>& dxt = s_composer.compose(s_picture.plan);
    s_picture.pending = true;
    s_picture.waited = 0;
#ifdef DEV_TOOLS
    std::string drawn, skipped;
    for (int i = 0; i < diagram::SlotCount; ++i)
    {
        std::string& list = s_picture.plan.leader[i] ? drawn : skipped;
        list += (list.empty() ? "" : " ") + std::string(diagram::slotName(diagram::Slot(i)));
    }
    pf::logInfo("controlsshell: picture composed (%s, %d layers) into buffer %p, %u bytes of %dx%d DXT3; texture pack %08x, %u bytes; "
                "leaders drawn: %s; skipped: %s", diagram::cacheKey(s_picture.plan).c_str(), diagram::layerCount(s_picture.plan),
                static_cast<const void*>(dxt.data()), unsigned(dxt.size()), diagram::width(), diagram::height(), unsigned(s_pack),
                unsigned(s_packBytes), drawn.c_str(), skipped.empty() ? "none" : skipped.c_str());
#else
    (void)dxt;
#endif
}

// Loads the picture's pack once; later pictures overwrite it and its texture.
reg32 uploadPicture(win32::WinApplication* app, x86::CPU& cpu)
{
    const std::vector<uint8_t>& dxt = s_composer.picture();
    if (!s_pack)
    {
        const std::vector<uint8_t> pack = controlspack::buildPack(PictureTexture, dxt.data(), dxt.size(), diagram::width(), diagram::height());
        s_packBytes = reg32(pack.size());
        s_pack = guestMemory(pack.size());
        s_image = s_pack + s_packBytes - reg32(dxt.size());
        s_lockedRect = guestMemory(8);
        for (size_t i = 0; i < pack.size(); ++i)
            rc::st8(s_pack + reg32(i), pack[i]);
        call(app, cpu, LoadChunks, 0, { s_pack, s_packBytes });
        return controlspack::nameHash(PictureTexture);
    }
    for (size_t i = 0; i < dxt.size(); ++i)
        rc::st8(s_image + reg32(i), dxt[i]);
    const reg32 info = call(app, cpu, TextureByHash, 0, { controlspack::nameHash(PictureTexture), 0, 0 });
    const reg32 texture = info ? rc::ld32(rc::ld32(info) + 0x18) : 0;
    if (!texture)
    {
        pf::logInfo("controlsshell: picture texture not created yet; its pack holds the new picture");
        return controlspack::nameHash(PictureTexture);
    }
    const reg32 vtable = rc::ld32(texture);
    if (call(app, cpu, rc::ld32(vtable + 4 * SlotLockRect), 0, { texture, 0, s_lockedRect, 0, 0 }) != 0)
    {
        pf::logError("controlsshell: picture texture %08x would not lock", unsigned(texture));
        return controlspack::nameHash(PictureTexture);
    }
    const reg32 pitch = rc::ld32(s_lockedRect), bits = rc::ld32(s_lockedRect + 4);
    const reg32 row = reg32(diagram::width() / 4 * 16);
    for (reg32 y = 0; y < reg32(diagram::height() / 4); ++y)
    {
        for (reg32 x = 0; x < row; ++x)
            rc::st8(bits + y * pitch + x, dxt[y * row + x]);
    }
    call(app, cpu, rc::ld32(vtable + 4 * SlotUnlockRect), 0, { texture, 0 });
    return controlspack::nameHash(PictureTexture);
}

reg32 labelFont(win32::WinApplication* app, x86::CPU& cpu, reg32 object)
{
    const reg32 font = call(app, cpu, FontOf, 0, { rc::ld32(object + ObjectFont) });
    return font && rc::ld32(font + 8) ? font : 0;
}

// Shows fitted labels and the picture together, once fonts load.
bool swapPicture(win32::WinApplication* app, x86::CPU& cpu)
{
    struct Set
    {
        reg32 object;
        bool shown;
        reg32 hash;         // the game text as it is, else 0 and text
        std::string text;
        double width;
    };
    Set sets[diagram::SlotCount];
    const bool late = ++s_picture.waited > FontWait;
    for (int i = 0; i < diagram::SlotCount; ++i)
    {
        const diagram::Slot slot = diagram::Slot(i);
        const diagram::Label label = diagram::labelFor(s_picture.record, slot);
        Set& set = sets[i];
        set = { findObject(app, cpu, controlsbake::labelHash(i)), diagram::assigned(label), 0, std::string(), 0.0 };
        if (!set.object || !set.shown)
            continue;
        const reg32 font = labelFont(app, cpu, set.object);
        if (!font && !late)
            return false;
        const std::string text = diagram::labelString(label, tableText);
        const float scale = rc::ldf32a(rc::ld32(set.object + DataOffset) + DataScale);
        const reg32 measureFont = font ? font : psprompt::lastFont();
        auto width = [&](const std::string& t) { return measureFont ? measure(app, cpu, measureFont, t) * scale : 0.0; };
        set.text = diagram::fitLine(text, diagram::labelRoom(), width);
        set.width = width(set.text);
        if (set.text == text && label.first.hash && !label.second.plain && !tableText(label.first.hash).empty())
            set.hash = label.first.hash;
    }
    for (int i = 0; i < diagram::SlotCount; ++i)
    {
        const Set& set = sets[i];
        if (!set.object)
        {
            pf::logError("controlsshell: no label object for %s", diagram::slotName(diagram::Slot(i)));
            continue;
        }
        if (set.shown)
        {
            if (set.hash)
                setHash(app, cpu, set.object, set.hash);
            else
                setPlain(app, cpu, set.object, set.text);
        }
        call(app, cpu, set.shown ? ShowObject : HideObject, 0, { set.object });
#ifdef DEV_TOOLS
        const reg32 data = rc::ld32(set.object + DataOffset);
        pf::logInfo("controlsshell: label %s %s \"%s\"%s at x %.1f y %.1f, %.1f units wide (align %08x)", diagram::slotName(diagram::Slot(i)),
                    set.shown ? "shows" : "hidden,", set.text.c_str(), set.hash ? " (game text)" : "", rc::ldf32a(data + DataX),
                    rc::ldf32a(data + DataX + 4), set.width, unsigned(rc::ld32(set.object + TextAlign)));
#endif
    }
    if (const reg32 box = findObject(app, cpu, controlsbake::PictureBox))
        call(app, cpu, ShowObject, 0, { box });
    const reg32 texture = uploadPicture(app, cpu);
    if (const reg32 image = findObject(app, cpu, controlsbake::PictureObject))
    {
        call(app, cpu, SetImage, 0, { image, texture });
        call(app, cpu, ShowObject, 0, { image });
#ifdef DEV_TOOLS
        const reg32 data = rc::ld32(image + DataOffset);
        pf::logInfo("controlsshell: picture object %08x at x %.1f y %.1f size %.1f x %.1f, texture %08x%s", unsigned(image),
                    rc::ldf32a(data + DataX), rc::ldf32a(data + DataX + 4), rc::ldf32a(data + DataScale), rc::ldf32a(data + DataScale + 4),
                    unsigned(texture), late ? ", labels fitted without their font" : "");
#endif
    }
    s_picture.shown = s_picture.record;
    s_picture.haveShown = true;
    s_picture.pending = false;
    return true;
}

// Hides the PC picture and its box, then swaps ours in when ready.
void startPicture(win32::WinApplication* app, x86::CPU& cpu)
{
    for (reg32 hash : { controlsbake::PictureObject, controlsbake::PictureBox })
    {
        if (const reg32 object = findObject(app, cpu, hash))
            call(app, cpu, HideObject, 0, { object });
    }
    s_picture.haveShown = false;
    preparePicture(controlslink::activeRecord());
    swapPicture(app, cpu);
}

// Follows the record in play; old picture stays until swapped.
void updatePicture(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_picture.pending)
    {
        const controlstore::Record record = controlslink::activeRecord();
        if (s_picture.haveShown && controlstore::operator==(record, s_picture.shown))
            return;
        preparePicture(record);
    }
    swapPicture(app, cpu);
}

reg32 rowTable(bool arrows)
{
    if (!s_rowTable)
    {
        s_rowTable = guestMemory(17 * 4);
        s_arrowlessTable = guestMemory(17 * 4);
        for (reg32 i = 0; i < 17; ++i)
        {
            rc::st32(s_rowTable + 4 * i, rc::ld32(TwoValueRow + 4 * i));
            rc::st32(s_arrowlessTable + 4 * i, rc::ld32(TwoValueRow + 4 * i));
        }
        for (reg32 table : { s_rowTable, s_arrowlessTable })
        {
            rc::st32(table + 4 * SlotMessage, FakeRowMessage);
            rc::st32(table + 4 * SlotUpdate, FakeRowUpdate);
        }
        for (reg32 slot : { 4u, 5u, 7u, 8u, 9u, 10u })
            rc::st32(s_arrowlessTable + 4 * slot, FakeArrowless + 0x10 * slot);
    }
    return arrows ? s_rowTable : s_arrowlessTable;
}

void addRow(win32::WinApplication* app, x86::CPU& cpu, reg32 screen, Kind kind, int index)
{
    const reg32 row = call(app, cpu, NewObject, 0, { 0x54 });
    if (!row)
        return;
    call(app, cpu, RowConstructor, row, { screen + 0xfc, screen + 0x10c, screen + 0x114, screen + 0x11c, 1 });
    rc::st32(row, rowTable(hasArrows(kind)));
    s_rows.push_back({ row, kind, index });
    call(app, cpu, AddRow, screen, { row, 1 });
}

// Adds the screen's rows, then finishes them once.
void buildRows(win32::WinApplication* app, x86::CPU& cpu, reg32 screen)
{
    s_rows.clear();
    switch (s_screen)
    {
    case ScreenTop:
    {
        addRow(app, cpu, screen, KindJoypad, 0);
        for (int i = 0; i < 6; ++i)
            addRow(app, cpu, screen, KindStick, i);
        const int room = s_slots[ScreenTop] - 7;
        if (room < ControlsSettingCount)
            pf::logError("controlsshell: Controls has %d row slots for %d rows", s_slots[ScreenTop], TopRowCount);
        for (int i = 0; i < ControlsSettingCount && i < room; ++i)
            addRow(app, cpu, screen, KindSetting, i);
        break;
    }
    case ScreenJoypad:
        addRow(app, cpu, screen, KindControlType, 0);
        addRow(app, cpu, screen, KindCustomize, 0);
        break;
    case ScreenCustomize:
    {
        const int rows = CustomizeRowCount;
        const int shown = s_slots[ScreenCustomize] < rows ? s_slots[ScreenCustomize] : rows;
        if (shown < rows)
            pf::logError("controlsshell: Customize has %d row slots for %d entries", s_slots[ScreenCustomize], rows);
        for (int i = 0; i < shown; ++i)
        {
            if (i < EntryCount)
                addRow(app, cpu, screen, KindEntry, i);
            else
                addRow(app, cpu, screen, KindVitaButton, i - EntryCount);
        }
        break;
    }
    default:
        break;
    }
    call(app, cpu, FinishRows, screen, { 0 });
    for (const Row& r : s_rows)
    {
        if (!hasArrows(r.kind))
            hideArrows(app, cpu, r.row);
    }
    takeValueReference();
    refreshRows(app, cpu);
    centreValues(app, cpu);
    s_settle = 4;
    pf::logInfo("controlsshell: %u rows, the scroll bar counts %u, %u lines shown", unsigned(s_rows.size()),
                unsigned(rc::ld32(screen + RowCount) - 1), unsigned(rc::ld32(screen + VisibleRows)));
}

// Logs the instruction line object; the wait text with debug_instruction=wait.
void logInstruction(win32::WinApplication* app, x86::CPU& cpu, bool build)
{
    const reg32 line = findObject(app, cpu, controlsbake::instructionHash());
    if (!line)
    {
        pf::logError("controlsshell: no instruction line object %08x", unsigned(controlsbake::instructionHash()));
        return;
    }
#ifdef DEV_SETTINGS
    static const std::string mode = pf::Config::getString("debug_instruction", "");
    if (build && mode == "wait")
        setHash(app, cpu, line, controlsbake::TextPressButton);
#endif
    const reg32 data = rc::ld32(line + DataOffset);
    const reg32 master = findObject(app, cpu, controlsbake::TitleMasterObject);
    const reg32 masterData = master ? rc::ld32(master + DataOffset) : 0;
    const float rowTop = masterData ? rc::ldf32a(masterData + DataX + 4) - rc::ldf32a(masterData + 0x3c) * 0.5f : 0.0f;
    const float row = masterData ? rc::ldf32a(masterData + 0x3c) : 0.0f;
    const reg32 lines = s_screenObject ? rc::ld32(s_screenObject + VisibleRows) : 0;
    pf::logInfo("controlsshell: instruction line %08x hash %08x text %08x at x %.1f y %.1f colour %u,%u,%u,%u align %08x flags %08x%s; "
                "rows from %.1f, %.1f a row (screen row height %.1f, spacing %.1f), %u lines end at %.1f",
                unsigned(line), unsigned(controlsbake::instructionHash()), unsigned(rc::ld32(line + ObjectTextHash)),
                rc::ldf32a(data + DataX), rc::ldf32a(data + DataX + 4), unsigned(rc::ld32(data)), unsigned(rc::ld32(data + 4)),
                unsigned(rc::ld32(data + 8)), unsigned(rc::ld32(data + 12)), unsigned(rc::ld32(line + TextAlign)),
                unsigned(rc::ld32(line + ObjectFlags)), build ? " (built)" : " (shown)", rowTop, row,
                s_screenObject ? rc::ldf32a(s_screenObject + 0x108) : 0.0f, s_screenObject ? rc::ldf32a(s_screenObject + 0x128) : 0.0f,
                unsigned(lines), rowTop + float(lines) * row);
    if (!build && (rc::ld32(data + 12) == 0 || (rc::ld32(line + ObjectFlags) & 1)))
        pf::logError("controlsshell: the instruction line is shown invisible (alpha %u, flags %08x)", unsigned(rc::ld32(data + 12)),
                     unsigned(rc::ld32(line + ObjectFlags)));
}

void checkTitle(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 title = findObject(app, cpu, TitleObject);
    const reg32 have = title ? rc::ld32(title + ObjectTextHash) : 0, want = ScreenTitles[s_screen];
    if (title && have != want)
        setHash(app, cpu, title, want);
    pf::logInfo("controlsshell: title object %08x text %08x from the package, %08x wanted%s", unsigned(title), unsigned(have),
                unsigned(want), title && have != want ? ", set" : "");
}

void construct(win32::WinApplication* app, x86::CPU& cpu, Screen which)
{
    const reg32 entry = cpu.esp, data = rc::ld32(entry + 4);
    const x86::CPU saved = cpu;
    const reg32 screen = call(app, cpu, NewObject, 0, { ScreenSize });
    if (screen && !cpu.terminate)
    {
        call(app, cpu, ScreenBase, screen, { data });
        if (!s_vtable)
        {
            s_vtable = guestMemory(5 * 4);
            for (reg32 i = 0; i < 5; ++i)
                rc::st32(s_vtable + 4 * i, rc::ld32(OptionsVtable + 4 * i));
            rc::st32(s_vtable, FakeDestructor);
            rc::st32(s_vtable + 4 * 2, FakeMessage);
            rc::st32(s_vtable + 4 * 4, FakeRows);
        }
        rc::st32(screen, s_vtable);
        for (reg32 b : { 0x14cu, 0x14du, 0x14eu })
            rc::st8(screen + b, 0);
        rc::st32(screen + 0x154, 0);
        rc::st32(screen + 0x158, 0);
        rc::st8(screen + 0x2c, 1);
        rc::st32(screen + 0x134, 8);
        rc::st32(screen + 0x28, 0xed586157);
        s_screen = which;
        s_screenObject = screen;
        s_package = rc::ld32(screen + 4);
        s_haveReference = false;
        if (which == ScreenCustomize)
            alignAllValues(app, cpu);
        if (which == ScreenJoypad)
            startPicture(app, cpu);
        buildRows(app, cpu, screen);
        checkTitle(app, cpu);
        if (which == ScreenCustomize)
            logInstruction(app, cpu, true);
        s_checkIn = 10;
        pf::logInfo("controlsshell: %s screen %08x built, %u rows (%d slots), game mode %u", ScreenNames[which],
                    unsigned(screen), unsigned(s_rows.size()), s_slots[which], unsigned(rc::ld32(GameMode)));
    }
    cpu = saved;
    cpu.eax = screen;
    cpu.esp = entry + 4;
}

template <int Which>
void constructScreen(win32::WinApplication* app, x86::CPU& cpu)
{
    construct(app, cpu, Screen(Which));
}

void open(win32::WinApplication* app, x86::CPU& cpu, Screen target)
{
    pf::logInfo("controlsshell: %s opens %s", ScreenNames[s_screen], ScreenNames[target]);
    call(app, cpu, SwitchScreen, 0, { s_package, ownscreens::guestName(ScreenNames[target]), 0 });
}

// Screen message filter: Back leads to the parent screen.
void screenMessage(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp, screen = cpu.ecx, message = rc::ld32(entry + 4);
    if (screen == s_screenObject && s_screen == ScreenCustomize && padMessage(message) &&
        (s_wait.phase != WaitNone || message == MessageButton1))
    {
        const x86::CPU saved = cpu;
        if (s_wait.phase == WaitNone)
            removeHighlighted(app, cpu);
        else
            BIND_LOG("controlsshell: pad message %08x dropped during the bind wait", unsigned(message));
        cpu = saved;
        cpu.esp = entry + 0x14;
        return;
    }
    if (screen == s_screenObject && message == MessageOutroDone && rc::ld32(screen + ScreenLeaving) == MessageBack)
    {
        const Screen parent = ScreenParents[s_screen];
        if (parent != ScreenNone)
        {
            const x86::CPU saved = cpu;
            pf::logInfo("controlsshell: back from %s to %s", ScreenNames[s_screen], ScreenNames[parent]);
            call(app, cpu, SwitchScreen, 0, { s_package, ownscreens::guestName(ScreenNames[parent]), 0 });
            cpu = saved;
            cpu.esp = entry + 0x14;
            return;
        }
        pf::logInfo("controlsshell: back from %s to the Options menu", ScreenNames[s_screen]);
        s_screen = ScreenNone;
        s_rows.clear();
    }
    app->dynamic_call(OptionsMessage, cpu);
    if (cpu.terminate || s_screen != ScreenCustomize || screen != s_screenObject)
        return;
    const x86::CPU saved = cpu;
    centreValues(app, cpu);
    cpu = saved;
}

void screenRows(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp, screen = cpu.ecx;
    const x86::CPU saved = cpu;
    pf::logInfo("controlsshell: row builder slot called on %08x", unsigned(screen));
    if (screen == s_screenObject)
        refreshRows(app, cpu);
    cpu = saved;
    cpu.esp = entry + 4;
}

void screenDestructor(win32::WinApplication* app, x86::CPU& cpu)
{
    if (cpu.ecx == s_screenObject)
    {
        if (s_wait.phase != WaitNone)
        {
            const x86::CPU saved = cpu;
            if (s_wait.phase == WaitArmed)
                endCapture(s_wait.slot);
            call(app, cpu, SetPadMask, 0, { HelpBarName, 0xff });
            cpu = saved;
            s_wait.phase = WaitNone;
            pf::logInfo("controlsshell: Customize closed during the bind wait: capture ended, help bar input back");
        }
        s_screenObject = 0;
        s_screen = ScreenNone;
        s_rows.clear();
    }
    app->dynamic_call(OptionsDestructor, cpu);
}

// Row message: accept opens the next screen, nothing else.
void rowMessage(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp, row = cpu.ecx, message = rc::ld32(entry + 8);
    const x86::CPU saved = cpu;
    const Row* r = rowOf(row);
    if (r && message == MessageAccept && r->kind == KindJoypad)
        open(app, cpu, ScreenJoypad);
    else if (r && message == MessageAccept && r->kind == KindCustomize)
        open(app, cpu, ScreenCustomize);
    else if (r && message == MessageAccept && r->kind == KindEntry && s_wait.phase == WaitNone)
        startWait(app, cpu, *r);
    else if (r && hasArrows(r->kind) && (message == MessageLeft || message == MessageRight) && s_wait.phase == WaitNone)
    {
        const int direction = message == MessageLeft ? -1 : 1;
        if (r->kind == KindControlType)
            changeType(direction);
        else if (r->kind == KindVitaButton || r->kind == KindSetting)
        {
            const char* key = r->kind == KindVitaButton ? VitaButtons[r->index].key : ControlsSettings[r->index];
            if (!options::cfgCycle(key, direction))
                pf::logError("controlsshell: %s not saved", key);
        }
        else
            changeStick(r->index, direction);
        call(app, cpu, rc::ld32(rc::ld32(row) + 4 * SlotBase), row, { message });
    }
    if (r && rowOf(row))
        rowTexts(app, cpu, *rowOf(row));
    cpu = saved;
    cpu.esp = entry + 12;
}

void rowUpdate(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp, row = cpu.ecx;
    const x86::CPU saved = cpu;
    if (const Row* r = rowOf(row))
        rowTexts(app, cpu, *r);
    cpu = saved;
    cpu.esp = entry + 4;
}

// The row's own method, then its arrows hidden again.
template <int Slot>
void arrowlessSlot(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 row = cpu.ecx;
    app->dynamic_call(rc::ld32(TwoValueRow + 4 * Slot), cpu);
    if (cpu.terminate)
        return;
    const x86::CPU saved = cpu;
    hideArrows(app, cpu, row);
    cpu = saved;
}

// The objects baked hidden, read back from the loaded screen.
void checkHidden(win32::WinApplication* app, x86::CPU& cpu)
{
    const std::vector<reg32> options = { nameHash("ColorCal_Blurb"), nameHash("Colour_Calibration_Backing"), nameHash("PLAYER") };
    const std::vector<reg32> joypad = { 0xbaa4198e, 0x0f274b82, nameHash("PLAYER") };
    int hidden = 0, total = 0;
    for (reg32 hash : s_screen == ScreenJoypad ? joypad : options)
    {
        if (const reg32 object = findObject(app, cpu, hash))
        {
            ++total;
            hidden += (rc::ld32(object + ObjectFlags) & 1) ? 1 : 0;
        }
    }
    pf::logInfo("controlsshell: %s check: %d of %d baked-hidden objects hidden, highlighted row %d of %u", ScreenNames[s_screen],
                hidden, total, rowIndex(rc::ld32(s_screenObject + HighlightedRow)), unsigned(s_rows.size()));
}

}

void configure(win32::WinApplication& app)
{
    app.registerMethod(FakeConstructor + 0x10 * ScreenTop, { "controls screen", constructScreen<ScreenTop> });
    app.registerMethod(FakeConstructor + 0x10 * ScreenJoypad, { "joypad configuration screen", constructScreen<ScreenJoypad> });
    app.registerMethod(FakeConstructor + 0x10 * ScreenCustomize, { "customize screen", constructScreen<ScreenCustomize> });
    app.registerMethod(FakeDestructor, { "controls screen destructor", screenDestructor });
    app.registerMethod(FakeMessage, { "controls screen message", screenMessage });
    app.registerMethod(FakeRows, { "controls screen rows", screenRows });
    app.registerMethod(FakeRowMessage, { "controls row message", rowMessage });
    app.registerMethod(FakeRowUpdate, { "controls row update", rowUpdate });
    app.registerMethod(FakeArrowless + 0x10 * 4, { "controls row layout", arrowlessSlot<4> });
    app.registerMethod(FakeArrowless + 0x10 * 5, { "controls row show", arrowlessSlot<5> });
    app.registerMethod(FakeArrowless + 0x10 * 7, { "controls row focus", arrowlessSlot<7> });
    app.registerMethod(FakeArrowless + 0x10 * 8, { "controls row unfocus", arrowlessSlot<8> });
    app.registerMethod(FakeArrowless + 0x10 * 9, { "controls row select", arrowlessSlot<9> });
    app.registerMethod(FakeArrowless + 0x10 * 10, { "controls row refocus", arrowlessSlot<10> });

    for (int s = ScreenTop; s < ScreenCount; ++s)
    {
        const Screen which = Screen(s);
        ownscreens::addScreen(ScreenNames[s], FakeConstructor + 0x10 * reg32(s));
        ownscreens::addCopy(ScreenSources[s], ScreenNames[s], [which](fepackage::Package& p)
        {
            fepackage::Package donor;
            const bool haveDonor = which == ScreenCustomize && ownscreens::loadPackage(controlsbake::InstructionDonor, &donor);
            const controlsbake::Result r = which == ScreenJoypad ? controlsbake::joypad(p, ScreenTitles[which]) :
                controlsbake::options(p, ScreenTitles[which], which == ScreenCustomize, CustomizeRowCount, haveDonor ? &donor : nullptr);
            if (which == ScreenCustomize && r.instruction < 0)
                pf::logError("controlsshell: no instruction line: %s", r.error.c_str());
            s_slots[which] = r.slots;
#ifdef DEV_TOOLS
            if (which == ScreenJoypad)
                pf::logInfo("controlsshell: %s picture at x %.1f y %.1f size %.1f x %.1f (screen centre x %.1f), box x %.1f..%.1f "
                            "y %.1f..%.1f, %d of %d labels made", ScreenNames[which], r.picture[0], r.picture[1], r.picture[2],
                            r.picture[3], r.centreX, r.box[0], r.box[2], r.box[1], r.box[3], r.labels, diagram::SlotCount);
#endif
            pf::logInfo("controlsshell: %s baked: %u objects hidden, title text %08x, %d row slots (%d slot objects copied "
                        "from slot %d), %d row bars fitted, %u buttons, instruction line object %ld, %d highlights steady",
                        ScreenNames[which], unsigned(r.hidden), unsigned(ScreenTitles[which]), r.slots, r.cloned,
                        controlsbake::SlotModel, r.bars, unsigned(p.buttonCount()), r.instruction, r.steady);
        });
    }
    // Renames the string Options > Controls pushes; table keeps a copy.
    rc::st32(PcControllerEntryName, ownscreens::guestName("UI_Options_PC_Controller.fng"));
    const char* const top = ScreenNames[ScreenTop];
    for (size_t i = 0; i <= strlen(top); ++i)
        rc::st8(PcControllerName + reg32(i), uint8_t(top[i]));
    pf::logInfo("controlsshell: Options > Controls opens %s (Joypad Configuration, Customize below it)", top);
}

namespace {

// PC capture finds (0x5CA720): ours while our slot is armed.
bool takeFind(reg32 slot, int kind, const std::function<customize::Verdict(std::string*)>& judge)
{
    if (s_wait.phase != WaitArmed || slot != s_wait.slot)
        return false;
    ++s_wait.finds[kind];
    std::string why;
    const customize::Verdict v = judge(&why);
    BIND_LOG("controlsshell: bind wait frame %d: PC capture: %s", s_wait.frames, why.c_str());
    if (v != customize::Ignore)
        endCapture(slot);
    return true;
}

}

bool captureButton(reg32 slot, int button)
{
    return takeFind(slot, 0, [&](std::string* why) { return customize::waitButton(&s_wait.data, button, why); });
}

bool captureAxis(reg32 slot, int32_t axisId, int direction)
{
    return takeFind(slot, 1, [&](std::string* why) { return customize::waitAxis(&s_wait.data, axisId, direction, why); });
}

bool captureHat(reg32 slot, int hat)
{
    return takeFind(slot, 2, [&](std::string* why) {
        const reg32 words = rc::ld32(PadWords), pov = words ? rc::ld32(words + rc::ld32(slot + PadIndex) * PadStateBytes + PovOffset + 4 * hat) : 0;
        return customize::waitHat(&s_wait.data, hat, pov, why);
    });
}

bool inputHeld()
{
    return s_wait.phase != WaitNone;
}

void frame(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::Gamepad::setTouchBlocked(!customize::touchReachesGame(inputHeld()));
    if (s_screen == ScreenNone)
        return;
    const x86::CPU saved = cpu;
    if (s_settle > 0)
    {
        s_settle = s_settle > 0 ? s_settle - 1 : 0;
        refreshRows(app, cpu);
    }
    for (const Row& r : s_rows)
    {
        if (!hasArrows(r.kind))
            hideArrows(app, cpu, r.row);
    }
    centreValues(app, cpu);
    if (s_wait.phase != WaitNone && s_screen == ScreenCustomize)
        waitFrame(app, cpu);
    if (s_screen == ScreenJoypad)
        updatePicture(app, cpu);
    if (s_checkIn > 0 && --s_checkIn == 0)
    {
        checkHidden(app, cpu);
        if (s_screen == ScreenCustomize)
            logInstruction(app, cpu, false);
    }
    cpu = saved;
}

}}
