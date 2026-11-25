// Movie backend: replaces the VP6 codec's frame decode (0x700aff) with frames from <name>.mp4 beside <name>.vp6, or
// from libavcodec's VP6 decoder, laid out as the codec's bordered, bottom-up YV12. The game's demuxer, clock, audio
// and skip stay as they are.

#include "movies.h"

#include <lib/memmap.h>
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <platform/video.h>
#include <recomp.h>

#include "vp6native.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <strings.h>
#include <string>
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#endif
#include <vector>

namespace ug2 { namespace movies {

namespace
{

using x86::reg32;
namespace video = pf::video;
using win32::MemMap;

const reg32 CreateFileSlot = 0x007831D0;       // kernel32 CreateFileA import
const reg32 CodecDecode = 0x00700AFF;          // VP6 codec vtable 0x7C35B8 entry 2: thiscall (time), ret 4
const reg32 CodecDestroy = 0x00700D67;         // vtable entry 0: thiscall (flags), ret 4
const reg32 PopPacket = 0x007007F3;            // thiscall (packet**), ret 4; counts the packet in codec+0x20
const reg32 ReleasePacket = 0x0070080E;        // thiscall (packet), ret 4
const reg32 PacketTag = 0x007007C0;            // eax packet in, eax the 4-byte tag out
const reg32 GetFrame = 0x007008EF;             // thiscall, returns a FRAME from the codec's free list
const reg32 ImageHeader = 0x006F4820;          // thiscall on an image, returns its header
const reg32 SetImageData = 0x006F49D0;         // thiscall (pointer, size), ret 8
const reg32 SetImageWidth = 0x00700775;        // thiscall (width), ret 4
const reg32 SetImageHeight = 0x0070078A;       // thiscall (height), ret 4
const reg32 TagKey = 0x4D56304B;               // 'MV0K'
const reg32 TagPredicted = 0x4D563046;         // 'MV0F'
const reg32 CodecFirstFlag = 0x1C;             // nonzero until the header packet is read
const reg32 CodecPopped = 0x20;                // packets popped
const reg32 CodecWidth = 0x8;
const reg32 CodecHeight = 0xC;
const reg32 FrameImage = 0x4;
const reg32 PacketData = 0x4;                  // pointer to the chunk: tag, size, payload
const reg32 PacketSize = 0x8;                  // chunk size, header included
const reg32 ChunkHeader = 8;
const int LateFrames = 0x10;                   // frames behind the clock before the codec skips to a key frame
const int RingSize = 4;

// ModeGame: the game's decoder (host default); ModeVp6: .mp4 if present, else native VP6; ModeVp6Only: native VP6.
enum Mode { ModeGame, ModeVp6, ModeVp6Only };

struct Session
{
    video::Stream* stream = nullptr;
    video::Info info = {};
    std::string name;
    reg32 codec = 0;
    int popped = 0;                            // frame packets popped
    int delivered = 0;                         // frames taken from the stream
    int used = 0;
    int skipped = 0;
    int repeated = 0;
    uint64_t waitUs = 0;                       // time blocked in the decoder
    bool ended = false;
    unsigned startMs = 0;
    std::unique_ptr<vp6::Decoder> vp6;          // libavcodec on the game thread (movies_vp6_worker=off, dev)
    std::unique_ptr<vp6::Player> player;        // libavcodec on its own thread
    int late = 0;                              // frames shown after their time
    int lastShown = 0;
    int vp6Frames = 0;
    int vp6Errors = 0;
    uint64_t vp6Us = 0;
    uint64_t vp6MaxUs = 0;
    int hooks = 0;                             // native decode calls
    uint64_t lastHookUs = 0;
    uint64_t hookGapUs = 0;                    // between calls: game-thread time outside the hook
    uint64_t hookGapMaxUs = 0;
    uint64_t hookUs = 0;                       // inside the hook
    uint64_t hookMaxUs = 0;
    std::vector<uint8_t> last;

    bool active() const { return stream || vp6 || player; }
};

Mode s_mode = ModeGame;
std::string s_dir;
bool s_unlockDecode = false;                   // movies_vp6_unlock (dev): decode without the execution lock
bool s_worker = true;                          // movies_vp6_worker (dev)
// movies_vp6_split (dev): coefficients on a second thread: off, on (core 3 only) or core0.
enum Split { SplitOff, SplitCore3, SplitCore0 };
int s_split = SplitCore3;
int s_queue = 6;                               // movies_vp6_queue (dev): frames decoded ahead
#ifndef NFSU_VP6_CPUMASK
#define NFSU_VP6_CPUMASK pf::Thread::Cpu2
#endif
const int WorkerPriority = 100;                // below the game (96) and audio (64, 72)
const uint32_t FirstFrameWaitUs = 2000000;
std::vector<MemMap*> s_frameBlocks;            // worker frame buffers
uint8_t* s_linger = nullptr;                   // last frame of an ended movie, freed with its codec
std::string s_redirectFrom, s_redirectTo;      // movies_redirect=<from>:<to> (dev): the game opens <to>.vp6
MemMap* s_redirectPath = nullptr;
std::string s_gameDir;                         // movies_game_dir (dev): the game opens its .vp6 files from this folder
win32::MethodPtr s_createFile = nullptr;
win32::MethodPtr s_decode = nullptr;
win32::MethodPtr s_destroy = nullptr;
Session s_session;
std::atomic_flag s_nameLock = ATOMIC_FLAG_INIT;
char s_lastName[96] = {};
MemMap* s_ring[RingSize] = {};
reg32 s_ringBytes = 0;
int s_ringNext = 0;

void lockName()
{
    while (s_nameLock.test_and_set(std::memory_order_acquire))
    {
    }
}

void unlockName()
{
    s_nameLock.clear(std::memory_order_release);
}

// Calls a guest function from native code: ecx, eax and stack arguments (callee or caller cleans; esp is restored).
reg32 call(win32::WinApplication* app, x86::CPU& cpu, reg32 address, reg32 self, std::initializer_list<reg32> args,
           reg32 eax = 0)
{
    const reg32 saved = cpu.esp;
    reg32 sp = saved;
    for (const reg32* it = args.end(); it != args.begin();)
    {
        --it;
        sp -= 4;
        rc::st32(sp, *it);
    }
    sp -= 4;
    rc::st32(sp, 0);
    cpu.esp = sp;
    cpu.ecx = self;
    cpu.eax = eax;
    app->dynamic_call(address, cpu);
    cpu.esp = saved;
    return cpu.eax;
}

bool isFrameTag(reg32 tag)
{
    return tag == TagKey || tag == TagPredicted;
}

// Name of an opened ".vp6" path: letters and digits between the last separator and the extension.
bool movieName(reg32 path, char* name, size_t size)
{
    char text[260];
    size_t n = 0;
    while (n < sizeof(text) - 1 && rc::ld8(path + reg32(n)))
    {
        text[n] = char(rc::ld8(path + reg32(n)));
        ++n;
    }
    text[n] = 0;
    if (n < 5 || strcasecmp(text + n - 4, ".vp6") != 0)
        return false;
    text[n - 4] = 0;
    const char* base = text;
    for (const char* c = text; *c; ++c)
    {
        if (*c == '/' || *c == '\\')
            base = c + 1;
    }
    if (!*base || strlen(base) >= size)
        return false;
    strcpy(name, base);
    return true;
}

// skip_intro_movies: 1 the logos and the notice, 2 the opening movie too. Their screens are taken out of the
// boot flow's list of screens (0x4D9580 builds it from the name tables 0x7F65E8 and 0x7F6620, and skips the empty
// names the game itself leaves there), so nothing of them shows; the movies still play from anywhere else.
const reg32 EmptyName = 0x00783FE5;          // the game's ""
const reg32 BootScreenTables[][2] = { { 0x007F65E8, 0x007F6620 }, { 0x007F6620, 0x007F6654 } };

void removeIntroScreens(int skip)
{
    static const char* const Logos[] = { "LS_BlankMovie.fng", "LS_EAlogo.fng", "LS_THXMovie.fng", "LS_PSAMovie.fng" };
    int removed = 0;
    for (const auto& table : BootScreenTables)
    {
        for (reg32 slot = table[0]; slot < table[1]; slot += 4)
        {
            std::string name;
            for (reg32 p = rc::ld32(slot); p && rc::ld8(p) && name.size() < 64; ++p)
                name += char(rc::ld8(p));
            bool remove = skip >= 2 && name == "UG_LS_IntroFMV.fng";
            for (const char* logo : Logos)
                remove = remove || name == logo;
            if (!remove)
                continue;
            rc::st32(slot, EmptyName);
            ++removed;
        }
    }
    pf::logInfo("movies: skip_intro_movies: %d start-up screens taken out of the boot flow", removed);
}

// A guest copy of the path with the movie's name (name, the path's last part without .vp6) replaced by to.
reg32 replacedPath(reg32 path, const char* name, const std::string& to)
{
    std::string text;
    for (reg32 i = 0; rc::ld8(path + i) && i < 255; ++i)
        text += char(rc::ld8(path + i));
    const size_t at = text.size() - 4 - strlen(name);
    text = text.substr(0, at) + to + ".vp6";
    if (!s_redirectPath)
        s_redirectPath = new MemMap(4096, MemMap::High, pf::perf::AllocOther);
    const reg32 copy = s_redirectPath->getBlockStart();
    for (size_t i = 0; i <= text.size(); ++i)
        rc::st8(copy + reg32(i), uint8_t(text.c_str()[i]));
    return copy;
}

// movies_redirect: a guest copy of the path with the movie's name replaced, or 0.
reg32 redirectPath(reg32 path, const char* name)
{
    if (s_redirectFrom.empty() || strcasecmp(name, s_redirectFrom.c_str()) != 0)
        return 0;
    return replacedPath(path, name, s_redirectTo);
}

// movies_game_dir: a guest copy of the path in that folder, or 0.
reg32 relocatePath(const char* name)
{
    if (s_gameDir.empty())
        return 0;
    const std::string text = "C:\\" + s_gameDir + "\\" + name + ".vp6";
    if (!s_redirectPath)
        s_redirectPath = new MemMap(4096, MemMap::High, pf::perf::AllocOther);
    const reg32 copy = s_redirectPath->getBlockStart();
    for (size_t i = 0; i <= text.size(); ++i)
        rc::st8(copy + reg32(i), uint8_t(text.c_str()[i]));
    return copy;
}

void createFileHook(win32::WinApplication* app, x86::CPU& cpu)
{
    char name[sizeof(s_lastName)];
    const reg32 path = rc::ld32(cpu.esp + 4);
    if (movieName(path, name, sizeof(name)))
    {
        if (const reg32 copy = redirectPath(path, name))
        {
            rc::st32(cpu.esp + 4, copy);
            strcpy(name, s_redirectTo.c_str());
        }
        if (const reg32 copy = relocatePath(name))
            rc::st32(cpu.esp + 4, copy);
        lockName();
        strcpy(s_lastName, name);
        unlockName();
    }
    s_createFile(app, cpu);
}

std::string moviePath(const char* name)
{
    return s_dir + "/" + name + ".mp4";
}

std::string armClockText()
{
#ifdef __vita__
    return ", arm clock " + std::to_string(scePowerGetArmClockFrequency()) + " MHz";
#else
    return "";
#endif
}

uint8_t* allocFrame(size_t bytes, void*)
{
    MemMap* block = MemMap::tryCreate(reg32(bytes), MemMap::High, pf::perf::AllocOther);
    if (!block)
        return nullptr;
    s_frameBlocks.push_back(block);
    return win32::g_guestBase + block->getBlockStart();
}

void releaseFrame(uint8_t* buffer, void*)
{
    for (size_t i = 0; i < s_frameBlocks.size(); ++i)
    {
        if (win32::g_guestBase + s_frameBlocks[i]->getBlockStart() == buffer)
        {
            delete s_frameBlocks[i];
            s_frameBlocks.erase(s_frameBlocks.begin() + long(i));
            return;
        }
    }
}

void freeLinger()
{
    if (s_linger)
        releaseFrame(s_linger, nullptr);
    s_linger = nullptr;
}

vp6::Player::Options playerOptions()
{
    vp6::Player::Options o;
    o.queue = s_queue;
    o.priority = WorkerPriority;
    o.cpuMask = NFSU_VP6_CPUMASK;
    // On core 0 the coefficient thread competes with the game thread and loses more than it gains (device, 20 Nov).
    o.helperCpuMask = s_split == SplitCore0 ? pf::Thread::Cpu0
                    : s_split == SplitCore3 && pf::cores::hasCore3() ? pf::Thread::Cpu3 : -1;
    o.helperPriority = WorkerPriority;
    o.alloc = allocFrame;
    o.release = releaseFrame;
    return o;
}

void endSession()
{
    Session& s = s_session;
    if (!s.active())
        return;
    if (s.player)
    {
        const vp6::PlayerStats p = s.player->stats();
        freeLinger();
        s_linger = s.player->close();
        const int n = p.decoded ? p.decoded : 1;
        const int calls = s.hooks > 1 ? s.hooks : 2;
        pf::logInfo("movies: %s ended (libavcodec VP6 worker): %d frames shown, %d of them late, %d repeated, "
                    "%d decoded, %d dropped, %d errors, %d jumps, %d buffers; worker decode %.2f ms average, %.2f ms "
                    "worst, read %.2f, border %.2f, idle %u ms; %u ms%s; %d calls, in the call %.3f ms average (%.2f "
                    "worst), between calls %.2f ms average (%.2f worst)",
                    s.name.c_str(), s.used, s.late, s.repeated, p.decoded, p.dropped, p.errors, p.jumps, p.buffers,
                    double(p.decodeUs) / n / 1000.0, double(p.decodeMaxUs) / 1000.0, double(p.readUs) / n / 1000.0,
                    double(p.borderUs) / n / 1000.0, unsigned(p.idleUs / 1000), unsigned(pf::ticksMs() - s.startMs),
                    armClockText().c_str(), s.hooks, double(s.hookUs) / calls / 1000.0, double(s.hookMaxUs) / 1000.0,
                    double(s.hookGapUs) / (calls - 1) / 1000.0, double(s.hookGapMaxUs) / 1000.0);
        s = Session();
        return;
    }
    if (s.vp6)
    {
        const int n = s.vp6Frames ? s.vp6Frames : 1;
        const int calls = s.hooks > 1 ? s.hooks : 2;
        pf::logInfo("movies: %s ended (libavcodec VP6): %d frames used, %d repeated, %d decoded, %d errors, decode "
                    "%.2f ms average, %.2f ms worst, %u ms%s; %d calls, in the call %.2f ms average (%.2f worst), "
                    "between calls %.2f ms average (%.2f worst)",
                    s.name.c_str(), s.used, s.repeated, s.vp6Frames, s.vp6Errors, double(s.vp6Us) / n / 1000.0,
                    double(s.vp6MaxUs) / 1000.0, unsigned(pf::ticksMs() - s.startMs), armClockText().c_str(),
                    s.hooks, double(s.hookUs) / calls / 1000.0, double(s.hookMaxUs) / 1000.0,
                    double(s.hookGapUs) / (calls - 1) / 1000.0, double(s.hookGapMaxUs) / 1000.0);
        s = Session();
        return;
    }
    pf::logInfo("movies: %s ended: %d frames used, %d skipped, %d repeated, %d of %d taken from the decoder, %u ms, waited %u ms for it",
                s.name.c_str(), s.used, s.skipped, s.repeated, s.delivered, s.info.frames,
                unsigned(pf::ticksMs() - s.startMs), unsigned(s.waitUs / 1000));
    video::close(s.stream);
    s = Session();
}

// Feeds the movie's first key frame, which the game's own decoder consumed, to a new libavcodec decoder.
bool startVp6(reg32 codec, const char* name, int width, int height)
{
    const std::string path = s_dir + "/" + name + ".vp6";
    if (s_worker)
    {
        freeLinger();
        std::unique_ptr<vp6::Player> player(new vp6::Player);
        if (!player->open(path.c_str(), width, height, playerOptions()))
        {
            pf::logError("movies: %s: the libavcodec VP6 worker could not start on %s (%dx%d)", name, path.c_str(),
                         width, height);
            return false;
        }
        Session& s = s_session;
        s.player = std::move(player);
        s.info.width = width;
        s.info.height = height;
        s.name = name;
        s.codec = codec;
        s.popped = 1;
        s.used = 1;
        s.startMs = unsigned(pf::ticksMs());
        pf::logInfo("movies: %s plays with the libavcodec VP6 worker, %dx%d, %d frames ahead, coefficients %s", name,
                    width, height, s_queue, playerOptions().helperCpuMask < 0 ? "on the worker"
                    : s_split == SplitCore0 ? "on core 0" : "on core 3");
        return true;
    }
    vp6::MovieFile file;
    std::vector<uint8_t> payload;
    bool key = false;
    std::unique_ptr<vp6::Decoder> decoder(new vp6::Decoder);
    if (!decoder->open())
    {
        pf::logError("movies: %s: this build has no libavcodec VP6 decoder", name);
        return false;
    }
    if (!file.open(path.c_str()) || !file.next(&payload, &key) || !key ||
        !decoder->decode(payload.data(), int(payload.size())))
    {
        pf::logError("movies: %s: libavcodec could not decode the first frame of %s", name, path.c_str());
        return false;
    }
    if (decoder->width() != width || decoder->height() != height)
    {
        pf::logError("movies: %s: libavcodec decodes %dx%d, the movie is %dx%d; the game decodes it", name,
                     decoder->width(), decoder->height(), width, height);
        return false;
    }
    Session& s = s_session;
    s.vp6 = std::move(decoder);
    s.info.width = width;
    s.info.height = height;
    s.name = name;
    s.codec = codec;
    s.popped = 1;
    s.used = 1;
    s.startMs = unsigned(pf::ticksMs());
    pf::logInfo("movies: %s plays with libavcodec VP6, %dx%d", name, width, height);
    return true;
}

// After the first original decode: opens the movie's MP4 when it matches the codec's frame size.
void startSession(reg32 codec)
{
    char name[sizeof(s_lastName)];
    lockName();
    strcpy(name, s_lastName);
    unlockName();
    if (!name[0])
        return;
    video::Info info;
    video::Stream* stream = s_mode == ModeVp6Only ? nullptr : video::open(moviePath(name).c_str(), &info);
    const int width = int(rc::ld32(codec + CodecWidth));
    const int height = int(rc::ld32(codec + CodecHeight));
    if (!stream)
    {
        if (startVp6(codec, name, width, height))
            return;
        pf::logInfo("movies: %s: no native video, the game decodes it", name);
        return;
    }
    if (info.width != width || info.height != height)
    {
        pf::logError("movies: %s: the file is %dx%d, the movie %dx%d; the game decodes it", name, info.width,
                     info.height, width, height);
        video::close(stream);
        return;
    }
    Session& s = s_session;
    s.stream = stream;
    s.info = info;
    s.name = name;
    s.codec = codec;
    s.popped = 1;
    s.used = 1;
    s.last.assign(size_t(width) * size_t(height) * 3 / 2, 0);
    s.startMs = unsigned(pf::ticksMs());
    pf::logInfo("movies: %s plays natively, %dx%d, %d frames", name, width, height, info.frames);
}

// Frame k of the stream (in the session's I420 copy), or the last one when the stream is short.
const uint8_t* frameAt(Session& s, int k)
{
    const uint8_t* data = nullptr;
    while (!s.ended && s.delivered <= k)
    {
        const uint64_t before = pf::ticksUs();
        const bool got = video::next(s.stream, &data);
        s.waitUs += pf::ticksUs() - before;
        if (!got)
        {
            s.ended = true;
            break;
        }
        if (s.delivered == k)
            memcpy(s.last.data(), data, s.last.size());
        else if (s.delivered != 0)
            ++s.skipped;
        ++s.delivered;
    }
    if (s.delivered <= k)
        ++s.repeated;
    return s.last.data();
}

reg32 ringBuffer(reg32 bytes)
{
    if (s_ringBytes != bytes)
    {
        for (MemMap*& m : s_ring)
        {
            delete m;
            m = nullptr;
        }
        s_ringBytes = bytes;
    }
    MemMap*& slot = s_ring[s_ringNext];
    s_ringNext = (s_ringNext + 1) % RingSize;
    if (!slot)
        slot = new MemMap(bytes, MemMap::High, pf::perf::AllocOther);
    return slot->getBlockStart();
}

reg32 setImage(win32::WinApplication* app, x86::CPU& cpu, reg32 frame, reg32 buffer, reg32 bytes, int w, int h)
{
    const reg32 image = rc::ld32(frame + FrameImage);
    const reg32 header = call(app, cpu, ImageHeader, image, {});
    call(app, cpu, SetImageData, header, { buffer, bytes });
    call(app, cpu, SetImageWidth, image, { reg32(w) });
    call(app, cpu, SetImageHeight, image, { reg32(h) });
    return frame;
}

// The codec's output: a FRAME whose image points at a bordered, bottom-up YV12 buffer of the movie frame.
reg32 buildFrame(win32::WinApplication* app, x86::CPU& cpu, Session& s, int k)
{
    const reg32 frame = call(app, cpu, GetFrame, s.codec, {});
    if (!frame)
        return 0;
    const int w = s.info.width, h = s.info.height;
    const reg32 bytes = reg32(vp6::layoutBytes(w, h));
    if (s.player)
    {
        int index = -1;
        const uint8_t* shown = s.player->take(k, &index, 0);
        if (!shown)
        {
            // Before the worker's first frame: a decode that returns no frame stops the game's player for good.
            app->unlockContext(cpu);
            shown = s.player->take(k, &index, FirstFrameWaitUs);
            app->lockContext(cpu);
            if (!shown)
                return 0;
        }
        if (index < k)
            ++s.late;
        if (index == s.lastShown)
            ++s.repeated;
        s.lastShown = index;
        ++s.used;
        return setImage(app, cpu, frame, reg32(shown - win32::g_guestBase), bytes, w, h);
    }
    const reg32 buffer = ringBuffer(bytes);
    uint8_t* dst = win32::g_guestBase + buffer;
    vp6::Planes planes;
    if (s.vp6)
    {
        if (!s.vp6->planes(&planes))
            return 0;
    }
    else
    {
        const uint8_t* src = frameAt(s, k);
        planes.data[0] = src;
        planes.data[1] = src + w * h;
        planes.data[2] = src + w * h * 5 / 4;
        planes.pitch[0] = w;
        planes.pitch[1] = planes.pitch[2] = w / 2;
    }
    vp6::layoutFrame(dst, planes, w, h);
    ++s.used;
    return setImage(app, cpu, frame, buffer, bytes, w, h);
}

reg32 popPacket(win32::WinApplication* app, x86::CPU& cpu, reg32 codec, reg32 slot, Session& s)
{
    const reg32 packet = call(app, cpu, PopPacket, codec, { slot });
    if (packet && isFrameTag(call(app, cpu, PacketTag, 0, {}, packet)))
        ++s.popped;
    return packet;
}

// Decodes a frame packet (tag, size, then the VP6 data) with libavcodec; the frame stays current on an error.
void feedVp6(win32::WinApplication* app, x86::CPU& cpu, Session& s, reg32 packet)
{
    const reg32 data = rc::ld32(packet + PacketData);
    const reg32 size = rc::ld32(packet + PacketSize);
    if (size <= ChunkHeader)
        return;
    if (s_unlockDecode)
        app->unlockContext(cpu);
    const uint64_t start = pf::ticksUs();
    const bool ok = s.vp6->decode(win32::g_guestBase + data + ChunkHeader, int(size - ChunkHeader));
    const uint64_t took = pf::ticksUs() - start;
    if (s_unlockDecode)
        app->lockContext(cpu);
    s.vp6Us += took;
    if (took > s.vp6MaxUs)
        s.vp6MaxUs = took;
    ++s.vp6Frames;
    if (!ok)
        ++s.vp6Errors;
}

// Native version of the codec's decode for a movie in a session: the original's steps without the VP6 work.
void decodeNative(win32::WinApplication* app, x86::CPU& cpu)
{
    Session& s = s_session;
    const reg32 codec = cpu.ecx;
    const int time = int(rc::ld32(cpu.esp + 4));
    const reg32 entryEsp = cpu.esp;
    cpu.esp -= 16;
    const reg32 slot = cpu.esp;
    reg32 packet = popPacket(app, cpu, codec, slot, s);
    const int late = time - int(rc::ld32(codec + CodecPopped));
    if (late > LateFrames)
    {
        while (packet)
        {
            call(app, cpu, ReleasePacket, codec, { packet });
            packet = popPacket(app, cpu, codec, slot, s);
            if (packet && call(app, cpu, PacketTag, 0, {}, packet) == TagKey)
                break;
        }
    }
    else if (late > 0 && packet && isFrameTag(call(app, cpu, PacketTag, 0, {}, packet)))
    {
        if (s.vp6)
            feedVp6(app, cpu, s, packet);
        call(app, cpu, ReleasePacket, codec, { packet });
        packet = popPacket(app, cpu, codec, slot, s);
    }
    reg32 frame = 0;
    if (packet)
    {
        if (s.vp6 && isFrameTag(call(app, cpu, PacketTag, 0, {}, packet)))
            feedVp6(app, cpu, s, packet);
        frame = buildFrame(app, cpu, s, s.popped > 0 ? s.popped - 1 : 0);
        call(app, cpu, ReleasePacket, codec, { packet });
    }
    cpu.eax = frame;
    cpu.esp = entryEsp + 8;
}

void decodeHook(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 codec = cpu.ecx;
    const bool first = rc::ld32(codec + CodecFirstFlag) != 0;
    if (!first && s_session.active() && s_session.codec == codec)
    {
        Session& s = s_session;
        const uint64_t start = pf::ticksUs();
        if (s.hooks == 0)
            pf::logInfo("movies: %s: first decode call on guest thread 0x%08X", s.name.c_str(), unsigned(cpu.efs));
        if (s.hooks++ > 0)
        {
            const uint64_t gap = start - s.lastHookUs;
            s.hookGapUs += gap;
            s.hookGapMaxUs = gap > s.hookGapMaxUs ? gap : s.hookGapMaxUs;
        }
        decodeNative(app, cpu);
        s.lastHookUs = pf::ticksUs();
        const uint64_t took = s.lastHookUs - start;
        s.hookUs += took;
        s.hookMaxUs = took > s.hookMaxUs ? took : s.hookMaxUs;
        if (s.hooks % 250 == 0 && s.player)
        {
            const vp6::PlayerStats p = s.player->stats();
            pf::logInfo("movies: %s: %d calls, %d shown, %d late, %d decoded, worker decode %.2f ms, in the call "
                        "%.3f ms, between calls %.2f ms (averages so far)", s.name.c_str(), s.hooks, s.used, s.late,
                        p.decoded, p.decoded ? double(p.decodeUs) / p.decoded / 1000.0 : 0.0,
                        double(s.hookUs) / s.hooks / 1000.0, double(s.hookGapUs) / (s.hooks - 1) / 1000.0);
        }
        else if (s.hooks % 250 == 0)
            pf::logInfo("movies: %s: %d calls, %d used, %d decoded, decode %.2f ms, in the call %.2f ms, between "
                        "calls %.2f ms (averages so far)", s.name.c_str(), s.hooks, s.used, s.vp6Frames,
                        s.vp6Frames ? double(s.vp6Us) / s.vp6Frames / 1000.0 : 0.0,
                        double(s.hookUs) / s.hooks / 1000.0, double(s.hookGapUs) / (s.hooks - 1) / 1000.0);
        return;
    }
    if (first)
        endSession();
    s_decode(app, cpu);
    if (first && s_mode != ModeGame)
        startSession(codec);
}

void destroyHook(win32::WinApplication* app, x86::CPU& cpu)
{
    if (s_session.active() && s_session.codec == cpu.ecx)
        endSession();
    s_destroy(app, cpu);
    freeLinger();
}

// Decodes a whole file through the platform decoder and logs what came out.
void selfTest(const char* name)
{
    video::Info info;
    const unsigned start = unsigned(pf::ticksMs());
    video::Stream* stream = video::open(moviePath(name).c_str(), &info);
    if (!stream)
    {
        pf::logError("movies_selftest: %s did not open", moviePath(name).c_str());
        return;
    }
    int count = 0;
    uint32_t sum = 0, firstSum = 0;
    const uint8_t* data = nullptr;
    const size_t bytes = size_t(info.width) * size_t(info.height) * 3 / 2;
    while (video::next(stream, &data))
    {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < bytes; i += 61)
            h = (h ^ data[i]) * 16777619u;
        if (count == 0)
            firstSum = h;
        sum ^= h;
        ++count;
    }
    video::close(stream);
    pf::logInfo("movies_selftest: %s %dx%d: %d of %d frames decoded in %u ms, first frame hash %08X, all %08X", name,
                info.width, info.height, count, info.frames, unsigned(pf::ticksMs()) - start,
                unsigned(firstSum), unsigned(sum));
}

// Decodes a whole .vp6 file with libavcodec and logs the time per frame and a hash of every frame.
uint8_t* allocNative(size_t bytes, void*)
{
    return static_cast<uint8_t*>(malloc(bytes));
}

void releaseNative(uint8_t* buffer, void*)
{
    free(buffer);
}

// The worker as the game uses it, every frame waited for: its speed alone, and the hash of what the game would get.
void selfTestPlayer(const std::string& path, const char* name)
{
    int width = 0, height = 0, frames = 0;
    vp6::MovieFile probe;
    if (!probe.open(path.c_str()) || !probe.header(&width, &height, &frames))
    {
        pf::logError("movies_vp6_selftest: %s did not open", path.c_str());
        return;
    }
    vp6::Player::Options o = playerOptions();
    o.alloc = allocNative;
    o.release = releaseNative;
    vp6::Player player;
    if (!player.open(path.c_str(), width, height, o))
    {
        pf::logError("movies_vp6_selftest: the worker did not start on %s", path.c_str());
        return;
    }
    const size_t bytes = vp6::layoutBytes(width, height);
    const uint64_t start = pf::ticksUs();
    uint32_t sum = 0, firstSum = 0;
    int count = 0, missing = 0;
    for (int k = 0; k < frames; ++k)
    {
        int index = -1;
        const uint8_t* frame = player.take(k, &index, 2000000);
        if (!frame || index != k)
        {
            ++missing;
            continue;
        }
        const uint32_t h = vp6::layoutHash(frame, bytes);
        if (count == 0)
            firstSum = h;
        sum ^= h;
        ++count;
#ifdef __vita__
        if (count % 20 == 0)
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
#endif
    }
    const double wall = double(pf::ticksUs() - start) / (count ? count : 1) / 1000.0;
    const vp6::PlayerStats p = player.stats();
    const int n = p.decoded ? p.decoded : 1;
    pf::logInfo("movies_vp6_selftest: %s %dx%d (worker): %d frames, %d missing, %d errors, decode %.2f ms average, "
                "%.2f ms worst, read %.2f, border %.2f, wall %.2f ms per frame, %d buffers, first frame hash %08X, "
                "all %08X%s", name, width, height, count, missing, p.errors, double(p.decodeUs) / n / 1000.0,
                double(p.decodeMaxUs) / 1000.0, double(p.readUs) / n / 1000.0, double(p.borderUs) / n / 1000.0,
                wall, p.buffers, unsigned(firstSum), unsigned(sum), armClockText().c_str());
}

void selfTestVp6(const char* name)
{
    const std::string path = s_dir + "/" + name + ".vp6";
    if (s_worker)
    {
        selfTestPlayer(path, name);
        return;
    }
    vp6::MovieFile file;
    vp6::Decoder decoder;
    std::vector<uint8_t> payload, layout;
    bool key = false;
    if (!decoder.open() || !file.open(path.c_str()))
    {
        pf::logError("movies_vp6_selftest: %s did not open", path.c_str());
        return;
    }
    int count = 0, errors = 0;
    uint64_t totalUs = 0, maxUs = 0;
    uint32_t sum = 0, firstSum = 0;
    while (file.next(&payload, &key))
    {
        const uint64_t start = pf::ticksUs();
        const bool ok = decoder.decode(payload.data(), int(payload.size()));
        const uint64_t took = pf::ticksUs() - start;
        totalUs += took;
        maxUs = took > maxUs ? took : maxUs;
        vp6::Planes planes;
        if (!ok || !decoder.planes(&planes))
        {
            ++errors;
            continue;
        }
        layout.resize(vp6::layoutBytes(decoder.width(), decoder.height()));
        vp6::layoutFrame(layout.data(), planes, decoder.width(), decoder.height());
        const uint32_t h = vp6::layoutHash(layout.data(), layout.size());
        if (count == 0)
            firstSum = h;
        sum ^= h;
        ++count;
#ifdef __vita__
        // The Vita suspends an app that does not tick power, and this loop runs before the game does.
        if (count % 20 == 0)
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
#endif
    }
    pf::logInfo("movies_vp6_selftest: %s %dx%d: %d frames, %d errors, decode %.2f ms average, %.2f ms worst, "
                "first frame hash %08X, all %08X%s",
                name, decoder.width(), decoder.height(), count, errors, count ? double(totalUs) / count / 1000.0 : 0.0,
                double(maxUs) / 1000.0, unsigned(firstSum), unsigned(sum), armClockText().c_str());
}

}

void configure(win32::WinApplication& app)
{
#ifdef __vita__
    const Mode defaultMode = ModeVp6;
    const char* const defaultDir = "ux0:data/NeedForSpeedUnderground2/MOVIES";
#else
    const Mode defaultMode = ModeGame;
    const char* const defaultDir = "";
#endif
    static const char* const modes[] = { "vp6preferred", "vp6only" };
    const std::string choice = pf::Config::getString("movies", "");
    s_mode = defaultMode;
    if (strcasecmp(choice.c_str(), "vp6only") == 0)
        s_mode = ModeVp6Only;
    else if (strcasecmp(choice.c_str(), "vp6preferred") == 0 || strcasecmp(choice.c_str(), "vp6") == 0)
        s_mode = ModeVp6;
    else if (!choice.empty())
    {
        s_mode = ModeVp6;
        pf::logInfo("movies: movies=%s is not one of %s/%s, %s used", choice.c_str(), modes[0], modes[1], modes[0]);
    }
    static const char* const IntroSkips[] = { "off", "logos", "opening" };
    if (const int skip = pf::Config::getChoice("skip_intro_movies", IntroSkips, 3, 0))
        removeIntroScreens(skip);
    s_dir = pf::Config::getString("movies_dir", defaultDir);
    while (s_dir.size() > 1 && (s_dir.back() == '/' || s_dir.back() == '\\'))
        s_dir.pop_back();
    const std::string test = pf::Config::getString("movies_selftest", "");
    if (!test.empty())
        selfTest(test.c_str());
#ifdef DEV_SETTINGS
    s_worker = pf::Config::getBool("movies_vp6_worker", true);
    s_queue = pf::Config::getInt("movies_vp6_queue", s_queue);
    static const char* const splits[] = { "off", "on", "core0" };
    s_split = pf::Config::getChoice("movies_vp6_split", splits, 3, SplitCore3);
#endif
    const std::string vp6Test = pf::Config::getString("movies_vp6_selftest", "");
    if (!vp6Test.empty())
        selfTestVp6(vp6Test.c_str());
    if (s_mode == ModeGame)
    {
        pf::logInfo("movies: movies=game, the game decodes its VP6 movies");
        return;
    }
    if (s_dir.empty())
    {
        pf::logError("movies: movies=%s needs movies_dir; the game decodes its movies", modes[s_mode - 1]);
        s_mode = ModeGame;
        return;
    }
#ifdef DEV_SETTINGS
    s_unlockDecode = pf::Config::getBool("movies_vp6_unlock", false);
    s_gameDir = pf::Config::getString("movies_game_dir", "");
    const std::string redirect = pf::Config::getString("movies_redirect", "");
    const size_t colon = redirect.find(':');
    if (colon != std::string::npos)
    {
        s_redirectFrom = redirect.substr(0, colon);
        s_redirectTo = redirect.substr(colon + 1);
        pf::logInfo("movies: movies_redirect: %s.vp6 opens %s.vp6", s_redirectFrom.c_str(), s_redirectTo.c_str());
    }
#endif
    if (!s_createFile)
        s_createFile = app.replaceMethod(rc::ld32(CreateFileSlot), createFileHook);
    s_decode = app.replaceMethod(CodecDecode, decodeHook);
    s_destroy = app.replaceMethod(CodecDestroy, destroyHook);
    pf::logInfo("movies: movies=%s, playback from %s (the game's VP6 decoder when no source works)", modes[s_mode - 1],
                s_dir.c_str());
}

}}
