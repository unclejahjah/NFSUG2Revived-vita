#ifndef UG2_DIAGRAM_H_
#define UG2_DIAGRAM_H_

// The Joypad Configuration picture: diagram_art.h's layers composed for a control layout, and each slot's label text.
// No game access; host tested.

#include "controlsdata.h"
#include "diagram_layout.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ug2 { namespace diagram {

// The generated slots: 12 controls, then the 4 touch zones.
enum Slot
{
    SlotTriangle = diagramlayout::SlotTriangle, SlotCircle = diagramlayout::SlotCircle, SlotCross = diagramlayout::SlotCross,
    SlotSquare = diagramlayout::SlotSquare, SlotL = diagramlayout::SlotL, SlotR = diagramlayout::SlotR,
    SlotDpadUp = diagramlayout::SlotDpadUp, SlotDpadDown = diagramlayout::SlotDpadDown, SlotDpadLeft = diagramlayout::SlotDpadLeft,
    SlotDpadRight = diagramlayout::SlotDpadRight, SlotLeftStick = diagramlayout::SlotLeftStick,
    SlotRightStick = diagramlayout::SlotRightStick, SlotTouchMap = diagramlayout::SlotTouchMap,
    SlotTouchSms = diagramlayout::SlotTouchSms, SlotTouchShop = diagramlayout::SlotTouchShop,
    SlotTouchCareer = diagramlayout::SlotTouchCareer, SlotCount = diagramlayout::SlotCount
};
const int ControlSlots = diagramlayout::ControlSlots;

// A game text by hash, else plain English; neither for no text.
struct Text
{
    uint32_t hash;
    const char* plain;
};

// What a slot reads; second is set when a stick carries a pair.
struct Label
{
    Text first, second;
};

// Leader layers drawn over the body.
struct Plan
{
    bool leader[SlotCount];
};

const char* slotName(Slot slot);
bool isTouch(Slot slot);
// Label place: column edge, row centre; units from screen centre.
float labelX(Slot slot);
float labelY(Slot slot);
bool rightColumn(Slot slot);
float labelRoom();

// Touch labels are empty when a button shows the action.
Label labelFor(const controlsdata::Binds& binds, Slot slot);
bool assigned(const Label& label);
// Leaders for assigned controls; touch only when no button has it.
Plan planFor(const controlsdata::Binds& binds);
std::string cacheKey(const Plan& plan);
int layerCount(const Plan& plan);

typedef std::function<std::string(uint32_t)> Lookup;
typedef std::function<double(const std::string&)> WidthFn;
// "{First} / {Second}", game texts through lookup.
std::string labelString(const Label& label, const Lookup& lookup);
// One line, cut with "..." when wider than limit.
std::string fitLine(const std::string& text, double limit, const WidthFn& width);

int width();
int height();
// Texture top left (units from screen centre), texels a unit.
float originX();
float originY();
float scale();
// RGBA, straight alpha: the body, then the plan's leaders.
std::vector<uint8_t> compose(const Plan& plan);
// DXT3 of an RGBA picture; sides multiples of 4.
std::vector<uint8_t> encodeDxt3(const uint8_t* rgba, int width, int height);

// One RGBA canvas and one DXT3 picture, reused for every plan.
class Composer
{
public:
    // The plan's DXT3 picture; the same buffer every time.
    const std::vector<uint8_t>& compose(const Plan& plan);
    const std::vector<uint8_t>& picture() const { return m_dxt; }

private:
    std::vector<uint8_t> m_rgba, m_dxt;
};

}}

#endif /* !UG2_DIAGRAM_H_ */
