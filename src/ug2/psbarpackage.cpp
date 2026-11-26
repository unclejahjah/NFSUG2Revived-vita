// Builds the PS2 bottom bar package from the PC help bar package and a screen package that has the PS2 bar group.

#include "psbarpackage.h"
#include "psbarlogic.h"
#include <cmath>
#include <cstring>

namespace ug2 { namespace psbar {

namespace {

const uint16_t TagData = 0x4153;     // an object's start data
const size_t DataY = 0x20;           // its y in that data
const float IconDrop = 1.0f / 1.1333f;   // 1 Vita pixel lower than the PS2 places the bar's button icons

}

bool buildBar(fepackage::Package& bar, const fepackage::Package& donor, Built* out)
{
    fepackage::Package work = bar;
    const size_t resourcesBefore = work.resourceNames().size();
    size_t hidden = 0;
    for (size_t i = 0; i < work.objectCount(); ++i)
    {
        if (!(work.flags(i) & fepackage::FlagHidden))
        {
            work.setFlags(i, work.flags(i) | fepackage::FlagHidden);
            ++hidden;
        }
    }
    for (size_t i : work.find(PcRealGroup))
        work.setHash(i, PcRealGroup + 1);
    for (uint32_t hash : { PcName, PcMoney })
    {
        for (size_t i : work.find(hash))
            work.setHash(i, hash ^ PcOldSalt);
    }
    size_t root = donor.objectCount();
    for (size_t i : donor.find(PsRoot))
    {
        if (donor.parent(i) == 0)
            root = i;
    }
    if (root == donor.objectCount())
    {
        out->error = "the donor has no top-level PS2 bar group";
        return false;
    }
    const std::vector<size_t> added = work.copyFrom(donor, root, Salt, &out->error);
    if (added.empty())
        return false;
    for (uint32_t hash : { PsHelpIcon, PsSecondIcon })
    {
        for (size_t i : work.find(hash))
        {
            std::vector<uint8_t>* data = work.tagData(i, TagData);
            if (data && data->size() >= DataY + 4)
            {
                float y;
                memcpy(&y, &(*data)[DataY], 4);
                y += IconDrop;
                memcpy(&(*data)[DataY], &y, 4);
            }
        }
    }
    for (uint32_t hash : { PsDpad, PsSecondWord, PsSecondIcon })
        work.hide(hash);
    work.hide(PsRoot);
    out->copied = added.size();
    out->hiddenPc = hidden;
    out->resourcesAdded = work.resourceNames().size() - resourcesBefore;
    bar = work;
    return true;
}

namespace {

const size_t DataX = 0x1C, DataZ = 0x24;

float dataFloat(const fepackage::Package& package, size_t object, size_t at)
{
    const std::vector<uint8_t>* data = package.tagData(object, TagData);
    float value = 0;
    if (data && data->size() >= at + 4)
        memcpy(&value, &(*data)[at], 4);
    return value;
}

void setDataFloat(fepackage::Package& package, size_t object, size_t at, float value)
{
    std::vector<uint8_t>* data = package.tagData(object, TagData);
    if (data && data->size() >= at + 4)
        memcpy(&(*data)[at], &value, 4);
}

size_t single(const fepackage::Package& package, uint32_t hash)
{
    const std::vector<size_t> found = package.find(hash);
    return found.size() == 1 ? found[0] : package.objectCount();
}

size_t byGuid(const fepackage::Package& package, uint32_t guid)
{
    for (size_t i = 0; i < package.objectCount(); ++i)
    {
        if (package.guid(i) == guid)
            return i;
    }
    return package.objectCount();
}

// The object's z on screen: its own plus its parents'.
float screenZ(const fepackage::Package& package, size_t object)
{
    float z = 0;
    for (size_t at = object, depth = 0; at < package.objectCount() && depth < 32; ++depth)
    {
        z += dataFloat(package, at, DataZ);
        at = package.parent(at) ? byGuid(package, package.parent(at)) : package.objectCount();
    }
    return z;
}

}

bool buildInGame(fepackage::Package& bar, InGameBuilt* out)
{
    fepackage::Package work = bar;
    const size_t none = work.objectCount();
    const size_t icon = single(work, PsSecondIcon), word = single(work, PsSecondWord);
    if (icon == none || word == none)
    {
        out->error = "the bar has no single second prompt icon and word";
        return false;
    }
    const float wordDy = dataFloat(work, word, DataY), iconDy = dataFloat(work, icon, DataY);
    const float iconZ = screenZ(work, icon), wordZ = screenZ(work, word);
    InGameBuilt built;
    const fepackage::Package source = work;
    for (int layout = 0; layout < LayoutCount; ++layout)
    {
        const uint32_t script = fepackage::nameHash(LayoutScripts[layout]);
        for (int item = 0; item < ItemCount; ++item)
        {
            const size_t button = single(work, PcButton0 + uint32_t(item)), text = single(work, PcText0 + uint32_t(item));
            Slot slot;
            float textX = 0, textY = 0;
            if (button == none || !work.scriptEndPosition(button, script, &slot.x, &slot.y))
                continue;
            if (text == none || !work.scriptEndPosition(text, script, &textX, &textY))
                textY = slot.y - 2;
            if (!work.find(pairIcon(layout, item)).empty() || !work.find(pairWord(layout, item)).empty())
            {
                out->error = "a prompt pair hash is already taken";
                return false;
            }
            const float left = pairLeft(slot.x, 6);
            slot.y = textY + wordDy;
            slot.iconY = textY + iconDy;
            const std::pair<size_t, uint32_t> copies[2] = { { icon, pairIcon(layout, item) }, { word, pairWord(layout, item) } };
            for (const auto& c : copies)
            {
                const std::vector<size_t> added = work.copyFrom(source, c.first, Salt, &out->error);
                if (added.size() != 1)
                {
                    if (out->error.empty())
                        out->error = "a prompt copy has children";
                    return false;
                }
                const size_t at = added[0];
                work.setHash(at, c.second);
                const bool isIcon = c.first == icon;
                setDataFloat(work, at, DataX, isIcon ? left : left + PairGap);
                setDataFloat(work, at, DataY, isIcon ? slot.iconY : slot.y);
                setDataFloat(work, at, DataZ, isIcon ? iconZ : wordZ);
                work.setFlags(at, (work.flags(at) & ~0x40u) | fepackage::FlagHidden);
            }
            slot.baked = true;
            built.slots[layout][item] = slot;
            ++built.pairs;
        }
    }
    if (!built.pairs)
    {
        out->error = "no PC item has an in-game layout script";
        return false;
    }
    *out = built;
    bar = work;
    return true;
}

int freeItem(const Slot slots[ItemCount], const bool used[ItemCount])
{
    const float Clear = 100.0f;
    for (int item = 0; item < ItemCount; ++item)
    {
        if (used[item] || !slots[item].baked)
            continue;
        bool clear = true;
        for (int other = 0; other < ItemCount; ++other)
        {
            if (used[other] && slots[other].baked && fabsf(slots[other].x - slots[item].x) < Clear && fabsf(slots[other].y - slots[item].y) < Clear)
                clear = false;
        }
        if (clear)
            return item;
    }
    return -1;
}

}}
