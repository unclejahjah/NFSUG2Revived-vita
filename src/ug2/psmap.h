#ifndef UG2_PSMAP_H_
#define UG2_PSMAP_H_

// Front-end button map: Cross select, Circle back, Triangle help, Square the old Triangle action. Pure logic, host tested.

#include <cstdint>
#include <cstring>

namespace ug2 { namespace psmap {

// The game's joy event ids (names JOY_EVENT_FENG_*).
enum Event { EventStart = 30, EventButton0 = 31, EventButton1 = 32, EventButton2 = 33, EventButton3 = 34,
             EventLTrigger = 35, EventRTrigger = 36, EventSelect = 37, EventSelectAlt = 38, EventCancel = 39,
             EventCancelAlt = 40, EventHelp = 41, EventPadUp = 42, EventPadDown = 43, EventPadLeft = 44,
             EventPadRight = 45, EventUpAlt = 46, EventDownAlt = 47, EventLeftAlt = 48, EventRightAlt = 49 };

// The EMS adapter's menu map: button index 0..19 (Triangle, Circle, Cross, Square, ..., L at 6, R at 7, Start at 9)
// to the game's binding action. Actions 31 select, 34 cancel, 35 Button1, 36 start, 37 Button3, 38 L, 39 R, 27..30 d-pad.
const uint32_t MenuActions[20] =
{
    35, 34, 31, 37,
    0,  0,  38, 39,
    0,  36, 0,  0,
    27, 30, 28, 29,
    0,  0,  0,  0,
};

// The event each binding action sends (the game's input table).
inline int eventForAction(uint32_t action)
{
    switch (action)
    {
    case 31: return EventSelect;
    case 34: return EventCancel;
    case 35: return EventButton1;
    case 36: return EventStart;
    case 37: return EventButton3;
    case 38: return EventLTrigger;
    case 39: return EventRTrigger;
    case 27: return EventPadUp;
    case 28: return EventPadDown;
    case 29: return EventPadLeft;
    case 30: return EventPadRight;
    default: return 0;
    }
}

// The port's menu map gives Triangle Button1 and Square Button3; Triangle becomes Help and Square Button1.
inline int remapEvent(int event)
{
    if (event == EventButton1)
        return EventHelp;
    if (event == EventButton3)
        return EventButton1;
    return event;
}

enum Icon { IconNone = -1, IconCross, IconCircle, IconSquare, IconTriangle, IconL, IconR, IconDpadUp, IconDpadDown,
            IconDpadLeft, IconDpadRight, IconLStick, IconRStick, IconStart, IconSelect,
            IconRearUL, IconRearUR, IconRearLL, IconRearLR,     // rear touch quadrants: 32x32 icons sharing cell 14
            IconArmUp, IconArmDown, IconArmLeft, IconArmRight,  // d-pad with one arm filled: 32x32 icons sharing cell 15
            IconCount };

// Event to the icon of the Vita button that sends it.
inline Icon iconForEvent(int event)
{
    switch (event)
    {
    case EventSelect: case EventSelectAlt: return IconCross;
    case EventCancel: case EventCancelAlt: return IconCircle;
    case EventHelp: return IconTriangle;
    case EventButton1: return IconSquare;
    case EventLTrigger: return IconL;
    case EventRTrigger: return IconR;
    case EventStart: case EventButton2: return IconStart;
    case EventPadUp: return IconDpadUp;
    case EventPadDown: return IconDpadDown;
    case EventPadLeft: return IconDpadLeft;
    case EventPadRight: return IconDpadRight;
    case EventUpAlt: case EventDownAlt: case EventLeftAlt: return IconLStick;
    case EventRightAlt: return IconRStick;
    default: return IconNone;
    }
}

struct TokenEvent
{
    const char* name;
    int         event;
};

const TokenEvent TokenEvents[] =
{
    { "JOY_EVENT_FENG_START", EventStart }, { "JOY_EVENT_FENG_BUTTON0", EventButton0 },
    { "JOY_EVENT_FENG_BUTTON1", EventButton1 }, { "JOY_EVENT_FENG_BUTTON2", EventButton2 },
    { "JOY_EVENT_FENG_BUTTON3", EventButton3 }, { "JOY_EVENT_FENG_LTRIGGER", EventLTrigger },
    { "JOY_EVENT_FENG_RTRIGGER", EventRTrigger }, { "JOY_EVENT_FENG_SELECT", EventSelect },
    { "JOY_EVENT_FENG_ACCEPT", EventSelect }, { "JOY_EVENT_FENG_SELECT_ALTERNATE", EventSelectAlt },
    { "JOY_EVENT_FENG_CANCEL", EventCancel }, { "JOY_EVENT_FENG_CANCEL_ALTERNATE", EventCancelAlt },
    { "JOY_EVENT_FENG_HELP", EventHelp }, { "JOY_EVENT_FENG_PAD_UP", EventPadUp },
    { "JOY_EVENT_FENG_PAD_DOWN", EventPadDown }, { "JOY_EVENT_FENG_PAD_LEFT", EventPadLeft },
    { "JOY_EVENT_FENG_PAD_RIGHT", EventPadRight }, { "JOY_EVENT_FENG_PAD_UP_ALTERNATE", EventUpAlt },
    { "JOY_EVENT_FENG_PAD_DOWN_ALTERNATE", EventDownAlt }, { "JOY_EVENT_FENG_PAD_LEFT_ALTERNATE", EventLeftAlt },
    { "JOY_EVENT_FENG_PAD_RIGHT_ALTERNATE", EventRightAlt },
};

// Icon for a text token such as JOY_EVENT_FENG_SELECT (no dollar signs), IconNone when the game keeps its own.
inline Icon iconForToken(const char* name)
{
    for (const TokenEvent& t : TokenEvents)
    {
        if (strcmp(t.name, name) == 0)
            return iconForEvent(t.event);
    }
    return IconNone;
}

// Tokens such as BIND_CROSS draw the icon in the colour of the text around it (the Customize list).
inline Icon iconForBindToken(const char* name)
{
    static const char* const Names[IconCount] = { "BIND_CROSS", "BIND_CIRCLE", "BIND_SQUARE", "BIND_TRIANGLE", "BIND_L",
        "BIND_R", "", "", "", "", "BIND_LSTICK", "BIND_RSTICK",
        "BIND_START", "BIND_SELECT", "BIND_REAR_UL", "BIND_REAR_UR", "BIND_REAR_LL", "BIND_REAR_LR", "BIND_DPAD_UP",
        "BIND_DPAD_DOWN", "BIND_DPAD_LEFT", "BIND_DPAD_RIGHT" };
    for (int i = 0; i < IconCount; ++i)
    {
        if (*Names[i] && strcmp(Names[i], name) == 0)
            return Icon(i);
    }
    return IconNone;
}

const char* const AtlasName = "PSBTN_ATLAS";
const char* const TintAtlasName = "PSBTN_TINT";    // the same cells in white, for the vertex colour to tint   // one 256x256 texture, 4x4 cells of 64x64 in Icon order

// The game's hash of an upper case name.
inline uint32_t nameHash(const char* name)
{
    uint32_t hash = 0xFFFFFFFF;
    for (; *name; ++name)
        hash = uint8_t(*name) + 33 * hash;
    return hash;
}

inline uint32_t atlasHash() { return nameHash(AtlasName); }
inline uint32_t tintAtlasHash() { return nameHash(TintAtlasName); }

// Texture coordinates of an icon's cell, a quarter texel inside to keep neighbours out. The four rear icons are
// 32x32 and share the 64x64 cell after Select; the single-arm d-pad icons share the cell after that.
struct Cell
{
    float u0, v0, u1, v1;
};

inline Cell cellOf(Icon icon)
{
    const float inset = 0.25f / 256.0f;
    if (icon >= IconArmUp)
    {
        const float step = 0.125f, sub = float(int(icon) - IconArmUp);
        const float u = 0.75f + float(int(sub) % 2) * step, v = 0.75f + float(int(sub) / 2) * step;
        return { u + inset, v + inset, u + step - inset, v + step - inset };
    }
    if (icon >= IconRearUL)
    {
        // 30x20 texels, columns 32 and rows 20 apart
        const float sub = float(int(icon) - IconRearUL);
        const float u = 0.5f + float(int(sub) % 2) * 0.125f, v = 0.75f + float(int(sub) / 2) * (20.0f / 256.0f);
        return { u + inset, v + inset, u + 30.0f / 256.0f - inset, v + 20.0f / 256.0f - inset };
    }
    const float step = 0.25f;
    const float u = float(int(icon) % 4) * step, v = float(int(icon) / 4) * step;
    return { u + inset, v + inset, u + step - inset, v + step - inset };
}

// Size of an icon in a line of text: square at the line height, except the rear pad: its 3:2 cell (a 14:9 pad
// inside) at three quarters of the line height, vertically centred.
struct IconBox
{
    float width, height, top;   // top: the offset from the line's top
};

inline IconBox boxOf(Icon icon, float lineHeight)
{
    if (icon >= IconRearUL && icon < IconArmUp)
    {
        const float h = lineHeight * 0.75f;
        return { h * 1.5f, h, (lineHeight - h) * 0.5f };
    }
    return { lineHeight, lineHeight, 0.0f };
}

}}

#endif /* !UG2_PSMAP_H_ */
