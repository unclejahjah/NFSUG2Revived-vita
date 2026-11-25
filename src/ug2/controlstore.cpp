// Controls kept in registry.txt as the game's PS2 record; see controlstore.h.

#include "controlstore.h"
#include "controlsdata.h"

#include <cstring>

namespace ug2 { namespace controlstore {

namespace
{

void put32(uint8_t*& at, uint32_t v)
{
    memcpy(at, &v, 4);
    at += 4;
}

uint32_t get32(const uint8_t*& at)
{
    uint32_t v;
    memcpy(&v, at, 4);
    at += 4;
    return v;
}

}

void toBytes(const Record& r, uint8_t out[RecordBytes])
{
    uint8_t* at = out;
    for (int i = 0; i < 4; ++i)
        put32(at, uint32_t(r.axisId[i]));
    for (int i = 0; i < 4; ++i)
        put32(at, uint32_t(r.axisSign[i]));
    for (int i = 0; i < 4; ++i)
        *at++ = r.flags[i];
    *at++ = r.merged;
    for (int i = 0; i < MapEntries; ++i)
        put32(at, r.map[i]);
}

void fromBytes(const uint8_t in[RecordBytes], Record* r)
{
    memset(r, 0, sizeof(*r));
    const uint8_t* at = in;
    for (int i = 0; i < 4; ++i)
        r->axisId[i] = int32_t(get32(at));
    for (int i = 0; i < 4; ++i)
        r->axisSign[i] = int32_t(get32(at));
    for (int i = 0; i < 4; ++i)
        r->flags[i] = *at++;
    r->merged = *at++;
    for (int i = 0; i < MapEntries; ++i)
        r->map[i] = get32(at);
}

bool operator==(const Record& a, const Record& b)
{
    uint8_t x[RecordBytes], y[RecordBytes];
    toBytes(a, x);
    toBytes(b, y);
    return memcmp(x, y, RecordBytes) == 0;
}

Record presetRecord(int type)
{
    return controlsdata::presetBinds(type < 0 || type > 3 ? 1 : type + 1);
}

namespace
{

bool readType(Backend& backend, int* out)
{
    std::vector<uint8_t> bytes;
    if (!backend.read(TypeName, &bytes) || bytes.size() < 4)
        return false;
    uint32_t v;
    memcpy(&v, bytes.data(), 4);
    *out = v <= uint32_t(TypeCustom) ? int(v) : 0;
    return true;
}

void writeType(Backend& backend, int type)
{
    const uint32_t v = uint32_t(type);
    backend.write(TypeName, std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&v), reinterpret_cast<const uint8_t*>(&v) + 4));
}

bool readCustom(Backend& backend, Record* r)
{
    std::vector<uint8_t> bytes;
    if (!backend.read(CustomName, &bytes) || bytes.size() != size_t(RecordBytes))
        return false;
    fromBytes(bytes.data(), r);
    return true;
}

void writeCustom(Backend& backend, const Record& r)
{
    uint8_t bytes[RecordBytes];
    toBytes(r, bytes);
    backend.write(CustomName, std::vector<uint8_t>(bytes, bytes + RecordBytes));
}

}

const char* stickName(bool right, int part)
{
    static const char* const names[2][StickParts] =
    {
        { "Controls_LeftStickDeadzone", "Controls_LeftStickSaturation", "Controls_LeftStickSensitivity" },
        { "Controls_RightStickDeadzone", "Controls_RightStickSaturation", "Controls_RightStickSensitivity" },
    };
    return names[right ? 1 : 0][part < 0 || part >= StickParts ? 0 : part];
}

int stickDefault(int part)
{
    return part == 0 ? 15 : 100;
}

int stickClamp(int part, int percent)
{
    const int low = part == 1 ? 50 : 0, high = part == 0 ? 50 : 100;
    return percent < low ? low : percent > high ? high : percent;
}

bool stickStored(Backend& backend, bool right, int part, int* percent)
{
    std::vector<uint8_t> bytes;
    if (!backend.read(stickName(right, part), &bytes) || bytes.size() != 4)
        return false;
    uint32_t v;
    memcpy(&v, bytes.data(), 4);
    *percent = stickClamp(part, v > 1000u ? 1000 : int(v));
    return true;
}

void setStick(Backend& backend, bool right, int part, int percent)
{
    const uint32_t v = uint32_t(stickClamp(part, percent));
    backend.write(stickName(right, part), std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&v), reinterpret_cast<const uint8_t*>(&v) + 4));
}

bool has(Backend& backend)
{
    int t;
    return readType(backend, &t);
}

int type(Backend& backend)
{
    int t = 0;
    readType(backend, &t);
    return t;
}

Record custom(Backend& backend)
{
    Record r;
    if (!readCustom(backend, &r))
        return presetRecord(0);
    controlsdata::onePerAction(&r);
    return r;
}

Record active(Backend& backend)
{
    return activeFor(backend, type(backend));
}

Record activeFor(Backend& backend, int t)
{
    return t == TypeCustom ? custom(backend) : presetRecord(t);
}

void setType(Backend& backend, int t)
{
    Record r;
    if (!readCustom(backend, &r))
        writeCustom(backend, presetRecord(0));
    writeType(backend, t < 0 || t > TypeCustom ? 0 : t);
}

void setCustom(Backend& backend, const Record& record)
{
    Record clean = record;
    controlsdata::onePerAction(&clean);
    writeCustom(backend, clean);
    writeType(backend, TypeCustom);
}

namespace {

bool keepIfChanged(Backend& backend, const Record& before, Record next)
{
    Record clean = before;
    controlsdata::onePerAction(&clean);
    controlsdata::onePerAction(&next);
    if (next == clean)
        return false;
    setCustom(backend, next);
    return true;
}

}

bool bindEntry(Backend& backend, controlsdata::Entry entry, controlsdata::Control control)
{
    const Record before = active(backend);
    Record next = before;
    return controlsdata::bindControl(&next, entry, control) && keepIfChanged(backend, before, next);
}

bool removeEntry(Backend& backend, controlsdata::Entry entry)
{
    const Record before = active(backend);
    Record next = before;
    return controlsdata::unbindEntry(&next, entry) && keepIfChanged(backend, before, next);
}

Record forProfile(Backend& backend)
{
    return custom(backend);
}

void patchProfile(uint8_t* records, int slot, const Record& record)
{
    toBytes(record, records + slot * ProfileRecordBytes + 16);
}

bool importProfile(Backend& backend, const Record& profile)
{
    if (has(backend))
        return false;
    Record clean = profile;
    controlsdata::onePerAction(&clean);
    setCustom(backend, clean);
    return true;
}

bool startWithoutProfile(Backend& backend)
{
    if (has(backend))
        return false;
    setType(backend, 0);
    return true;
}

Start atBoot(Backend& backend, bool profileOnDisk)
{
    if (has(backend))
        return StartStored;
    if (profileOnDisk)
        return StartWaitForProfile;
    startWithoutProfile(backend);
    return StartFresh;
}

}}
