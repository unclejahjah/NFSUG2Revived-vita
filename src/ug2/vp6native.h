#ifndef UG2_VP6NATIVE_H_
#define UG2_VP6NATIVE_H_

// Native VP6: libavcodec decodes the game's EA VP6 packets; frames go to the codec's bordered, bottom-up YV12 layout.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ug2 { namespace vp6 {

struct Planes
{
    const uint8_t* data[3] = {};               // Y, U, V
    int pitch[3] = {};
};

class Decoder
{
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool open();                               // false when this build has no libavcodec
    bool decode(const uint8_t* payload, int size);   // payload: the chunk after its 8-byte header
    bool planes(Planes* out) const;            // the last decoded frame, valid until the next decode
    int width() const;
    int height() const;

private:
    struct Impl;
    Impl* m_;
};

// Reads the MV0K and MV0F chunks of an EA .vp6 file in order, as the game's demuxer hands them out.
class MovieFile
{
public:
    MovieFile() = default;
    MovieFile(const MovieFile&) = delete;
    MovieFile& operator=(const MovieFile&) = delete;
    ~MovieFile();
    // block: bytes per read (0: 64 KB); ahead: blocks a reader thread keeps filled (0: reads on the caller's thread).
    bool open(const char* path, size_t block = 0, int ahead = 0, int priority = 0, int cpuMask = 0);
    // The payload is followed by Padding zero bytes that its size does not count.
    bool next(std::vector<uint8_t>* payload, bool* key, size_t* size = nullptr);
    // The MVhd chunk at the start: frame size and count.
    bool header(int* width, int* height, int* frames);
    long tell() const;
    bool seek(long offset);
    // Passes over the next frame chunk without reading it; offset is where the chunk starts.
    bool skip(bool* key, long* offset);

    static const int Padding = 64;

private:
    struct Ahead;
    size_t read(uint8_t* dst, size_t n);       // a null dst skips

    FILE* file_ = nullptr;
    long pos_ = 0;                             // file offset of the next byte read
    size_t block_ = 0;
    std::vector<uint8_t> buf_;
    size_t at_ = 0, end_ = 0;
    Ahead* ahead_ = nullptr;
};

struct PlayerStats
{
    int decoded = 0;
    int errors = 0;
    int dropped = 0;                           // decoded, never shown
    int jumps = 0;                             // jumps ahead to a key frame
    uint64_t decodeUs = 0;
    uint64_t decodeMaxUs = 0;
    uint64_t readUs = 0;
    uint64_t borderUs = 0;
    uint64_t idleUs = 0;                       // worker waiting for queue space or a free buffer
    uint64_t helperUs = 0;                     // coefficient thread, from frame start to its last row
    int buffers = 0;                           // frame buffers allocated
};

// Decodes a movie ahead on its own thread into frame buffers already in the game's layout.
class Player
{
public:
    typedef uint8_t* (*Alloc)(size_t bytes, void* context);   // null when none is free
    typedef void (*Release)(uint8_t* buffer, void* context);

    struct Options
    {
        int queue = 6;                         // decoded frames kept ahead
        int priority = 100;
        int cpuMask = 0;
        int helperPriority = 100;              // coefficient thread
        int readPriority = 98;                 // read-ahead thread, on the worker's cores
        int helperCpuMask = -1;                // negative: no coefficient thread
        Alloc alloc = nullptr;
        Release release = nullptr;
        void* context = nullptr;
    };

    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    bool open(const char* path, int width, int height, const Options& options);
    // The newest frame up to k, waiting up to waitUs for k; null before the first. Kept for HoldTakes more takes.
    const uint8_t* take(int k, int* index, uint32_t waitUs);
    // Stops the worker and frees the buffers but the last one shown, which the caller then owns.
    uint8_t* close();
    PlayerStats stats() const;

    static const int HoldTakes = 4;

private:
    struct Impl;
    Impl* m_;
};

size_t layoutBytes(int width, int height);
void layoutFrame(uint8_t* dst, const Planes& src, int width, int height);
uint32_t layoutHash(const uint8_t* layout, size_t bytes);   // FNV-1a over every 61st byte

}}

#endif /* !UG2_VP6NATIVE_H_ */
