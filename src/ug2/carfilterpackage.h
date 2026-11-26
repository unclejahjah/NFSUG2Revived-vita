#ifndef UG2_CARFILTERPACKAGE_H_
#define UG2_CARFILTERPACKAGE_H_

#include "fepackage.h"

#include <string>

// The car filter row of the car select packages: the PS2's L1/R1 icon texts take the PC mouse arrows' place. No game access.

namespace ug2 { namespace carfilter {

const uint32_t LeftArrow = 0xCF6F0439, RightArrow = 0xABA93D3F;   // a click sends PAD_LTRIGGER / PAD_RTRIGGER (0x4FC809)
const uint32_t PcFilterText = 0x3E81DE59, PsFilterText = 0xD999B6E7;
const uint32_t FlagConsoleOnly = 0x40;      // the PC hides these objects when it instantiates a package (0x51C860)
const uint32_t CentreBoth = 5;              // Sj: centred across (1) and down (4)
const char* const LeftToken = "$JOY_EVENT_FENG_LTRIGGER$", *const RightToken = "$JOY_EVENT_FENG_RTRIGGER$";

// Packages of the quick race car select class (0x4F9D90).
const char* const Screens[] = { "UI_QRCarSelect.fng", "UI_OLCarSelect.fng", "UI_OLCarLot.fng" };

struct Point
{
    float x, y;
};

struct Baked
{
    uint32_t leftIcon = 0, rightIcon = 0;   // the icon objects' hashes
    Point left = { 0, 0 }, right = { 0, 0 }; // their centres on screen: the arrows' centres
    float scale = 0;
    std::string error;
};

// A string object's text, empty when it has none.
std::string text(const fepackage::Package& package, size_t object);
// Screen position: the object's own plus its parents'.
Point screenPosition(const fepackage::Package& package, size_t object);
// Edits in place; unchanged on failure, Baked::error says why.
bool bake(fepackage::Package& package, Baked* out);

}}

#endif /* !UG2_CARFILTERPACKAGE_H_ */
