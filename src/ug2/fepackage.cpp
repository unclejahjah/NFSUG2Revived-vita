// FEng package bytes: the chunk layout (FEn, PkHd, Obj with one FOb chunk per object, ObjD tags) and JDLZ unpacking.

#include "fepackage.h"

#include <algorithm>
#include <cstring>

namespace ug2 { namespace fepackage {

namespace {

const uint32_t IdPackage = 0xe76e4546;      // "FEn\xe7"
const uint32_t IdHeader = 0x64486b50;       // "PkHd"
const uint32_t IdObjects = 0xcc6a624f;      // "Obj\xcc"
const uint32_t IdObject = 0xea624f46;       // "FOb\xea"
const uint32_t IdObjectData = 0x446a624f;   // "ObjD"
const uint32_t IdButtons = 0x6e747542;      // "Butn"
const uint32_t IdResources = 0xcc736552;    // "Res\xcc": RsNm names, RsRq one 24-byte record per resource
const uint32_t IdResNames = 0x6d4e7352, IdResRequests = 0x71527352, IdMessages = 0x5267734d;  // "RsNm", "RsRq", "MsgR"
const uint32_t IdTargets = 0x67726154;      // "Targ": per message, the ids of the objects it goes to
const uint16_t TagTargetList = 0x744d;      // "Mt": message hash, then object ids
const uint32_t IdScript = 0x70726353;       // "Scrp"
const uint16_t TagScriptHeader = 0x6853, TagTrackInfo = 0x4946, TagKeys = 0x644b;    // "Sh" (name hash, length, flags, tracks), "FI", "Kd"
const uint8_t TrackColour = 6, TrackVector = 4;
const uint16_t TagTrackOffset = 0x6f54;     // "To": the track's word offset in the object's data, 7 for the position
const uint32_t Jdlz = 0x5a4c444a, Raww = 0x57574152;
const uint16_t TagType = 0x744f, TagHash = 0x684f, TagParams = 0x504f, TagParent = 0x4150;

uint32_t get32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

void put32(std::vector<uint8_t>& out, uint32_t v)
{
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), b, b + 4);
}

void set32(std::vector<uint8_t>& data, size_t at, uint32_t v)
{
    memcpy(&data[at], &v, 4);
}

bool readChunks(const uint8_t* p, size_t size, std::vector<Chunk>* out)
{
    size_t at = 0;
    while (at < size)
    {
        if (size - at < 8)
            return false;
        const uint32_t id = get32(p + at), length = get32(p + at + 4);
        if (length > size - at - 8)
            return false;
        out->push_back({ id, std::vector<uint8_t>(p + at + 8, p + at + 8 + length) });
        at += 8 + length;
    }
    return true;
}

void writeChunk(std::vector<uint8_t>& out, uint32_t id, const std::vector<uint8_t>& data)
{
    put32(out, id);
    put32(out, uint32_t(data.size()));
    out.insert(out.end(), data.begin(), data.end());
}

bool readTags(const std::vector<uint8_t>& data, std::vector<Tag>* out)
{
    size_t at = 0;
    while (at < data.size())
    {
        if (data.size() - at < 4)
            return false;
        uint16_t id, length;
        memcpy(&id, &data[at], 2);
        memcpy(&length, &data[at + 2], 2);
        if (length > data.size() - at - 4)
            return false;
        out->push_back({ id, std::vector<uint8_t>(data.begin() + long(at + 4), data.begin() + long(at + 4 + length)) });
        at += 4 + length;
    }
    return true;
}

std::vector<uint8_t> writeTags(const std::vector<Tag>& tags)
{
    std::vector<uint8_t> out;
    for (const Tag& t : tags)
    {
        const uint16_t length = uint16_t(t.data.size());
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&t.id), reinterpret_cast<const uint8_t*>(&t.id) + 2);
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(&length), reinterpret_cast<const uint8_t*>(&length) + 2);
        out.insert(out.end(), t.data.begin(), t.data.end());
    }
    return out;
}

// JDLZ: literal and match flag streams, two match forms.
bool unpackJdlz(const uint8_t* data, size_t size, std::vector<uint8_t>* out)
{
    if (size < 0x12 || data[4] != 2)
        return false;
    const uint32_t unpacked = get32(data + 8), packed = get32(data + 12);
    if (packed > size)
        return false;
    out->clear();
    out->reserve(unpacked);
    uint32_t flags1 = data[0x10] | 0x100, flags2 = data[0x11] | 0x100;
    size_t src = 0x12;
    int64_t left = int64_t(packed) - 0x12;
    while (left > 0)
    {
        if (flags1 & 1)
        {
            if (src + 2 > size)
                return false;
            const uint32_t b0 = data[src], b1 = data[src + 1];
            uint32_t length, distance;
            if (flags2 & 1)
            {
                length = (((b0 & 0xf0) << 4) | b1) + 3;
                distance = (b0 & 0xf) + 1;
            }
            else
            {
                distance = (((b0 & 0xe0) << 3) | b1) + 0x11;
                length = (b0 & 0x1f) + 3;
            }
            if (distance > out->size())
                return false;
            for (uint32_t i = 0; i < length; ++i)
                out->push_back((*out)[out->size() - distance]);
            flags2 >>= 1;
            src += 2;
            left -= 2;
        }
        else
        {
            if (out->size() < unpacked)
            {
                if (src >= size)
                    return false;
                out->push_back(data[src++]);
            }
            --left;
        }
        flags1 >>= 1;
        if (flags1 == 1 && left > 0)
        {
            flags1 = data[src++] | 0x100;
            --left;
        }
        if (flags2 == 1 && left > 0)
        {
            flags2 = data[src++] | 0x100;
            --left;
        }
    }
    if (out->size() < unpacked)
        return false;
    out->resize(unpacked);
    return true;
}

}

uint32_t nameHash(const std::string& name)
{
    uint32_t hash = 0xFFFFFFFF;
    for (char c : name)
        hash = uint32_t(uint8_t(c >= 'a' && c <= 'z' ? c - 32 : c)) + 33 * hash;
    return hash;
}

bool unpack(const uint8_t* data, size_t size, std::vector<uint8_t>* out)
{
    if (size < 0x10)
        return false;
    if (get32(data) == Jdlz)
        return unpackJdlz(data, size, out);
    if (get32(data) == Raww && data[4] == 1 && get32(data + 8) <= size - 0x10)
    {
        out->assign(data + 0x10, data + 0x10 + get32(data + 8));
        return true;
    }
    return false;
}

std::vector<uint8_t> packageChunk(const std::string& name, const std::vector<uint8_t>& package)
{
    std::vector<uint8_t> out;
    put32(out, ChunkPackage);
    put32(out, uint32_t(4 + 0x10 + package.size()));
    put32(out, nameHash(name));
    put32(out, Raww);
    put32(out, 1);
    put32(out, uint32_t(package.size()));
    put32(out, uint32_t(package.size() + 0x10));
    out.insert(out.end(), package.begin(), package.end());
    return out;
}

bool findPackage(const std::vector<uint8_t>& chunks, const std::string& name, Package* out)
{
    const uint32_t wanted = nameHash(name);
    for (size_t at = 0; at + 8 <= chunks.size();)
    {
        const uint32_t id = get32(&chunks[at]), size = get32(&chunks[at + 4]);
        if (size > chunks.size() - at - 8)
            return false;
        std::vector<uint8_t> bytes;
        bool have = false;
        if (id == ChunkRawPackage)
        {
            bytes.assign(chunks.begin() + long(at + 8), chunks.begin() + long(at + 8 + size));
            have = true;
        }
        else if (id == ChunkPackage && size > 4 + 0x10 && get32(&chunks[at + 8]) == wanted)
            have = unpack(&chunks[at + 12], size - 4, &bytes);
        Package candidate;
        if (have && candidate.parse(bytes) && nameHash(candidate.name()) == wanted)
        {
            *out = candidate;
            return true;
        }
        at += 8 + size;
    }
    return false;
}

bool Package::parse(const std::vector<uint8_t>& bytes)
{
    *this = Package();
    if (bytes.size() < 8 || get32(bytes.data()) != IdPackage || get32(bytes.data() + 4) != bytes.size() - 8)
        return false;
    if (!readChunks(bytes.data() + 8, bytes.size() - 8, &m_top))
        return false;
    bool haveHeader = false, haveObjects = false;
    for (size_t i = 0; i < m_top.size(); ++i)
    {
        const Chunk& c = m_top[i];
        if (c.id == IdHeader && !haveHeader)
        {
            if (c.data.size() < 24)
                return false;
            const uint8_t* p = c.data.data();
            m_version = get32(p);
            m_zero = get32(p + 4);
            m_resources = get32(p + 8);
            m_topLevel = get32(p + 12);
            const uint32_t nameLength = get32(p + 16), pathLength = get32(p + 20);
            if (nameLength == 0 || pathLength == 0 || 24 + size_t(nameLength) + pathLength > c.data.size())
                return false;
            m_name.assign(reinterpret_cast<const char*>(p + 24), strnlen(reinterpret_cast<const char*>(p + 24), nameLength));
            m_path.assign(reinterpret_cast<const char*>(p + 24 + nameLength),
                          strnlen(reinterpret_cast<const char*>(p + 24 + nameLength), pathLength));
            m_rawHeader = c.data;
            m_headerChunk = i;
            haveHeader = true;
        }
        else if (c.id == IdObjects && !haveObjects)
        {
            std::vector<Chunk> children;
            if (!readChunks(c.data.data(), c.data.size(), &children))
                return false;
            for (Chunk& child : children)
            {
                if (child.id != IdObject)
                {
                    m_entries.push_back({ false, std::move(child), 0 });
                    continue;
                }
                Object object;
                if (!readChunks(child.data.data(), child.data.size(), &object.chunks))
                    return false;
                bool haveData = false;
                for (Chunk& sub : object.chunks)
                {
                    if (sub.id == IdObjectData && !haveData)
                    {
                        if (!readTags(sub.data, &object.tags))
                            return false;
                        sub.data.clear();
                        haveData = true;
                    }
                }
                if (!haveData)
                    return false;
                m_entries.push_back({ true, Chunk(), m_objects.size() });
                m_objects.push_back(std::move(object));
            }
            m_objChunk = i;
            haveObjects = true;
        }
    }
    return haveHeader && haveObjects;
}

std::vector<uint8_t> Package::header() const
{
    if (!m_renamed && !m_headerChanged && get32(m_rawHeader.data() + 12) == m_topLevel)
        return m_rawHeader;
    std::vector<uint8_t> out;
    put32(out, m_version);
    put32(out, m_zero);
    put32(out, m_resources);
    put32(out, m_topLevel);
    put32(out, uint32_t(m_name.size() + 1));
    put32(out, uint32_t(m_path.size() + 1));
    out.insert(out.end(), m_name.c_str(), m_name.c_str() + m_name.size() + 1);
    out.insert(out.end(), m_path.c_str(), m_path.c_str() + m_path.size() + 1);
    out.resize((out.size() + 3) & ~size_t(3), 0);
    return out;
}

std::vector<uint8_t> Package::write() const
{
    std::vector<uint8_t> body;
    for (size_t i = 0; i < m_top.size(); ++i)
    {
        if (i == m_headerChunk)
            writeChunk(body, IdHeader, header());
        else if (i == m_objChunk)
        {
            std::vector<uint8_t> list;
            for (const Entry& e : m_entries)
            {
                if (!e.object)
                {
                    writeChunk(list, e.chunk.id, e.chunk.data);
                    continue;
                }
                const Object& o = m_objects[e.index];
                std::vector<uint8_t> inner;
                for (const Chunk& sub : o.chunks)
                    writeChunk(inner, sub.id, sub.id == IdObjectData && sub.data.empty() ? writeTags(o.tags) : sub.data);
                writeChunk(list, IdObject, inner);
            }
            writeChunk(body, IdObjects, list);
        }
        else
            writeChunk(body, m_top[i].id, m_top[i].data);
    }
    std::vector<uint8_t> out;
    writeChunk(out, IdPackage, body);
    return out;
}

void Package::rename(const std::string& name)
{
    const size_t slash = m_path.rfind('\\');
    m_path = (slash == std::string::npos ? std::string() : m_path.substr(0, slash + 1)) + name;
    m_name = name;
    m_renamed = true;
}

const Tag* Package::findTag(size_t object, uint16_t id) const
{
    for (const Tag& t : m_objects[object].tags)
    {
        if (t.id == id)
            return &t;
    }
    return nullptr;
}

Tag* Package::findTag(size_t object, uint16_t id)
{
    return const_cast<Tag*>(static_cast<const Package*>(this)->findTag(object, id));
}

bool Package::hasTag(size_t object, uint16_t id) const
{
    return findTag(object, id) != nullptr;
}

uint32_t Package::tag32(size_t object, uint16_t id) const
{
    const Tag* t = findTag(object, id);
    return t && t->data.size() >= 4 ? get32(t->data.data()) : 0;
}

bool Package::setTag32(size_t object, uint16_t id, uint32_t value)
{
    Tag* t = findTag(object, id);
    if (!t || t->data.size() < 4)
        return false;
    set32(t->data, 0, value);
    return true;
}

std::vector<uint8_t>* Package::tagData(size_t object, uint16_t id)
{
    Tag* t = findTag(object, id);
    return t ? &t->data : nullptr;
}

const std::vector<uint8_t>* Package::tagData(size_t object, uint16_t id) const
{
    const Tag* t = findTag(object, id);
    return t ? &t->data : nullptr;
}

std::vector<size_t> Package::find(uint32_t hash) const
{
    std::vector<size_t> out;
    for (size_t i = 0; i < m_objects.size(); ++i)
    {
        if (tag32(i, TagHash) == hash)
            out.push_back(i);
    }
    return out;
}

uint32_t Package::type(size_t object) const { return tag32(object, TagType); }
uint32_t Package::hash(size_t object) const { return tag32(object, TagHash); }
uint32_t Package::guid(size_t object) const { return tag32(object, TagParams); }
uint32_t Package::parent(size_t object) const { return tag32(object, TagParent); }

uint32_t Package::flags(size_t object) const
{
    const Tag* t = findTag(object, TagParams);
    return t && t->data.size() >= 12 ? get32(t->data.data() + 8) : 0;
}

void Package::setFlags(size_t object, uint32_t flags)
{
    Tag* t = findTag(object, TagParams);
    if (t && t->data.size() >= 12)
        set32(t->data, 8, flags);
}

void Package::setHash(size_t object, uint32_t value)
{
    setTag32(object, TagHash, value);
    Tag* t = findTag(object, TagParams);
    if (t && t->data.size() >= 8)
        set32(t->data, 4, value);
}

std::vector<size_t> Package::subtree(size_t object) const
{
    std::vector<size_t> out = { object };
    for (size_t at = 0; at < out.size(); ++at)
    {
        const uint32_t id = guid(out[at]);
        for (size_t i = 0; i < m_objects.size(); ++i)
        {
            if (hasTag(i, TagParent) && parent(i) == id && std::find(out.begin(), out.end(), i) == out.end())
                out.push_back(i);
        }
    }
    return out;
}

size_t Package::hide(uint32_t hash)
{
    size_t count = 0;
    for (size_t root : find(hash))
    {
        for (size_t i : subtree(root))
        {
            if (!(flags(i) & FlagHidden))
            {
                setFlags(i, flags(i) | FlagHidden);
                ++count;
            }
        }
    }
    return count;
}

uint32_t Package::buttonCount() const
{
    uint32_t count = 0;
    for (size_t i = 0; i < m_objects.size(); ++i)
        count += (flags(i) & FlagButton) ? 1 : 0;
    return count;
}

size_t Package::clone(size_t object, uint32_t newHash)
{
    const std::vector<size_t> source = subtree(object);
    uint32_t next = 0;
    for (size_t i = 0; i < m_objects.size(); ++i)
        next = std::max(next, guid(i) + 1);
    std::vector<std::pair<uint32_t, uint32_t>> ids;
    for (size_t i : source)
        ids.push_back({ guid(i), next++ });
    size_t last = 0;
    for (size_t e = 0; e < m_entries.size(); ++e)
    {
        if (m_entries[e].object && std::find(source.begin(), source.end(), m_entries[e].index) != source.end())
            last = e;
    }
    std::vector<Entry> added;
    size_t first = m_objects.size();
    uint32_t buttons = 0;
    for (size_t n = 0; n < source.size(); ++n)
    {
        Object copy = m_objects[source[n]];
        m_objects.push_back(copy);
        const size_t at = m_objects.size() - 1;
        Tag* params = findTag(at, TagParams);
        set32(params->data, 0, ids[n].second);
        if (n == 0)
        {
            set32(params->data, 4, newHash);
            setTag32(at, TagHash, newHash);
        }
        if (Tag* p = findTag(at, TagParent))
        {
            for (const auto& id : ids)
            {
                if (get32(p->data.data()) == id.first)
                    set32(p->data, 0, id.second);
            }
        }
        buttons += (flags(at) & FlagButton) ? 1 : 0;
        added.push_back({ true, Chunk(), at });
    }
    m_entries.insert(m_entries.begin() + long(last + 1), added.begin(), added.end());
    addTargets(ids);
    if (!hasTag(first, TagParent))
        ++m_topLevel;
    for (Entry& e : m_entries)
    {
        if (!e.object && e.chunk.id == IdButtons && e.chunk.data.size() >= 4 && buttons)
            set32(e.chunk.data, 0, get32(e.chunk.data.data()) + buttons);
    }
    return first;
}

void Package::addTargets(const std::vector<std::pair<uint32_t, uint32_t>>& ids)
{
    for (Chunk& c : m_top)
    {
        std::vector<Tag> tags;
        if (c.id != IdTargets || !readTags(c.data, &tags))
            continue;
        for (Tag& t : tags)
        {
            if (t.id != TagTargetList || t.data.size() < 4)
                continue;
            const size_t count = t.data.size() / 4;
            for (const auto& id : ids)
            {
                for (size_t k = 1; k < count; ++k)
                {
                    if (get32(&t.data[4 * k]) == id.first)
                    {
                        put32(t.data, id.second);
                        break;
                    }
                }
            }
        }
        c.data = writeTags(tags);
    }
}

std::vector<uint32_t> Package::targetsOf(uint32_t id) const
{
    std::vector<uint32_t> out;
    for (const Chunk& c : m_top)
    {
        std::vector<Tag> tags;
        if (c.id != IdTargets || !readTags(c.data, &tags))
            continue;
        for (const Tag& t : tags)
        {
            for (size_t k = 1; t.id == TagTargetList && k < t.data.size() / 4; ++k)
            {
                if (get32(&t.data[4 * k]) == id)
                    out.push_back(get32(t.data.data()));
            }
        }
    }
    return out;
}

int Package::extendSeries(const std::string& prefix, int model, int count)
{
    const std::vector<size_t> models = find(nameHash(prefix + std::to_string(model)));
    if (models.empty())
        return 0;
    size_t source = models[0];
    int made = 0;
    for (int n = model + 1; n <= count; ++n)
    {
        for (size_t old : find(nameHash(prefix + std::to_string(n))))
        {
            setHash(old, nameHash(prefix + std::to_string(n) + "_UNUSED"));
            hide(hash(old));
        }
        source = clone(source, nameHash(prefix + std::to_string(n)));
        ++made;
    }
    return made;
}

namespace {

// Visits each colour track's keys after the first: the alpha of key k at keys[k * stride + 16].
template <typename Visit>
void colourTracks(std::vector<Tag>& tags, Visit visit)
{
    for (size_t i = 0; i + 1 < tags.size(); ++i)
    {
        if (tags[i].id != TagTrackInfo || tags[i].data.size() < 2 || tags[i].data[0] != TrackColour || tags[i].data[1] != 16)
            continue;
        for (size_t k = i + 1; k < tags.size() && tags[k].id != TagTrackInfo; ++k)
        {
            if (tags[k].id != TagKeys)
                continue;
            for (size_t at = 20; at + 20 <= tags[k].data.size(); at += 20)
                visit(tags[k].data, at + 16);
            break;
        }
    }
}

uint32_t scriptName(const std::vector<Tag>& tags)
{
    for (const Tag& t : tags)
    {
        if (t.id == TagScriptHeader && t.data.size() >= 4)
            return get32(t.data.data());
    }
    return 0;
}

}

bool Package::hasScript(size_t object, uint32_t script) const
{
    for (const Chunk& c : m_objects[object].chunks)
    {
        std::vector<Tag> tags;
        if (c.id == IdScript && readTags(c.data, &tags) && scriptName(tags) == script)
            return true;
    }
    return false;
}

std::vector<int32_t> Package::colourAlphaDeltas(size_t object, uint32_t script) const
{
    std::vector<int32_t> out;
    for (const Chunk& c : m_objects[object].chunks)
    {
        std::vector<Tag> tags;
        if (c.id != IdScript || !readTags(c.data, &tags) || scriptName(tags) != script)
            continue;
        colourTracks(tags, [&](std::vector<uint8_t>& keys, size_t at) { out.push_back(int32_t(get32(&keys[at]))); });
    }
    return out;
}

int Package::steadyColourScript(uint32_t script, uint32_t keepAs)
{
    int made = 0;
    for (Object& o : m_objects)
    {
        for (size_t c = 0; c < o.chunks.size(); ++c)
        {
            std::vector<Tag> tags;
            if (o.chunks[c].id != IdScript || !readTags(o.chunks[c].data, &tags) || scriptName(tags) != script)
                continue;
            bool varies = false;
            colourTracks(tags, [&](std::vector<uint8_t>& keys, size_t at) { varies = varies || get32(&keys[at]) != 0; });
            if (!varies)
                continue;
            Chunk copy = o.chunks[c];
            std::vector<Tag> kept;
            readTags(copy.data, &kept);
            for (Tag& t : kept)
            {
                if (t.id == TagScriptHeader && t.data.size() >= 4)
                    set32(t.data, 0, keepAs);
            }
            copy.data = writeTags(kept);
            colourTracks(tags, [&](std::vector<uint8_t>& keys, size_t at) { set32(keys, at, 0); });
            o.chunks[c].data = writeTags(tags);
            o.chunks.insert(o.chunks.begin() + long(c + 1), copy);
            ++c;
            ++made;
        }
    }
    return made;
}

namespace {

struct ResourceTable
{
    size_t chunk = size_t(-1), names = 0, requests = 0;     // chunk: index in the top chunks; names, requests: index among its children
    std::vector<Chunk> children;
};

bool readResources(const std::vector<Chunk>& top, ResourceTable* out)
{
    for (size_t i = 0; i < top.size(); ++i)
    {
        if (top[i].id != IdResources)
            continue;
        out->chunk = i;
        if (!readChunks(top[i].data.data(), top[i].data.size(), &out->children))
            return false;
        bool haveNames = false, haveRequests = false;
        for (size_t c = 0; c < out->children.size(); ++c)
        {
            if (out->children[c].id == IdResNames && !haveNames)
                out->names = c, haveNames = true;
            if (out->children[c].id == IdResRequests && !haveRequests)
                out->requests = c, haveRequests = true;
        }
        return haveNames && haveRequests && out->children[out->requests].data.size() >= 4;
    }
    return false;
}

std::vector<std::string> namesOf(const ResourceTable& t)
{
    std::vector<std::string> out;
    const std::vector<uint8_t>& names = t.children[t.names].data;
    const uint32_t count = get32(t.children[t.requests].data.data());
    size_t at = 0;
    for (uint32_t i = 0; i < count && at <= names.size(); ++i)
    {
        const size_t end = std::find(names.begin() + long(at), names.end(), uint8_t(0)) - names.begin();
        out.push_back(std::string(names.begin() + long(at), names.begin() + long(end)));
        at = end + 1;
    }
    return out;
}

}

std::vector<std::string> Package::resourceNames() const
{
    ResourceTable table;
    return readResources(m_top, &table) ? namesOf(table) : std::vector<std::string>();
}

std::vector<size_t> Package::copyFrom(const Package& other, size_t object, uint32_t salt, std::string* error)
{
    std::vector<size_t> none;
    ResourceTable mine, theirs;
    if (!readResources(m_top, &mine) || !readResources(other.m_top, &theirs))
    {
        *error = "a package has no resource table";
        return none;
    }
    std::vector<std::string> myNames = namesOf(mine);
    const std::vector<std::string> theirNames = namesOf(theirs);
    const std::vector<size_t> source = other.subtree(object);

    std::vector<std::pair<uint32_t, uint32_t>> resources;     // their index, our index
    bool changed = false;
    for (size_t i : source)
    {
        const uint32_t type = other.type(i);
        const Tag* params = other.findTag(i, TagParams);
        if ((type != 1 && type != 2) || !params || params->data.size() < 16)
            continue;
        const uint32_t index = get32(params->data.data() + 12);
        bool known = false;
        for (const auto& r : resources)
            known = known || r.first == index;
        if (known)
            continue;
        if (index >= theirNames.size() || theirNames[index].empty())
        {
            *error = "object has resource index " + std::to_string(index) + " the donor does not list";
            return none;
        }
        size_t at = 0;
        while (at < myNames.size() && myNames[at] != theirNames[index])
            ++at;
        if (at == myNames.size())
        {
            std::vector<uint8_t>& names = mine.children[mine.names].data;
            std::vector<uint8_t>& requests = mine.children[mine.requests].data;
            size_t used = 0;
            for (const std::string& n : myNames)
                used += n.size() + 1;
            names.resize(used);
            const uint32_t offset = uint32_t(used);
            names.insert(names.end(), theirNames[index].begin(), theirNames[index].end());
            names.push_back(0);
            while (names.size() % 4)
                names.push_back(0);
            std::vector<uint8_t> record(theirs.children[theirs.requests].data.begin() + long(4 + 24 * index),
                                        theirs.children[theirs.requests].data.begin() + long(4 + 24 * (index + 1)));
            set32(record, 4, offset);
            requests.insert(requests.end(), record.begin(), record.end());
            set32(requests, 0, uint32_t(myNames.size() + 1));
            myNames.push_back(theirNames[index]);
            ++m_resources;
            changed = true;
        }
        resources.push_back({ index, uint32_t(at) });
    }
    if (changed)
    {
        std::vector<uint8_t> body;
        for (const Chunk& c : mine.children)
            writeChunk(body, c.id, c.data);
        m_top[mine.chunk].data = body;
    }

    std::vector<uint32_t> taken;
    for (size_t i = 0; i < m_objects.size(); ++i)
        taken.push_back(hash(i));
    uint32_t next = 0;
    for (size_t i = 0; i < m_objects.size(); ++i)
        next = std::max(next, guid(i) + 1);
    std::vector<std::pair<uint32_t, uint32_t>> ids;
    for (size_t i : source)
        ids.push_back({ other.guid(i), next++ });

    std::vector<size_t> added;
    for (size_t n = 0; n < source.size(); ++n)
    {
        Object copy = other.m_objects[source[n]];
        copy.chunks.erase(std::remove_if(copy.chunks.begin(), copy.chunks.end(), [](const Chunk& c) { return c.id == IdMessages; }),
                          copy.chunks.end());
        m_objects.push_back(copy);
        const size_t at = m_objects.size() - 1;
        Tag* params = findTag(at, TagParams);
        set32(params->data, 0, ids[n].second);
        const uint32_t old = other.hash(source[n]);
        const bool collides = std::find(taken.begin(), taken.end(), old) != taken.end();
        if (collides)
        {
            set32(params->data, 4, old ^ salt);
            setTag32(at, TagHash, old ^ salt);
        }
        if (params->data.size() >= 16)
        {
            for (const auto& r : resources)
            {
                if ((other.type(source[n]) == 1 || other.type(source[n]) == 2) && get32(params->data.data() + 12) == r.first)
                {
                    set32(params->data, 12, r.second);
                    break;
                }
            }
        }
        if (n == 0)
        {
            auto& tags = m_objects[at].tags;
            tags.erase(std::remove_if(tags.begin(), tags.end(), [](const Tag& t) { return t.id == TagParent; }), tags.end());
        }
        else if (Tag* p = findTag(at, TagParent))
        {
            for (const auto& id : ids)
            {
                if (get32(p->data.data()) == id.first)
                {
                    set32(p->data, 0, id.second);
                    break;
                }
            }
        }
        m_entries.push_back({ true, Chunk(), at });
        added.push_back(at);
    }
    ++m_topLevel;
    m_headerChanged = true;
    return added;
}

namespace {

const uint16_t TagNextScript = 0x6353, TagMessage = 0x694d;     // "Sc" in a script, "Mi" in a message response

}

bool Package::scriptChained(size_t object, uint32_t script) const
{
    for (const Chunk& c : m_objects[object].chunks)
    {
        std::vector<Tag> tags;
        if (c.id != IdScript || !readTags(c.data, &tags) || scriptName(tags) != script)
            continue;
        for (const Tag& t : tags)
        {
            if (t.id == TagNextScript)
                return true;
        }
    }
    return false;
}

bool Package::unchainScript(size_t object, uint32_t script)
{
    bool removed = false;
    for (Chunk& c : m_objects[object].chunks)
    {
        std::vector<Tag> tags;
        if (c.id != IdScript || !readTags(c.data, &tags) || scriptName(tags) != script)
            continue;
        const size_t before = tags.size();
        tags.erase(std::remove_if(tags.begin(), tags.end(), [](const Tag& t) { return t.id == TagNextScript; }), tags.end());
        removed = removed || tags.size() != before;
        c.data = writeTags(tags);
    }
    return removed;
}

bool Package::addColourScript(size_t object, const Package& other, size_t from, uint32_t script)
{
    for (const Chunk& c : other.m_objects[from].chunks)
    {
        std::vector<Tag> tags;
        if (c.id != IdScript || !readTags(c.data, &tags) || scriptName(tags) != script)
            continue;
        std::vector<Tag> kept;
        uint32_t tracks = 0;
        bool keep = true;
        for (const Tag& t : tags)
        {
            if (t.id == TagTrackInfo)
            {
                keep = t.data.size() >= 2 && t.data[0] == TrackColour && t.data[1] == 16;
                tracks += keep ? 1 : 0;
            }
            if (keep)
                kept.push_back(t);
        }
        for (Tag& t : kept)
        {
            if (t.id == TagScriptHeader && t.data.size() >= 16)
                set32(t.data, 12, tracks);
        }
        std::vector<Chunk>& mine = m_objects[object].chunks;
        for (size_t i = mine.size(); i-- > 0;)
        {
            std::vector<Tag> old;
            if (mine[i].id == IdScript && readTags(mine[i].data, &old) && scriptName(old) == script)
                mine.erase(mine.begin() + long(i));
        }
        size_t at = mine.size();
        for (size_t i = 0; i < mine.size(); ++i)
        {
            if (mine[i].id == IdScript)
                at = i + 1;
        }
        mine.insert(mine.begin() + long(at), Chunk{ IdScript, writeTags(kept) });
        return tracks > 0;
    }
    return false;
}

int Package::trackCount(size_t object, uint32_t script) const
{
    for (const Chunk& c : m_objects[object].chunks)
    {
        std::vector<Tag> tags;
        if (c.id == IdScript && readTags(c.data, &tags) && scriptName(tags) == script)
            return int(std::count_if(tags.begin(), tags.end(), [](const Tag& t) { return t.id == TagTrackInfo; }));
    }
    return -1;
}

int32_t Package::colourStartAlpha(size_t object, uint32_t script) const
{
    for (const Chunk& c : m_objects[object].chunks)
    {
        std::vector<Tag> tags;
        if (c.id != IdScript || !readTags(c.data, &tags) || scriptName(tags) != script)
            continue;
        for (size_t i = 0; i + 1 < tags.size(); ++i)
        {
            if (tags[i].id != TagTrackInfo || tags[i].data.size() < 2 || tags[i].data[0] != TrackColour || tags[i].data[1] != 16)
                continue;
            for (size_t k = i + 1; k < tags.size() && tags[k].id != TagTrackInfo; ++k)
            {
                if (tags[k].id == TagKeys && tags[k].data.size() >= 20)
                    return int32_t(get32(&tags[k].data[16]));
            }
        }
    }
    return -1;
}

bool Package::scriptEndPosition(size_t object, uint32_t script, float* x, float* y) const
{
    for (const Chunk& c : m_objects[object].chunks)
    {
        std::vector<Tag> tags;
        if (c.id != IdScript || !readTags(c.data, &tags) || scriptName(tags) != script)
            continue;
        for (size_t i = 0; i + 1 < tags.size(); ++i)
        {
            if (tags[i].id != TagTrackInfo || tags[i].data.size() < 2 || tags[i].data[0] != TrackVector || tags[i].data[1] != 12)
                continue;
            const Tag* offset = nullptr;
            const Tag* keys = nullptr;
            for (size_t k = i + 1; k < tags.size() && tags[k].id != TagTrackInfo; ++k)
            {
                if (tags[k].id == TagTrackOffset && tags[k].data.size() >= 4)
                    offset = &tags[k];
                if (tags[k].id == TagKeys)
                    keys = &tags[k];
            }
            if (!offset || get32(offset->data.data()) != 7 || !keys || keys->data.size() < 16 || keys->data.size() % 16)
                continue;
            float base[2], last[2] = { 0, 0 };
            memcpy(base, &keys->data[4], 8);
            if (keys->data.size() >= 32)
                memcpy(last, &keys->data[keys->data.size() - 12], 8);
            *x = base[0] + last[0];
            *y = base[1] + last[1];
            return true;
        }
    }
    return false;
}

std::vector<uint32_t> Package::responseMessages(size_t object) const
{
    std::vector<uint32_t> out;
    for (const Chunk& c : m_objects[object].chunks)
    {
        std::vector<Tag> tags;
        if (c.id != IdMessages || !readTags(c.data, &tags))
            continue;
        for (const Tag& t : tags)
        {
            if (t.id == TagMessage && t.data.size() >= 4)
                out.push_back(get32(t.data.data()));
        }
    }
    return out;
}

void Package::addResponses(size_t object, const Package& other, size_t from)
{
    for (const Chunk& c : other.m_objects[from].chunks)
    {
        if (c.id == IdMessages)
            m_objects[object].chunks.push_back(c);
    }
}

bool Package::addTarget(uint32_t message, uint32_t guid)
{
    bool added = false;
    for (Chunk& c : m_top)
    {
        std::vector<Tag> tags;
        if (c.id != IdTargets || !readTags(c.data, &tags))
            continue;
        for (Tag& t : tags)
        {
            if (t.id == TagTargetList && t.data.size() >= 4 && get32(t.data.data()) == message)
            {
                put32(t.data, guid);
                added = true;
            }
        }
        c.data = writeTags(tags);
    }
    return added;
}

void Package::setParent(size_t object, uint32_t parentGuid)
{
    std::vector<Tag>& tags = m_objects[object].tags;
    if (Tag* t = findTag(object, TagParent))
    {
        set32(t->data, 0, parentGuid);
        return;
    }
    size_t at = 0;
    while (at < tags.size() && tags[at].id != TagParams)
        ++at;
    Tag parent = { TagParent, {} };
    put32(parent.data, parentGuid);
    tags.insert(tags.begin() + long(at < tags.size() ? at + 1 : at), parent);
    if (m_topLevel)
        --m_topLevel;
    m_headerChanged = true;
}

}}
