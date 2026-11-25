// The Controls screens' baked edits (controlsbake.h): pure package edits, logged by the caller.

#include "controlsbake.h"
#include "diagram_layout.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace ug2 { namespace controlsbake {

namespace {

using fepackage::nameHash;

const uint32_t TitleObject = 0x42adb44c, InnerPanel = 0x371f1c93;     // the title text, the list's box
const uint32_t RowBar = 0xa67c3fe2, RowBarRight = 0xd8fdd418, RowBarLeft = 0xd8fdd419;     // a row group's bar and end caps
const float BarCenter = 3.115f, BarWidth = 523.35f;
const uint16_t TagData = 0x4153, TagTextId = 0x4853, TagBuffer = 0x6253, TagText = 0x7453, TagAlign = 0x6a53;
const char* const SlotPrefixes[] = { "OPTION_NAME_", "OPTION_DATA_", "OPTION_BACKING_", "LEFT_ARROW_", "RIGHT_ARROW_" };

void setFloat(std::vector<uint8_t>* data, size_t at, float value)
{
    if (data && data->size() >= at + 4)
        memcpy(&(*data)[at], &value, 4);
}

void common(fepackage::Package& p, Result* r, uint32_t title, std::initializer_list<uint32_t> hidden)
{
    for (uint32_t hash : hidden)
        r->hidden += p.hide(hash);
    for (size_t t : p.find(TitleObject))
        p.setTag32(t, TagTextId, title);
}

void roomForTexts(fepackage::Package& p, int rows)
{
    for (int n = 1; n <= rows; ++n)
    {
        for (const char* prefix : { "OPTION_NAME_", "OPTION_DATA_" })
        {
            for (size_t o : p.find(nameHash(prefix + std::to_string(n))))
            {
                if (p.tag32(o, TagBuffer) < 64)
                    p.setTag32(o, TagBuffer, 64);
            }
        }
    }
}

// The PC instruction label, shown from Init on; same gap under the list.
long instructionLine(fepackage::Package& p, const fepackage::Package* donor, std::string* error, float* gap)
{
    const std::vector<size_t> label = donor ? donor->find(nameHash(InstructionLabel)) : std::vector<size_t>();
    if (label.size() != 1)
    {
        *error = donor ? "the donor has no single Instructions_Label" : "no donor package";
        return -1;
    }
    const std::vector<size_t> added = p.copyFrom(*donor, label[0], 0x5a5a5a5a, error);
    if (added.empty())
        return -1;
    const size_t line = added[0];
    p.setHash(line, instructionHash());
    p.unchainScript(line, nameHash("Init"));
    p.addResponses(line, *donor, label[0]);
    for (uint32_t message : p.responseMessages(line))
        p.addTarget(message, p.guid(line));
    const ListLayout ours = listLayout(p, OptionsLines), theirs = listLayout(*donor, PcControllerLines);
    float x, y;
    if (ours.valid && theirs.valid && position(*donor, nameHash(InstructionLabel), &x, &y))
    {
        *gap = y - theirs.bottom;
        setFloat(p.tagData(line, TagData), 0x20, ours.bottom + *gap - InstructionRaise);
    }
    p.setTag32(line, TagTextId, TextConfigure);
    if (p.tag32(line, TagBuffer) < 64)
        p.setTag32(line, TagBuffer, 64);
    if (std::vector<uint8_t>* text = p.tagData(line, TagText))
    {
        const std::string plain = "Configure your settings";
        text->assign((plain.size() + 1) * 2, 0);
        for (size_t i = 0; i < plain.size(); ++i)
            (*text)[2 * i] = uint8_t(plain[i]);
        text->resize((text->size() + 3) & ~size_t(3), 0);
    }
    return long(line);
}

// PC Pulse on each value slot; colour only, not PC positions.
int pulseScripts(fepackage::Package& p, const fepackage::Package* donor, int rows)
{
    const std::vector<size_t> model = donor ? donor->find(nameHash("OPTION_DATA_1")) : std::vector<size_t>();
    if (model.size() != 1)
        return 0;
    int made = 0;
    for (int n = 1; n <= rows; ++n)
    {
        for (size_t o : p.find(nameHash("OPTION_DATA_" + std::to_string(n))))
            made += p.addColourScript(o, *donor, model[0], pulseScript()) ? 1 : 0;
    }
    return made;
}

}

uint32_t pulseScript()
{
    return nameHash("Pulse");
}

uint32_t labelHash(int slot)
{
    return nameHash(std::string("CTRL_LABEL_") + (slot >= 0 && slot < diagramlayout::SlotCount ? diagramlayout::Slots[slot].name : "NONE"));
}

uint32_t flashScript()
{
    return nameHash("HighlightFlash");
}

uint32_t instructionHash()
{
    return nameHash("CONTROLS_INSTRUCTION");
}

int countSlots(const fepackage::Package& p)
{
    int n = 0;
    auto has = [&](int k)
    {
        for (const char* prefix : SlotPrefixes)
        {
            if (p.find(nameHash(prefix + std::to_string(k))).empty())
                return false;
        }
        return true;
    };
    while (has(n + 1))
        ++n;
    return n;
}

bool position(const fepackage::Package& p, uint32_t hash, float* x, float* y)
{
    const std::vector<size_t> found = p.find(hash);
    if (found.size() != 1)
        return false;
    const std::vector<uint8_t>* data = p.tagData(found[0], TagData);
    if (!data || data->size() < 0x24)
        return false;
    memcpy(x, &(*data)[0x1c], 4);
    memcpy(y, &(*data)[0x20], 4);
    return true;
}

namespace {

float size(const fepackage::Package& p, size_t object, size_t at)
{
    const std::vector<uint8_t>* data = p.tagData(object, TagData);
    float v = 0;
    if (data && data->size() >= at + 4)
        memcpy(&v, &(*data)[at], 4);
    return v;
}

}

namespace {

// The picture object covers the texture's canvas, centred on screen.
void picture(fepackage::Package& p, Result* r)
{
    const float w = diagramlayout::Width / diagramlayout::Scale, h = diagramlayout::Height / diagramlayout::Scale;
    r->picture[0] = r->centreX + diagramlayout::OriginX + w * 0.5f;
    r->picture[1] = diagramlayout::OriginY + h * 0.5f;
    r->picture[2] = w;
    r->picture[3] = h;
    for (size_t image : p.find(PictureObject))
    {
        std::vector<uint8_t>* sa = p.tagData(image, TagData);
        setFloat(sa, 0x1c, r->picture[0]);
        setFloat(sa, 0x20, r->picture[1]);
        setFloat(sa, 0x38, r->picture[2]);
        setFloat(sa, 0x3c, r->picture[3]);
    }
}

// The box's edges, left as the package has them.
void boxBounds(const fepackage::Package& p, Result* r)
{
    const std::vector<size_t> found = p.find(PictureBox);
    float gx = 0, gy = 0;
    if (found.size() != 1 || !position(p, PictureBox, &gx, &gy))
        return;
    float left = 1e9f, top = 1e9f, right = -1e9f, bottom = -1e9f;
    for (size_t part : p.subtree(found[0]))
    {
        if (part == found[0])
            continue;
        const float x = gx + size(p, part, 0x1c), y = gy + size(p, part, 0x20), w = size(p, part, 0x38), h = size(p, part, 0x3c);
        left = std::min(left, x - w * 0.5f);
        right = std::max(right, x + w * 0.5f);
        top = std::min(top, y - h * 0.5f);
        bottom = std::max(bottom, y + h * 0.5f);
    }
    r->box[0] = left;
    r->box[1] = top;
    r->box[2] = right;
    r->box[3] = bottom;
}

// A hidden PC label copy at the slot's column and row.
bool label(fepackage::Package& p, int slot, float centreX)
{
    const std::vector<size_t> model = p.find(LabelModel);
    if (model.size() != 1 || !p.find(labelHash(slot)).empty())
        return false;
    const size_t o = p.clone(model[0], labelHash(slot));
    const diagramlayout::Slot& s = diagramlayout::Slots[slot];
    std::vector<uint8_t>* sa = p.tagData(o, TagData);
    setFloat(sa, 0x1c, centreX + s.x);
    setFloat(sa, 0x20, s.y);
    p.setTag32(o, TagAlign, s.right ? AlignLeftCentred : AlignRightCentred);
    if (p.tag32(o, TagBuffer) < 96)
        p.setTag32(o, TagBuffer, 96);
    p.setFlags(o, p.flags(o) | fepackage::FlagHidden);
    return true;
}

}

ListLayout listLayout(const fepackage::Package& p, int lines)
{
    ListLayout l;
    const std::vector<size_t> master = p.find(TitleMasterObject);
    if (master.size() != 1)
        return l;
    const float y = size(p, master[0], 0x20), height = size(p, master[0], 0x3c);
    l.top = y - height * 0.5f;
    l.row = height;
    l.bottom = l.top + float(lines) * height;
    l.valid = height > 0;
    return l;
}

bool panelBottom(const fepackage::Package& p, float* bottom)
{
    const std::vector<size_t> title = p.find(TitleGroupObject), panel = p.find(InnerPanel);
    if (title.size() != 1 || panel.size() != 1)
        return false;
    const float base = size(p, title[0], 0x20) + size(p, panel[0], 0x20);
    bool have = false;
    for (size_t part : p.subtree(panel[0]))
    {
        if (part == panel[0] || p.type(part) != 1)
            continue;
        const float edge = base + size(p, part, 0x20) + size(p, part, 0x3c) * 0.5f;
        *bottom = have && *bottom > edge ? *bottom : edge;
        have = true;
    }
    return have;
}

Result options(fepackage::Package& p, uint32_t title, bool customize, int entries, const fepackage::Package* donor)
{
    Result r;
    common(p, &r, title, { nameHash("ColorCal_Logo"), nameHash("ColorCal_Blurb"), nameHash("Colour_Calibration_Backing"),
                           nameHash("PLAYER"), 0x010cba23, 0x97b71dde, 0xc90dc4ab, 0x444969fd, 0x444969fe });
    if (customize)
    {
        for (const char* prefix : SlotPrefixes)
            r.cloned += p.extendSeries(prefix, SlotModel, entries);
        r.instruction = instructionLine(p, donor, &r.error, &r.gap);
        r.pulses = pulseScripts(p, donor, entries);
    }
    roomForTexts(p, entries > 13 ? entries : 13);
    r.steady = p.steadyColourScript(nameHash("Highlight"), flashScript());
    r.slots = countSlots(p);
    return r;
}

Result joypad(fepackage::Package& p, uint32_t title)
{
    Result r;
    common(p, &r, title, { nameHash("PLAYER"), 0x010cba23, 0x97b71dde, 0xc90dc4ab, 0x444969fd, 0x444969fe, 0xbaa4198e,
                           0xbaa4198f, 0xbaa41990, 0xbaa41991, 0xbaa41992, 0xbaa41993, 0xbaa41994, 0xbaa41995, 0xbaa41996,
                           0x0f274b7e, 0x0f274b7f, 0x0f274b80, 0x0f274b81, 0x0f274b82 });
    roomForTexts(p, 13);
    for (size_t group = 0; group < p.objectCount(); ++group)
    {
        if (p.type(group) != 5)
            continue;
        const std::vector<size_t> parts = p.subtree(group);
        if (parts.size() != 4 || p.hash(parts[1]) != RowBar || p.hash(parts[2]) != RowBarRight || p.hash(parts[3]) != RowBarLeft)
            continue;
        setFloat(p.tagData(parts[1], TagData), 0x1c, BarCenter);
        setFloat(p.tagData(parts[1], TagData), 0x38, BarWidth);
        setFloat(p.tagData(parts[2], TagData), 0x1c, BarCenter + BarWidth / 2 + 18);
        setFloat(p.tagData(parts[3], TagData), 0x1c, BarCenter - BarWidth / 2 - 18);
        ++r.bars;
    }
    float groupX = 0, groupY = 0;
    position(p, TitleGroupObject, &groupX, &groupY);
    r.centreX = -groupX;
    picture(p, &r);
    boxBounds(p, &r);
    for (int slot = 0; slot < diagramlayout::SlotCount; ++slot)
        r.labels += label(p, slot, r.centreX) ? 1 : 0;
    r.steady = p.steadyColourScript(nameHash("Highlight"), flashScript());
    r.slots = countSlots(p);
    return r;
}

}}
