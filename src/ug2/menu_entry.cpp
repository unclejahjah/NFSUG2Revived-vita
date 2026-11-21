// Vita Settings in the game's own screens. The main menu gets one more icon, the first, to the left of Career, made
// the way the game makes its own (as NFSU2 Revived's split screen adds its icon). It is an Options icon, so Cross
// on it opens the game's UI_OptionsMain carousel, which then shows the groups of options_menu.cpp instead of the
// game's categories; a group opens the game's UI_Options list with that group's settings as its rows, changed with
// left and right like the game's, and a text row opens the Vita keyboard on Cross. Leaving a list saves; the last
// icon saves and starts the game again, which applies the settings.
//
// UI_Main's setup 0x4AEE30 (thiscall, the menu in ecx) makes its 7 icons in order: Career, Quick Race, Online, LAN,
// Customize, Profile Manager and Options, each a 0x48-byte item from 0x51F670 (icon texture hash, label hash, 0)
// with the item's own class, added with the menu's method 0x18 (0x50DAB0: the icon scroller's 0x543C10 gives it the
// package's next free icon object, "%s%d"). Then it selects the icon the package last had (0x50B8A0), 2 the first
// time (0x50B8C0), through the scroller's method 0x20 (0x538A30, from 1). Ours is added before the game's, so the
// game's icons and the remembered one are each one further. UI_OptionsMain's setup 0x4B6230 makes its category
// icons the same way; each category's class sets the category (0x83AA04) that UI_Options's setup 0x4B8470 builds
// rows for: 0x54-byte rows from 0x51E230, added with 0x54EF30 (row, 1), which binds their name (+0x2C) and value
// (+0x30) text objects. A row's method 1 (thiscall (x, message), ret 8) takes left and right, method 3 sets its
// texts with 0x50C900 (object, text hash), and the text of a hash comes from 0x4FF9D0 (edx), answered here for ours.

#include "menu_entry.h"
#include "options_menu.h"

#include <lib/gamepad.h>
#include <platform/log.h>
#include <lib/winapp.h>
#include <recomp.h>

#include <map>
#include <string>
#include <vector>

namespace ug2
{

void sub_4aee30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4b6230_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4b8470_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4ff9d0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

using x86::reg32;

const reg32 OperatorNew      = 0x00575620;  // cdecl (size)
const reg32 ItemConstructor  = 0x0051F670;  // thiscall (icon, label, 0)
const reg32 PackageIconGet   = 0x0050B8A0;  // cdecl (package name): the icon it had last, 0 for none
const reg32 PackageIconSet   = 0x0050B8C0;  // cdecl (package name, icon)
const reg32 FindObject       = 0x005379C0;  // cdecl (package, object hash)
const reg32 HideObject       = 0x0050CA00;  // cdecl (object)
const reg32 StringHash       = 0x00505450;  // cdecl (name)
const reg32 SetTextHash      = 0x0050C900;  // cdecl (object, text hash)
const reg32 RowConstructor   = 0x0051E230;  // thiscall (screen +0xFC, +0x10C, +0x114, +0x11C, 1)
const reg32 AddRow           = 0x0054EF30;  // thiscall on the screen (row, 1)
const reg32 SelectRow        = 0x00539650;  // thiscall on the screen (0)
const reg32 GameRowClass     = 0x00791C90;  // a toggle row's class: 18 methods, copied for ours
const reg32 SoundManager     = 0x0082B884;  // the front end's sounds (0x458520: thiscall (sound))
const reg32 SoundPlay        = 0x00458520;
const reg32 SoundTable       = 0x00837FE0;  // signed byte per event (0x1B apart) and menu style (menu +0x30)
const reg32 OptionsItem      = 0x00790C40;  // the main menu's Options icon class: its React opens UI_OptionsMain
const reg32 CategoryItem     = 0x007918C8;  // an options category class (category 2, a list of rows)
const reg32 OptionsIcon      = 0x3058FE37;  // the main menu's Options icon texture
const reg32 MainReact        = 0x004F98B0;  // UI_Main's React (thiscall; message, 3 more; ret 0x10)
const reg32 OptionsMainReact = 0x004B6080;  // UI_OptionsMain's
const reg32 OptionsReact     = 0x0049C690;  // UI_Options's
const reg32 PeekMessageSlot  = 0x00783330;  // import slot of PeekMessageA, called every frame
const reg32 RowReactAddress  = 0x00FF0010;  // our row methods, at addresses no code has
const reg32 RowTextsAddress  = 0x00FF0020;
const reg32 Accept           = 0x0C407210;  // Cross
const reg32 Left             = 0x9120409E;
const reg32 Right            = 0xB5971BF1;
const reg32 LeaveList        = 0x11111111;  // UI_Options goes back to UI_OptionsMain
const reg32 LabelHash        = 0x56495441;  // our main menu label ('VITA')
const reg32 OurHashes        = 0x56540000;  // our other texts: kind << 12 | group << 8 | item << 1 | parity
const reg32 RowSize          = 0x58;        // the game's 0x54, then our item index
const unsigned AcceptSound   = 8;
const int MaxGroups          = 15;

// The game's own category icons for ours, and the last for Save and Restart.
const reg32 GroupIcons[] = { 0x8A006328, 0x4DF98FB2, 0xD708EFEF, 0xF37AF144, 0xA04A7B26, 0x51009E20 };
const reg32 RestartIcon = 0x83070A37;
const char RestartLabel[] = "Save and Restart";

enum TextKind { KindName = 0, KindValue = 1, KindGroup = 2 };

reg32 hashOf(TextKind kind, int g, int i, int parity) { return OurHashes | reg32(kind) << 12 | reg32(g) << 8 | reg32(i) << 1 | reg32(parity); }

win32::MethodPtr s_mainReact = nullptr;
win32::MethodPtr s_optionsMainReact = nullptr;
win32::MethodPtr s_optionsReact = nullptr;
win32::MethodPtr s_peekMessage = nullptr;

reg32 s_menu = 0, s_item = 0;           // UI_Main and our icon
bool s_vita = false;                    // UI_OptionsMain and UI_Options show ours
reg32 s_optionsMain = 0;
std::vector<reg32> s_groupItems;        // UI_OptionsMain's icons, by group; the last is Save and Restart
int s_group = 0;
reg32 s_options = 0;                    // UI_Options
reg32 s_rowClass = 0;
reg32 s_editRow = 0;                    // the row whose text the keyboard edits
std::map<reg32, reg32> s_texts;         // our hashes' texts in guest memory, 128 bytes each
std::map<reg32, std::string> s_shown;   // the value each row shows, by (group, item) hash
std::map<reg32, int> s_parity;

reg32 callGuest(win32::WinApplication* app, x86::CPU& cpu, reg32 function, reg32 ecx, std::initializer_list<reg32> args)
{
    x86::CPU saved = cpu;
    reg32 esp = cpu.esp - 64;   // below anything the caller still reads
    const reg32* a = args.begin();
    for (size_t i = args.size(); i-- > 0;)
        rc::st32(esp -= 4, a[i]);
    esp -= 4;
    cpu.esp = esp;
    cpu.ecx = ecx;
    app->dynamic_call(function, cpu);
    const reg32 result = cpu.eax;
    cpu = saved;
    return result;
}

// Our text of a hash, kept in guest memory for the game to read.
bool setText(win32::WinApplication* app, x86::CPU& cpu, reg32 hash, const std::string& text)
{
    const size_t Size = 128;
    reg32& buffer = s_texts[hash];
    if (!buffer)
        buffer = callGuest(app, cpu, OperatorNew, 0, { reg32(Size) });
    if (!buffer)
        return false;
    const size_t n = text.size() < Size - 1 ? text.size() : Size - 1;
    for (size_t i = 0; i < n; ++i)
        rc::st8(buffer + reg32(i), x86::reg8(text[i]));
    rc::st8(buffer + reg32(n), 0);
    return true;
}

void playAccept(win32::WinApplication* app, x86::CPU& cpu, reg32 menu)
{
    const reg32 manager = rc::ld32(SoundManager);
    const int sound = int(int8_t(rc::ld8(SoundTable + AcceptSound * 0x1B + rc::ld32(menu + 0x30))));
    if (manager && sound >= 0)
        callGuest(app, cpu, SoundPlay, manager, { reg32(sound) });
}

bool isOurRow(reg32 row)
{
    return row && s_rowClass && rc::ld32(row) == s_rowClass;
}

int rowItem(reg32 row)
{
    return int(rc::ld32(row + 0x54));
}

// A row's name and value. The game redraws a text only when its hash changes, so a changed value takes the other
// parity.
void setRowTexts(win32::WinApplication* app, x86::CPU& cpu, reg32 row)
{
    const int g = s_group, i = rowItem(row);
    if (i < 0 || i >= options::itemCount(g))
        return;
    const reg32 name = hashOf(KindName, g, i, 0);
    setText(app, cpu, name, options::itemLabel(g, i));
    const reg32 key = hashOf(KindValue, g, i, 0);
    const std::string value = options::itemValue(g, i);
    int& parity = s_parity[key];
    if (s_shown.count(key) && s_shown[key] != value)
        parity ^= 1;
    s_shown[key] = value;
    const reg32 shown = hashOf(KindValue, g, i, parity);
    setText(app, cpu, shown, value);
    callGuest(app, cpu, SetTextHash, 0, { rc::ld32(row + 0x2C), name });
    callGuest(app, cpu, SetTextHash, 0, { rc::ld32(row + 0x30), shown });
}

void openEditor(reg32 row)
{
    const int g = s_group, i = rowItem(row);
    if (s_editRow || i < 0 || i >= options::itemCount(g) || !options::itemIsText(g, i))
        return;
    const std::string text = options::itemText(g, i);
    if (win32::Gamepad::requestText(options::itemLabel(g, i), text.c_str(), reg32(options::itemMaxLength(g, i)),
                                    options::itemIsSecret(g, i)))
        s_editRow = row;
}

// Our row's method 1 (thiscall (x, message), ret 8): left and right change a choice.
void rowReact(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 row = cpu.ecx;
    const reg32 message = rc::ld32(cpu.esp + 8);
    const int g = s_group, i = rowItem(row);
    if ((message == Left || message == Right) && i >= 0 && i < options::itemCount(g) && !options::itemIsText(g, i))
    {
        options::cycle(g, i, message == Right ? 1 : -1);
        callGuest(app, cpu, rc::ld32(rc::ld32(row) + 0x40), row, { message });    // the game's blink
        setRowTexts(app, cpu, row);
    }
    else if (message == Accept)
    {
        openEditor(row);
    }
    cpu.esp += 4 + 8;
}

// Our row's method 3 (thiscall, ret).
void rowTexts(win32::WinApplication* app, x86::CPU& cpu)
{
    setRowTexts(app, cpu, cpu.ecx);
    cpu.esp += 4;
}

// UI_Main's React: Cross on our icon opens UI_OptionsMain (its class does) with our groups.
void mainReact(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 menu = cpu.ecx;
    if (s_item && menu == s_menu && rc::ld32(cpu.esp + 4) == Accept)
    {
        s_vita = rc::ld32(menu + 0x58) == s_item;
        if (s_vita)
        {
            options::load();
            pf::logInfo("menu entry: Vita Settings");
        }
    }
    s_mainReact(app, cpu);
}

// UI_OptionsMain's React: Cross on a group remembers it, on the last icon saves and starts again.
void optionsMainReact(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 menu = cpu.ecx;
    if (s_vita && menu == s_optionsMain && rc::ld32(cpu.esp + 4) == Accept)
    {
        const reg32 current = rc::ld32(menu + 0x58);
        for (size_t g = 0; g < s_groupItems.size(); ++g)
        {
            if (s_groupItems[g] != current)
                continue;
            if (g + 1 == s_groupItems.size())
            {
                playAccept(app, cpu, menu);
                options::save();
                options::restart();
                cpu.esp += 4 + 16;
                return;
            }
            s_group = int(g);
        }
    }
    s_optionsMainReact(app, cpu);
}

// UI_Options's React: Cross on a text row opens the keyboard; leaving the list saves.
void optionsReact(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 screen = cpu.ecx;
    if (s_vita && screen == s_options)
    {
        const reg32 message = rc::ld32(cpu.esp + 4);
        if (message == Accept && isOurRow(rc::ld32(screen + 0x54)))
            openEditor(rc::ld32(screen + 0x54));
        else if (message == LeaveList)
        {
            if (options::dirty())
                options::save();
            s_options = 0;
            s_editRow = 0;
        }
    }
    s_optionsReact(app, cpu);
}

// Every frame: the keyboard's text.
void peekMessageHook(win32::WinApplication* app, x86::CPU& cpu)
{
    s_peekMessage(app, cpu);
    if (!s_editRow)
        return;
    char text[256];
    const win32::Gamepad::TextResult result = win32::Gamepad::takeText(text, sizeof(text));
    if (result == win32::Gamepad::TextPending || result == win32::Gamepad::TextNone)
        return;
    const reg32 row = s_editRow;
    s_editRow = 0;
    if (result == win32::Gamepad::TextConfirmed)
        options::setText(s_group, rowItem(row), text);
    for (char& c : text)
        c = 0;
    if (s_options && isOurRow(row))
        setRowTexts(app, cpu, row);
}

}

namespace menu
{

void configure(win32::WinApplication& app)
{
    if (!options::enabled())
        return;
    s_mainReact = app.replaceMethod(MainReact, mainReact);
    s_optionsMainReact = app.replaceMethod(OptionsMainReact, optionsMainReact);
    s_optionsReact = app.replaceMethod(OptionsReact, optionsReact);
    s_peekMessage = app.replaceMethod(rc::ld32(PeekMessageSlot), peekMessageHook);
    app.registerMethod(RowReactAddress, { "vita settings row react", rowReact });
    app.registerMethod(RowTextsAddress, { "vita settings row texts", rowTexts });
    pf::logInfo("menu entry: Vita Settings in the main menu");
}

}

namespace native
{

// UI_Main's setup: ours first, then the game's.
void sub_4aee30(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 menu = cpu.ecx;
    s_vita = false;
    if (!options::enabled())
    {
        sub_4aee30_guest(app, cpu);
        return;
    }
    setText(app, cpu, LabelHash, "Vita Settings");
    const reg32 package = rc::ld32(menu + 4);
    const reg32 remembered = callGuest(app, cpu, PackageIconGet, 0, { package });
    reg32 item = s_texts[LabelHash] ? callGuest(app, cpu, OperatorNew, 0, { 0x48 }) : 0;
    if (item)
    {
        callGuest(app, cpu, ItemConstructor, item, { OptionsIcon, LabelHash, 0 });
        rc::st32(item, OptionsItem);
        callGuest(app, cpu, rc::ld32(rc::ld32(menu) + 0x18), menu, { item });
    }
    s_menu = menu;
    s_item = item;
    sub_4aee30_guest(app, cpu);
    // The first time, the game's choice (2) is the icon it meant, now one further.
    if (item && !remembered)
    {
        callGuest(app, cpu, PackageIconSet, 0, { package, 3 });
        const reg32 scroller = menu + 0x4C;
        callGuest(app, cpu, rc::ld32(rc::ld32(scroller) + 0x20), scroller, { 3 });
    }
    pf::logInfo("menu entry: Vita Settings icon %s", item ? "added to the main menu" : "not added");
}

// UI_OptionsMain's setup: our groups, then Save and Restart, as the game adds its categories.
void sub_4b6230(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 menu = cpu.ecx;
    if (!s_vita)
    {
        sub_4b6230_guest(app, cpu);
        return;
    }
    s_optionsMain = menu;
    s_groupItems.clear();
    const int groups = options::groupCount() < MaxGroups ? options::groupCount() : MaxGroups;
    for (int g = 0; g <= groups; ++g)
    {
        const bool restart = g == groups;
        const reg32 label = hashOf(KindGroup, g, 0, 0);
        setText(app, cpu, label, restart ? RestartLabel : options::groupTitle(g));
        const reg32 icon = restart ? RestartIcon : GroupIcons[g % (sizeof(GroupIcons) / sizeof(GroupIcons[0]))];
        const reg32 item = callGuest(app, cpu, OperatorNew, 0, { 0x48 });
        if (item)
        {
            callGuest(app, cpu, ItemConstructor, item, { icon, label, 0 });
            rc::st32(item, CategoryItem);
            callGuest(app, cpu, rc::ld32(rc::ld32(menu) + 0x18), menu, { item });
        }
        s_groupItems.push_back(item);
    }
    const reg32 package = rc::ld32(menu + 4);
    const reg32 remembered = callGuest(app, cpu, PackageIconGet, 0, { package });
    const reg32 scroller = menu + 0x4C;
    callGuest(app, cpu, rc::ld32(rc::ld32(scroller) + 0x20), scroller, { remembered });
    callGuest(app, cpu, rc::ld32(rc::ld32(menu) + 0x10), menu, {});
    cpu.esp += 4;
}

// UI_Options's setup: the game's hides, then our group's rows.
void sub_4b8470(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 screen = cpu.ecx;
    if (!s_vita)
    {
        sub_4b8470_guest(app, cpu);
        return;
    }
    const reg32 package = rc::ld32(screen + 4);
    const reg32 hidden[] = { 0x010CBA23, 0x97B71DDE, 0x7326E44C, 0xC90DC4AB, 0, 0, 0, 0x444969FD, 0x444969FE };
    const reg32 hiddenNames[] = { 0, 0, 0, 0, 0x007945B4, 0x007945A4, 0x00794588, 0, 0 };
    for (size_t k = 0; k < sizeof(hidden) / sizeof(hidden[0]); ++k)
    {
        const reg32 hash = hiddenNames[k] ? callGuest(app, cpu, StringHash, 0, { hiddenNames[k] }) : hidden[k];
        callGuest(app, cpu, HideObject, 0, { callGuest(app, cpu, FindObject, 0, { package, hash }) });
    }
    rc::st8(screen + 0x14C, 0);
    if (!s_rowClass)
    {
        s_rowClass = callGuest(app, cpu, OperatorNew, 0, { 18 * 4 });
        for (reg32 k = 0; s_rowClass && k < 18; ++k)
            rc::st32(s_rowClass + k * 4, rc::ld32(GameRowClass + k * 4));
        if (s_rowClass)
        {
            rc::st32(s_rowClass + 1 * 4, RowReactAddress);
            rc::st32(s_rowClass + 3 * 4, RowTextsAddress);
        }
    }
    s_options = screen;
    s_editRow = 0;
    const int count = s_group < options::groupCount() ? options::itemCount(s_group) : 0;
    for (int i = 0; s_rowClass && i < count; ++i)
    {
        const reg32 row = callGuest(app, cpu, OperatorNew, 0, { RowSize });
        if (!row)
            break;
        callGuest(app, cpu, RowConstructor, row, { screen + 0xFC, screen + 0x10C, screen + 0x114, screen + 0x11C, 1 });
        rc::st32(row, s_rowClass);
        rc::st32(row + 0x54, reg32(i));
        callGuest(app, cpu, AddRow, screen, { row, 1 });
    }
    callGuest(app, cpu, SelectRow, screen, { 0 });
    pf::logInfo("menu entry: %s, %d settings", options::groupTitle(s_group), count);
    cpu.esp += 4;
}

// The game's text of a label hash (edx; the text in eax): ours for our hashes.
void sub_4ff9d0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (cpu.edx == LabelHash || (cpu.edx & 0xFFFF0000) == OurHashes)
    {
        const std::map<reg32, reg32>::const_iterator it = s_texts.find(cpu.edx);
        if (it != s_texts.end() && it->second)
        {
            cpu.eax = it->second;
            cpu.esp += 4;
            return;
        }
    }
    sub_4ff9d0_guest(app, cpu);
}

}

}
