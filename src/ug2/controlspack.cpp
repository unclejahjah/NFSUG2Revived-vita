// Texture pack in the layout of the game's own TPK files holding one DXT3 picture, loaded from memory.

#include "controlspack.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace ug2 { namespace controlspack {

namespace {

class Writer
{
public:
    std::vector<uint8_t> data;

    size_t begin(uint32_t id)
    {
        put32(id);
        put32(0);
        return data.size();
    }
    void end(size_t start)
    {
        const uint32_t size = uint32_t(data.size() - start);
        memcpy(&data[start - 4], &size, 4);
    }
    void put32(uint32_t v) { putBytes(&v, 4); }
    void put16(uint16_t v) { putBytes(&v, 2); }
    void put8(uint8_t v) { data.push_back(v); }
    void putBytes(const void* bytes, size_t n)
    {
        data.insert(data.end(), static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + n);
    }
    void fill(uint8_t v, size_t n) { data.insert(data.end(), n, v); }
    void text(const std::string& s, size_t n)
    {
        std::string field(n, '\0');
        field.replace(0, std::min(s.size(), n - 1), s.substr(0, n - 1));
        putBytes(field.data(), n);
    }
    // Pads so the next chunk is aligned.
    void align(size_t alignment)
    {
        const size_t padding = (alignment - (data.size() + 8) % alignment) % alignment;
        put32(0);
        put32(uint32_t(padding));
        fill(0, padding);
    }
};

const uint32_t Dxt3 = 0x33545844;       // "DXT3": explicit alpha, a format the Direct3D layer takes

}

uint32_t nameHash(const char* name)
{
    uint32_t hash = 0xFFFFFFFF;
    for (; *name; ++name)
        hash = uint8_t(*name) + 33 * hash;
    return hash;
}

int log2of(int v)
{
    int n = 0;
    while ((1 << n) < v)
        ++n;
    return n;
}

std::vector<uint8_t> buildPack(const char* textureName, const uint8_t* dxt3, size_t bytes, int width, int height)
{
    const std::vector<uint8_t> image(dxt3, dxt3 + bytes);
    const uint32_t hash = nameHash(textureName);
    const std::string packName = std::string(textureName).substr(0, 0x1B);
    const uint32_t packHash = nameHash(packName.c_str());
    Writer w;
    const size_t capsule = w.begin(0xB3300000);
    w.align(0x40);

    const size_t info = w.begin(0xB3310000);
    size_t chunk = w.begin(0x33310001);
    w.put32(4);
    w.text(packName, 0x1C);
    w.text(packName, 0x40);
    w.put32(packHash);
    w.fill(0, 24);
    w.end(chunk);

    chunk = w.begin(0x33310002);
    w.put32(hash);
    w.put32(0);
    w.end(chunk);

    chunk = w.begin(0x33310004);
    const int32_t placement = 0;
    {
        w.fill(0, 12);
        w.text(textureName, 24);
        w.put32(hash);
        w.put32(0x1A93CF);
        w.put32(0);
        w.put32(uint32_t(placement));
        w.put32(0);
        w.put32(uint32_t(image.size()));
        w.put32(0);
        w.put32(uint32_t(width / 4 * height / 4 * 16));
        w.put16(uint16_t(width));
        w.put16(uint16_t(height));
        w.put8(uint8_t(log2of(width / 4)));
        w.put8(uint8_t(log2of(height / 4)));
        w.put8(0x24);
        w.put8(0);
        w.put16(0);
        w.put8(1);
        w.fill(0, 2);
        w.put8(5);
        w.fill(0, 1);
        w.put8(0);
        w.put8(0);
        w.put8(2);
        w.put8(1);
        w.fill(0, 3);
        w.put16(0);
        w.put16(0);
        w.put16(0);
        w.put16(0);
        w.put16(uint16_t(width));
        w.put16(uint16_t(height));
        w.put16(0);
        w.fill(0, 20);
        
    }
    w.end(chunk);

    chunk = w.begin(0x33310005);
    {
        w.put32(0);
        w.put32(0);
        w.put32(1);
        w.put32(5);
        w.put32(6);
        w.put32(Dxt3);
        w.put32(0);
        w.put32(0);
    }
    w.end(chunk);
    w.end(info);

    w.align(0x80);
    const size_t pixels = w.begin(0xB3320000);
    chunk = w.begin(0x33320001);
    w.put32(0);
    w.put32(0);
    w.put32(1);
    w.put32(packHash);
    w.put32(0);
    w.put32(0);
    w.end(chunk);
    w.align(0x80);
    chunk = w.begin(0x33320002);
    w.fill(0x11, 0x78);
    w.putBytes(image.data(), image.size());
    w.end(chunk);
    w.end(pixels);
    w.end(capsule);
    return std::move(w.data);
}

}}
