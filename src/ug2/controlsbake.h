#ifndef UG2_CONTROLSBAKE_H_
#define UG2_CONTROLSBAKE_H_

// Edits baked into the Controls screens' package copies: hidden objects, titles, row slots, bars, picture, labels, instruction line.

#include "fepackage.h"

#include <cstdint>
#include <string>

namespace ug2 { namespace controlsbake {

const uint32_t TextConfigure = 0x8bab66ef, TextPressButton = 0x23cf49aa;     // "Configure your settings", the bind wait
const uint32_t PictureObject = 0xf1534c25;     // CONTROLLER_IMAGE
const uint32_t PictureBox = 0x38747312;        // the dark rounded box behind the picture (a 9-slice group)
const uint32_t LabelModel = 0xbaa4198e;        // the PC picture's top left label, the labels' model
const uint32_t AlignRightCentred = 6, AlignLeftCentred = 4;     // text justification (Sj): 2 right, 4 vertically centred
const uint32_t TitleGroupObject = 0x8d7e0c53;  // the group holding the title and the panel
const uint32_t TitleMasterObject = 0xa753c46c;  // TITLE_MASTER: the first row's label box
const int OptionsLines = 8, PcControllerLines = 7;     // lines the options screen (0x4CF750) and PC controller screen show
const float InstructionRaise = 4.0f;           // units the line sits above the PC gap, set by eye on screen

// The list's place from the package: TITLE_MASTER's top and height (the row step), and the last visible row's bottom.
struct ListLayout
{
    float top = 0, row = 0, bottom = 0;
    bool valid = false;
};
ListLayout listLayout(const fepackage::Package& p, int lines);
// The bottom edge of the inner panel box behind the list, in screen units; false when the package has none.
bool panelBottom(const fepackage::Package& p, float* bottom);
// An object's position (data x, y) from its package.
bool position(const fepackage::Package& p, uint32_t hash, float* x, float* y);
const int SlotModel = 12;                      // the last row slot of UI_Options.fng

struct Result
{
    size_t hidden = 0;
    int slots = 0;          // rows the copy can show
    int cloned = 0;         // slot objects copied from SlotModel
    int bars = 0;           // row bars fitted
    long instruction = -1;  // the instruction line object, -1 for none
    int steady = 0;         // looping Highlight scripts made steady (kept as HighlightFlash)
    int pulses = 0;         // value slots given the PC's Pulse
    std::string error;      // why the instruction line is missing
    float gap = 0;          // the PC screen's gap from its last row to its label, kept here
    int labels = 0;         // picture labels made
    float centreX = 0;      // the screen centre in the picture group's units
    float picture[4] = {};  // centre x, y, width, height
    float box[4] = {};      // the dark box as baked: left, top, right, bottom
};

// The picture label object of a diagram_layout.h slot.
uint32_t labelHash(int slot);

// The flashing highlight, kept under its own name.
uint32_t flashScript();
// The PC's bind wait flash of a row's value ("Pulse").
uint32_t pulseScript();
// CONTROLS_INSTRUCTION: the Customize instruction line's own object.
uint32_t instructionHash();
// Rows the package can show: every slot object up to n exists.
int countSlots(const fepackage::Package& p);
// The package the instruction line comes from, and the object.
const char* const InstructionDonor = "UI_Options_PC_Controller.fng";
const char* const InstructionLabel = "Instructions_Label";
// A copy of UI_Options.fng: the Controls list, or Customize with a row slot per entry and the instruction line from donor.
Result options(fepackage::Package& p, uint32_t title, bool customize, int entries, const fepackage::Package* donor = nullptr);
// Joypad Configuration's copy: picture and a hidden label per slot.
Result joypad(fepackage::Package& p, uint32_t title);

}}

#endif /* !UG2_CONTROLSBAKE_H_ */
