#ifndef UG2_PSBARLOGIC_H_
#define UG2_PSBARLOGIC_H_

// The PS2 bottom bar as data: its two prompts and where they sit. No game access, so the host tests cover it.

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ug2 { namespace psbar {

const uint32_t HelpText = 0x00169268;

// A prompt: the language text, and the front-end button the PS2 shows before it.
struct Prompt
{
    uint32_t    text;
    const char* token;      // JOY_EVENT name without the dollar signs
};

// The messages the PC bar items send (from 0x52D170) and the front-end button the PS2 shows for them.
const uint32_t MsgAccept = 0x406415E3, MsgBack = 0x911AB364, MsgButton0 = 0xC519BFBF, MsgButton1 = 0xC519BFC0;

// Button token for an item message, nullptr when the PS2 bar never shows it (Back, Button0, unnamed messages).
inline const char* tokenForMessage(uint32_t message)
{
    if (message == MsgAccept)
        return "JOY_EVENT_FENG_SELECT";
    if (message == MsgButton1)
        return "JOY_EVENT_FENG_BUTTON1";
    return nullptr;
}

// Bits the PS2 bar never shows: Quit Game (user rule) and EA Messenger (no EA online on the Vita).
const uint32_t DroppedBits = 0x1 | 0x4000;

// Prompts of our own screens by name, overriding the PC flags: Remove on the Controls Customize screen is Square + 0x7758F62D.
struct ScreenPrompt
{
    const char* screen;
    Prompt      prompt;
};

const ScreenPrompt ScreenPrompts[] =
{
    { "UI_ControlsCustomize.fng", { 0x7758F62D, "JOY_EVENT_FENG_BUTTON1" } },
};

// Bits the PC bar fill never gives a text.
inline bool bitHasText(int bit)
{
    return !(bit == 15 || bit == 16 || bit == 17 || bit == 31);
}

// The second prompt of a screen: a table row, else the first flag bit whose PC message has a PS2 button. Square actions come
// before Cross ones, as the PS2 shows Customize rather than Continue on car select.
template <typename MessageOfBit, typename TextOfBit>
Prompt secondPrompt(const char* screen, uint32_t flags, MessageOfBit messageOfBit, TextOfBit textOfBit)
{
    for (const ScreenPrompt& s : ScreenPrompts)
    {
        if (s.prompt.text && screen && strcmp(s.screen, screen) == 0)
            return s.prompt;
    }
    Prompt cross = { 0, nullptr };
    for (int bit = 0; bit < 32; ++bit)
    {
        const uint32_t mask = 1u << bit;
        if (!(flags & mask) || (mask & DroppedBits) || !bitHasText(bit))
            continue;
        const char* token = tokenForMessage(messageOfBit(mask));
        if (!token)
            continue;
        if (strcmp(token, "JOY_EVENT_FENG_BUTTON1") == 0)
            return { textOfBit(mask), token };
        if (!cross.token)
            cross = { textOfBit(mask), token };
    }
    return cross;
}

// The PS2 bar group of the screen packages (UI_OptionsController.fng) copied into UI_PC_Help_Bar.fng. Hashes the bar package
// already has (the d-pad group and panels) get a salt on the copy. The PC's name and money texts are renamed (PcOldSalt) first,
// so the PS2 name and money take their hashes and the PC bank/name switcher (0x537E60) finds, fills and fades them.
const uint32_t Salt = 0x5A5A5A5A, PcOldSalt = 0xA5A5A5A5;
const uint32_t PcMoney = 0xEB3A688A, PcName = 0xEB406FEC;
const uint32_t PsRoot = 0x02DDF58C, PsName = PcName, PsMoney = PcMoney, PsDpad = 0x2BAC0CEE ^ Salt;
const uint32_t PsHelpWord = 0x00169268, PsHelpIcon = 0x35E8F0B2, PsSecondWord = 0x010CBA23, PsSecondIcon = 0x97B71DDE;
const uint32_t PcRealGroup = 0x3B949CD0;    // the PC art group the fill shows; renamed so the fill finds nothing

// Whether Help does anything on a screen. Two kinds: the general help popup, driven by the screen's help id (screen +0x28,
// set from its table entry) when the language table has a text for it, and the screen's own response to the help event
// (the Tip item bit 0x400000, PAD_BUTTON4, which the tuning screens set). liveHelp is the screen's help id, 0 when unknown.
const uint32_t TipBit = 0x00400000;

// Screens that open the highlighted setting's info on the help event, with or without offering Tip: the part-tuning
// sub-screens. No per-row signal was found, so the whole screen counts; Master and the Dyno screens do not.
const char* const OwnHelpScreens[] =
{
    "UI_PerformanceTuning_Sliders.fng", "UI_PerformanceTuning_Graph.fng", "UI_PerformanceTuning_NOS.fng",
    "UI_PerformanceTuning_Drivetrain.fng", "UI_PerformanceTuning_Setting.fng",
};

// Package names compare without case (the game hashes them upper-cased).
inline bool sameName(const char* a, const char* b)
{
    if (!a || !b)
        return false;
    for (; *a && *b && (*a | 0x20) == (*b | 0x20); ++a, ++b)
        ;
    return !*a && !*b;
}

inline bool hasOwnHelp(const char* screen)
{
    for (const char* name : OwnHelpScreens)
    {
        if (sameName(name, screen))
            return true;
    }
    return false;
}

struct HelpVerdict
{
    bool        available;
    const char* kind;       // "general", "tip" or "none"
    const char* why;
};

inline HelpVerdict helpVerdict(uint32_t liveHelp, bool resolves, uint32_t flags, const char* screen)
{
    if (liveHelp && resolves)
        return { true, "general", "the screen's help id resolves to a text" };
    if (flags & TipBit)
        return { true, "tip", "the screen offers the Tip item (PAD_BUTTON4)" };
    if (hasOwnHelp(screen))
        return { true, "screen", "the tuning screen opens its part info on the help event" };
    if (!liveHelp)
        return { false, "none", "the screen's help id is 0 and it offers no Tip" };
    return { false, "none", "the screen's help id has no text and it offers no Tip" };
}

// In-race and free-roam overlays: no bar, prompts in the panel's button row. Every other screen keeps the bar.
const char* const PanelRowScreens[] =
{
    "UI_Pause.fng", "UI_PauseOptions.fng", "UI_PauseOptionsMain.fng",
    "UI_PostRace.fng", "UI_PostRaceStats.fng", "UI_PostRaceResults.fng", "UI_PostRace_TournResults.fng",
    "UI_PostRace_TournStandings.fng", "UI_PostRaceReward.fng", "UI_SponsorPopup.fng", "UI_Sponsorship_new.fng",
    "UI_OLPreRaceStart.fng", "UI_ReplayControl.fng",
    "UI_SMS_Mailbox.fng", "UI_InGame_WorldMap.fng",
    "UI_Status_Master.fng", "UI_Status_Career.fng", "UI_Status_Region.fng", "UI_Status_DVD.fng",
    "UI_EngageMessageDialog.fng", "UI_EngageEventDialog.fng", "UI_EngageRaceDialog.fng", "UI_EngageShopDialog.fng",
    "UI_Showcase_Preview.fng", "UI_Showcase_DPAD.fng",
};

inline bool panelRowScreen(const char* screen)
{
    for (const char* name : PanelRowScreens)
    {
        if (sameName(name, screen))
            return true;
    }
    return false;
}

// The PC's in-game layouts of its bar items (0x5428B0): flag 0x10000 picks one by script name.
enum Layout { LayoutPauseOptions, LayoutInGame, LayoutEventResults, LayoutPauseMenuMain, LayoutCount };
const char* const LayoutScripts[LayoutCount] = { "Pause_Options", "In_Game", "Event_Results", "Pause_Menu_Main" };
const uint32_t LayoutBit = 0x10000;

// The PC's layout (0x5428B0); In_Game without the layout bit.
inline Layout layoutOf(uint32_t flags)
{
    if (!(flags & LayoutBit) || (flags & 0x8000))
        return LayoutInGame;
    if (flags & 0x80000000u)
        return LayoutEventResults;
    return (flags & 0x20000) ? LayoutPauseMenuMain : LayoutPauseOptions;
}

// The PC bar items in list order: item i is button PcButton0 + i with text PcText0 + i.
const int ItemCount = 6;
const uint32_t PcButton0 = 0x62376D18, PcText0 = 0xB8A7C6CC;

// Hashes of our baked prompt pair's icon and word.
inline uint32_t pairIcon(int layout, int item) { return 0x7A1A0000u + uint32_t(layout) * 16 + uint32_t(item) * 2; }
inline uint32_t pairWord(int layout, int item) { return pairIcon(layout, item) + 1; }

// In-game the PS2 also shows Back, on Circle.
inline const char* inGameToken(uint32_t message)
{
    return message == MsgBack ? "JOY_EVENT_FENG_CANCEL" : tokenForMessage(message);
}

// Each PC item's prompt, in bit order; the items used.
template <typename MessageOfBit, typename TextOfBit>
int inGamePrompts(uint32_t flags, MessageOfBit messageOfBit, TextOfBit textOfBit, Prompt out[ItemCount])
{
    int item = 0;
    for (int i = 0; i < ItemCount; ++i)
        out[i] = { 0, nullptr };
    for (int bit = 0; bit < 32 && item < ItemCount; ++bit)
    {
        const uint32_t mask = 1u << bit;
        if (!(flags & mask) || !bitHasText(bit))
            continue;
        const int at = item++;
        if (mask & DroppedBits)
            continue;
        if (const char* token = inGameToken(messageOfBit(mask)))
            out[at] = { textOfBit(mask), token };
    }
    return item;
}

// Inlined Show and Hide (0x8384D8 +0x6C, then 0x542990) bypass our hooks; -1 is never applied.
inline bool visibilityChanged(int applied, bool shown)
{
    return applied != (shown ? 1 : 0);
}

// The PC bar's bank/name switcher (0x537E60) fades PcMoney and PcName (now the PS2 bar's own texts) in turn; stopped on panel-row screens.
struct Switcher
{
    uint32_t current;   // bar +0x3C: the object shown now, 0 stops the switcher (0x537F50)
    uint32_t saved;     // ours while it is stopped
};

// The switcher on a panel-row screen (stopped) or with the bar.
inline Switcher switcherFor(bool panelRow, Switcher s)
{
    if (panelRow)
        return { 0, s.current ? s.current : s.saved };
    if (s.saved && !s.current)
        return { s.saved, 0 };
    return { s.current, 0 };
}

const float PairGap = 30.0f;          // icon left edge to word left edge
const float PairCharWidth = 8.0f;     // estimated word width per character

inline float pairLeft(float centreX, size_t chars)
{
    return centreX - (PairGap + PairCharWidth * float(chars)) / 2;
}

// The text a prompt shows: its button token, then the language word.
inline void promptLine(const Prompt& prompt, const char* word, char* out, size_t size)
{
    snprintf(out, size, "$%s$ %s", prompt.token, word);
}

}}

#endif /* !UG2_PSBARLOGIC_H_ */
