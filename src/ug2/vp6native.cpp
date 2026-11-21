// Native VP6: libavcodec decodes the game's EA VP6 packets; frames go to the codec's bordered, bottom-up YV12 layout.

#include "vp6native.h"

#include <platform/sync.h>

#include <atomic>
#include <climits>
#include <cstring>
#include <string>
#include <utility>
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#else
#include <chrono>
#endif

// Built with -fno-short-enums like FFmpeg, so libavcodec's structures have the same layout on both sides.
#ifdef NFSU_VP6_NATIVE
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>

// tools/vita/ffmpeg-patches/0001: the frame's coefficients parsed on a helper thread.
struct VP56SplitOps
{
    void* opaque;
    void (*start)(void* opaque, void (*job)(void* arg), void* arg);
    void (*join)(void* opaque);
    void (*wait)(void* opaque);
};
void ff_vp56_set_split(AVCodecContext* avctx, const VP56SplitOps* ops);
}
#endif

namespace ug2 { namespace vp6 {

namespace
{

const int Border = 48;                         // VP6 frame border, luma pixels
const uint32_t TagKey = 0x4B30564D;            // 'MV0K' read little-endian
const uint32_t TagPredicted = 0x4630564D;      // 'MV0F'
const uint32_t TagHeader = 0x6468564D;         // 'MVhd'

uint32_t readLe32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

uint64_t nowUs()
{
#ifdef __vita__
    return sceKernelGetProcessTimeWide();
#else
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

void copyPlane(uint8_t* dst, int dstPitch, const uint8_t* src, int srcPitch, int width, int height, int border)
{
    for (int row = 0; row < height; ++row)
    {
        uint8_t* line = dst + (border + row) * dstPitch + border;
        memcpy(line, src + (height - 1 - row) * srcPitch, size_t(width));
        memset(line - border, line[0], size_t(border));
        memset(line + width, line[width - 1], size_t(border));
    }
    for (int row = 0; row < border; ++row)
    {
        memcpy(dst + row * dstPitch, dst + border * dstPitch, size_t(dstPitch));
        memcpy(dst + (border + height + row) * dstPitch, dst + (border + height - 1) * dstPitch, size_t(dstPitch));
    }
}

// Fills a plane's border around its picture, as copyPlane does.
void fillBorder(uint8_t* dst, int pitch, int width, int height, int border)
{
    for (int row = 0; row < height; ++row)
    {
        uint8_t* line = dst + (border + row) * pitch + border;
        memset(line - border, line[0], size_t(border));
        memset(line + width, line[width - 1], size_t(border));
    }
    for (int row = 0; row < border; ++row)
    {
        memcpy(dst + row * pitch, dst + border * pitch, size_t(pitch));
        memcpy(dst + (border + height + row) * pitch, dst + (border + height - 1) * pitch, size_t(pitch));
    }
}

struct Layout
{
    int yPitch, cPitch, yRows, cRows;
    size_t uOffset, vOffset;

    Layout(int w, int h)
        : yPitch(w + 2 * Border), cPitch(yPitch / 2), yRows(h + 2 * Border), cRows(yRows / 2),
          uOffset(size_t(yPitch) * yRows), vOffset(uOffset + size_t(cPitch) * cRows)
    {
    }
};

}

#ifdef NFSU_VP6_NATIVE

struct Decoder::Impl
{
    AVCodecContext* ctx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* next = nullptr;
    bool have = false;
    std::vector<uint8_t> padded;
};

Decoder::Decoder() : m_(new Impl) {}

Decoder::~Decoder()
{
    avcodec_free_context(&m_->ctx);
    av_packet_free(&m_->packet);
    av_frame_free(&m_->frame);
    av_frame_free(&m_->next);
    delete m_;
}

bool Decoder::open()
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_VP6);
    if (!codec)
        return false;
    m_->ctx = avcodec_alloc_context3(codec);
    m_->packet = av_packet_alloc();
    m_->frame = av_frame_alloc();
    m_->next = av_frame_alloc();
    if (!m_->ctx || !m_->packet || !m_->frame || !m_->next)
        return false;
    return avcodec_open2(m_->ctx, codec, nullptr) >= 0;
}

bool Decoder::decode(const uint8_t* payload, int size)
{
    // libavcodec may read past the end of its input: the packet needs zeroed padding.
    m_->padded.resize(size_t(size) + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(m_->padded.data(), payload, size_t(size));
    memset(m_->padded.data() + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    m_->packet->data = m_->padded.data();
    m_->packet->size = size;
    if (avcodec_send_packet(m_->ctx, m_->packet) < 0)
        return false;
    if (avcodec_receive_frame(m_->ctx, m_->next) < 0)
        return false;
    std::swap(m_->frame, m_->next);
    av_frame_unref(m_->next);
    m_->have = m_->frame->format == AV_PIX_FMT_YUV420P;
    return m_->have;
}

bool Decoder::planes(Planes* out) const
{
    if (!m_->have)
        return false;
    for (int i = 0; i < 3; ++i)
    {
        out->data[i] = m_->frame->data[i];
        out->pitch[i] = m_->frame->linesize[i];
    }
    return true;
}

int Decoder::width() const
{
    return m_->have ? m_->frame->width : 0;
}

int Decoder::height() const
{
    return m_->have ? m_->frame->height : 0;
}

#else

struct Decoder::Impl {};
Decoder::Decoder() : m_(new Impl) {}
Decoder::~Decoder() { delete m_; }
bool Decoder::open() { return false; }
bool Decoder::decode(const uint8_t*, int) { return false; }
bool Decoder::planes(Planes*) const { return false; }
int Decoder::width() const { return 0; }
int Decoder::height() const { return 0; }

#endif

// Reads the file in blocks on its own thread, a ring of them ahead of the caller.
struct MovieFile::Ahead
{
    FILE* file = nullptr;
    size_t block = 0;
    int count = 0;
    std::vector<std::vector<uint8_t>> data;
    std::vector<size_t> size;
    int head = 0;                              // first filled block
    int filled = 0;
    size_t headPos = 0;                        // bytes of the head block already taken
    long readOffset = 0;                       // the reader's next file offset
    long seekTo = -1;
    bool eof = false;
    bool stop = false;
    int generation = 0;
    pf::Mutex mutex{"vp6 read", false};
    pf::CondVar cond{"vp6 read", mutex};
    pf::Thread* thread = nullptr;

    int run()
    {
        mutex.lock();
        for (;;)
        {
            while (!stop && seekTo < 0 && (filled == count || eof))
                cond.wait();
            if (stop)
                break;
            if (seekTo >= 0)
            {
                fseek(file, seekTo, SEEK_SET);
                readOffset = seekTo;
                seekTo = -1;
            }
            const int slot = (head + filled) % count;
            const int gen = generation;
            const long offset = readOffset;
            mutex.unlock();
            const size_t n = fread(data[size_t(slot)].data(), 1, block, file);
            mutex.lock();
            if (gen != generation)
                continue;
            size[size_t(slot)] = n;
            readOffset = offset + long(n);
            ++filled;
            if (n < block)
                eof = true;
            cond.broadcast();
        }
        mutex.unlock();
        return 0;
    }

    static int entry(void* argument)
    {
        return static_cast<Ahead*>(argument)->run();
    }

    // Copies or skips up to n bytes; fewer at the end.
    size_t take(uint8_t* dst, size_t n)
    {
        pf::LockGuard guard(mutex);
        size_t got = 0;
        while (got < n)
        {
            if (filled == 0)
            {
                if (eof)
                    break;
                cond.wait();
                continue;
            }
            const size_t avail = size[size_t(head)] - headPos;
            const size_t part = avail < n - got ? avail : n - got;
            if (dst)
                memcpy(dst + got, data[size_t(head)].data() + headPos, part);
            headPos += part;
            got += part;
            if (headPos == size[size_t(head)])
            {
                head = (head + 1) % count;
                --filled;
                headPos = 0;
                cond.broadcast();
            }
        }
        return got;
    }

    size_t buffered()
    {
        pf::LockGuard guard(mutex);
        size_t n = 0;
        for (int i = 0; i < filled; ++i)
            n += size[size_t((head + i) % count)];
        return n - headPos;
    }

    void restart(long offset)
    {
        pf::LockGuard guard(mutex);
        seekTo = offset;
        head = filled = 0;
        headPos = 0;
        eof = false;
        ++generation;
        cond.broadcast();
    }

    ~Ahead()
    {
        if (thread)
        {
            mutex.lock();
            stop = true;
            cond.broadcast();
            mutex.unlock();
            delete thread;
        }
    }
};

MovieFile::~MovieFile()
{
    delete ahead_;
    if (file_)
        fclose(file_);
}

bool MovieFile::open(const char* path, size_t block, int ahead, int priority, int cpuMask)
{
    file_ = fopen(path, "rb");
    if (!file_)
        return false;
    setvbuf(file_, nullptr, _IONBF, 0);
    block_ = block ? block : 64 * 1024;
    if (ahead > 0)
    {
        ahead_ = new Ahead;
        ahead_->file = file_;
        ahead_->block = block_;
        ahead_->count = ahead;
        ahead_->data.assign(size_t(ahead), std::vector<uint8_t>(block_));
        ahead_->size.assign(size_t(ahead), 0);
        ahead_->thread = new pf::Thread("vp6 read", &Ahead::entry, ahead_, priority, 16 * 1024, cpuMask);
        ahead_->thread->start();
    }
    return true;
}

size_t MovieFile::read(uint8_t* dst, size_t n)
{
    if (!file_)
        return 0;
    size_t got = 0;
    if (ahead_)
        got = ahead_->take(dst, n);
    else
    {
        while (got < n)
        {
            if (at_ == end_)
            {
                buf_.resize(block_);
                at_ = 0;
                end_ = fread(buf_.data(), 1, block_, file_);
                if (end_ == 0)
                    break;
            }
            const size_t part = end_ - at_ < n - got ? end_ - at_ : n - got;
            if (dst)
                memcpy(dst + got, buf_.data() + at_, part);
            at_ += part;
            got += part;
        }
    }
    pos_ += long(got);
    return got;
}

bool MovieFile::skip(bool* key, long* offset)
{
    uint8_t head[8];
    for (;;)
    {
        const long at = pos_;
        if (read(head, 8) != 8)
            return false;
        const uint32_t tag = readLe32(head);
        const uint32_t size = readLe32(head + 4);
        if (size < 8 || !seek(pos_ + long(size - 8)))
            return false;
        if (tag == TagKey || tag == TagPredicted)
        {
            *key = tag == TagKey;
            *offset = at;
            return true;
        }
    }
}

bool MovieFile::next(std::vector<uint8_t>* payload, bool* key, size_t* bytes)
{
    uint8_t head[8];
    while (read(head, 8) == 8)
    {
        const uint32_t tag = readLe32(head);
        const uint32_t size = readLe32(head + 4);
        if (size < 8)
            return false;
        const bool frame = tag == TagKey || tag == TagPredicted;
        if (!frame)
        {
            if (!seek(pos_ + long(size - 8)))
                return false;
            continue;
        }
        const size_t n = size - 8;
        payload->resize(n + Padding);
        if (read(payload->data(), n) != n)
            return false;
        memset(payload->data() + n, 0, Padding);
        if (bytes)
            *bytes = n;
        else
            payload->resize(n);
        *key = tag == TagKey;
        return true;
    }
    return false;
}

bool MovieFile::header(int* width, int* height, int* frames)
{
    uint8_t head[32];
    if (read(head, sizeof(head)) != sizeof(head) || readLe32(head) != TagHeader || readLe32(head + 4) < sizeof(head))
        return false;
    *width = head[12] | head[13] << 8;
    *height = head[14] | head[15] << 8;
    *frames = int(readLe32(head + 16));
    return seek(long(readLe32(head + 4)));
}

long MovieFile::tell() const
{
    return file_ ? pos_ : -1;
}

// Skips forward within the buffer, else reads again from there.
bool MovieFile::seek(long offset)
{
    if (!file_)
        return false;
    if (offset >= pos_)
    {
        const size_t ahead = ahead_ ? ahead_->buffered() : end_ - at_;
        if (size_t(offset - pos_) <= ahead)
        {
            read(nullptr, size_t(offset - pos_));
            return true;
        }
    }
    pos_ = offset;
    if (ahead_)
    {
        ahead_->restart(offset);
        return true;
    }
    at_ = end_ = 0;
    return fseek(file_, offset, SEEK_SET) == 0;
}

#ifdef NFSU_VP6_NATIVE

namespace
{

const int MaxQueue = 16;
const int MaxBuffers = MaxQueue + 8;           // queue, golden, current, the held and pinned ones, one spare
const size_t ReadBlock = 64 * 1024;
const int ReadBlocks = 16;                     // read ahead of the worker
const int ScanAhead = 64;                      // frames indexed past the target in one scan for key frames

struct Buffer
{
    uint8_t* base = nullptr;
    std::atomic<int> libav{0};                 // libavcodec or the queue still refers to it
    std::atomic<int> handedAt{INT_MIN / 2};    // take that last returned it
};

struct Entry
{
    int index = 0;
    AVFrame* frame = nullptr;
    Buffer* buffer = nullptr;
};

void freeBuffer(void* opaque, uint8_t*)
{
    static_cast<Buffer*>(opaque)->libav.store(0, std::memory_order_release);
}

// Runs the coefficient job of each frame on its own thread.
struct Helper
{
    pf::Thread* thread = nullptr;
    pf::Semaphore go{"vp6 split go", 0};
    pf::Semaphore done{"vp6 split done", 0};
    void (*job)(void*) = nullptr;
    void* arg = nullptr;
    std::atomic<bool> quit{false};
    std::atomic<uint64_t> busyUs{0};
    VP56SplitOps ops = { this, &Helper::start, &Helper::join, &Helper::wait };

    static void start(void* opaque, void (*job)(void*), void* arg)
    {
        Helper* h = static_cast<Helper*>(opaque);
        h->job = job;
        h->arg = arg;
        h->go.post();
    }

    static void join(void* opaque)
    {
        static_cast<Helper*>(opaque)->done.wait();
    }

    // A sleep takes far longer than a row: spin briefly instead.
    static void wait(void*)
    {
        for (volatile int i = 0; i < 64; i = i + 1)
        {
        }
    }

    static int entry(void* argument)
    {
        Helper* h = static_cast<Helper*>(argument);
        for (;;)
        {
            h->go.wait();
            if (h->quit.load(std::memory_order_acquire))
                return 0;
            const uint64_t start = nowUs();
            h->job(h->arg);
            h->busyUs += nowUs() - start;
            h->done.post();
        }
    }

    void open(int priority, int cpuMask)
    {
        thread = new pf::Thread("vp6 coefficients", &Helper::entry, this, priority, 64 * 1024, cpuMask);
        thread->start();
    }

    ~Helper()
    {
        if (thread)
        {
            quit.store(true, std::memory_order_release);
            go.post();
            delete thread;
        }
    }
};

}

struct Player::Impl
{
    Options options;
    int width = 0, height = 0, frames = 0;
    size_t bytes = 0;
    MovieFile file;
    MovieFile scan;                            // for key frame offsets ahead, small reads
    AVCodecContext* ctx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* decoded = nullptr;
    std::vector<uint8_t> payload;
    Buffer pool[MaxBuffers];
    int poolCount = 0;
    Entry ring[MaxQueue];
    int queue = 6;
    std::atomic<int> head{0};                  // first entry the game has not consumed
    std::atomic<int> tail{0};                  // next entry the worker fills
    int freed = 0;                             // worker: entries below are unreferenced
    std::atomic<int> takes{0};
    std::atomic<Buffer*> pinned{nullptr};
    std::atomic<int> wanted{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    pf::Semaphore space{"vp6 space", 0};
    pf::Semaphore ready{"vp6 ready", 0};
    pf::Thread* thread = nullptr;
    Helper* helper = nullptr;
    int next = 0;                              // index of the frame chunk at the file position
    std::vector<std::pair<int, long>> keys;    // key frames seen: index, chunk offset
    int scanIndex = 0;                         // frames indexed so far
    long scanOffset = 0;
    Buffer* lastBuffer = nullptr;              // game thread
    int lastIndex = -1;
    int dropped = 0;
    std::atomic<int> decodedCount{0};
    std::atomic<int> errors{0};
    std::atomic<int> jumps{0};
    std::atomic<uint64_t> decodeUs{0}, decodeMaxUs{0}, readUs{0}, borderUs{0}, idleUs{0};
    uint64_t helperUs = 0;

    void releaseConsumed()
    {
        const int h = head.load(std::memory_order_acquire);
        for (; freed < h; ++freed)
            av_frame_unref(ring[freed % MaxQueue].frame);
    }

    void idle(uint32_t us)
    {
        const uint64_t start = nowUs();
        space.waitForUs(us);
        idleUs += nowUs() - start;
    }

    Buffer* freeBufferSlot()
    {
        for (;;)
        {
            releaseConsumed();
            const int now = takes.load(std::memory_order_acquire);
            const Buffer* pin = pinned.load(std::memory_order_acquire);
            for (int i = 0; i < poolCount; ++i)
            {
                Buffer& b = pool[i];
                if (b.libav.load(std::memory_order_acquire) == 0 && &b != pin &&
                    now - b.handedAt.load(std::memory_order_acquire) >= HoldTakes)
                    return &b;
            }
            if (poolCount < MaxBuffers)
            {
                if (uint8_t* base = options.alloc(bytes, options.context))
                {
                    pool[poolCount].base = base;
                    return &pool[poolCount++];
                }
            }
            if (stop.load(std::memory_order_relaxed))
                return nullptr;
            idle(5000);
        }
    }

    static int getBuffer(AVCodecContext* c, AVFrame* frame, int)
    {
        Impl* m = static_cast<Impl*>(c->opaque);
        if (frame->format != AV_PIX_FMT_YUV420P || frame->width != m->width || frame->height != m->height)
            return AVERROR(EINVAL);
        Buffer* b = m->freeBufferSlot();
        if (!b)
            return AVERROR(ENOMEM);
        b->libav.store(1, std::memory_order_relaxed);
        frame->buf[0] = av_buffer_create(b->base, m->bytes, freeBuffer, b, 0);
        if (!frame->buf[0])
        {
            b->libav.store(0, std::memory_order_relaxed);
            return AVERROR(ENOMEM);
        }
        const Layout l(m->width, m->height);
        frame->data[0] = b->base + Border * l.yPitch + Border;
        frame->data[1] = b->base + l.uOffset + Border / 2 * l.cPitch + Border / 2;
        frame->data[2] = b->base + l.vOffset + Border / 2 * l.cPitch + Border / 2;
        frame->linesize[0] = l.yPitch;
        frame->linesize[1] = frame->linesize[2] = l.cPitch;
        frame->extended_data = frame->data;
        return 0;
    }

    Buffer* bufferOf(const AVFrame* frame)
    {
        const Layout l(width, height);
        const uint8_t* base = frame->data[0] - (Border * l.yPitch + Border);
        for (int i = 0; i < poolCount; ++i)
        {
            if (pool[i].base == base)
                return &pool[i];
        }
        return nullptr;
    }

    // Far behind: continues from the latest key frame wanted.
    void jumpAhead()
    {
        const int target = wanted.load(std::memory_order_acquire);
        if (target <= next + 1)
            return;
        if (scanIndex < next)
        {
            scanIndex = next;
            scanOffset = file.tell();
        }
        if (scanIndex <= target)
        {
            if (!scan.seek(scanOffset))
                return;
            bool key = false;
            long offset = 0;
            while (scanIndex <= target + ScanAhead && scan.skip(&key, &offset))
            {
                if (key && (keys.empty() || keys.back().first < scanIndex))
                    keys.emplace_back(scanIndex, offset);
                ++scanIndex;
                scanOffset = scan.tell();
            }
        }
        for (auto it = keys.rbegin(); it != keys.rend(); ++it)
        {
            if (it->first <= target)
            {
                if (it->first > next && file.seek(it->second))
                {
                    next = it->first;
                    ++jumps;
                }
                return;
            }
        }
    }

    void decodeOne()
    {
        jumpAhead();
        bool key = false;
        size_t size = 0;
        const uint64_t readStart = nowUs();
        const long at = file.tell();
        if (!file.next(&payload, &key, &size))
        {
            finished.store(true, std::memory_order_release);
            ready.post();
            return;
        }
        readUs += nowUs() - readStart;
        if (key && next >= scanIndex && (keys.empty() || keys.back().first < next))
            keys.emplace_back(next, at);
        const int index = next++;
        const uint64_t start = nowUs();
        packet->data = payload.data();
        packet->size = int(size);
        bool ok = avcodec_send_packet(ctx, packet) >= 0 && avcodec_receive_frame(ctx, decoded) >= 0;
        Buffer* b = ok ? bufferOf(decoded) : nullptr;
        const uint64_t took = nowUs() - start;
        decodeUs += took;
        if (took > decodeMaxUs)
            decodeMaxUs = took;
        ++decodedCount;
        if (!b)
        {
            ++errors;
            av_frame_unref(decoded);
            return;
        }
        const uint64_t borderStart = nowUs();
        const Layout l(width, height);
        fillBorder(b->base, l.yPitch, width, height, Border);
        fillBorder(b->base + l.uOffset, l.cPitch, width / 2, height / 2, Border / 2);
        fillBorder(b->base + l.vOffset, l.cPitch, width / 2, height / 2, Border / 2);
        borderUs += nowUs() - borderStart;
        const int t = tail.load(std::memory_order_relaxed);
        Entry& e = ring[t % MaxQueue];
        av_frame_move_ref(e.frame, decoded);
        e.index = index;
        e.buffer = b;
        tail.store(t + 1, std::memory_order_release);
        ready.post();
    }

    int run()
    {
        while (!stop.load(std::memory_order_relaxed))
        {
            releaseConsumed();
            if (finished.load(std::memory_order_relaxed) ||
                tail.load(std::memory_order_relaxed) - head.load(std::memory_order_acquire) >= queue)
            {
                idle(20000);
                continue;
            }
            decodeOne();
        }
        return 0;
    }

    static int entry(void* argument)
    {
        return static_cast<Impl*>(argument)->run();
    }
};

Player::Player() : m_(new Impl) {}

Player::~Player()
{
    if (uint8_t* kept = close())
        m_->options.release(kept, m_->options.context);
    delete m_;
}

bool Player::open(const char* path, int width, int height, const Options& options)
{
    Impl& m = *m_;
    m.options = options;
    m.queue = options.queue < 1 ? 1 : options.queue > MaxQueue - 1 ? MaxQueue - 1 : options.queue;
    m.width = width;
    m.height = height;
    m.bytes = layoutBytes(width, height);
    int w = 0, h = 0;
    if (!options.alloc || !options.release || width % 16 || height % 16 ||
        !m.file.open(path, ReadBlock, ReadBlocks, options.readPriority, options.cpuMask) ||
        !m.file.header(&w, &h, &m.frames) || w != width || h != height || !m.scan.open(path, 2048))
        return false;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_VP6F);
    if (!codec)
        return false;
    m.ctx = avcodec_alloc_context3(codec);
    m.packet = av_packet_alloc();
    m.decoded = av_frame_alloc();
    if (!m.ctx || !m.packet || !m.decoded)
        return false;
    for (Entry& e : m.ring)
    {
        e.frame = av_frame_alloc();
        if (!e.frame)
            return false;
    }
    // VP6F is VP6 without the vertical flip: its rows come out bottom-up, as the game lays them out.
    m.ctx->opaque = &m;
    m.ctx->get_buffer2 = &Impl::getBuffer;
    if (avcodec_open2(m.ctx, codec, nullptr) < 0)
        return false;
    if (options.helperCpuMask >= 0)
    {
        m.helper = new Helper;
        m.helper->open(options.helperPriority, options.helperCpuMask);
        ff_vp56_set_split(m.ctx, &m.helper->ops);
    }
    m.thread = new pf::Thread("vp6 decode", &Impl::entry, &m, options.priority, 256 * 1024, options.cpuMask);
    m.thread->start();
    return true;
}

const uint8_t* Player::take(int k, int* index, uint32_t waitUs)
{
    Impl& m = *m_;
    if (!m.thread)
        return nullptr;
    m.wanted.store(k, std::memory_order_release);
    const int h = m.head.load(std::memory_order_relaxed);
    int t = m.tail.load(std::memory_order_acquire);
    if (waitUs)
    {
        const uint64_t start = nowUs();
        while ((t == h || m.ring[(t - 1) % MaxQueue].index < k) && !m.finished.load(std::memory_order_acquire))
        {
            const uint64_t waited = nowUs() - start;
            if (waited >= waitUs)
                break;
            m.ready.waitForUs(uint32_t(waitUs - waited));
            t = m.tail.load(std::memory_order_acquire);
        }
        t = m.tail.load(std::memory_order_acquire);
    }
    int pick = -1;
    for (int i = h; i < t && m.ring[i % MaxQueue].index <= k; ++i)
        pick = i;
    const int n = m.takes.load(std::memory_order_relaxed) + 1;
    if (pick >= 0)
    {
        Entry& e = m.ring[pick % MaxQueue];
        m.dropped += pick - h;
        e.buffer->handedAt.store(n, std::memory_order_release);
        m.pinned.store(e.buffer, std::memory_order_release);
        m.takes.store(n, std::memory_order_release);
        m.lastBuffer = e.buffer;
        m.lastIndex = e.index;
        m.head.store(pick + 1, std::memory_order_release);
        m.space.post();
    }
    else if (m.lastBuffer)
    {
        m.lastBuffer->handedAt.store(n, std::memory_order_release);
        m.takes.store(n, std::memory_order_release);
    }
    *index = m.lastIndex;
    return m.lastBuffer ? m.lastBuffer->base : nullptr;
}

uint8_t* Player::close()
{
    Impl& m = *m_;
    if (m.thread)
    {
        m.stop.store(true, std::memory_order_release);
        m.space.post();
        delete m.thread;
        m.thread = nullptr;
    }
    for (Entry& e : m.ring)
        av_frame_free(&e.frame);
    av_frame_free(&m.decoded);
    av_packet_free(&m.packet);
    avcodec_free_context(&m.ctx);
    if (m.helper)
        m.helperUs = m.helper->busyUs.load();
    delete m.helper;
    m.helper = nullptr;
    uint8_t* kept = m.lastBuffer ? m.lastBuffer->base : nullptr;
    for (int i = 0; i < m.poolCount; ++i)
    {
        if (m.pool[i].base != kept)
            m.options.release(m.pool[i].base, m.options.context);
        m.pool[i].base = nullptr;
    }
    m.poolCount = 0;
    m.lastBuffer = nullptr;
    return kept;
}

PlayerStats Player::stats() const
{
    const Impl& m = *m_;
    PlayerStats s;
    s.decoded = m.decodedCount.load();
    s.errors = m.errors.load();
    s.dropped = m.dropped;
    s.jumps = m.jumps.load();
    s.decodeUs = m.decodeUs.load();
    s.decodeMaxUs = m.decodeMaxUs.load();
    s.readUs = m.readUs.load();
    s.borderUs = m.borderUs.load();
    s.idleUs = m.idleUs.load();
    s.helperUs = m.helper ? m.helper->busyUs.load() : m.helperUs;
    s.buffers = m.poolCount;
    return s;
}

#else

struct Player::Impl {};
Player::Player() : m_(new Impl) {}
Player::~Player() { delete m_; }
bool Player::open(const char*, int, int, const Options&) { return false; }
const uint8_t* Player::take(int, int*, uint32_t) { return nullptr; }
uint8_t* Player::close() { return nullptr; }
PlayerStats Player::stats() const { return PlayerStats(); }

#endif

size_t layoutBytes(int width, int height)
{
    const Layout l(width, height);
    return l.vOffset + size_t(l.cPitch) * l.cRows;
}

void layoutFrame(uint8_t* dst, const Planes& src, int w, int h)
{
    const Layout l(w, h);
    copyPlane(dst, l.yPitch, src.data[0], src.pitch[0], w, h, Border);
    copyPlane(dst + l.uOffset, l.cPitch, src.data[1], src.pitch[1], w / 2, h / 2, Border / 2);
    copyPlane(dst + l.vOffset, l.cPitch, src.data[2], src.pitch[2], w / 2, h / 2, Border / 2);
}

uint32_t layoutHash(const uint8_t* layout, size_t bytes)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < bytes; i += 61)
        h = (h ^ layout[i]) * 16777619u;
    return h;
}

}}
