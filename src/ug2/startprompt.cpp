// Start splash: "Press Enter Key" points at the game's "Press START button", else "Press START", else English in place.

#include "startprompt.h"
#include <platform/log.h>
#include <recomp.h>
#include <string>

namespace ug2 { namespace startprompt {

namespace {

using x86::reg32;

const reg32 LanguageTable = 0x8383d8, LanguageCount = 0x8383d0;    // (hash, text) pairs
const reg32 TextPressEnter = 0x1f182080, TextPressStart = 0x1f181f57, TextPressStartButton = 0xeeeb248e;

std::string readString(reg32 address)
{
    std::string text;
    for (char c; address && (c = char(rc::ld8(address))) != 0 && text.size() < 200; ++address)
        text += c;
    return text;
}

}

void frame()
{
    static unsigned s_tries = 0;
    const reg32 table = rc::ld32(LanguageTable), count = rc::ld32(LanguageCount);
    if (!table || !count || s_tries > 900)
        return;
    if (++s_tries % 10 != 1)
        return;
    reg32 enter = 0, button = 0, start = 0;
    for (reg32 i = 0; i < count; ++i)
    {
        const reg32 entry = table + 8 * i, hash = rc::ld32(entry);
        if (hash == TextPressEnter || (!enter && readString(rc::ld32(entry + 4)) == "Press Enter Key"))
            enter = entry;
        else if (hash == TextPressStartButton && rc::ld8(rc::ld32(entry + 4)))
            button = entry;
        else if (hash == TextPressStart && rc::ld8(rc::ld32(entry + 4)))
            start = entry;
    }
    if (button)
        start = button;
    if (!enter)
        return;
    const reg32 text = rc::ld32(enter + 4);
    if (start)
    {
        rc::st32(enter + 4, rc::ld32(start + 4));
        s_tries = 1000;
        pf::logInfo("startprompt: splash text %08x now the game's \"%s\"", unsigned(rc::ld32(enter)), readString(rc::ld32(start + 4)).c_str());
        return;
    }
    if (readString(text) == "Press START" || readString(text).size() < 11)
        return;
    for (const char* c = "Press START"; ; ++c)
    {
        rc::st8(text + reg32(c - "Press START"), uint8_t(*c));
        if (!*c)
            break;
    }
    s_tries = 1000;
    pf::logInfo("startprompt: splash text %08x now \"Press START\"", unsigned(rc::ld32(enter)));
}

}}
