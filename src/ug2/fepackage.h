#ifndef UG2_FEPACKAGE_H_
#define UG2_FEPACKAGE_H_

// FEng package bytes (FRONTB.LZC): unpack, parse, edit objects, rename and write back. No game access; host tested.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ug2 { namespace fepackage {

const uint32_t ChunkPackage = 0x30210;     // FRONTB chunk: name hash, then a packed package
const uint32_t FlagHidden = 0x1, FlagButton = 0x10000000;

// Upper-cased 33-multiply hash of package and object names.
uint32_t nameHash(const std::string& name);
// Unpacks a JDLZ or RAWW block; false otherwise.
bool unpack(const uint8_t* data, size_t size, std::vector<uint8_t>* out);
// FRONTB chunk 0x30210: name hash, then a RAWW block.
std::vector<uint8_t> packageChunk(const std::string& name, const std::vector<uint8_t>& package);

const uint32_t ChunkRawPackage = 0x30203;  // FRONTB chunk holding an unpacked package

struct Tag
{
    uint16_t id;        // two characters, "Ot" is 0x744f
    std::vector<uint8_t> data;
};

struct Chunk
{
    uint32_t id;
    std::vector<uint8_t> data;
};

struct Object
{
    std::vector<Chunk> chunks;      // ObjD (data left empty, see tags), scripts, message responses
    std::vector<Tag> tags;          // the ObjD chunk's tags
};

class Package;
// Finds a package by name in an unpacked chunk list (packed 0x30210 or raw 0x30203 chunks, as FrontB.lzc and GlobalB.lzc hold).
bool findPackage(const std::vector<uint8_t>& chunks, const std::string& name, Package* out);

class Package
{
public:
    bool parse(const std::vector<uint8_t>& bytes);
    std::vector<uint8_t> write() const;

    std::string name() const { return m_name; }
    void rename(const std::string& name);

    size_t objectCount() const { return m_objects.size(); }
    // Objects with this name hash, in list order.
    std::vector<size_t> find(uint32_t hash) const;
    uint32_t type(size_t object) const;
    uint32_t hash(size_t object) const;
    uint32_t guid(size_t object) const;
    uint32_t parent(size_t object) const;      // 0 at the top level
    uint32_t flags(size_t object) const;
    void setFlags(size_t object, uint32_t flags);
    // Renames an object: the Oh tag and the hash in OP.
    void setHash(size_t object, uint32_t hash);
    bool hasTag(size_t object, uint16_t id) const;
    uint32_t tag32(size_t object, uint16_t id) const;
    bool setTag32(size_t object, uint16_t id, uint32_t value);
    // A tag's bytes to read or edit in place (same size), null when the object has none.
    std::vector<uint8_t>* tagData(size_t object, uint16_t id);
    const std::vector<uint8_t>* tagData(size_t object, uint16_t id) const;
    // The object and every object below it.
    std::vector<size_t> subtree(size_t object) const;
    // Hides these objects and their children; how many changed.
    size_t hide(uint32_t hash);
    // Copies an object subtree after itself; the copy's index.
    size_t clone(size_t object, uint32_t newHash);
    // prefix<model+1>..prefix<count> become copies of prefix<model>; older ones are renamed and hidden. The number made.
    int extendSeries(const std::string& prefix, int model, int count);
    // Scripts named script whose colour keys change alpha: a copy named keepAs, then alpha held. The number made steady.
    int steadyColourScript(uint32_t script, uint32_t keepAs);
    // The alpha changes of every colour key (after the first) of the object's scripts named script.
    std::vector<int32_t> colourAlphaDeltas(size_t object, uint32_t script) const;
    bool hasScript(size_t object, uint32_t script) const;
    // Drops the "next script" link (Sc) of the object's script; false when it had none.
    bool unchainScript(size_t object, uint32_t script);
    bool scriptChained(size_t object, uint32_t script) const;
    // Copies other's script onto the object, colour tracks only.
    bool addColourScript(size_t object, const Package& other, size_t from, uint32_t script);
    // Tracks of the object's script, -1 when it has none.
    int trackCount(size_t object, uint32_t script) const;
    // Alpha of the first colour key of the object's script, -1 when it has none.
    int32_t colourStartAlpha(size_t object, uint32_t script) const;
    // End of the script's position track: base plus last key.
    bool scriptEndPosition(size_t object, uint32_t script, float* x, float* y) const;
    // Messages the object responds to (MsgR), and copying another object's responses onto it.
    std::vector<uint32_t> responseMessages(size_t object) const;
    void addResponses(size_t object, const Package& other, size_t from);
    // Adds an object id to a message's target list (Targ); false when the package has no list for it.
    bool addTarget(uint32_t message, uint32_t guid);
    // Makes a top-level object a child of another (PA after OP).
    void setParent(size_t object, uint32_t parentGuid);
    // The message target lists (Targ) holding this object id.
    std::vector<uint32_t> targetsOf(uint32_t guid) const;
    // Resource names (RsNm) in index order.
    std::vector<std::string> resourceNames() const;
    // Copies an object subtree out of another package after this one's objects, as a new top-level object. Resources are
    // matched by name and appended when missing; hashes this package already has get hash ^ salt; MsgR chunks are dropped.
    // The copy's object indices in subtree order; empty with the reason in error on failure.
    std::vector<size_t> copyFrom(const Package& other, size_t object, uint32_t salt, std::string* error);
    uint32_t topLevelCount() const { return m_topLevel; }
    uint32_t buttonCount() const;

private:
    struct Entry
    {
        bool object;
        Chunk chunk;        // when not an object
        size_t index;       // into m_objects when an object
    };
    const Tag* findTag(size_t object, uint16_t id) const;
    Tag* findTag(size_t object, uint16_t id);
    std::vector<uint8_t> header() const;
    void addTargets(const std::vector<std::pair<uint32_t, uint32_t>>& ids);

    std::vector<Chunk> m_top;           // top-level chunks; the Obj chunk and PkHd are rebuilt
    size_t m_objChunk = 0, m_headerChunk = 0;
    std::vector<uint8_t> m_rawHeader;   // the PkHd body as read
    bool m_renamed = false, m_headerChanged = false;
    std::string m_name, m_path;
    uint32_t m_version = 0, m_zero = 0, m_resources = 0, m_topLevel = 0;
    std::vector<Entry> m_entries;       // the Obj chunk's children in order
    std::vector<Object> m_objects;
};

}}

#endif /* !UG2_FEPACKAGE_H_ */
