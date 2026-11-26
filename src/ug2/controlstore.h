#ifndef UG2_CONTROLSTORE_H_
#define UG2_CONTROLSTORE_H_

// Controls kept per Vita in registry.txt as the game's PS2 record: Controls_Type (0..3 the Control Types, 4 Custom)
// and Controls_Custom, plus the six stick shape percents. No game access, so host tests cover it.

#include "controlsdata.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ug2 { namespace controlstore {

const int MapEntries = controlsdata::MapEntries;
const int RecordBytes = 165;    // axis ids 16, signs 16, flags 4, merged 1, race map 128 (the profile's order after the GUID)
const int ProfileRecordBytes = 0xB5;    // per input slot in the profile: instance GUID 16, then the record
const int TypeCustom = 4;
const char* const TypeName = "Controls_Type";
const char* const CustomName = "Controls_Custom";

typedef controlsdata::Binds Record;

void toBytes(const Record& record, uint8_t out[RecordBytes]);
void fromBytes(const uint8_t in[RecordBytes], Record* record);
bool operator==(const Record& a, const Record& b);

// Control Type 0..3 (1..4 on screen); data in controlsdata.cpp.
Record presetRecord(int type);

// Where the two values live.
class Backend
{
public:
    virtual ~Backend() {}
    virtual bool read(const char* name, std::vector<uint8_t>* out) = 0;
    virtual void write(const char* name, const std::vector<uint8_t>& bytes) = 0;
};

bool has(Backend& backend);                 // Controls_Type is stored
int type(Backend& backend);                 // 0..4; 0 when nothing is stored
Record custom(Backend& backend);            // Controls_Custom, or Type 0's record when none is stored
Record active(Backend& backend);            // what the pad slot holds: the type's preset, or the custom record
Record activeFor(Backend& backend, int type);   // the same for a given type
void setType(Backend& backend, int type);   // a preset leaves Controls_Custom alone (a first write copies Type 0 into it)
void setCustom(Backend& backend, const Record& record);     // stores the record and makes the type Custom
// Customize edits: Custom only on a real change, else false.
bool bindEntry(Backend& backend, controlsdata::Entry entry, controlsdata::Control control);
bool removeEntry(Backend& backend, controlsdata::Entry entry);
Record forProfile(Backend& backend);        // what a profile save writes: always the custom record, never a preset
// Puts record into the slot's entry of the profile block.
void patchProfile(uint8_t* records, int slot, const Record& record);
// A 0.8.5 profile's record becomes Custom; false when already stored.
bool importProfile(Backend& backend, const Record& profile);
// No profile: Type 0, Custom its copy; false when stored.
bool startWithoutProfile(Backend& backend);

// The six stick shape values: DWORD percents Controls_LeftStickDeadzone ... Controls_RightStickSensitivity.
const int StickParts = 3;       // dead zone, outer limit, sensitivity
const char* stickName(bool right, int part);
int stickDefault(int part);     // 15, 100, 100
int stickClamp(int part, int percent);      // 0..50, 50..100, 0..100
bool stickStored(Backend& backend, bool right, int part, int* percent);     // false when the key is missing
void setStick(Backend& backend, bool right, int part, int percent);         // writes only that key, clamped

// What boot does with registry.txt.
enum Start
{
    StartStored,            // registry.txt has controls: they are used
    StartFresh,             // no controls and no profile on disk: Type 0 and Custom stored now
    StartWaitForProfile,    // no controls, a profile on disk: the first profile load imports it
};
Start atBoot(Backend& backend, bool profileOnDisk);

}}

#endif /* !UG2_CONTROLSTORE_H_ */
