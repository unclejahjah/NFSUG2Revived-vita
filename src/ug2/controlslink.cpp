// The controls store joined to the game: registry backend, pad slot, the profile's control load and save.

#include "controlslink.h"
#include "controlsrows.h"

#include <lib/controls.h>
#include <lib/file.h>
#include <lib/registry.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace ug2 { namespace controlslink {

namespace
{

using x86::reg32;
using namespace controlstore;
namespace data = controlsdata;

const reg32 Slots = 0x874C40, SlotSize = 0x370, SlotCount = 6;
const reg32 RaceMap = 0x18, AxisIds = 0x344, AxisSigns = 0x354, AxisFlags = 0x364, AxisMerged = 0x368;
const char* const ProfileFolder = "C:\\save\\NFS Underground 2";

struct RegistryBackend : Backend
{
    static win32::registry::Key* key()
    {
        return win32::registry::find(win32::registry::root(win32::HKEY_LOCAL_MACHINE), "Software\\EA Games\\Need for Speed Underground 2", true);
    }

    bool read(const char* name, std::vector<uint8_t>* out) override
    {
        win32::registry::Key* k = key();
        reg32 size = 0;
        if (!k || win32::registry::query(k, name, nullptr, nullptr, &size) != 0)
            return false;
        out->resize(size);
        return win32::registry::query(k, name, nullptr, out->data(), &size) == 0;
    }

    void write(const char* name, const std::vector<uint8_t>& bytes) override
    {
        if (win32::registry::Key* k = key())
            win32::registry::setValue(k, name, bytes.size() == 4 ? win32::registry::REG_DWORD : win32::registry::REG_BINARY, bytes.data(), reg32(bytes.size()));
    }
};

struct Active
{
    int    type = 0;        // Controls_Type value in play: 0..3 presets, 4 Custom
    Record record = {};
    bool   known = false;   // record is set
};

Active s_active;
bool   s_stored = false;        // registry.txt has Controls_Type: the slot follows s_active
bool   s_slotLogged = false;

reg32 findPadSlot()
{
    for (reg32 slot = Slots; slot < Slots + SlotCount * SlotSize; slot += SlotSize)
    {
        if (rc::ld32(slot + 0x230) == 0 && rc::ld32(slot + 0x234) != 0x13)
            return slot;
    }
    return 0;
}

// Any entry in the profile folder counts as a profile.
bool profileOnDisk()
{
    const std::string folder = win32::File::hostPath(ProfileFolder);
    if (DIR* d = opendir(folder.c_str()))
    {
        bool any = false;
        while (dirent* entry = readdir(d))
        {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
                any = true;
        }
        closedir(d);
        return any;
    }
    struct stat status;
    return stat(folder.c_str(), &status) == 0;
}

int forcedType()
{
#ifdef DEV_SETTINGS
    const int forced = pf::Config::getInt("debug_control_type", 0);
    if (forced >= 1 && forced <= 5)
        return forced == 5 ? TypeCustom : forced - 1;
#endif
    return -1;
}

std::string typeLabel(int type)
{
    char text[48];
    if (type == TypeCustom)
        snprintf(text, sizeof(text), "Controls_Type=%d (Custom)", type);
    else
        snprintf(text, sizeof(text), "Controls_Type=%d (Control Type %d)", type, type + 1);
    return text;
}

std::string summary(const Record& r)
{
    char text[512];
    snprintf(text, sizeof(text),
             "Triangle %s, Circle %s, Cross %s, Square %s, L %s, R %s; d-pad up %s, right %s, down %s, left %s; "
             "rear L2 %s, R2 %s, L3 %s, R3 %s; Select %s, Start %s; left stick %s, right stick %s",
             data::actionName(r.map[data::Triangle]), data::actionName(r.map[data::Circle]),
             data::actionName(r.map[data::Cross]), data::actionName(r.map[data::Square]),
             data::actionName(r.map[data::L1]), data::actionName(r.map[data::R1]),
             data::actionName(r.map[data::DpadUp]), data::actionName(r.map[data::DpadRight]),
             data::actionName(r.map[data::DpadDown]), data::actionName(r.map[data::DpadLeft]),
             data::actionName(r.map[data::L2]), data::actionName(r.map[data::R2]),
             data::actionName(r.map[data::L3]), data::actionName(r.map[data::R3]),
             data::actionName(r.map[data::Select]), data::actionName(r.map[data::Start]),
             data::roleName(data::stickRole(r, 0)), data::roleName(data::stickRole(r, 1)));
    return text;
}

// The record in play follows registry.txt, or debug_control_type.
void refresh(const char* why)
{
    s_stored = has(backend());
    const int forced = forcedType();
    const int type = forced >= 0 ? forced : controlstore::type(backend());
    const Record record = activeFor(backend(), type);
    if (s_active.known && s_active.type == type && s_active.record == record)
        return;
    s_active.type = type;
    s_active.record = record;
    s_active.known = true;
    s_slotLogged = false;
    win32::controls::setStickRoles(data::inputRoles(record));
#ifdef DEV_TOOLS
    pf::logInfo("controls: active %s%s after %s: %s", typeLabel(type).c_str(), forced >= 0 ? ", forced by debug_control_type" : "",
                why, summary(record).c_str());
#else
    (void)why;
#endif
}

}

reg32 padSlot()
{
    return findPadSlot();
}

void storeChanged(const char* why)
{
    refresh(why);
}

Backend& backend()
{
    static RegistryBackend s;
    return s;
}

bool readSlot(Record* r)
{
    const reg32 slot = findPadSlot();
    if (!slot)
        return false;
    memset(r, 0, sizeof(*r));
    for (int i = 0; i < 4; ++i)
    {
        r->axisId[i] = int32_t(rc::ld32(slot + AxisIds + 4 * i));
        r->axisSign[i] = int32_t(rc::ld32(slot + AxisSigns + 4 * i));
        r->flags[i] = rc::ld8(slot + AxisFlags + i);
    }
    r->merged = rc::ld8(slot + AxisMerged);
    for (int i = 0; i < MapEntries; ++i)
        r->map[i] = rc::ld32(slot + RaceMap + 4 * i);
    return true;
}

void writeSlot(const Record& source)
{
    Record r = source;
    data::onePerAction(&r);
    const reg32 slot = findPadSlot();
    if (!slot)
        return;
    for (int i = 0; i < 4; ++i)
    {
        rc::st32(slot + AxisIds + 4 * i, uint32_t(r.axisId[i]));
        rc::st32(slot + AxisSigns + 4 * i, uint32_t(r.axisSign[i]));
        rc::st8(slot + AxisFlags + i, r.flags[i]);
    }
    rc::st8(slot + AxisMerged, r.merged);
    for (int i = 0; i < MapEntries; ++i)
        rc::st32(slot + RaceMap + 4 * i, r.map[i]);
}

// Fork keys retired for 0.9.0's stick rows (Vita Settings had them before): stick_deadzone was both sticks' dead
// zone, stick_response the share of the straight line in the same curve (standard 0.6, linear 1.0, precise 0.3).
// They fill in a stick row once, only where registry.txt and upstream's own old keys have nothing.
static const char* oldStickValue(const char* key)
{
    if (const char* value = pf::Config::getRetired(key))
        return value;
    const std::string k = key;
    if (k == "left_stick_deadzone" || k == "right_stick_deadzone")
        return pf::Config::getRetired("stick_deadzone");
    if (k == "left_stick_sensitivity" || k == "right_stick_sensitivity")
    {
        const char* response = pf::Config::getRetired("stick_response");
        if (!response)
            return nullptr;
        const std::string r = response;
        return r == "linear" ? "100" : r == "precise" ? "30" : r == "standard" ? "60" : nullptr;
    }
    return nullptr;
}

void configure()
{
    switch (atBoot(backend(), profileOnDisk()))
    {
    case StartStored:
        pf::logInfo("controls: registry.txt used: %s", typeLabel(type(backend())).c_str());
        break;
    case StartFresh:
        pf::logInfo("controls: no controls in registry.txt and no profile: stored %s, Controls_Custom = Control Type 1",
                    typeLabel(type(backend())).c_str());
        break;
    case StartWaitForProfile:
        pf::logInfo("controls: no controls in registry.txt, a profile is on disk: its controls are imported as Custom "
                    "when it loads; until then the game's own bindings stay");
        break;
    }
    if (forcedType() >= 0)
        pf::logInfo("controls: debug_control_type plays %s; registry.txt is not changed", typeLabel(forcedType()).c_str());
    controlsrows::loadShapes(backend(), oldStickValue);
    refresh("boot");
}

Record activeRecord()
{
    Record r = s_active.record;
    if (!s_stored)
        readSlot(&r);
    data::onePerAction(&r);
    return r;
}

int defaultsType()
{
    return s_active.type == TypeCustom ? 1 : s_active.type + 1;
}

void frame()
{
    Record have;
    if (!s_stored)
    {
        if (readSlot(&have))
            win32::controls::setStickRoles(data::inputRoles(have));
        return;
    }
    if (!readSlot(&have) || have == s_active.record)
        return;
    writeSlot(s_active.record);
#ifdef DEV_TOOLS
    if (!s_slotLogged)
        pf::logInfo("controls: pad slot set to the active %s", typeLabel(s_active.type).c_str());
#endif
    s_slotLogged = true;
}

void afterProfileLoad()
{
    if (has(backend()))
        pf::logInfo("controls: profile loaded, registry.txt used: %s; the profile's own controls are not read", typeLabel(type(backend())).c_str());
    else
    {
        Record loaded;
        if (!readSlot(&loaded))
        {
            pf::logError("controls: profile loaded with nothing in registry.txt, but there is no pad slot: nothing imported");
            return;
        }
        importProfile(backend(), loaded);
        pf::logInfo("controls: profile loaded with nothing in registry.txt: its controls imported as Custom, stored %s",
                    typeLabel(type(backend())).c_str());
        pf::logInfo("controls: the profile's controls: %s", summary(loaded).c_str());
        pf::logInfo("controls: Controls_Custom (one control per action): %s", summary(custom(backend())).c_str());
    }
    refresh("the profile load");
    writeSlot(s_active.record);
}

void patchProfileSave(reg32 out)
{
    if (!has(backend()))
    {
        pf::logInfo("controls: profile save with nothing in registry.txt: the game's own controls block is kept");
        return;
    }
    const Record record = forProfile(backend());
    const reg32 pad = findPadSlot();
    if (pad)
    {
        const int slot = int((pad - Slots) / SlotSize);
        uint8_t block[SlotCount * ProfileRecordBytes];
        for (reg32 i = 0; i < sizeof(block); ++i)
            block[i] = rc::ld8(out + i);
        patchProfile(block, slot, record);
        for (reg32 i = 0; i < sizeof(block); ++i)
            rc::st8(out + i, block[i]);
    }
    pf::logInfo("controls: profile save: the pad record %s Controls_Custom, registry.txt keeps %s",
                pad ? "carries" : "is missing (no pad slot), so it does not carry", typeLabel(type(backend())).c_str());
#ifdef DEV_TOOLS
    pf::logInfo("controls: saved layout: %s", summary(record).c_str());
#endif
}

void saveShape(bool right, controlsrows::StickPart part)
{
    controlsrows::saveShape(backend(), right, part);
}

}}
