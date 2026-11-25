// Car filter row: the PC arrows hidden, the PS2's console-only L1/R1 icon texts shown at the arrows' centres.

#include "carfilterpackage.h"

#include <cstring>

namespace ug2 { namespace carfilter {

namespace {

const uint16_t TagText = 0x7453, TagJustify = 0x6a53, TagState = 0x4153;    // "St", "Sj", "SA"
const size_t StatePosition = 28, StateScale = 56;   // SA: colour, pivot, position, rotation, scale (or size)

float stateFloat(const fepackage::Package& package, size_t object, size_t at)
{
    const std::vector<uint8_t>* state = package.tagData(object, TagState);
    float value = 0;
    if (state && state->size() >= at + 4)
        memcpy(&value, state->data() + at, 4);
    return value;
}

void setStateFloat(fepackage::Package& package, size_t object, size_t at, float value)
{
    std::vector<uint8_t>* state = package.tagData(object, TagState);
    if (state && state->size() >= at + 4)
        memcpy(state->data() + at, &value, 4);
}

bool hasState(const fepackage::Package& package, size_t object, size_t bytes)
{
    const std::vector<uint8_t>* state = package.tagData(object, TagState);
    return state && state->size() >= bytes;
}

// The one object with this hash and type, else objectCount().
size_t single(const fepackage::Package& package, uint32_t hash, uint32_t type)
{
    const std::vector<size_t> found = package.find(hash);
    return found.size() == 1 && package.type(found[0]) == type ? found[0] : package.objectCount();
}

size_t byText(const fepackage::Package& package, const char* wanted)
{
    size_t at = package.objectCount();
    for (size_t i = 0; i < package.objectCount(); ++i)
    {
        if (package.type(i) != 2 || text(package, i) != wanted)
            continue;
        if (at != package.objectCount())
            return package.objectCount();
        at = i;
    }
    return at;
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

bool ancestorsShown(const fepackage::Package& package, size_t object)
{
    for (size_t at = object, depth = 0; package.parent(at) != 0 && depth < 32; ++depth)
    {
        at = byGuid(package, package.parent(at));
        if (at == package.objectCount() || (package.flags(at) & (FlagConsoleOnly | fepackage::FlagHidden)))
            return false;
    }
    return true;
}

}

std::string text(const fepackage::Package& package, size_t object)
{
    const std::vector<uint8_t>* data = package.tagData(object, TagText);
    std::string out;
    for (size_t i = 0; data && i + 1 < data->size(); i += 2)
    {
        const unsigned c = (*data)[i] | (unsigned((*data)[i + 1]) << 8);
        if (c == 0)
            break;
        out += c < 128 ? char(c) : '?';
    }
    return out;
}

Point screenPosition(const fepackage::Package& package, size_t object)
{
    Point p = { 0, 0 };
    for (size_t at = object, depth = 0; at < package.objectCount() && depth < 32; ++depth)
    {
        p.x += stateFloat(package, at, StatePosition);
        p.y += stateFloat(package, at, StatePosition + 4);
        at = package.parent(at) ? byGuid(package, package.parent(at)) : package.objectCount();
    }
    return p;
}

bool bake(fepackage::Package& package, Baked* out)
{
    fepackage::Package work = package;
    const size_t none = work.objectCount();
    const size_t arrows[2] = { single(work, LeftArrow, 1), single(work, RightArrow, 1) };
    const size_t icons[2] = { byText(work, LeftToken), byText(work, RightToken) };
    const size_t pcText = single(work, PcFilterText, 2), psText = single(work, PsFilterText, 2);
    if (arrows[0] == none || arrows[1] == none)
    {
        out->error = "the package has no PC filter arrows";
        return false;
    }
    if (icons[0] == none || icons[1] == none)
    {
        out->error = "the package has no single L1 and R1 icon text";
        return false;
    }
    if (pcText == none || psText == none || stateFloat(work, psText, StateScale) <= 0)
    {
        out->error = "the package has no PC and PS2 filter text";
        return false;
    }
    for (size_t side = 0; side < 2; ++side)
    {
        if (!hasState(work, arrows[side], StateScale) || !hasState(work, icons[side], StateScale + 8) || !work.tagData(icons[side], TagJustify) ||
            work.tagData(icons[side], TagJustify)->size() < 4 || !ancestorsShown(work, icons[side]))
        {
            out->error = "an icon or arrow lacks its state, its justification or a shown parent";
            return false;
        }
    }
    const float scale = stateFloat(work, icons[0], StateScale) * stateFloat(work, pcText, StateScale) / stateFloat(work, psText, StateScale);
    Point centres[2];
    for (size_t side = 0; side < 2; ++side)
    {
        const size_t icon = icons[side], arrow = arrows[side];
        centres[side] = screenPosition(work, arrow);
        const Point now = screenPosition(work, icon);
        setStateFloat(work, icon, StatePosition, stateFloat(work, icon, StatePosition) + centres[side].x - now.x);
        setStateFloat(work, icon, StatePosition + 4, stateFloat(work, icon, StatePosition + 4) + centres[side].y - now.y);
        setStateFloat(work, icon, StateScale, scale);
        setStateFloat(work, icon, StateScale + 4, scale);
        const uint32_t centred = CentreBoth;
        memcpy(work.tagData(icon, TagJustify)->data(), &centred, 4);
        work.setFlags(icon, work.flags(icon) & ~(FlagConsoleOnly | fepackage::FlagHidden));
        work.setFlags(arrow, work.flags(arrow) | fepackage::FlagHidden);
    }
    out->leftIcon = work.hash(icons[0]);
    out->rightIcon = work.hash(icons[1]);
    out->left = centres[0];
    out->right = centres[1];
    out->scale = scale;
    package = work;
    return true;
}

}}
