// IDirect3DDevice9: the device state machine on the game thread. Draws and clears are recorded into the frame
// of platform/render.h; Present hands the frame to the device (render thread on the Vita).

#include <winapi/d3d9_internal.h>
#include <winapi/d3d9_probe.h>
#include <winapi/support.h>
#include <lib/memlog.h>
#include <platform/switches.h>
#include <platform/config.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/limiter.h>
#include <platform/perf.h>
#include <algorithm>
#include <type_traits>
#include <atomic>
#include <cmath>
#include <cstring>

namespace win32 { namespace d3d9
{

static_assert(sizeof(D3DVIEWPORT9) % 4 == 0 && sizeof(render::Viewport) % 4 == 0, "viewports are whole words");

Device* g_streamDevice = nullptr;

namespace
{

pf::gxm::Device& gpu()
{
    return pf::gxm::Device::get();
}

enum : DWORD
{
    D3DRS_ZENABLE = 7, D3DRS_ZWRITEENABLE = 14, D3DRS_ALPHATESTENABLE = 15, D3DRS_SRCBLEND = 19,
    D3DRS_DESTBLEND = 20, D3DRS_CULLMODE = 22, D3DRS_ZFUNC = 23, D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25,
    D3DRS_ALPHABLENDENABLE = 27, D3DRS_FOGENABLE = 28, D3DRS_TEXTUREFACTOR = 60, D3DRS_LIGHTING = 137,
    D3DRS_COLORWRITEENABLE = 168, D3DRS_BLENDOP = 171, D3DRS_BLENDFACTOR = 193,

    D3DTS_VIEW = 2, D3DTS_PROJECTION = 3, D3DTS_TEXTURE0 = 16, D3DTS_WORLD = 256,

    D3DSAMP_ADDRESSU = 1, D3DSAMP_ADDRESSV = 2, D3DSAMP_MAGFILTER = 5, D3DSAMP_MINFILTER = 6, D3DSAMP_MIPFILTER = 7,

    D3DCLEAR_TARGET = 1, D3DCLEAR_ZBUFFER = 2,

    D3DPT_TRIANGLELIST = 4, D3DPT_TRIANGLESTRIP = 5, D3DPT_TRIANGLEFAN = 6
};

const D3DMATRIX s_identity = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };

// d3d_fast_draw=off: every draw looks its viewport up again.
#ifdef DEV_SETTINGS
bool s_fastDraw = true;
#else
const bool s_fastDraw = RENDER_FAST_PATHS != 0;
#endif

// debug_frame_dump=on (development): one frame in 150, 40 frames in all, is written to the log call by call.
std::atomic<bool> s_dumping{false};
uint64_t s_recorded = 0;    // draws recorded in the frames submitted so far

bool dumpThisFrame(uint32_t index)
{
    static const bool on = pf::Config::getBool("debug_frame_dump", false);
    static uint32_t dumped = 0;
    if (!on || dumped >= 40 || index % 150 != 1)
        return false;
    ++dumped;
    return true;
}

// Perf builds: game thread time spent in the Draw* methods, logged every 300 frames. One draw in DrawSampleMask + 1
// is timed; the time per draw is the mean of the timed ones.
const uint32_t DrawSampleMask = 7;
uint64_t s_drawUs = 0;
uint32_t s_drawCalls = 0, s_drawTimed = 0, s_drawFrames = 0;

struct DrawTimer
{
    uint32_t start;
    DrawTimer()
        :   start(__builtin_expect(pf::perf::enabled(), 0) && (s_drawCalls++ & DrawSampleMask) == 0 ? pf::perf::now()
                                                                                                     : 0)
    {
    }
    ~DrawTimer()
    {
        if (__builtin_expect(start == 0, 1))
            return;
        s_drawUs += pf::perf::now() - start;
        ++s_drawTimed;
    }
};

void drawFrameDone()
{
    if (!pf::perf::enabled() || ++s_drawFrames < 300)
        return;
    const double perDraw = s_drawTimed ? double(s_drawUs) / double(s_drawTimed) : 0.0;
    pf::logInfo("d3d9: draws on the game thread: %u us/frame, %u draws/frame, %.2f us/draw (1 in %u timed)",
                uint32_t(perDraw * s_drawCalls / s_drawFrames), s_drawCalls / s_drawFrames, perDraw,
                unsigned(DrawSampleMask + 1));
    s_drawUs = 0;
    s_drawCalls = s_drawTimed = s_drawFrames = 0;
}

void color(DWORD argb, float out[4])
{
    out[0] = float((argb >> 16) & 255) / 255.0f;
    out[1] = float((argb >> 8) & 255) / 255.0f;
    out[2] = float(argb & 255) / 255.0f;
    out[3] = float(argb >> 24) / 255.0f;
}

const render::TextureStorage* storageOf(const Object* object)
{
    if (!object || (object->kind() != KindTexture && object->kind() != KindCubeTexture))
        return nullptr;
    return static_cast<const Texture*>(object)->storage();
}

// Inputs of the vertex shader by primitive count.
uint32_t vertexCount(DWORD type, UINT primitives)
{
    switch (type)
    {
    case D3DPT_TRIANGLELIST:  return primitives * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:   return primitives + 2;
    default:                  return 0;
    }
}

uint8_t primitive(DWORD type)
{
    return type == D3DPT_TRIANGLESTRIP ? render::PrimitiveStrip
         : type == D3DPT_TRIANGLEFAN ? render::PrimitiveFan : render::PrimitiveTriangles;
}

}

// ---- Construction --------------------------------------------------------------------------------------------------

Device::Device(WinApplication* app, Direct3D* d3d, const D3DPRESENT_PARAMETERS& params, HWND window, DWORD flags)
    :   Object(KindDevice)
    ,   m_app(app)
    ,   m_d3d(d3d)
    ,   m_params(params)
    ,   m_window(window)
    ,   m_flags(flags)
{
    d3d->addRef();
    memset(&m_state, 0, sizeof(m_state));
#ifdef DEV_SETTINGS
    s_fastDraw = NFSU_RENDER_FAST("d3d_fast_draw");
#endif
    m_fastLayer = NFSU_RENDER_FAST("d3d_fast_layer");
    if (!g_streamDevice)
        g_streamDevice = this;
    // frame_rate=0 (unlocked) and 60 show each frame for one vblank; 30 (default) shows each for two.
    const int frameRate = pf::Config::getInt("frame_rate", 30);
    m_vblanks = frameRate == 0 || frameRate >= 60 ? 1 : 2;
    resetBackBuffer(params);
    defaults();
}

Device::Device(Device& parent)
    :   Object(KindDevice)
    ,   m_app(parent.m_app)
    ,   m_d3d(parent.m_d3d)
    ,   m_params(parent.m_params)
    ,   m_window(parent.m_window)
    ,   m_flags(parent.m_flags)
    ,   m_parent(&parent)
{
    memset(&m_state, 0, sizeof(m_state));
    m_fastLayer = parent.m_fastLayer;
    m_vblanks = parent.m_vblanks;
}

Device::~Device()
{
    if (m_parent)
        return;
    for (Device* clone : m_clones)
        delete clone;
    bridge::deviceGone(this);
    if (g_streamDevice == this)
    {
        g_streamDevice = nullptr;
        bridge::g_hot.stream = nullptr;
    }
    // Values recorded into a frame that is never submitted.
    if (m_frame)
        markEffectValuesDirty();
    bindSlot(m_state.declaration, static_cast<VertexDeclaration*>(nullptr));
    bindSlot(m_state.stream, static_cast<Buffer*>(nullptr));
    bindSlot(m_state.indices, static_cast<Buffer*>(nullptr));
    bindSlot(m_state.target, static_cast<Surface*>(nullptr));
    bindSlot(m_state.depth, static_cast<Surface*>(nullptr));
    for (Object*& texture : m_state.textures)
        bindSlot(texture, static_cast<Object*>(nullptr));
    for (VertexDeclaration* d : m_fvfDeclarations)
        delete d;
    delete m_backBuffer;
    delete m_autoDepth;
    m_d3d->release();
}

template< typename T >
void Device::bindSlot(T*& slot, T* object)
{
    if (slot == object)
        return;
    if (__builtin_expect(m_parent != nullptr, 0))
    {
        slot = object;
        return;
    }
    if constexpr (std::is_same<T, Buffer>::value || std::is_same<T, VertexDeclaration>::value)
    {
        if (m_fastLayer)
        {
            if (object)
                object->bindPlain();
            if (slot)
                slot->unbindPlain();
            slot = object;
            return;
        }
    }
    if (object)
        object->bind();
    if (slot)
        slot->unbind();
    slot = object;
}

void Device::resetBackBuffer(const D3DPRESENT_PARAMETERS& params)
{
    m_params = params;
    if (!m_params.BackBufferWidth || !m_params.BackBufferHeight)
    {
        m_params.BackBufferWidth = pf::gxm::DisplayWidth;
        m_params.BackBufferHeight = pf::gxm::DisplayHeight;
    }
    if (m_state.target == m_backBuffer)
        bindSlot(m_state.target, static_cast<Surface*>(nullptr));
    if (m_state.depth == m_autoDepth)
        bindSlot(m_state.depth, static_cast<Surface*>(nullptr));
    m_sceneCached = false;
    for (Surface** implicit : { &m_backBuffer, &m_autoDepth })
    {
        if (!*implicit)
            continue;
        (*implicit)->unpin();
        (*implicit)->release();
        *implicit = nullptr;
    }
    const uint32_t w = m_params.BackBufferWidth, h = m_params.BackBufferHeight;
    m_backBuffer = new Surface(this, Surface::RoleBackBuffer, w, h, D3DFMT_A8R8G8B8);
    m_backBuffer->pin();
    publish(m_app, m_backBuffer);
    if (m_params.EnableAutoDepthStencil)
    {
        m_autoDepth = new Surface(this, Surface::RoleDepth, w, h, m_params.AutoDepthStencilFormat);
        m_autoDepth->setDepth(gpu().createDepth(w, h));
        m_autoDepth->pin();
        publish(m_app, m_autoDepth);
    }
    pf::logInfo("d3d9: back buffer %ux%u%s", w, h, m_autoDepth ? " with depth" : "");
}

void Device::defaults()
{
    DWORD* rs = m_ctx.fixed.renderStates;
    memset(rs, 0, sizeof(m_ctx.fixed.renderStates));
    rs[D3DRS_ZENABLE] = m_autoDepth ? 1 : 0;
    rs[8] = 3;                      // FILLMODE solid
    rs[9] = 2;                      // SHADEMODE gouraud
    rs[D3DRS_ZWRITEENABLE] = 1;
    rs[16] = 1;                     // LASTPIXEL
    rs[D3DRS_SRCBLEND] = 2;         // ONE
    rs[D3DRS_DESTBLEND] = 1;        // ZERO
    rs[D3DRS_CULLMODE] = 3;         // CCW
    rs[D3DRS_ZFUNC] = 4;            // LESSEQUAL
    rs[D3DRS_ALPHAFUNC] = 8;        // ALWAYS
    rs[D3DRS_TEXTUREFACTOR] = 0xffffffffu;
    rs[D3DRS_LIGHTING] = 1;
    rs[D3DRS_COLORWRITEENABLE] = 0xf;
    rs[D3DRS_BLENDOP] = 1;
    rs[D3DRS_BLENDFACTOR] = 0xffffffffu;
    for (DWORD s = 0; s < MaxStages; ++s)
    {
        DWORD* tss = m_ctx.fixed.stageStates[s];
        memset(tss, 0, sizeof(m_ctx.fixed.stageStates[s]));
        tss[D3DTSS_COLOROP] = s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        tss[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        tss[D3DTSS_COLORARG2] = D3DTA_CURRENT;
        tss[D3DTSS_ALPHAOP] = s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        tss[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        tss[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        tss[D3DTSS_TEXCOORDINDEX] = s;
    }
    for (DWORD s = 0; s < MaxSamplerStages; ++s)
    {
        DWORD* ss = m_ctx.fixed.samplerStates[s];
        memset(ss, 0, sizeof(m_ctx.fixed.samplerStates[s]));
        ss[D3DSAMP_ADDRESSU] = ss[D3DSAMP_ADDRESSV] = ss[3] = 1;   // WRAP
        ss[D3DSAMP_MAGFILTER] = ss[D3DSAMP_MINFILTER] = 1;         // POINT
        ss[10] = 1;                                                 // MAXANISOTROPY
    }
    for (D3DMATRIX& m : m_ctx.fixed.transforms)
        m = s_identity;
    bindSlot(m_state.target, m_backBuffer);
    bindSlot(m_state.depth, m_autoDepth);
    m_ctx.fixed.viewport = D3DVIEWPORT9{ 0, 0, m_params.BackBufferWidth, m_params.BackBufferHeight, 0.0f, 1.0f };
    m_viewportChanged = true;
    m_sceneOpen = false;
    m_stateDirty = true;
    m_sceneCached = false;
    m_state.layoutValid = false;
}

// ---- Recording -----------------------------------------------------------------------------------------------------

render::Frame* Device::frame(WinApplication* app, x86::CPU& cpu)
{
    if (!m_frame)
    {
        if (bridge::onQueueThread())
            pf::fatal("d3d9: the queue thread would open a frame");
        const bool limited = pf::limiter::on();
        const bool timed = pf::perf::timed() || limited;
        const uint32_t start = timed ? pf::perf::now() : 0;
        if (limited)
            pf::limiter::gameFrame();
        app->unlockContext(cpu);
        m_frame = gpu().beginFrame(m_params.BackBufferWidth, m_params.BackBufferHeight);
        app->lockContext(cpu);
        if (bridge::g_frameHook)
            bridge::g_frameHook(m_frame->index);
        if (timed)
        {
            const uint32_t waited = pf::perf::now() - start;
            pf::perf::event(pf::perf::EventSlotWaitUs, waited);
            pf::perf::add(pf::perf::SlotWait, waited);
            if (limited)
                pf::limiter::add(pf::limiter::GameSlotWait, waited);
        }
        m_sceneOpen = false;
        m_frameFull = false;
        m_drawCount = 0;
        m_sceneDraws = 0;
        m_ctx.stream = &frameStream(m_frame->index);
        if (g_streamDevice == this)
            bridge::g_hot.stream = m_ctx.stream;
        s_dumping = dumpThisFrame(m_frame->index);
        if (__builtin_expect(s_dumping.load(std::memory_order_relaxed), 0))
            pf::logInfo("dump: frame %u, back buffer %ux%u", m_frame->index, unsigned(m_params.BackBufferWidth),
                        unsigned(m_params.BackBufferHeight));
    }
    return m_frame;
}

namespace bridge
{

bool dumpingFrame()
{
    return s_dumping;
}

uint64_t recordedDraws()
{
    return s_recorded;
}

}

// Hands the recorded frame to the device.
void Device::submit()
{
    s_recorded += m_drawCount;
    m_frame->drawCount = m_drawCount;
    if (m_frame->sceneCount)
    {
        render::Scene& last = m_frame->scenes[m_frame->sceneCount - 1];
        last.drawCount = m_drawCount - last.firstDraw;
    }
    m_frame->resolve = &resolveFrame;
    m_frame->commands = m_ctx.stream->data;
    m_frame->commandCount = m_ctx.stream->size;
    gpu().submitFrame(m_frame, m_vblanks);
    m_frame = nullptr;
    m_ctx.stream = nullptr;
    if (g_streamDevice == this)
        bridge::g_hot.stream = nullptr;
    if (!bridge::onQueueThread())
        Texture::trimKept();
}

bool Device::showEarly(WinApplication* app, x86::CPU& cpu, uint32_t* frameIndex)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!m_frame)
        return false;
    *frameIndex = m_frame->index;
    submit();
    m_sceneOpen = false;
    drawFrameDone();
    m_shownEarly = true;
    return true;
}

bool Device::segmentBegin()
{
    if (!m_ctx.stream || m_ctx.segment)
        return false;
    if (m_stateDirty)
    {
        recordWords(CmdFixedState, &m_ctx.fixed, sizeof(FixedState) / 4);
        m_stateDirty = false;
    }
    syncEffectValues(*m_ctx.stream);
    beginSegment(m_ctx, m_frame->index);
    if (g_streamDevice == this)
        bridge::g_hot.stream = m_ctx.stream;
    return true;
}

void Device::segmentEnd()
{
    mergeSegmentEffects(*m_ctx.stream->shadow);
    endSegment(m_ctx);
    if (g_streamDevice == this)
        bridge::g_hot.stream = m_ctx.stream;
}

Device* Device::segmentClone(uint32_t arenaBytes, uint32_t draws, void (*await)(void*), void* job)
{
    if (!m_ctx.stream || m_ctx.segment || m_parent || m_frameFull)
        return nullptr;
    render::Frame& f = *m_frame;
    if (m_drawCount + draws > f.drawCapacity)
        return nullptr;
    Device* spare = nullptr;
    for (Device* d : m_clones)
        if (!d->m_segBusy)
        {
            spare = d;
            break;
        }
    if (!spare)
    {
        if (m_clones.size() == MaxClones)
            return nullptr;
        m_clones.push_back(spare = new Device(*this));
    }
    render::Scene* s = scene();
    uint16_t viewport;
    if (!s || !drawViewport(f, &viewport))
        return nullptr;
    uint8_t* arena = nullptr;
    if (arenaBytes && !(arena = static_cast<uint8_t*>(render::allocateVertices(f, arenaBytes))))
        return nullptr;
    if (m_stateDirty)
    {
        recordWords(CmdFixedState, &m_ctx.fixed, sizeof(FixedState) / 4);
        m_stateDirty = false;
    }
    syncEffectValues(*m_ctx.stream);
    Device& c = *spare;
    c.m_segBusy = true;
    c.m_segFrame = f.index;
    memcpy(&c.m_state, &m_state, sizeof(m_state));
    c.m_frame = m_frame;
    c.m_frameFull = false;
    c.m_drawCount = 0;
    c.m_drawLimit = draws;
    c.m_segFirst = m_drawCount;
    c.m_sceneDraws = m_sceneDraws;
    m_drawCount += draws;
    c.m_segScene = s;
    c.m_segViewport = viewport;
    c.m_segTag = render::g_drawTag ? render::g_drawTag() : 0;
    c.m_arena = arena;
    c.m_arenaUsed = 0;
    c.m_arenaSize = arenaBytes;
    c.m_effectPass = nullptr;
    c.m_deviceSamplers = c.m_deviceStates = 0;
    c.m_stateDirty = false;
    beginSegmentClone(m_ctx, c.m_ctx, f.index, await, job, uint32_t(s - f.scenes), draws);
    bridge::g_deferDeletes = true;
    ++bridge::g_pendingSegments;
    return &c;
}

void Device::segmentCloseClone(Device* clone)
{
    Device& c = *clone;
#ifdef DEV_TOOLS
    if (probe::on())
    {
        const void* a[22];
        const void* b[22];
        for (uint32_t i = 0; i < 16; ++i)
        {
            a[i] = m_state.textures[i];
            b[i] = c.m_state.textures[i];
        }
        a[16] = m_state.declaration; b[16] = c.m_state.declaration;
        a[17] = m_state.stream; b[17] = c.m_state.stream;
        a[18] = m_state.indices; b[18] = c.m_state.indices;
        a[19] = m_state.target; b[19] = c.m_state.target;
        a[20] = m_state.depth; b[20] = c.m_state.depth;
        a[21] = reinterpret_cast<const void*>(uintptr_t(m_state.fvf)); b[21] = reinterpret_cast<const void*>(uintptr_t(c.m_state.fvf));
        probe::segmentClosed(reinterpret_cast<const uint32_t*>(&m_ctx.fixed), reinterpret_cast<const uint32_t*>(&c.m_ctx.fixed),
                             sizeof(FixedState) / 4, a, b, 22);
        probe::count(probe::SegmentDraws, c.m_drawCount);
        if (c.m_arena)
        {
            probe::count(probe::ArenaSegments);
            probe::count(probe::ArenaReserved, c.m_arenaSize);
            probe::count(probe::ArenaUsed, c.m_arenaUsed);
            probe::count(probe::ArenaShrinks, c.m_arenaUsed != c.m_arenaSize);
        }
    }
#endif
    forgetSegmentEffects(*c.m_ctx.stream->shadow);
    endSegmentClone(c.m_ctx, c.m_drawCount);
    if (--bridge::g_pendingSegments == 0)
        bridge::joinDeletes();
    if (m_frame && m_frame->index == c.m_segFrame)
    {
        m_sceneDraws = c.m_sceneDraws;
        m_frameFull = m_frameFull || c.m_frameFull;
    }
    c.m_arena = nullptr;
    c.m_frame = nullptr;
    c.m_segBusy = false;
}

void Device::segmentFinishClone(Device* clone)
{
    publishSegmentClone(clone->m_ctx, clone->m_drawCount);
}

void* Device::arenaAllocate(uint32_t bytes)
{
    const uint32_t start = (m_arenaUsed + 15) & ~15u;
    if (start + bytes > m_arenaSize)
        return nullptr;
    m_arenaUsed = start + bytes;
    return m_arena + start;
}

void Device::record(uint32_t header, DWORD value)
{
    if (!m_ctx.stream)
    {
        m_stateDirty = true;
        return;
    }
    uint32_t* w = m_ctx.stream->append(2);
    w[0] = header;
    w[1] = value;
}

void Device::recordWords(uint32_t header, const void* payload, uint32_t words)
{
    if (!m_ctx.stream)
    {
        m_stateDirty = true;
        return;
    }
    uint32_t* w = m_ctx.stream->append(1 + words);
    w[0] = header;
    memcpy(w + 1, payload, words * 4);
}

// Records the frame's next draw slot as filled whole.
void Device::recordDrawCall()
{
    uint32_t* w = m_ctx.stream->append(2);
    w[0] = CmdDrawCall;
    w[1] = m_drawCount;
    ++m_drawCount;
    ++m_sceneDraws;
}

__attribute__((always_inline)) inline render::Scene* Device::scene()
{
    render::Frame& f = *m_frame;
    if (m_sceneCached && m_sceneOpen && m_sceneFrame == f.index && m_sceneCount == f.sceneCount
        && m_sceneEpoch == bridge::g_hot.textureEpoch)
        return &f.scenes[f.sceneCount - 1];
    render::Target target = {};
    uint16_t width = uint16_t(m_params.BackBufferWidth), height = uint16_t(m_params.BackBufferHeight);
    Surface* t = m_state.target;
    if (t && t->role() == Surface::RoleLevel && t->container())
    {
        target.color = t->container()->storage();
        target.face = uint8_t(t->face());
        width = uint16_t(t->width());
        height = uint16_t(t->height());
    }
    if (m_state.depth && m_state.depth->depth())
        target.depth = m_state.depth->depth();
    render::Scene* s = scene(target, width, height);
    if (s && m_fastLayer)
    {
        m_sceneCached = true;
        m_sceneFrame = f.index;
        m_sceneCount = f.sceneCount;
        m_sceneEpoch = bridge::g_hot.textureEpoch;
    }
    return s;
}

render::Scene* Device::scene(const render::Target& target, uint16_t width, uint16_t height)
{
    render::Frame& f = *m_frame;
    if (m_sceneOpen && f.sceneCount)
    {
        render::Scene& last = f.scenes[f.sceneCount - 1];
        if (last.target.color == target.color && last.target.face == target.face
            && last.target.depth == target.depth)
            return &last;
    }
    if (f.sceneCount == f.sceneCapacity)
    {
        if (!m_frameFull)
            pf::logError("d3d9: frame %u: scene limit reached, draws dropped", f.index);
        m_frameFull = true;
        return nullptr;
    }
    if (f.sceneCount)
    {
        render::Scene& last = f.scenes[f.sceneCount - 1];
        last.drawCount = m_drawCount - last.firstDraw;
    }
    render::Scene& s = f.scenes[f.sceneCount++];
    memset(&s, 0, sizeof(s));
    s.target = target;
    s.width = width;
    s.height = height;
    s.firstDraw = m_drawCount;
    m_sceneDraws = 0;
    if (target.color)
        const_cast<render::TextureStorage*>(target.color)->lastUse.store(f.index, std::memory_order_relaxed);
    if (target.depth)
        const_cast<render::DepthStorage*>(target.depth)->lastUse.store(f.index, std::memory_order_relaxed);
    m_sceneOpen = true;
    return &s;
}

// Device viewport's index, reused while nothing changed.
inline bool Device::drawViewport(render::Frame& f, uint16_t* index)
{
    if (__builtin_expect(!m_viewportChanged && m_viewportFrame == f.index && m_viewportCount == f.viewportCount, 1))
    {
        *index = uint16_t(m_viewportCount - 1);
        return true;
    }
    const D3DVIEWPORT9& vp = m_ctx.fixed.viewport;
    if (!viewportIndex(f, render::Viewport{ float(vp.X), float(vp.Y), float(vp.Width), float(vp.Height), vp.MinZ,
                                            vp.MaxZ }, index))
        return false;
    m_viewportChanged = !s_fastDraw;
    m_viewportFrame = f.index;
    m_viewportCount = f.viewportCount;
    return true;
}

bool Device::viewportIndex(render::Frame& f, const render::Viewport& viewport, uint16_t* index)
{
    if (f.viewportCount && sameWords(&f.viewports[f.viewportCount - 1], &viewport, sizeof(viewport) / 4))
    {
        *index = uint16_t(f.viewportCount - 1);
        return true;
    }
    if (f.viewportCount == f.viewportCapacity)
    {
        if (!m_frameFull)
            pf::logError("d3d9: frame %u: viewport limit reached, draws dropped", f.index);
        m_frameFull = true;
        return false;
    }
    f.viewports[f.viewportCount] = viewport;
    *index = uint16_t(f.viewportCount++);
    return true;
}

__attribute__((always_inline)) inline void Device::markRead(
    render::Frame& f, const render::TextureStorage* const textures[render::MaxSamplers],
    render::BufferStorage* vertexStorage, render::BufferStorage* indexStorage)
{
    if (m_markFrame != f.index || m_markEpoch != bridge::g_hot.textureEpoch || !m_fastLayer)
    {
        m_markFrame = f.index;
        m_markEpoch = bridge::g_hot.textureEpoch;
        memset(m_marked, 0, sizeof(m_marked));
    }
    for (uint32_t i = 0; i < render::MaxSamplers; ++i)
    {
        const render::TextureStorage* t = textures[i];
        if (!t || t == m_marked[i])
            continue;
        const_cast<render::TextureStorage*>(t)->lastUse.store(f.index, std::memory_order_relaxed);
        if (t->uploads.load(std::memory_order_relaxed))
            __atomic_store_n(&f.uploads, true, __ATOMIC_RELAXED);
        if (m_fastLayer)
            m_marked[i] = t;
    }
    if (vertexStorage && vertexStorage != m_marked[render::MaxSamplers])
    {
        vertexStorage->lastUse.store(f.index, std::memory_order_relaxed);
        if (m_fastLayer)
            m_marked[render::MaxSamplers] = vertexStorage;
    }
    if (indexStorage && indexStorage != m_marked[render::MaxSamplers + 1])
    {
        indexStorage->lastUse.store(f.index, std::memory_order_relaxed);
        if (m_fastLayer)
            m_marked[render::MaxSamplers + 1] = indexStorage;
    }
}

__attribute__((always_inline)) inline void Device::drawLayout(VertexDeclaration* declaration, uint32_t stride,
                                                              uint8_t& layout, uint32_t& texComponents,
                                                              bool& positionT)
{
    if (m_state.layoutValid && stride == m_state.layoutStride)
    {
        layout = m_state.layoutId;
        texComponents = m_state.layoutTexComponents;
        positionT = m_state.layoutPositionT;
        return;
    }
    layout = m_parent ? declaration->layoutAt(stride) : declaration->layout(stride);
    texComponents = declaration->texComponents();
    positionT = declaration->positionT();
    if (m_fastLayer)
    {
        m_state.layoutValid = true;
        m_state.layoutStride = stride;
        m_state.layoutId = layout;
        m_state.layoutTexComponents = uint8_t(texComponents);
        m_state.layoutPositionT = positionT;
    }
}

// Records the geometry, the textures (marked read by this frame) and the active effect pass; the render thread
// resolves the state (d3d9_record.cpp).
__attribute__((always_inline)) inline HRESULT Device::draw(
    WinApplication* app, x86::CPU& cpu, DWORD type, UINT primitives, const void* vertices, const uint16_t* indices,
    uint32_t count, uint8_t layout, uint32_t texComponents, bool positionT, render::BufferStorage* vertexStorage,
    render::BufferStorage* indexStorage)
{
    const uint32_t inputs = vertexCount(type, primitives);
    if (effectSkipped())
        return D3D_OK;
    if (!inputs)
    {
        if (type < D3DPT_TRIANGLELIST || type > D3DPT_TRIANGLEFAN)
            D3D_LOG_ONCE("d3d9: primitive type %u not drawn", type);
        return type >= 1 && type <= 6 ? D3D_OK : D3DERR_INVALIDCALL;
    }
    render::Frame* f = m_frame;
    if (__builtin_expect(!f, 0))
        f = frame(app, cpu);
    if (m_frameFull)
        return D3D_OK;
    render::Scene* s;
    uint16_t viewport;
    uint8_t tag;
    uint32_t slot = m_drawCount;
    if (__builtin_expect(m_parent != nullptr, 0))
    {
        s = m_segScene;
        viewport = m_segViewport;
        tag = m_segTag;
        slot += m_segFirst;
        if (m_drawCount == m_drawLimit)
        {
            pf::logError("d3d9: frame %u: draw limit reached, draws dropped", f->index);
            m_frameFull = true;
            return D3D_OK;
        }
    }
    else
    {
        s = scene();
        if (!s)
            return D3D_OK;
        if (m_drawCount == f->drawCapacity)
        {
            pf::logError("d3d9: frame %u: draw limit reached, draws dropped", f->index);
            m_frameFull = true;
            return D3D_OK;
        }
        if (!drawViewport(*f, &viewport))
            return D3D_OK;
        if (m_stateDirty)
        {
            recordWords(CmdFixedState, &m_ctx.fixed, sizeof(FixedState) / 4);
            m_stateDirty = false;
        }
        syncEffectValues(*m_ctx.stream);
        tag = render::g_drawTag ? render::g_drawTag() : 0;
    }
    const D3DVIEWPORT9& vp = m_ctx.fixed.viewport;
    if (__builtin_expect(s_dumping.load(std::memory_order_relaxed), 0))
    {
        const Object* t = m_state.textures[0];
        const Texture* texture = t && t->kind() == KindTexture ? static_cast<const Texture*>(t) : nullptr;
        const DWORD* rs = m_ctx.fixed.renderStates;
        pf::logInfo("dump: draw %u to %s %ux%u, viewport %lu,%lu %lux%lu, type %lu, %u vertices, positionT %d, "
                    "texture %ux%u%s, blend %lu (%lu, %lu), alpha test %lu, z %lu/%lu, cull %lu, fvf %lx, effect %d",
                    m_drawCount, s->target.color ? "texture" : "back buffer", unsigned(s->width),
                    unsigned(s->height), (unsigned long)vp.X, (unsigned long)vp.Y, (unsigned long)vp.Width,
                    (unsigned long)vp.Height, (unsigned long)type, count, int(positionT),
                    texture ? unsigned(texture->desc().width) : 0u, texture ? unsigned(texture->desc().height) : 0u,
                    texture && texture->desc().linear ? " (movie)" : "", (unsigned long)rs[D3DRS_ALPHABLENDENABLE],
                    (unsigned long)rs[D3DRS_SRCBLEND], (unsigned long)rs[D3DRS_DESTBLEND],
                    (unsigned long)rs[D3DRS_ALPHATESTENABLE], (unsigned long)rs[D3DRS_ZENABLE],
                    (unsigned long)rs[D3DRS_ZWRITEENABLE], (unsigned long)rs[D3DRS_CULLMODE],
                    (unsigned long)m_state.fvf, int(m_effectPass != nullptr));
    }

    uint32_t* w = m_ctx.stream->append(1 + sizeof(DrawRecord) / 4);
    w[0] = CmdDraw;
    DrawRecord& r = *reinterpret_cast<DrawRecord*>(w + 1);
    render::DrawCall& d = f->draws[slot];
    d.vertices = vertices;
    d.indices = indices;
    d.count = inputs;
    d.vertexCount = count;
    d.viewport = viewport;
    d.tag = tag;
    r.draw = slot;
    r.primitive = primitive(type);
    r.layout = layout;
    r.texComponents = uint8_t(texComponents);
    r.positionT = positionT ? 1 : 0;
    r.vertexStorage = vertexStorage;
    const render::TextureStorage* textures[render::MaxSamplers];
    if (m_effectPass)
    {
        uint8_t deviceSlots = 0;
        m_effectPass->record(r.effect, textures, deviceSlots);
        r.deviceSamplers = m_deviceSamplers;
        r.deviceStates = m_deviceStates;
        if ((m_deviceSamplers & 0x01010101u) || deviceSlots)
        {
            // Device textures set after the pass (or of stages the pass binds none to) replace the pass's.
            for (uint32_t i = 0; i < render::MaxSamplers; ++i)
            {
                const int stage = effectSamplerStage(r.effect, i);
                if (stage >= 0 && (((deviceSlots >> i) & 1) || ((m_deviceSamplers >> (stage * 8)) & DeviceTexture)))
                    textures[i] = storageOf(m_state.textures[stage]);
            }
        }
        if (__builtin_expect(s_dumping.load(std::memory_order_relaxed), 0))
        {
            const render::TextureStorage* t = textures[0];
            pf::logInfo("dump: draw %u effect %s pass %u, sampler 0 %ux%u%s, %u texture coordinates, stage 0 TTF 0x%x",
                        m_drawCount, effectName(r.effect), unsigned(r.effect.pass), t ? unsigned(t->desc.width) : 0u,
                        t ? unsigned(t->desc.height) : 0u, t && t->desc.linear ? " linear" : "",
                        unsigned(texComponents), unsigned(m_ctx.fixed.stageStates[0][D3DTSS_TEXTURETRANSFORMFLAGS]));
        }
    }
    else
    {
        r.effect = EffectDraw{ NoBlock, NoBlock, NoEffect, 0, { 0, 0 } };
        textures[0] = storageOf(m_state.textures[0]);
        textures[1] = textures[2] = textures[3] = nullptr;
        r.deviceSamplers = 0;
        r.deviceStates = 0;
    }
    memcpy(r.textures, textures, sizeof(textures));
    markRead(*f, textures, vertexStorage, indexStorage);
    ++m_drawCount;
    ++m_sceneDraws;
    return D3D_OK;
}

VertexDeclaration* Device::fvfDeclaration(DWORD fvf)
{
    if (m_parent)
        return m_parent->fvfDeclaration(fvf);
    for (VertexDeclaration* d : m_fvfDeclarations)
    {
        if (d->fvf() == fvf)
            return d;
    }
    VertexDeclaration* d = new VertexDeclaration(this, nullptr, 0, fvf);
    m_fvfDeclarations.push_back(d);
    return d;
}

// ---- Methods -------------------------------------------------------------------------------------------------------

HRESULT Device::TestCooperativeLevel(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

UINT Device::GetAvailableTextureMem(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 96 * 1024 * 1024;
}

HRESULT Device::EvictManagedResources(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

HRESULT Device::GetDirect3D(WinApplication* app, x86::CPU& cpu, x86::reg32* d3d)
{
    RC_USE(app);
    RC_USE(cpu);
    m_d3d->addRef();
    *d3d = m_d3d->guest();
    return D3D_OK;
}

HRESULT Device::GetDeviceCaps(WinApplication* app, x86::CPU& cpu, D3DCAPS9* caps)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!caps)
        return D3DERR_INVALIDCALL;
    Direct3D::fillCaps(caps);
    return D3D_OK;
}

HRESULT Device::GetDisplayMode(WinApplication* app, x86::CPU& cpu, UINT swapChain, D3DDISPLAYMODE* mode)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(swapChain);
    if (!mode)
        return D3DERR_INVALIDCALL;
    *mode = D3DDISPLAYMODE{ m_params.BackBufferWidth, m_params.BackBufferHeight, 60, D3DFMT_X8R8G8B8 };
    return D3D_OK;
}

HRESULT Device::GetCreationParameters(WinApplication* app, x86::CPU& cpu, D3DDEVICE_CREATION_PARAMETERS* params)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!params)
        return D3DERR_INVALIDCALL;
    *params = D3DDEVICE_CREATION_PARAMETERS{ 0, 1, m_window, m_flags };
    return D3D_OK;
}

HRESULT Device::SetCursorProperties(WinApplication* app, x86::CPU& cpu, UINT x, UINT y, x86::reg32 surface)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(x);
    RC_USE(y);
    RC_USE(surface);
    return D3D_OK;
}

void Device::SetCursorPosition(WinApplication* app, x86::CPU& cpu, int x, int y, DWORD flags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(x);
    RC_USE(y);
    RC_USE(flags);
}

BOOL Device::ShowCursor(WinApplication* app, x86::CPU& cpu, BOOL show)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(show);
    return 0;
}

UINT Device::GetNumberOfSwapChains(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 1;
}

HRESULT Device::Reset(WinApplication* app, x86::CPU& cpu, D3DPRESENT_PARAMETERS* params)
{
    if (!params)
        return D3DERR_INVALIDCALL;
    if (m_frame)
        submit();
    RC_USE(app);
    RC_USE(cpu);
    pf::logInfo("d3d9: Reset, back buffer %ux%u asked (kept %ux%u), multisample %u, interval 0x%X",
                unsigned(params->BackBufferWidth), unsigned(params->BackBufferHeight),
                unsigned(m_params.BackBufferWidth), unsigned(m_params.BackBufferHeight),
                unsigned(params->MultiSampleType), unsigned(params->PresentationInterval));
    // The display size is fixed: a resolution change in the Display menu is a no-op.
    params->BackBufferWidth = m_params.BackBufferWidth;
    params->BackBufferHeight = m_params.BackBufferHeight;
    resetBackBuffer(*params);
    for (Object*& texture : m_state.textures)
        bindSlot(texture, static_cast<Object*>(nullptr));
    bindSlot(m_state.stream, static_cast<Buffer*>(nullptr));
    bindSlot(m_state.indices, static_cast<Buffer*>(nullptr));
    bindSlot(m_state.declaration, static_cast<VertexDeclaration*>(nullptr));
    defaults();
    return D3D_OK;
}

HRESULT Device::Present(WinApplication* app, x86::CPU& cpu, const RECT* source, const RECT* dest, HWND window,
                        x86::reg32 dirtyRegion)
{
    RC_USE(window);
    RC_USE(dirtyRegion);
    if (source || dest)
        D3D_LOG_ONCE("d3d9: Present rectangles ignored");
    if (m_shownEarly)
    {
        m_shownEarly = false;
        if (!m_frame)
            return D3D_OK;
    }
    // A frame without draws still shows (black), so the pacing stays the same.
    frame(app, cpu);
    if (__builtin_expect(s_dumping.load(std::memory_order_relaxed), 0))
        pf::logInfo("dump: present frame %u, %u draws, %u scenes", m_frame->index, m_drawCount,
                    m_frame->sceneCount);
    s_dumping = false;
#ifdef DEV_TOOLS
    probe::frameEnd();
#endif
    submit();
    m_sceneOpen = false;
    drawFrameDone();
    memlog::frame();
    return D3D_OK;
}

HRESULT Device::GetBackBuffer(WinApplication* app, x86::CPU& cpu, UINT swapChain, UINT index, DWORD type,
                              x86::reg32* surface)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(type);
    if (swapChain != 0 || index != 0 || !surface)
        return D3DERR_INVALIDCALL;
    m_backBuffer->addRef();
    *surface = m_backBuffer->guest();
    return D3D_OK;
}

HRESULT Device::GetRasterStatus(WinApplication* app, x86::CPU& cpu, UINT swapChain, DWORD* status)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(swapChain);
    if (status)
    {
        status[0] = 0;
        status[1] = 0;
    }
    return D3D_OK;
}

void Device::SetDialogBoxMode(WinApplication* app, x86::CPU& cpu, BOOL enable)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(enable);
}

void Device::SetGammaRamp(WinApplication* app, x86::CPU& cpu, UINT swapChain, DWORD flags, const WORD* ramp)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(swapChain);
    RC_USE(flags);
    RC_USE(ramp);
    D3D_LOG_ONCE("d3d9: gamma ramps are ignored");
}

void Device::GetGammaRamp(WinApplication* app, x86::CPU& cpu, UINT swapChain, WORD* ramp)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(swapChain);
    if (!ramp)
        return;
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; ++i)
            ramp[c * 256 + i] = WORD(i * 257);
}

namespace
{

// Direct3D format to texture format; FormatNone when not supported.
texture::Format textureFormat(DWORD format)
{
    switch (format)
    {
    case D3DFMT_A8R8G8B8: return texture::FormatARGB8888;
    case D3DFMT_X8R8G8B8:
        D3D_LOG_ONCE("d3d9: X8R8G8B8 textures are kept as A8R8G8B8");
        return texture::FormatARGB8888;
    case D3DFMT_DXT1:     return texture::FormatBC1;
    case D3DFMT_DXT3:     return texture::FormatBC2;
    default:              return texture::FormatNone;
    }
}

uint32_t fullChain(uint32_t width, uint32_t height)
{
    uint32_t levels = 1;
    while ((width >> levels) || (height >> levels))
        ++levels;
    return levels;
}

bool powerOfTwo(uint32_t v)
{
    return v && !(v & (v - 1));
}

}

HRESULT Device::CreateTexture(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, UINT levels, DWORD usage,
                              DWORD format, DWORD pool, x86::reg32* texture, x86::reg32 sharedHandle)
{
    RC_USE(sharedHandle);
    const texture::Format f = textureFormat(format);
    const bool target = (usage & D3DUSAGE_RENDERTARGET) != 0;
    if (!texture || !width || !height || f == texture::FormatNone)
    {
        pf::logError("d3d9: CreateTexture %ux%u format 0x%x usage 0x%x not supported", width, height, format, usage);
        return D3DERR_INVALIDCALL;
    }
    // Other sizes (SPEED2.EXE's 640x480 movie frames; the caps claim no POW2 limit): one linear level.
    const bool linear = !target && (!powerOfTwo(width) || !powerOfTwo(height));
    if (linear && (levels > 1 || texture::isBlock(f)))
    {
        pf::logError("d3d9: CreateTexture %ux%u with %u levels, format 0x%x: other sizes than powers of two take one "
                     "uncompressed level (from 0x%08x)", width, height, levels, format,
                     unsigned(callerAddress(app, cpu)));
        return D3DERR_INVALIDCALL;
    }
    if (linear)
        D3D_LOG_ONCE("d3d9: CreateTexture %ux%u format 0x%x: linear (not a power of two)", width, height, format);
    const uint32_t chain = fullChain(width, height);
    render::TextureDesc desc;
    desc.format = f;
    desc.levels = uint8_t(target || linear ? 1 : (levels == 0 || levels > chain ? chain : levels));
    desc.cube = false;
    desc.target = target;
    desc.width = uint16_t(width);
    desc.height = uint16_t(height);
    desc.linear = linear;
    Texture* t = new Texture(this, desc, format, usage, pool);
    if (!t->valid())
    {
        static uint32_t s_failures = 0;
        if (++s_failures <= 4 || s_failures % 64 == 0)
            pf::logError("d3d9: out of texture memory for %ux%u format 0x%x (failure %u)", width, height, format,
                         unsigned(s_failures));
        delete t;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *texture = publish(app, t);
    return D3D_OK;
}

HRESULT Device::CreateCubeTexture(WinApplication* app, x86::CPU& cpu, UINT edge, UINT levels, DWORD usage,
                                  DWORD format, DWORD pool, x86::reg32* texture, x86::reg32 sharedHandle)
{
    RC_USE(cpu);
    RC_USE(sharedHandle);
    const texture::Format f = textureFormat(format);
    const bool target = (usage & D3DUSAGE_RENDERTARGET) != 0;
    if (!texture || !powerOfTwo(edge) || f == texture::FormatNone)
    {
        pf::logError("d3d9: CreateCubeTexture %u format 0x%x not supported", edge, format);
        return D3DERR_INVALIDCALL;
    }
    const uint32_t chain = fullChain(edge, edge);
    render::TextureDesc desc;
    desc.format = f;
    desc.levels = uint8_t(target ? 1 : (levels == 0 || levels > chain ? chain : levels));
    if (target && levels > 1)
        D3D_LOG_ONCE("d3d9: render target cube texture with %u levels: level 0 only", levels);
    desc.cube = true;
    desc.target = target;
    desc.width = desc.height = uint16_t(edge);
    Texture* t = new Texture(this, desc, format, usage, pool);
    if (!t->valid())
    {
        delete t;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *texture = publish(app, t);
    return D3D_OK;
}

HRESULT Device::CreateVertexBuffer(WinApplication* app, x86::CPU& cpu, UINT length, DWORD usage, DWORD fvf,
                                   DWORD pool, x86::reg32* buffer, x86::reg32 sharedHandle)
{
    RC_USE(cpu);
    RC_USE(sharedHandle);
    if (!buffer || !length)
        return D3DERR_INVALIDCALL;
    Buffer* b = new Buffer(this, KindVertexBuffer, length, usage, pool, fvf);
    if (!b->valid())
    {
        delete b;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *buffer = publish(app, b);
    return D3D_OK;
}

HRESULT Device::CreateIndexBuffer(WinApplication* app, x86::CPU& cpu, UINT length, DWORD usage, DWORD format,
                                  DWORD pool, x86::reg32* buffer, x86::reg32 sharedHandle)
{
    RC_USE(cpu);
    RC_USE(sharedHandle);
    if (!buffer || !length)
        return D3DERR_INVALIDCALL;
    if (format != D3DFMT_INDEX16)
    {
        pf::logError("d3d9: index format %u not supported", format);
        return D3DERR_INVALIDCALL;
    }
    Buffer* b = new Buffer(this, KindIndexBuffer, length, usage, pool, format);
    if (!b->valid())
    {
        delete b;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *buffer = publish(app, b);
    return D3D_OK;
}

HRESULT Device::CreateRenderTarget(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, DWORD format,
                                   DWORD multiSample, DWORD quality, BOOL lockable, x86::reg32* surface,
                                   x86::reg32 sharedHandle)
{
    RC_USE(multiSample);
    RC_USE(quality);
    RC_USE(lockable);
    if (!surface)
        return D3DERR_INVALIDCALL;
    // A render target texture whose only reference is its surface.
    x86::reg32 texture = 0;
    const HRESULT result = CreateTexture(app, cpu, width, height, 1, D3DUSAGE_RENDERTARGET, format, 0, &texture,
                                         sharedHandle);
    if (result != D3D_OK)
        return result;
    Texture* t = static_cast<Texture*>(lookup(texture));
    *surface = t->surface(app, 0, 0)->guest();
    t->release();
    return D3D_OK;
}

HRESULT Device::CreateDepthStencilSurface(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, DWORD format,
                                          DWORD multiSample, DWORD quality, BOOL discard, x86::reg32* surface,
                                          x86::reg32 sharedHandle)
{
    RC_USE(cpu);
    RC_USE(multiSample);
    RC_USE(quality);
    RC_USE(discard);
    RC_USE(sharedHandle);
    if (!surface || !width || !height)
        return D3DERR_INVALIDCALL;
    render::DepthStorage* depth = gpu().createDepth(width, height);
    if (!depth)
        return D3DERR_OUTOFVIDEOMEMORY;
    Surface* s = new Surface(this, Surface::RoleDepth, width, height, format);
    s->setDepth(depth);
    *surface = publish(app, s);
    return D3D_OK;
}

HRESULT Device::SetRenderTarget(WinApplication* app, x86::CPU& cpu, DWORD index, Surface* surface)
{
    RC_USE(app);
    RC_USE(cpu);
    if (index != 0)
    {
        D3D_LOG_ONCE("d3d9: render target %u ignored", index);
        return index < 4 && !surface ? D3D_OK : D3DERR_INVALIDCALL;
    }
    if (!surface || surface->role() == Surface::RoleDepth
        || (surface->role() == Surface::RoleLevel && (!surface->container() || !surface->container()->desc().target)))
        return D3DERR_INVALIDCALL;
    bindSlot(m_state.target, surface);
    m_sceneCached = false;
    // A new render target resets the viewport to the whole target.
    m_ctx.viewportSet(D3DVIEWPORT9{ 0, 0, surface->width(), surface->height(), 0.0f, 1.0f });
    m_viewportChanged = true;
    recordWords(CmdViewport, &m_ctx.fixed.viewport, sizeof(D3DVIEWPORT9) / 4);
    return D3D_OK;
}

HRESULT Device::GetRenderTarget(WinApplication* app, x86::CPU& cpu, DWORD index, x86::reg32* surface)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadTarget, m_parent != nullptr, 19, cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (!surface)
        return D3DERR_INVALIDCALL;
    if (index != 0 || !m_state.target)
    {
        *surface = 0;
        return D3DERR_NOTFOUND;
    }
    m_state.target->addRef();
    *surface = m_state.target->guest();
    return D3D_OK;
}

namespace
{

// First use of each kind of copy, for the log.
bool newCopyKind(uint32_t key)
{
    static uint32_t s_seen[32];
    static uint32_t s_count = 0;
    for (uint32_t i = 0; i < s_count; ++i)
        if (s_seen[i] == key)
            return false;
    if (s_count < 32)
        s_seen[s_count++] = key;
    return true;
}

}

// A textured quad drawn into the destination, in a scene of its own after the draws before it.
HRESULT Device::StretchRect(WinApplication* app, x86::CPU& cpu, Surface* source, const RECT* sourceRect,
                            Surface* dest, const RECT* destRect, DWORD filter)
{
    if (!source || !dest)
        return D3DERR_INVALIDCALL;
    const render::TextureStorage* from = nullptr;
    if (source->role() == Surface::RoleBackBuffer)
        from = render::backBufferTexture();
    else if (source->role() == Surface::RoleLevel && source->container() && !source->container()->desc().cube
             && source->level() == 0)
        from = source->container()->storage();
    render::Target target = {};
    bool to = dest->role() == Surface::RoleBackBuffer;
    if (dest->role() == Surface::RoleLevel && dest->container() && dest->container()->desc().target)
    {
        target.color = dest->container()->storage();
        if (source->role() == Surface::RoleBackBuffer)
            dest->container()->setOpaque();
        target.face = uint8_t(dest->face());
        to = target.color != nullptr;
    }
    const uint32_t sw = source->width(), sh = source->height(), dw = dest->width(), dh = dest->height();
    const RECT whole = { 0, 0, LONG(sw), LONG(sh) }, wholeDest = { 0, 0, LONG(dw), LONG(dh) };
    const RECT& sr = sourceRect ? *sourceRect : whole;
    const RECT& dr = destRect ? *destRect : wholeDest;
    const char* const roles[3] = { "texture", "back buffer", "depth" };
    const uint32_t key = uint32_t(source->role()) | uint32_t(dest->role()) << 2 | (filter & 3) << 4
                       | (sw / 8 & 0x7f) << 6 | (dw / 8 & 0x7f) << 13 | (dh / 8 & 0x7f) << 20 | (sh / 8 & 0x1f) << 27;
    if (s_dumping || newCopyKind(key))
        pf::logInfo("d3d9: StretchRect %s %ux%u (%ld,%ld-%ld,%ld) -> %s %ux%u (%ld,%ld-%ld,%ld), filter %u%s",
                    roles[source->role() % 3], sw, sh, long(sr.left), long(sr.top), long(sr.right), long(sr.bottom),
                    roles[dest->role() % 3], dw, dh, long(dr.left), long(dr.top), long(dr.right), long(dr.bottom),
                    unsigned(filter), !from || !to || from == target.color ? " (not copied)" : "");
    if (!from || !to || from == target.color || (!target.color && from == render::backBufferTexture()))
        return D3D_OK;
    if (sr.left >= sr.right || sr.top >= sr.bottom || dr.left >= dr.right || dr.top >= dr.bottom)
        return D3DERR_INVALIDCALL;

    render::Frame* f = frame(app, cpu);
    if (m_frameFull)
        return D3D_OK;
    render::Scene* s = scene(target, uint16_t(dw), uint16_t(dh));
    if (!s)
        return D3D_OK;
    if (m_drawCount == f->drawCapacity)
    {
        m_frameFull = true;
        return D3D_OK;
    }
    uint16_t viewport;
    if (!viewportIndex(*f, render::Viewport{ 0, 0, float(dw), float(dh), 0.0f, 1.0f }, &viewport))
        return D3D_OK;
    struct Vertex
    {
        float x, y, z, rhw;
        uint32_t color;
        float u, v;
    };
    static const uint8_t s_layout = [] {
        uint32_t components = 0;
        return render::registerLayout(VertexDeclaration::fvfLayout(0x4 | 0x40 | 0x100, &components));
    }();
    Vertex* v = static_cast<Vertex*>(render::allocateVertices(*f, 4 * sizeof(Vertex)));
    float* vc = nullptr;
    float* fc = nullptr;
    render::DrawCall& d = f->draws[m_drawCount];
    memset(&d, 0, sizeof(d));
    if (v)
        vc = render::allocateConstants(*f, sizeof(render::FixedVertexConstants) / 4, &d.vertexConstants);
    if (vc)
        fc = render::allocateConstants(*f, sizeof(render::FixedFragmentConstants) / 4, &d.fragmentConstants);
    if (!fc)
    {
        m_frameFull = true;
        return D3D_OK;
    }
    const float l = float(dr.left) - 0.5f, r = float(dr.right) - 0.5f;
    const float t = float(dr.top) - 0.5f, b = float(dr.bottom) - 0.5f;
    const float u0 = float(sr.left) / float(sw), u1 = float(sr.right) / float(sw);
    const float v0 = float(sr.top) / float(sh), v1 = float(sr.bottom) / float(sh);
    v[0] = Vertex{ l, t, 0.0f, 1.0f, 0xffffffffu, u0, v0 };
    v[1] = Vertex{ r, t, 0.0f, 1.0f, 0xffffffffu, u1, v0 };
    v[2] = Vertex{ r, b, 0.0f, 1.0f, 0xffffffffu, u1, v1 };
    v[3] = Vertex{ l, b, 0.0f, 1.0f, 0xffffffffu, u0, v1 };

    render::FixedVertexConstants& vk = *reinterpret_cast<render::FixedVertexConstants*>(vc);
    memset(&vk, 0, sizeof(vk));
    vk.screen[0] = 2.0f / float(dw);
    vk.screen[1] = -2.0f / float(dh);
    vk.screen[2] = -1.0f;
    vk.screen[3] = 1.0f;
    memcpy(vk.texMatrix, s_identity.m, sizeof(vk.texMatrix));
    render::FixedFragmentConstants& fk = *reinterpret_cast<render::FixedFragmentConstants*>(fc);
    memset(&fk, 0, sizeof(fk));
    for (int i = 0; i < 4; ++i)
        fk.textureMask[i] = fk.scale[i] = 1.0f;

    render::DrawState& st = d.state;
    st.textures[0] = from;
    st.samplers[0] = render::Sampler{ render::AddressClamp, render::AddressClamp, uint8_t(filter == 2 ? 3 : 0),
                                      render::MipNone };
    st.vertexProgram = render::VpFixedScreen;
    st.fragmentProgram = render::FpFixedTexture;
    st.layout = s_layout;
    st.primitive = render::PrimitiveFan;
    st.colorMask = 0xf;
    st.cullMode = 1;
    st.depthFunc = 8;
    d.vertices = v;
    d.count = 4;
    d.vertexCount = 4;
    d.viewport = viewport;
    const_cast<render::TextureStorage*>(from)->lastUse.store(f->index, std::memory_order_relaxed);
    if (from->uploads.load(std::memory_order_relaxed))
        __atomic_store_n(&f->uploads, true, __ATOMIC_RELAXED);
    recordDrawCall();
    return D3D_OK;
}

HRESULT Device::SetDepthStencilSurface(WinApplication* app, x86::CPU& cpu, Surface* surface)
{
    RC_USE(app);
    RC_USE(cpu);
    if (surface && surface->role() != Surface::RoleDepth)
        return D3DERR_INVALIDCALL;
    bindSlot(m_state.depth, surface);
    m_sceneCached = false;
    return D3D_OK;
}

HRESULT Device::GetDepthStencilSurface(WinApplication* app, x86::CPU& cpu, x86::reg32* surface)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadDepth, m_parent != nullptr, 20, cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (!surface)
        return D3DERR_INVALIDCALL;
    if (!m_state.depth)
    {
        *surface = 0;
        return D3DERR_NOTFOUND;
    }
    m_state.depth->addRef();
    *surface = m_state.depth->guest();
    return D3D_OK;
}

HRESULT Device::BeginScene(WinApplication* app, x86::CPU& cpu)
{
    frame(app, cpu);
    return D3D_OK;
}

HRESULT Device::EndScene(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

HRESULT Device::Clear(WinApplication* app, x86::CPU& cpu, DWORD count, const D3DRECT* rects, DWORD flags,
                      DWORD argb, float z, DWORD stencil)
{
    RC_USE(stencil);
    render::Frame* f = frame(app, cpu);
    if (m_frameFull)
        return D3D_OK;
    render::Scene* s = scene();
    if (!s)
        return D3D_OK;
    if (!s->target.depth)
        flags &= ~D3DCLEAR_ZBUFFER;
    const D3DVIEWPORT9& vp = m_ctx.fixed.viewport;
    const LONG vx0 = LONG(vp.X), vy0 = LONG(vp.Y);
    const LONG vx1 = std::min<LONG>(vx0 + LONG(vp.Width), s->width), vy1 = std::min<LONG>(vy0 + LONG(vp.Height), s->height);
    if (__builtin_expect(s_dumping.load(std::memory_order_relaxed), 0))
        pf::logInfo("dump: clear target %s %ux%u, viewport %ld,%ld-%ld,%ld, %u rects, flags %lx, colour %08lx",
                    s->target.color ? "texture" : "back buffer", unsigned(s->width), unsigned(s->height), long(vx0),
                    long(vy0), long(vx1), long(vy1), unsigned(rects ? count : 0), (unsigned long)flags,
                    (unsigned long)argb);
    const uint32_t rectCount = rects && count ? count : 1;
    for (uint32_t i = 0; i < rectCount; ++i)
    {
        LONG x0 = vx0, y0 = vy0, x1 = vx1, y1 = vy1;
        if (rects && count)
        {
            x0 = std::max(x0, rects[i].x1);
            y0 = std::max(y0, rects[i].y1);
            x1 = std::min(x1, rects[i].x2);
            y1 = std::min(y1, rects[i].y2);
        }
        if (x0 >= x1 || y0 >= y1)
            continue;
        DWORD what = flags;
        // A whole depth clear before the scene's first draw is the scene's starting depth.
        if ((what & D3DCLEAR_ZBUFFER) && m_sceneDraws == 0 && x0 == 0 && y0 == 0 && x1 == s->width && y1 == s->height)
        {
            s->depthCleared = true;
            s->clearDepth = z;
            what &= ~D3DCLEAR_ZBUFFER;
        }
        if (!(what & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER)))
            continue;
        if (m_drawCount == f->drawCapacity)
        {
            m_frameFull = true;
            return D3D_OK;
        }
        uint16_t viewport;
        if (!viewportIndex(*f, render::Viewport{ 0, 0, float(s->width), float(s->height), 0.0f, 1.0f }, &viewport))
            return D3D_OK;
        render::DrawCall& d = f->draws[m_drawCount];
        memset(&d, 0, sizeof(d));
        render::VertexLayout layout;
        memset(&layout, 0, sizeof(layout));
        layout.stride = 12;
        layout.count = 1;
        layout.elements[0] = render::VertexElement{ render::UsagePosition, render::TypeFloat3, 0 };
        d.state.layout = render::registerLayout(layout);
        d.state.vertexProgram = render::VpClear;
        d.state.fragmentProgram = render::FpClear;
        d.state.primitive = render::PrimitiveFan;
        d.state.colorMask = (what & D3DCLEAR_TARGET) ? 0xf : 0;
        d.state.depthEnable = d.state.depthWrite = (what & D3DCLEAR_ZBUFFER) ? 1 : 0;
        d.state.depthFunc = 8;
        d.state.cullMode = 1;
        float* v = static_cast<float*>(render::allocateVertices(*f, 48));
        float* c = render::allocateConstants(*f, sizeof(render::ClearFragmentConstants) / 4, &d.fragmentConstants);
        if (!v || !c)
        {
            m_frameFull = true;
            return D3D_OK;
        }
        // Pixel edges (centres at integers) in clip space of the whole target.
        const float sx = 2.0f / s->width, sy = 2.0f / s->height;
        const float l = (float(x0) - 0.5f) * sx - 1.0f, r = (float(x1) - 0.5f) * sx - 1.0f;
        const float t = 1.0f - (float(y0) - 0.5f) * sy, b = 1.0f - (float(y1) - 0.5f) * sy;
        const float quad[12] = { l, t, z, r, t, z, r, b, z, l, b, z };
        memcpy(v, quad, sizeof(quad));
        memset(c, 0, sizeof(render::ClearFragmentConstants));
        color(argb, reinterpret_cast<render::ClearFragmentConstants*>(c)->color);
        d.vertices = v;
        d.count = 4;
        d.vertexCount = 4;
        d.vertexConstants = render::NoConstants;
        d.viewport = viewport;
        recordDrawCall();
    }
    return D3D_OK;
}

HRESULT Device::SetTransform(WinApplication* app, x86::CPU& cpu, DWORD state, const D3DMATRIX* matrix)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!matrix)
        return D3DERR_INVALIDCALL;
    uint32_t t;
    switch (state)
    {
    case D3DTS_WORLD:       t = TransformWorld; break;
    case D3DTS_VIEW:        t = TransformView; break;
    case D3DTS_PROJECTION:  t = TransformProjection; break;
    case D3DTS_TEXTURE0:    t = TransformTexture0; break;
    default:                D3D_LOG_ONCE("d3d9: transform %u ignored", state); return D3D_OK;
    }
    if (!m_ctx.transformNeeds(t, *matrix))
        return D3D_OK;
    m_ctx.transformSet(t, *matrix);
    recordWords(CmdTransform | t << 8, matrix, 16);
    return D3D_OK;
}

HRESULT Device::GetTransform(WinApplication* app, x86::CPU& cpu, DWORD state, D3DMATRIX* matrix)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadTransform, m_parent != nullptr, 744 + 16 * (state == D3DTS_WORLD ? TransformWorld : state == D3DTS_VIEW ? TransformView : state == D3DTS_PROJECTION ? TransformProjection : TransformTexture0), cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (!matrix)
        return D3DERR_INVALIDCALL;
    switch (state)
    {
    case D3DTS_WORLD:       *matrix = m_ctx.fixed.transforms[TransformWorld]; break;
    case D3DTS_VIEW:        *matrix = m_ctx.fixed.transforms[TransformView]; break;
    case D3DTS_PROJECTION:  *matrix = m_ctx.fixed.transforms[TransformProjection]; break;
    case D3DTS_TEXTURE0:    *matrix = m_ctx.fixed.transforms[TransformTexture0]; break;
    default:                *matrix = s_identity; break;
    }
    return D3D_OK;
}

HRESULT Device::MultiplyTransform(WinApplication* app, x86::CPU& cpu, DWORD state, const D3DMATRIX* matrix)
{
    if (!matrix)
        return D3DERR_INVALIDCALL;
    D3DMATRIX current;
    GetTransform(app, cpu, state, &current);
    D3DMATRIX product;
    multiply(matrix->m, current.m, product.m);
    return SetTransform(app, cpu, state, &product);
}

HRESULT Device::SetViewport(WinApplication* app, x86::CPU& cpu, const D3DVIEWPORT9* viewport)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!viewport)
        return D3DERR_INVALIDCALL;
    if (!m_ctx.viewportNeeds(*viewport))
        return D3D_OK;
    m_ctx.viewportSet(*viewport);
    m_viewportChanged = true;
    recordWords(CmdViewport, viewport, sizeof(D3DVIEWPORT9) / 4);
    return D3D_OK;
}

HRESULT Device::GetViewport(WinApplication* app, x86::CPU& cpu, D3DVIEWPORT9* viewport)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadViewport, m_parent != nullptr, 808, cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (!viewport)
        return D3DERR_INVALIDCALL;
    *viewport = m_ctx.fixed.viewport;
    return D3D_OK;
}

HRESULT Device::SetMaterial(WinApplication* app, x86::CPU& cpu, const DWORD* material)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!material)
        return D3DERR_INVALIDCALL;
    memcpy(m_material, material, sizeof(m_material));
    return D3D_OK;
}

HRESULT Device::GetMaterial(WinApplication* app, x86::CPU& cpu, DWORD* material)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadMaterial, m_parent != nullptr, 23, cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (!material)
        return D3DERR_INVALIDCALL;
    memcpy(material, m_material, sizeof(m_material));
    return D3D_OK;
}

HRESULT Device::SetLight(WinApplication* app, x86::CPU& cpu, DWORD index, const DWORD* light)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!light || index >= 8)
        return D3DERR_INVALIDCALL;
    memcpy(m_lights[index], light, sizeof(m_lights[index]));
    return D3D_OK;
}

HRESULT Device::GetLight(WinApplication* app, x86::CPU& cpu, DWORD index, DWORD* light)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadLight, m_parent != nullptr, 23, cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (!light || index >= 8)
        return D3DERR_INVALIDCALL;
    memcpy(light, m_lights[index], sizeof(m_lights[index]));
    return D3D_OK;
}

HRESULT Device::LightEnable(WinApplication* app, x86::CPU& cpu, DWORD index, BOOL enable)
{
    RC_USE(app);
    RC_USE(cpu);
    if (index >= 8)
        return D3DERR_INVALIDCALL;
    m_lightEnabled[index] = enable;
    return D3D_OK;
}

HRESULT Device::GetLightEnable(WinApplication* app, x86::CPU& cpu, DWORD index, BOOL* enable)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadLightEnable, m_parent != nullptr, 23, cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (index >= 8 || !enable)
        return D3DERR_INVALIDCALL;
    *enable = m_lightEnabled[index] ? 128 : 0;
    return D3D_OK;
}

HRESULT Device::SetClipPlane(WinApplication* app, x86::CPU& cpu, DWORD index, const float* plane)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(index);
    RC_USE(plane);
    D3D_LOG_ONCE("d3d9: clip planes are ignored");
    return D3D_OK;
}

HRESULT Device::SetRenderState(WinApplication* app, x86::CPU& cpu, DWORD state, DWORD value)
{
    RC_USE(app);
    RC_USE(cpu);
    if (state >= MaxRenderStates)
        return D3DERR_INVALIDCALL;
    // A write after the effect pass wins over the pass's state even when the value is the same.
    if (const int s = passStateOf(state); s >= 0)
        m_deviceStates |= 1u << s;
    if (!m_ctx.renderNeeds(state, value))
        return D3D_OK;
    m_ctx.renderSet(state, value);
    record(CmdRenderState | state << 8, value);
    return D3D_OK;
}

HRESULT Device::GetRenderState(WinApplication* app, x86::CPU& cpu, DWORD state, DWORD* value)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadRender, m_parent != nullptr, state & 255, cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (state >= MaxRenderStates || !value)
        return D3DERR_INVALIDCALL;
    *value = m_ctx.fixed.renderStates[state];
    return D3D_OK;
}

HRESULT Device::GetTexture(WinApplication* app, x86::CPU& cpu, DWORD stage, x86::reg32* texture)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadTexture, m_parent != nullptr, stage & 15, cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (stage >= MaxSamplerStages || !texture)
        return D3DERR_INVALIDCALL;
    Object* t = m_state.textures[stage];
    if (t)
        t->addRef();
    *texture = t ? t->guest() : 0;
    return D3D_OK;
}

HRESULT Device::SetTexture(WinApplication* app, x86::CPU& cpu, DWORD stage, Object* texture)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stage >= MaxSamplerStages)
    {
        D3D_LOG_ONCE("d3d9: texture stage %u ignored", stage);
        return D3D_OK;
    }
    if (texture && texture->kind() != KindTexture && texture->kind() != KindCubeTexture)
        return D3DERR_INVALIDCALL;
    if (stage < render::MaxSamplers)
        m_deviceSamplers |= uint32_t(DeviceTexture) << (stage * 8);
    bindSlot(m_state.textures[stage], texture);
    return D3D_OK;
}

HRESULT Device::GetTextureStageState(WinApplication* app, x86::CPU& cpu, DWORD stage, DWORD type, DWORD* value)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadStage, m_parent != nullptr, 256 + (stage & 7) * 33 + (type < 33 ? type : 0), cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (stage >= MaxStages || type >= D3DTSS_COUNT || !value)
        return D3DERR_INVALIDCALL;
    *value = m_ctx.fixed.stageStates[stage][type];
    return D3D_OK;
}

HRESULT Device::SetTextureStageState(WinApplication* app, x86::CPU& cpu, DWORD stage, DWORD type, DWORD value)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stage >= MaxStages || type >= D3DTSS_COUNT)
        return D3DERR_INVALIDCALL;
    if (!m_ctx.stageNeeds(stage, type, value))
        return D3D_OK;
    m_ctx.stageSet(stage, type, value);
    record(CmdStageState | stage << 8 | type << 16, value);
    return D3D_OK;
}

HRESULT Device::GetSamplerState(WinApplication* app, x86::CPU& cpu, DWORD sampler, DWORD type, DWORD* value)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadSampler, m_parent != nullptr, 520 + (sampler & 15) * 14 + (type < 14 ? type : 0), cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (sampler >= MaxSamplerStages || type >= MaxSamplerStates || !value)
        return D3DERR_INVALIDCALL;
    *value = m_ctx.fixed.samplerStates[sampler][type];
    return D3D_OK;
}

HRESULT Device::SetSamplerState(WinApplication* app, x86::CPU& cpu, DWORD sampler, DWORD type, DWORD value)
{
    RC_USE(app);
    RC_USE(cpu);
    if (sampler >= MaxSamplerStages || type >= MaxSamplerStates)
        return D3D_OK;
    if (sampler < render::MaxSamplers)
    {
        const uint32_t bit = type == D3DSAMP_ADDRESSU ? DeviceAddressU : type == D3DSAMP_ADDRESSV ? DeviceAddressV
                           : type == D3DSAMP_MAGFILTER ? DeviceMagFilter : type == D3DSAMP_MINFILTER ? DeviceMinFilter
                           : type == D3DSAMP_MIPFILTER ? DeviceMipFilter : 0u;
        m_deviceSamplers |= bit << (sampler * 8);
    }
    if (!m_ctx.samplerNeeds(sampler, type, value))
        return D3D_OK;
    m_ctx.samplerSet(sampler, type, value);
    record(CmdSamplerState | sampler << 8 | type << 16, value);
    return D3D_OK;
}

HRESULT Device::ValidateDevice(WinApplication* app, x86::CPU& cpu, DWORD* passes)
{
    RC_USE(app);
    RC_USE(cpu);
    if (passes)
        *passes = 1;
    return D3D_OK;
}

HRESULT Device::SetScissorRect(WinApplication* app, x86::CPU& cpu, const RECT* rect)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(rect);
    D3D_LOG_ONCE("d3d9: scissor rectangles are ignored");
    return D3D_OK;
}

HRESULT Device::SetSoftwareVertexProcessing(WinApplication* app, x86::CPU& cpu, BOOL software)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(software);
    return D3D_OK;
}

BOOL Device::GetSoftwareVertexProcessing(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

HRESULT Device::SetNPatchMode(WinApplication* app, x86::CPU& cpu, float segments)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(segments);
    return D3D_OK;
}

HRESULT Device::DrawPrimitive(WinApplication* app, x86::CPU& cpu, DWORD type, UINT start, UINT count)
{
    const DrawTimer timer;
    Buffer* vb = m_state.stream;
    VertexDeclaration* decl = m_state.declaration ? m_state.declaration
                            : m_state.fvf ? fvfDeclaration(m_state.fvf) : nullptr;
    if (!vb || !decl || !m_state.streamStride)
        return D3DERR_INVALIDCALL;
    const uint32_t vertices = vertexCount(type, count);
    const uint64_t first = m_state.streamOffset + uint64_t(start) * m_state.streamStride;
    render::BufferStorage* storage = nullptr;
    const uint8_t* base = vb->drawData(frameIndex(), first, first + uint64_t(vertices) * m_state.streamStride,
                                       &storage);
    uint8_t layout;
    uint32_t texComponents;
    bool positionT;
    drawLayout(decl, m_state.streamStride, layout, texComponents, positionT);
    return draw(app, cpu, type, count, base + first, nullptr, vertices, layout, texComponents, positionT, storage,
                nullptr);
}

HRESULT Device::DrawIndexedPrimitive(WinApplication* app, x86::CPU& cpu, DWORD type, LONG baseVertex, UINT minIndex,
                                     UINT vertices, UINT startIndex, UINT count)
{
    const DrawTimer timer;
    Buffer* vb = m_state.stream;
    Buffer* ib = m_state.indices;
    VertexDeclaration* decl = m_state.declaration ? m_state.declaration
                            : m_state.fvf ? fvfDeclaration(m_state.fvf) : nullptr;
    if (!vb || !ib || !decl || !m_state.streamStride)
        return D3DERR_INVALIDCALL;
    const uint32_t index = frameIndex();
    const int64_t first = int64_t(m_state.streamOffset) + int64_t(baseVertex) * int64_t(m_state.streamStride);
    render::BufferStorage* vertexStorage = nullptr;
    render::BufferStorage* indexStorage = nullptr;
    const uint8_t* base = first >= 0
                        ? vb->drawData(index, uint64_t(first),
                                       uint64_t(first) + uint64_t(minIndex + vertices) * m_state.streamStride,
                                       &vertexStorage)
                        : vb->drawData(index, 1, 0, &vertexStorage);
    const uint8_t* indexBase = ib->drawData(index, uint64_t(startIndex) * 2,
                                            (uint64_t(startIndex) + vertexCount(type, count)) * 2, &indexStorage);
    const uint16_t* indices = reinterpret_cast<const uint16_t*>(indexBase) + startIndex;
    uint8_t layout;
    uint32_t texComponents;
    bool positionT;
    drawLayout(decl, m_state.streamStride, layout, texComponents, positionT);
    return draw(app, cpu, type, count, base + first, indices, minIndex + vertices, layout, texComponents, positionT,
                vertexStorage, indexStorage);
}

void* Device::frameVertices(WinApplication* app, x86::CPU& cpu, uint32_t bytes)
{
    render::Frame* f = frame(app, cpu);
    void* data = m_parent ? arenaAllocate(bytes) : render::allocateVertices(*f, bytes);
    if (!data)
    {
        if (!m_frameFull)
            pf::logError("d3d9: frame %u: vertex arena full, draws dropped", f->index);
        m_frameFull = true;
    }
    return data;
}

HRESULT Device::drawIndexedFrameVertices(WinApplication* app, x86::CPU& cpu, DWORD type, UINT minIndex, UINT vertices,
                                         UINT startIndex, UINT count, const void* data)
{
    const DrawTimer timer;
    Buffer* vb = m_state.stream;
    Buffer* ib = m_state.indices;
    VertexDeclaration* decl = m_state.declaration ? m_state.declaration
                            : m_state.fvf ? fvfDeclaration(m_state.fvf) : nullptr;
    if (!vb || !ib || !decl || !m_state.streamStride)
        return D3DERR_INVALIDCALL;
    if (!data)
        return D3D_OK;
    render::BufferStorage* indexStorage = nullptr;
    const uint8_t* indexBase = ib->drawData(frameIndex(), uint64_t(startIndex) * 2,
                                            (uint64_t(startIndex) + vertexCount(type, count)) * 2, &indexStorage);
    const uint16_t* indices = reinterpret_cast<const uint16_t*>(indexBase) + startIndex;
    uint8_t layout;
    uint32_t texComponents;
    bool positionT;
    drawLayout(decl, m_state.streamStride, layout, texComponents, positionT);
    return draw(app, cpu, type, count, data, indices, minIndex + vertices, layout, texComponents, positionT, nullptr,
                indexStorage);
}

HRESULT Device::DrawPrimitiveUP(WinApplication* app, x86::CPU& cpu, DWORD type, UINT count, const void* data,
                                UINT stride)
{
    const DrawTimer timer;
    VertexDeclaration* decl = m_state.declaration ? m_state.declaration
                            : m_state.fvf ? fvfDeclaration(m_state.fvf) : nullptr;
    // A user-pointer draw unbinds stream 0.
    bindSlot(m_state.stream, static_cast<Buffer*>(nullptr));
    m_state.streamOffset = m_state.streamStride = 0;
    if (!decl || !data || !stride)
        return D3DERR_INVALIDCALL;
    if (effectSkipped())
        return D3D_OK;
    const uint32_t vertices = vertexCount(type, count);
    render::Frame* f = frame(app, cpu);
    void* copy = m_parent ? arenaAllocate(vertices * stride) : render::allocateVertices(*f, vertices * stride);
    if (!copy)
    {
        if (!m_frameFull)
            pf::logError("d3d9: frame %u: vertex arena full, draws dropped", f->index);
        m_frameFull = true;
        return D3D_OK;
    }
    memcpy(copy, data, size_t(vertices) * stride);
    if (__builtin_expect(s_dumping.load(std::memory_order_relaxed), 0))
    {
        // x, y: the first two floats of each vertex; diffuse after the position (and normal) of an FVF.
        float x0 = HUGE_VALF, x1 = -HUGE_VALF, y0 = HUGE_VALF, y1 = -HUGE_VALF;
        for (uint32_t i = 0; i < vertices; ++i)
        {
            const float* v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(copy) + size_t(i) * stride);
            x0 = std::min(x0, v[0]), x1 = std::max(x1, v[0]), y0 = std::min(y0, v[1]), y1 = std::max(y1, v[1]);
        }
        const DWORD fvf = m_state.declaration ? 0 : m_state.fvf;
        uint32_t diffuse = 0;
        if (fvf & 0x40)
        {
            const uint32_t position = (fvf & 0xe) == 0x4 ? 16 : 12, offset = position + ((fvf & 0x10) ? 12 : 0)
                                                                                + ((fvf & 0x20) ? 4 : 0);
            if (offset + 4 <= stride)
                memcpy(&diffuse, static_cast<const uint8_t*>(copy) + offset, 4);
        }
        pf::logInfo("dump: up draw, stride %u, x %.3f..%.3f, y %.3f..%.3f, diffuse %08x", stride, x0, x1, y0, y1,
                    diffuse);
    }
    uint8_t layout;
    uint32_t texComponents;
    bool positionT;
    drawLayout(decl, stride, layout, texComponents, positionT);
    return draw(app, cpu, type, count, copy, nullptr, vertices, layout, texComponents, positionT, nullptr, nullptr);
}

HRESULT Device::DrawIndexedPrimitiveUP(WinApplication* app, x86::CPU& cpu, DWORD type, UINT minIndex, UINT vertices,
                                       UINT count, const void* indices, DWORD indexFormat, const void* data,
                                       UINT stride)
{
    const DrawTimer timer;
    VertexDeclaration* decl = m_state.declaration ? m_state.declaration
                            : m_state.fvf ? fvfDeclaration(m_state.fvf) : nullptr;
    bindSlot(m_state.stream, static_cast<Buffer*>(nullptr));
    bindSlot(m_state.indices, static_cast<Buffer*>(nullptr));
    m_state.streamOffset = m_state.streamStride = 0;
    if (!decl || !data || !indices || !stride || indexFormat != D3DFMT_INDEX16)
        return D3DERR_INVALIDCALL;
    if (effectSkipped())
        return D3D_OK;
    const uint32_t inputs = vertexCount(type, count);
    const uint32_t reach = minIndex + vertices;
    render::Frame* f = frame(app, cpu);
    void* vcopy = m_parent ? arenaAllocate(reach * stride) : render::allocateVertices(*f, reach * stride);
    void* icopy = m_parent ? arenaAllocate(inputs * 2) : render::allocateVertices(*f, inputs * 2);
    if (!vcopy || !icopy)
    {
        m_frameFull = true;
        return D3D_OK;
    }
    memcpy(vcopy, data, size_t(reach) * stride);
    memcpy(icopy, indices, size_t(inputs) * 2);
    return draw(app, cpu, type, count, vcopy, static_cast<const uint16_t*>(icopy), reach, m_parent ? decl->layoutAt(stride) : decl->layout(stride),
                decl->texComponents(), decl->positionT(), nullptr, nullptr);
}

HRESULT Device::CreateVertexDeclaration(WinApplication* app, x86::CPU& cpu, const D3DVERTEXELEMENT9* elements,
                                        x86::reg32* declaration)
{
    RC_USE(cpu);
    if (!elements || !declaration)
        return D3DERR_INVALIDCALL;
    uint32_t count = 0;
    while (count < 64 && elements[count].Stream != 0xFF)
        ++count;
    *declaration = publish(app, new VertexDeclaration(this, elements, count, 0));
    return D3D_OK;
}

HRESULT Device::SetVertexDeclaration(WinApplication* app, x86::CPU& cpu, VertexDeclaration* declaration)
{
    RC_USE(app);
    RC_USE(cpu);
    bindSlot(m_state.declaration, declaration);
    m_state.fvf = 0;
    m_state.layoutValid = false;
    return D3D_OK;
}

HRESULT Device::GetVertexDeclaration(WinApplication* app, x86::CPU& cpu, x86::reg32* declaration)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadDeclaration, m_parent != nullptr, 16, cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (!declaration)
        return D3DERR_INVALIDCALL;
    if (m_state.declaration)
        m_state.declaration->addRef();
    *declaration = m_state.declaration ? m_state.declaration->guest() : 0;
    return D3D_OK;
}

HRESULT Device::SetFVF(WinApplication* app, x86::CPU& cpu, DWORD fvf)
{
    RC_USE(app);
    RC_USE(cpu);
    bindSlot(m_state.declaration, static_cast<VertexDeclaration*>(nullptr));
    m_state.fvf = fvf;
    m_state.layoutValid = false;
    return D3D_OK;
}

HRESULT Device::GetFVF(WinApplication* app, x86::CPU& cpu, DWORD* fvf)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadFvf, m_parent != nullptr, 21, cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (!fvf)
        return D3DERR_INVALIDCALL;
    *fvf = m_state.fvf;
    return D3D_OK;
}

HRESULT Device::SetVertexShader(WinApplication* app, x86::CPU& cpu, x86::reg32 shader)
{
    RC_USE(app);
    RC_USE(cpu);
    if (shader)
    {
        pf::logError("d3d9: vertex shader objects are not supported (effects are native)");
        return D3DERR_INVALIDCALL;
    }
    return D3D_OK;
}

HRESULT Device::GetVertexShader(WinApplication* app, x86::CPU& cpu, x86::reg32* shader)
{
    RC_USE(app);
    RC_USE(cpu);
    if (shader)
        *shader = 0;
    return D3D_OK;
}

HRESULT Device::SetStreamSource(WinApplication* app, x86::CPU& cpu, UINT stream, Buffer* buffer, UINT offset,
                                UINT stride)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stream != 0)
    {
        D3D_LOG_ONCE("d3d9: stream %u ignored", stream);
        return D3D_OK;
    }
    if (buffer && buffer->kind() != KindVertexBuffer)
        return D3DERR_INVALIDCALL;
    bindSlot(m_state.stream, buffer);
    m_state.streamOffset = offset;
    m_state.streamStride = stride;
    return D3D_OK;
}

HRESULT Device::GetStreamSource(WinApplication* app, x86::CPU& cpu, UINT stream, x86::reg32* buffer, UINT* offset,
                                UINT* stride)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadStream, m_parent != nullptr, 17, cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (stream != 0 || !buffer || !offset || !stride)
        return D3DERR_INVALIDCALL;
    if (m_state.stream)
        m_state.stream->addRef();
    *buffer = m_state.stream ? m_state.stream->guest() : 0;
    *offset = m_state.streamOffset;
    *stride = m_state.streamStride;
    return D3D_OK;
}

HRESULT Device::SetStreamSourceFreq(WinApplication* app, x86::CPU& cpu, UINT stream, UINT divider)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(stream);
    if (divider != 1)
        D3D_LOG_ONCE("d3d9: stream frequency %u ignored", divider);
    return D3D_OK;
}

HRESULT Device::SetIndices(WinApplication* app, x86::CPU& cpu, Buffer* buffer)
{
    RC_USE(app);
    RC_USE(cpu);
    if (buffer && buffer->kind() != KindIndexBuffer)
        return D3DERR_INVALIDCALL;
    bindSlot(m_state.indices, buffer);
    return D3D_OK;
}

HRESULT Device::GetIndices(WinApplication* app, x86::CPU& cpu, x86::reg32* buffer)
{
#ifdef DEV_TOOLS
    probe::deviceRead(probe::ReadIndices, m_parent != nullptr, 18, cpu.esp);
#endif
    RC_USE(app);
    RC_USE(cpu);
    if (!buffer)
        return D3DERR_INVALIDCALL;
    if (m_state.indices)
        m_state.indices->addRef();
    *buffer = m_state.indices ? m_state.indices->guest() : 0;
    return D3D_OK;
}

HRESULT Device::SetPixelShader(WinApplication* app, x86::CPU& cpu, x86::reg32 shader)
{
    RC_USE(app);
    RC_USE(cpu);
    if (shader)
    {
        pf::logError("d3d9: pixel shader objects are not supported (effects are native)");
        return D3DERR_INVALIDCALL;
    }
    return D3D_OK;
}

HRESULT Device::GetPixelShader(WinApplication* app, x86::CPU& cpu, x86::reg32* shader)
{
    RC_USE(app);
    RC_USE(cpu);
    if (shader)
        *shader = 0;
    return D3D_OK;
}

namespace bridge
{

// Quiet calls run here, one function each; with deferral or a capture they go through d3d9_native.cpp.

HRESULT setRenderState(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD state, DWORD value)
{
    if (__builtin_expect(quiet(), 1))
        return deviceOf(cpu, guest)->SetRenderState(app, cpu, state, value);
    return setRenderStateSlow(app, cpu, guest, state, value);
}

HRESULT setVertexDeclaration(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 declaration)
{
    if (__builtin_expect(quiet() && !g_hot.deferDevice, 1))
        return deviceOf(cpu, guest)->SetVertexDeclaration(app, cpu, static_cast<VertexDeclaration*>(lookup(declaration)));
    return setVertexDeclarationSlow(app, cpu, guest, declaration);
}

HRESULT setStreamSource(WinApplication* app, x86::CPU& cpu, reg32 guest, UINT stream, reg32 buffer, UINT offset,
                        UINT stride)
{
    if (__builtin_expect(quiet() && !g_hot.deferDevice, 1))
        return deviceOf(cpu, guest)->SetStreamSource(app, cpu, stream, static_cast<Buffer*>(lookup(buffer)), offset,
                                                    stride);
    return setStreamSourceSlow(app, cpu, guest, stream, buffer, offset, stride);
}

HRESULT setIndices(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 buffer)
{
    if (__builtin_expect(quiet() && !g_hot.deferDevice, 1))
        return deviceOf(cpu, guest)->SetIndices(app, cpu, static_cast<Buffer*>(lookup(buffer)));
    return setIndicesSlow(app, cpu, guest, buffer);
}

HRESULT drawIndexedPrimitive(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD type, LONG baseVertex,
                             UINT minIndex, UINT vertices, UINT startIndex, UINT count)
{
    if (__builtin_expect(quiet() && !g_hot.deferDevice, 1))
        return deviceOf(cpu, guest)->DrawIndexedPrimitive(app, cpu, type, baseVertex, minIndex, vertices, startIndex,
                                                         count);
    return drawIndexedPrimitiveSlow(app, cpu, guest, type, baseVertex, minIndex, vertices, startIndex, count);
}

HRESULT drawPrimitiveUP(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD type, UINT count, const void* data,
                        UINT stride)
{
    if (__builtin_expect(quiet() && !g_hot.deferDevice, 1))
        return deviceOf(cpu, guest)->DrawPrimitiveUP(app, cpu, type, count, data, stride);
    return drawPrimitiveUPSlow(app, cpu, guest, type, count, data, stride);
}

}

}}
