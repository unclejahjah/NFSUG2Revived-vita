#ifndef PLATFORM_VAULT_ZIP_H_
#define PLATFORM_VAULT_ZIP_H_

// What the cloud saves (the browser version's keyphrase saves, web/src/app/vault.ts and zip.ts) pack into: a zip of
// the save folder's files, stored (no compression), inside one raw deflate stream. Written here as stored blocks (a
// valid deflate stream, which the browser's DecompressionStream takes); read with a full inflate, so a version
// the browser deflated opens too. No dependencies, so a host test can check it against zlib.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace pf { namespace vaultzip
{

typedef std::vector<uint8_t> Bytes;

struct Entry
{
    std::string path;       // "NFS Underground 2/NAME/NAME", forward slashes
    Bytes       data;
};

inline uint32_t crc32(const uint8_t* data, size_t size)
{
    static uint32_t table[256];
    static bool made = false;
    if (!made)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---- Inflate (RFC 1951) ----------------------------------------------------------------------------------------------

namespace detail
{

struct Huffman
{
    uint16_t count[16];
    uint16_t symbol[288];
};

struct BitReader
{
    const uint8_t* data;
    size_t         size;
    size_t         pos = 0;
    uint32_t       bits = 0;
    int            have = 0;
    bool           bad = false;

    uint32_t get(int n)
    {
        while (have < n)
        {
            if (pos >= size)
            {
                bad = true;
                return 0;
            }
            bits |= uint32_t(data[pos++]) << have;
            have += 8;
        }
        const uint32_t v = bits & ((1u << n) - 1);
        bits >>= n;
        have -= n;
        return v;
    }
};

inline void build(Huffman* h, const uint8_t* lengths, int n)
{
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; ++i)
        ++h->count[lengths[i]];
    h->count[0] = 0;
    uint16_t offsets[16];
    offsets[1] = 0;
    for (int len = 1; len < 15; ++len)
        offsets[len + 1] = uint16_t(offsets[len] + h->count[len]);
    for (int i = 0; i < n; ++i)
        if (lengths[i])
            h->symbol[offsets[lengths[i]]++] = uint16_t(i);
}

inline int decode(BitReader& in, const Huffman& h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len)
    {
        code |= int(in.get(1));
        if (in.bad)
            return -1;
        const int count = h.count[len];
        if (code - count < first)
            return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

}

// The bytes a raw deflate stream holds; false when it is damaged or inflates past max.
inline bool inflate(const uint8_t* data, size_t size, Bytes* out, size_t max)
{
    using namespace detail;
    static const uint16_t LengthBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
                                             115, 131, 163, 195, 227, 258 };
    static const uint16_t LengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const uint16_t DistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
                                           1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const uint16_t DistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    BitReader in;
    in.data = data;
    in.size = size;
    out->clear();
    bool last = false;
    while (!last)
    {
        last = in.get(1) != 0;
        const uint32_t type = in.get(2);
        if (in.bad)
            return false;
        if (type == 0)
        {
            in.bits = 0;
            in.have = 0;
            if (in.pos + 4 > size)
                return false;
            const uint32_t len = data[in.pos] | (uint32_t(data[in.pos + 1]) << 8);
            const uint32_t inv = data[in.pos + 2] | (uint32_t(data[in.pos + 3]) << 8);
            in.pos += 4;
            if ((len ^ 0xFFFF) != inv || in.pos + len > size || out->size() + len > max)
                return false;
            out->insert(out->end(), data + in.pos, data + in.pos + len);
            in.pos += len;
            continue;
        }
        if (type == 3)
            return false;
        Huffman lit, dist;
        if (type == 1)
        {
            uint8_t lengths[288];
            for (int i = 0; i < 144; ++i)
                lengths[i] = 8;
            for (int i = 144; i < 256; ++i)
                lengths[i] = 9;
            for (int i = 256; i < 280; ++i)
                lengths[i] = 7;
            for (int i = 280; i < 288; ++i)
                lengths[i] = 8;
            build(&lit, lengths, 288);
            uint8_t d[30];
            for (int i = 0; i < 30; ++i)
                d[i] = 5;
            build(&dist, d, 30);
        }
        else
        {
            const int nlen = int(in.get(5)) + 257, ndist = int(in.get(5)) + 1, ncode = int(in.get(4)) + 4;
            if (in.bad || nlen > 286 || ndist > 30)
                return false;
            static const uint8_t Order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            uint8_t lengths[320];
            memset(lengths, 0, sizeof(lengths));
            for (int i = 0; i < ncode; ++i)
                lengths[Order[i]] = uint8_t(in.get(3));
            Huffman codes;
            build(&codes, lengths, 19);
            int index = 0;
            memset(lengths, 0, sizeof(lengths));
            while (index < nlen + ndist)
            {
                const int symbol = decode(in, codes);
                if (symbol < 0)
                    return false;
                if (symbol < 16)
                {
                    lengths[index++] = uint8_t(symbol);
                    continue;
                }
                int repeat, value = 0;
                if (symbol == 16)
                {
                    if (index == 0)
                        return false;
                    value = lengths[index - 1];
                    repeat = 3 + int(in.get(2));
                }
                else if (symbol == 17)
                    repeat = 3 + int(in.get(3));
                else
                    repeat = 11 + int(in.get(7));
                if (in.bad || index + repeat > nlen + ndist)
                    return false;
                while (repeat--)
                    lengths[index++] = uint8_t(value);
            }
            build(&lit, lengths, nlen);
            build(&dist, lengths + nlen, ndist);
        }
        for (;;)
        {
            const int symbol = decode(in, lit);
            if (symbol < 0)
                return false;
            if (symbol < 256)
            {
                if (out->size() >= max)
                    return false;
                out->push_back(uint8_t(symbol));
                continue;
            }
            if (symbol == 256)
                break;
            const int l = symbol - 257;
            if (l >= 29)
                return false;
            const int length = LengthBase[l] + int(in.get(LengthExtra[l]));
            const int d = decode(in, dist);
            if (d < 0 || d >= 30)
                return false;
            const size_t distance = DistBase[d] + in.get(DistExtra[d]);
            if (in.bad || distance > out->size() || out->size() + size_t(length) > max)
                return false;
            const size_t from = out->size() - distance;
            for (int k = 0; k < length; ++k)
                out->push_back((*out)[from + size_t(k)]);
        }
    }
    return true;
}

// The bytes as a raw deflate stream of stored blocks.
inline Bytes deflateStored(const Bytes& data)
{
    Bytes out;
    size_t pos = 0;
    do
    {
        const size_t n = data.size() - pos < 65535 ? data.size() - pos : 65535;
        const bool last = pos + n >= data.size();
        out.push_back(last ? 1 : 0);
        out.push_back(uint8_t(n & 0xFF));
        out.push_back(uint8_t(n >> 8));
        out.push_back(uint8_t(~n & 0xFF));
        out.push_back(uint8_t((~n >> 8) & 0xFF));
        out.insert(out.end(), data.begin() + long(pos), data.begin() + long(pos + n));
        pos += n;
    } while (pos < data.size());
    return out;
}

// ---- Zip ---------------------------------------------------------------------------------------------------------------

namespace detail
{

inline void put16(Bytes& b, uint32_t v)
{
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}

inline void put32(Bytes& b, uint32_t v)
{
    put16(b, v & 0xFFFF);
    put16(b, v >> 16);
}

inline uint32_t get16(const uint8_t* p)
{
    return p[0] | (uint32_t(p[1]) << 8);
}

inline uint32_t get32(const uint8_t* p)
{
    return get16(p) | (get16(p + 2) << 16);
}

}

// A zip of the entries, stored. 1980-01-01 as every file's time.
inline Bytes zip(const std::vector<Entry>& entries)
{
    using namespace detail;
    Bytes out, central;
    for (const Entry& e : entries)
    {
        const uint32_t crc = crc32(e.data.data(), e.data.size());
        const uint32_t offset = uint32_t(out.size());
        put32(out, 0x04034B50);
        put16(out, 20);
        put16(out, 0x0800);     // names are UTF-8
        put16(out, 0);
        put16(out, 0);
        put16(out, 0x21);
        put32(out, crc);
        put32(out, uint32_t(e.data.size()));
        put32(out, uint32_t(e.data.size()));
        put16(out, uint32_t(e.path.size()));
        put16(out, 0);
        out.insert(out.end(), e.path.begin(), e.path.end());
        out.insert(out.end(), e.data.begin(), e.data.end());
        put32(central, 0x02014B50);
        put16(central, 20);
        put16(central, 20);
        put16(central, 0x0800);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0x21);
        put32(central, crc);
        put32(central, uint32_t(e.data.size()));
        put32(central, uint32_t(e.data.size()));
        put16(central, uint32_t(e.path.size()));
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, 0);
        put32(central, offset);
        central.insert(central.end(), e.path.begin(), e.path.end());
    }
    const uint32_t start = uint32_t(out.size());
    out.insert(out.end(), central.begin(), central.end());
    put32(out, 0x06054B50);
    put16(out, 0);
    put16(out, 0);
    put16(out, uint32_t(entries.size()));
    put16(out, uint32_t(entries.size()));
    put32(out, uint32_t(central.size()));
    put32(out, start);
    put16(out, 0);
    return out;
}

// The files of a zip (stored or deflated entries, read through the central directory). Folders are left out. False
// when it is damaged or inflates past max in all.
inline bool unzip(const Bytes& zipBytes, std::vector<Entry>* entries, size_t max)
{
    using namespace detail;
    entries->clear();
    const size_t size = zipBytes.size();
    if (size < 22)
        return false;
    size_t eocd = size - 22;
    for (;; --eocd)
    {
        if (get32(&zipBytes[eocd]) == 0x06054B50)
            break;
        if (eocd == 0 || size - eocd > 22 + 65535)
            return false;
    }
    const uint32_t count = get16(&zipBytes[eocd + 10]);
    size_t pos = get32(&zipBytes[eocd + 16]);
    size_t total = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (pos + 46 > size || get32(&zipBytes[pos]) != 0x02014B50)
            return false;
        const uint32_t method = get16(&zipBytes[pos + 10]), crc = get32(&zipBytes[pos + 16]);
        const uint32_t packed = get32(&zipBytes[pos + 20]), plain = get32(&zipBytes[pos + 24]);
        const uint32_t nameLength = get16(&zipBytes[pos + 28]), extra = get16(&zipBytes[pos + 30]);
        const uint32_t comment = get16(&zipBytes[pos + 32]), local = get32(&zipBytes[pos + 42]);
        if (pos + 46 + nameLength > size || size_t(local) + 30 > size)
            return false;
        Entry e;
        e.path.assign(reinterpret_cast<const char*>(&zipBytes[pos + 46]), nameLength);
        pos += 46 + size_t(nameLength) + extra + comment;
        if (get32(&zipBytes[local]) != 0x04034B50)
            return false;
        const size_t body = size_t(local) + 30 + get16(&zipBytes[local + 26]) + get16(&zipBytes[local + 28]);
        if (body + packed > size || (total += plain) > max)
            return false;
        if (!e.path.empty() && e.path.back() == '/')
            continue;
        if (method == 0)
            e.data.assign(zipBytes.begin() + long(body), zipBytes.begin() + long(body + packed));
        else if (method != 8 || !inflate(&zipBytes[body], packed, &e.data, plain))
            return false;
        if (e.data.size() != plain || crc32(e.data.data(), e.data.size()) != crc)
            return false;
        entries->push_back(e);
    }
    return true;
}

}}

#endif /* !PLATFORM_VAULT_ZIP_H_ */
