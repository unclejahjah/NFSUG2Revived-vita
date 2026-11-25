#ifndef UG2_PSBARPACKAGE_H_
#define UG2_PSBARPACKAGE_H_

#include "fepackage.h"
#include "psbarlogic.h"

#include <string>

// Rebuilds UI_PC_Help_Bar.fng bytes: the PC art hidden, the PS2 bar group copied in from a screen package. No game access.

namespace ug2 { namespace psbar {

struct Built
{
    size_t copied = 0, resourcesAdded = 0, hiddenPc = 0;
    std::string error;
};

// Edits bar in place; on failure bar is left unchanged and Built::error says why.
bool buildBar(fepackage::Package& bar, const fepackage::Package& donor, Built* out);

// Where the PC puts an item's button in a layout, and our prompt pair there.
struct Slot
{
    bool baked = false;
    float x = 0, y = 0;     // the button's centre x, the word's y
    float iconY = 0;
};

struct InGameBuilt
{
    Slot slots[LayoutCount][ItemCount];
    size_t pairs = 0;
    std::string error;
};

// In-game prompt pairs at the PC items' layout positions.
bool buildInGame(fepackage::Package& bar, InGameBuilt* out);
// The first free baked item clear of the used ones, else -1.
int freeItem(const Slot slots[ItemCount], const bool used[ItemCount]);

}}

#endif /* !UG2_PSBARPACKAGE_H_ */
