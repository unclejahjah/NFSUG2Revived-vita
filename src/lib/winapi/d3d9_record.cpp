// Recorded frames: the game thread appends state changes and draws to the frame's stream (d3d9_device.cpp,
// d3dx9.cpp); the render thread replays them into a mirror of the device and effect state and resolves each draw
// (programs, fixed function, alpha test, blend factor, constants) before the frame is drawn.

#include <winapi/d3d9_internal.h>
#include <platform/switches.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstddef>
#include <cstdlib>

namespace win32 { namespace d3d9
{

static_assert(sizeof(DrawRecord) % 4 == 0, "DrawRecord is whole words");
static_assert(sizeof(FixedState) % 4 == 0, "FixedState is whole words");
static_assert(sizeof(render::DrawCall) % 4 == 0, "DrawCall is whole words");
static_assert(sizeof(SpliceRecord) % 4 == 0, "SpliceRecord is whole words");

// One segment of a frame: its commands and the stream it was opened in.
struct SegmentBuf
{
    Stream          stream;
    Segment         segment;
    Stream*         parent = nullptr;
    RecordContext::Saved saved;     // the parent's recorder words at the marker, put back at the end
    ValuesShadow*   shadow = nullptr;   // the effect values of the open segment
    bool            published = false;
};

uint64_t g_extraStateWords[2] = {};

namespace
{
std::vector<ValuesShadow*> s_freeShadows;   // one serves one open segment

ValuesShadow* takeShadow()
{
    if (s_freeShadows.empty())
        return new ValuesShadow;
    ValuesShadow* shadow = s_freeShadows.back();
    s_freeShadows.pop_back();
    return shadow;
}
}

namespace bridge
{

uint64_t extraStateWords(int segment)
{
    return g_extraStateWords[segment];
}

#ifdef DEV_TOOLS
static ResolveTotals s_totals = {};
static uint64_t (*s_clock)() = nullptr;

ResolveTotals resolveTotals()
{
    return s_totals;
}

void setResolveClock(uint64_t (*clock)())
{
    s_clock = clock;
}
#else
ResolveTotals resolveTotals() { return ResolveTotals{}; }
void setResolveClock(uint64_t (*)()) {}
#endif

}

namespace
{

enum : DWORD
{
    D3DRS_ZENABLE = 7, D3DRS_ZWRITEENABLE = 14, D3DRS_ALPHATESTENABLE = 15, D3DRS_SRCBLEND = 19,
    D3DRS_DESTBLEND = 20, D3DRS_CULLMODE = 22, D3DRS_ZFUNC = 23, D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25,
    D3DRS_ALPHABLENDENABLE = 27, D3DRS_FOGENABLE = 28, D3DRS_TEXTUREFACTOR = 60, D3DRS_LIGHTING = 137,
    D3DRS_COLORWRITEENABLE = 168, D3DRS_BLENDOP = 171, D3DRS_BLENDFACTOR = 193
};

const float s_identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

// The recording of frame n is s_streams[n % StreamCount]; s_resolved: the last frame resolved.
const uint32_t StreamCount = 4;
Stream s_streams[StreamCount];
std::atomic<uint32_t> s_resolved{0};

// Segments of each stream slot's frame in the order opened; reused once the frame is resolved.
const uint32_t SegmentWords = 2048;     // first allocation of a segment's stream
std::vector<SegmentBuf*> s_segments[StreamCount];
uint32_t s_segmentsUsed[StreamCount] = {};
uint32_t s_serial = 1, s_mainSerial = 1;

void color(DWORD argb, float out[4])
{
    out[0] = float((argb >> 16) & 255) / 255.0f;
    out[1] = float((argb >> 8) & 255) / 255.0f;
    out[2] = float(argb & 255) / 255.0f;
    out[3] = float(argb >> 24) / 255.0f;
}

// What the constants of a draw depend on; equal keys reuse the previous constants.
struct ConstantsKey
{
    uint32_t    source;         // 0 fixed function, else the effect's block and pass
    uint32_t    version;
    DWORD       alphaFunc, alphaRef;
    DWORD       fog, fogColor;  // effect draws: the pass's (or device's) FOGENABLE and FOGCOLOR
    float       blendFactor[4];
    uint8_t     vertexProgram, fragmentProgram, texComponents, alphaTest;
};

static_assert(sizeof(ConstantsKey) % 4 == 0, "ConstantsKey is whole words");

// What the alpha test elision found for an alpha-tested draw.
enum AlphaResult : uint8_t
{
    AlphaElided,            // discards nothing: drawn without the test
    AlphaProgram,           // a program whose alpha is not bounded here (fixed function, worldbone, filter)
    AlphaFunction,          // LESS, LESSEQUAL or NEVER
    AlphaTexture,           // no texture, or its smallest alpha is 0 or unknown
    AlphaVertex,            // vertex alpha 0 or unknown
    AlphaCarConstants,      // car pass without a lower bound of its diffuse alpha
    AlphaThreshold,         // the smallest alpha can fail the test
    AlphaResultCount
};

const char* const s_alphaNames[AlphaResultCount] = {
    "elided", "program", "function", "texture", "vertex", "car constants", "threshold"
};

// debug_draw_hash: what each draw hands the GPU, hashed per frame and logged 32 frames a line.
class DrawHash
{
public:
    void frame(const render::Frame& f)
    {
        uint32_t h = 2166136261u;
        static const int first = pf::Config::getInt("debug_draw_hash_from", -1);
        const uint32_t frameNumber = m_first + m_count;
        const bool each = first >= 0 && frameNumber >= uint32_t(first) && frameNumber < uint32_t(first) + 40;
        for (uint32_t i = 0; i < f.drawCount; ++i)
        {
            const render::DrawCall& d = f.draws[i];
            const render::DrawState& s = d.state;
            if (each)
            {
                const uint32_t vf = render::vertexProgram(render::VertexProgram(s.vertexProgram)).constantFloats;
                const uint32_t ff = render::fragmentProgram(render::FragmentProgram(s.fragmentProgram)).constantFloats;
                render::DrawState copy = s;
                copy.layout = 0;
                pf::logInfo("drawhash %u.%u: vp %u fp %u count %u state %08x vc %08x fc %08x tex %u", unsigned(frameNumber), unsigned(i),
                            unsigned(s.vertexProgram), unsigned(s.fragmentProgram), unsigned(d.count),
                            unsigned(mix(1, copy.samplers, sizeof(copy) - offsetof(render::DrawState, samplers))),
                            unsigned(d.count && d.vertexConstants != render::NoConstants ? mix(1, f.constants + d.vertexConstants, vf * 4) : 0),
                            unsigned(d.count && d.fragmentConstants != render::NoConstants ? mix(1, f.constants + d.fragmentConstants, ff * 4) : 0),
                            unsigned(s.textures[0] ? s.textures[0]->desc.width : 0));
                if (d.count && d.fragmentConstants != render::NoConstants && (s.fragmentProgram == 9 || s.fragmentProgram == 10 || s.fragmentProgram == 13))
                {
                    char text[700];
                    int length = snprintf(text, sizeof(text), "drawfc %u.%u:", unsigned(frameNumber), unsigned(i));
                    const uint32_t* c = reinterpret_cast<const uint32_t*>(f.constants + d.fragmentConstants);
                    for (uint32_t k = 0; k < ff && k < 40 && length < 680; ++k)
                        length += snprintf(text + length, sizeof(text) - length, " %08x", unsigned(c[k]));
                    pf::logInfo("%s", text);
                }
            }
            for (const render::TextureStorage* t : s.textures)
            {
                const uint32_t id = t ? 1u + t->desc.width + (uint32_t(t->desc.height) << 16) : 0u;
                h = mix(h, &id, 4);
            }
            render::DrawState copy = s;
            copy.layout = 0;
            h = mix(h, copy.samplers, sizeof(copy) - offsetof(render::DrawState, samplers));
            h = mix(h, &render::layout(s.layout), sizeof(render::VertexLayout));
            h = mix(h, &d.count, 4);
            h = mix(h, &d.vertexCount, 4);
            if (!d.count)
                continue;
            const uint32_t vf = render::vertexProgram(render::VertexProgram(s.vertexProgram)).constantFloats;
            const uint32_t ff = render::fragmentProgram(render::FragmentProgram(s.fragmentProgram)).constantFloats;
            if (d.vertexConstants != render::NoConstants)
                h = mix(h, f.constants + d.vertexConstants, vf * 4);
            if (d.fragmentConstants != render::NoConstants)
                h = mix(h, f.constants + d.fragmentConstants, ff * 4);
        }
        m_line[m_count++] = h;
        if (m_count == 32)
            flush();
    }
    void flush()
    {
        char text[32 * 9 + 64];
        int length = snprintf(text, sizeof(text), "d3d9: draw hash %u:", m_first);
        for (uint32_t i = 0; i < m_count && length > 0 && length < int(sizeof(text)); ++i)
            length += snprintf(text + length, sizeof(text) - length, " %08x", unsigned(m_line[i]));
        pf::logInfo("%s", text);
        m_first += m_count;
        m_count = 0;
    }

private:
    static uint32_t mix(uint32_t h, const void* data, uint32_t bytes)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (uint32_t i = 0; i < bytes; ++i)
            h = (h ^ p[i]) * 16777619u;
        return h;
    }

    uint32_t m_line[32];
    uint32_t m_count = 0, m_first = 0;
};

DrawHash s_drawHash;

// Render thread only.
class Resolver
{
public:
    void resolve(render::Frame& frame);

private:
    void run(render::Frame& frame, const uint32_t* w, const uint32_t* end, uint32_t depth);
    void splice(render::Frame& frame, const SpliceRecord& record, uint32_t depth);
    render::DrawCall& moved(render::Frame& frame, uint32_t slot);
    // Logs m_state words a segment overwrites.
    void logWords(const void* at, uint32_t words)
    {
        const uint32_t first = uint32_t(static_cast<const uint32_t*>(at) - reinterpret_cast<const uint32_t*>(&m_state));
        for (uint32_t i = 0; i < words; ++i)
            m_undo.push_back(uint64_t(first + i) << 32 | static_cast<const uint32_t*>(at)[i]);
    }

    bool fillState(render::Frame& frame, const DrawRecord& record, render::DrawCall& draw);
    AlphaResult alphaTestResult(const DrawRecord& record, const render::DrawCall& draw, const float test[2],
                                const float* own, const float* shared, const EffectBinding* binding) const;
    const float* block(uint16_t id) const
    {
        return id < m_blocks.size() && !m_blocks[id].empty() ? m_blocks[id].data() : nullptr;
    }

    FixedState                      m_state = {};
    std::vector<std::vector<float>> m_blocks;       // effect value blocks by id
    uint32_t                        m_fixedVersion = 1, m_effectVersion = 1;
    uint32_t                        m_bindingVersion = 1;       // device states and effect values passes read
    const render::TextureStorage*   m_lastTexture = nullptr;    // stage 0 of the last fixed-function draw
    uint32_t                        m_lastEffect = 0;
    bool                            m_keyValid = false;
    ConstantsKey                    m_key = {};
    uint32_t                        m_vertexConstants = render::NoConstants;
    uint32_t                        m_fragmentConstants = render::NoConstants;
    uint32_t                        m_shift = 0;                // reserved slots segments left unused so far
    uint32_t                        m_skip = 0;                 // CmdSkipIf flags set
    std::vector<uint64_t>           m_undo;                     // m_state words (index << 32) and what they were
    bool                            m_logging = false;          // inside a segment
    uint32_t                        m_touched = 0;              // transforms (bits 0-3) and viewport (bit 4) logged
    bool                            m_full = false;             // the constant arena ran out
    uint32_t                        m_alphaFrames = 0;
    uint32_t                        m_alphaCounts[AlphaResultCount] = {};   // timing_log: alpha-tested draws
};

Resolver s_resolver;

const uint32_t FrameVertexScan = 4096;      // most vertices of a frame-arena draw scanned for their alpha
const uint8_t BufferScans = 4;              // scans of one vertex buffer before its alpha counts as unknown

// Smallest vertex alpha a draw reads (the alpha byte of its colour element), 0 when unknown. Vertex buffers: the
// smallest over the whole buffer at the element's position, kept until the buffer is written in place again.
uint8_t vertexAlpha(const DrawRecord& record, const render::DrawCall& draw)
{
    const render::VertexLayout& l = render::layout(record.layout);
    const render::VertexElement* color = nullptr;
    for (uint32_t i = 0; i < l.count; ++i)
    {
        if (l.elements[i].usage == render::UsageColor
            && (l.elements[i].type == render::TypeColor || l.elements[i].type == render::TypeUByte4N))
            color = &l.elements[i];
    }
    if (!color || !l.stride || !draw.vertices)
        return 0;
    const uint32_t stride = l.stride;
    render::BufferStorage* b = const_cast<render::BufferStorage*>(record.vertexStorage);
    if (!b)
    {
        if (draw.vertexCount > FrameVertexScan)
            return 0;
        const uint8_t* p = static_cast<const uint8_t*>(draw.vertices) + color->offset + 3;
        uint8_t lowest = 255;
        for (uint32_t i = 0; i < draw.vertexCount && lowest; ++i)
            lowest = p[size_t(i) * stride] < lowest ? p[size_t(i) * stride] : lowest;
        return lowest;
    }
    const ptrdiff_t start = static_cast<const uint8_t*>(draw.vertices) - static_cast<const uint8_t*>(b->data);
    if (start < 0 || uint32_t(start) >= b->size)
        return 0;
    const uint32_t offset = uint32_t(start + color->offset + 3) % stride;
    const uint32_t writes = b->writes.load(std::memory_order_acquire);
    if (b->alphaWrites == writes && b->alphaStride == stride && b->alphaOffset == offset)
        return b->alpha;
    if (b->alphaScans >= BufferScans)
        return 0;
    ++b->alphaScans;
    const uint8_t* data = static_cast<const uint8_t*>(b->data);
    uint8_t lowest = 255;
    for (uint32_t i = offset; i < b->size && lowest; i += stride)
        lowest = data[i] < lowest ? data[i] : lowest;
    b->alphaWrites = writes;
    b->alphaStride = uint16_t(stride);
    b->alphaOffset = uint16_t(offset);
    b->alpha = lowest;
    return lowest;
}

// Whether the alpha test of a draw (discard when test[0] * saturate(alpha) < test[1]) discards no fragment.
AlphaResult Resolver::alphaTestResult(const DrawRecord& record, const render::DrawCall& draw, const float test[2],
                                      const float* own, const float* shared, const EffectBinding* binding) const
{
    const render::DrawState& state = draw.state;
    switch (state.fragmentProgram)
    {
    case render::FpWorldReflect:
    case render::FpWorldHeadlightReflect:
    case render::FpWorldReflectFog:
    case render::FpWorldHeadlightReflectFog:
        // Alpha 1: the programs discard when test[0] < test[1].
        return test[0] < test[1] ? AlphaThreshold : AlphaElided;
    case render::FpWorld:
    case render::FpWorldProjected:
    case render::FpWorldHeadlight:
    case render::FpWorldHeadlightFog:
        // Alpha = texel alpha * vertex alpha (* 2 for headlight), from programs that pass the vertex colour through.
        if (state.vertexProgram != render::VpWorld && state.vertexProgram != render::VpWorldProjected
            && state.vertexProgram != render::VpWorldHeadlight && state.vertexProgram != render::VpWorldHeadlightFog)
            return AlphaProgram;
        break;
    case render::FpCar:
        // Alpha = texel alpha * diffuse alpha * 2.
        if (state.vertexProgram != render::VpCar)
            return AlphaProgram;
        break;
    default:
        return AlphaProgram;
    }
    if (test[0] <= 0.0f)
        return AlphaFunction;
    if (!state.textures[0])
        return AlphaTexture;
    const uint8_t texel = render::minAlpha(*state.textures[0]);
    if (!texel)
        return AlphaTexture;
    float factor;
    if (state.fragmentProgram == render::FpCar)
    {
        float diffuse;
        if (!own || !binding || !effectVertexAlpha(record, own, shared, *binding, &diffuse))
            return AlphaCarConstants;
        // Rounded down to a multiple of 1/255.
        factor = float(int(diffuse * 255.0f)) / 255.0f;
    }
    else
    {
        const uint8_t vertex = vertexAlpha(record, draw);
        if (!vertex)
            return AlphaVertex;
        factor = float(vertex) / 255.0f;
    }
    // UG2's world_f does not double alpha; car_f and worldheadlight_f do.
    const float scale = state.fragmentProgram == render::FpWorld || state.fragmentProgram == render::FpWorldProjected
                        || state.fragmentProgram == render::FpWorldHeadlightFog ? 1.0f : 2.0f;
    // Two steps below the lowest product for filtering and interpolation rounding.
    const float lowest = float(texel) / 255.0f * factor * scale - 2.0f / 255.0f;
    return test[0] * std::min(lowest, 1.0f) >= test[1] ? AlphaElided : AlphaThreshold;
}

bool Resolver::fillState(render::Frame& f, const DrawRecord& record, render::DrawCall& draw)
{
    render::DrawState& state = draw.state;
    memset(&state, 0, sizeof(state));
    const DWORD* rs = m_state.renderStates;
    const bool positionT = record.positionT != 0;
    state.layout = record.layout;
    state.primitive = record.primitive;

    uint32_t source = 0;
    uint32_t version = m_fixedVersion;
    FixedResult fixed = {};
    const EffectDraw& effect = record.effect;
    const float* own = nullptr;
    const float* shared = nullptr;
    const EffectBinding* b = nullptr;
    DWORD fixedStates[PassStateCount];
    const DWORD* states = fixedStates;

    if (effect.kind != NoEffect)
    {
        own = block(effect.own);
        shared = block(effect.shared);
        if (!own)
        {
            D3D_LOG_ONCE("d3d9: effect draw without parameter values dropped");
            draw.count = 0;
            return true;
        }
        b = &effectBinding(record, m_state, own, shared, m_bindingVersion);
        if (b->skip)
        {
            draw.count = 0;
            return true;
        }
    }
    const bool programs = b && !b->fixed;
    if (programs)
    {
        states = b->states;
        state.vertexProgram = b->vertexProgram;
        state.fragmentProgram = b->fragmentProgram;
        for (uint32_t i = 0; i < b->samplerCount && i < render::MaxSamplers; ++i)
        {
            const render::TextureStorage* t = record.textures[i];
            if ((b->cubeSlots >> i) & 1)
            {
                if (t && !t->desc.cube)
                    D3D_LOG_ONCE("d3d9: %s: a 2D texture on cube sampler %u drawn as black", effectName(effect), i);
                if (!t || !t->desc.cube)
                    t = effectBlackCube();
            }
            else if (t && t->desc.cube)
            {
                D3D_LOG_ONCE("d3d9: %s: a cube texture on 2D sampler %u ignored", effectName(effect), i);
                t = nullptr;
            }
            state.textures[i] = t;
            state.samplers[i] = b->samplers[i];
        }
        source = 1u + effect.own + (uint32_t(effect.pass) << 16);
        if (source != m_lastEffect)
            ++m_effectVersion;
        m_lastEffect = source;
        version = m_effectVersion;
    }
    else
    {
        if (b)
            states = b->states;
        for (uint32_t s = 0; s < PassStateCount && !b; ++s)
            fixedStates[s] = rs[g_passRenderState[s]];
        if (states[PassLighting] && !positionT)
            D3D_LOG_ONCE("d3d9: fixed-function lighting is not emulated (vertex colours used)");
        if (states[PassFog])
            D3D_LOG_ONCE("d3d9: fixed-function fog is not emulated");
        const DWORD* tss = m_state.stageStates[0];
        const render::TextureStorage* texture = record.textures[0];
        if (texture != m_lastTexture)
            version = ++m_fixedVersion;
        m_lastTexture = texture;
        if (texture && texture->desc.cube)
        {
            D3D_LOG_ONCE("d3d9: cube texture on fixed-function stage 0 ignored");
            texture = nullptr;
        }
        if (m_state.stageStates[1][D3DTSS_COLOROP] != D3DTOP_DISABLE)
            D3D_LOG_ONCE("d3d9: fixed-function stages after 0 are not emulated");
        FixedStage stage = { tss[D3DTSS_COLOROP], tss[D3DTSS_COLORARG1], tss[D3DTSS_COLORARG2], tss[D3DTSS_ALPHAOP],
                             tss[D3DTSS_ALPHAARG1], tss[D3DTSS_ALPHAARG2], tss[D3DTSS_TEXTURETRANSFORMFLAGS],
                             texture != nullptr };
        fixed = resolveFixed(stage);
        state.vertexProgram = positionT ? render::VpFixedScreen : render::VpFixed;
        state.fragmentProgram = fixed.fragmentProgram;
        if (fixed.fragmentProgram != render::FpFixedColor)
        {
            state.textures[0] = texture;
            state.samplers[0] = samplerOf(m_state.samplerStates[0]);
        }
    }
    state.colorMask = uint8_t(states[PassColorWrite] & 0xf);
    state.depthEnable = states[PassZEnable] ? 1 : 0;
    state.depthWrite = states[PassZWrite] ? 1 : 0;
    state.depthFunc = uint8_t(states[PassZFunc]);
    state.cullMode = uint8_t(states[PassCull]);
    state.blendEnable = states[PassBlend] ? 1 : 0;
    state.srcBlend = uint8_t(states[PassSrcBlend]);
    state.dstBlend = uint8_t(states[PassDestBlend]);
    if (state.blendEnable && rs[D3DRS_BLENDOP] >= 2 && rs[D3DRS_BLENDOP] <= 5)
        state.blendOp = uint8_t(rs[D3DRS_BLENDOP]);
    const bool alphaEnabled = states[PassAlphaTest] != 0;
    const DWORD alphaRef = states[PassAlphaRef];
    if (state.blendEnable && (state.srcBlend == 7 || state.srcBlend == 8 || state.dstBlend == 7 || state.dstBlend == 8))
        D3D_LOG_ONCE("d3d9: a draw blends with destination alpha (%u, %u)",
                     unsigned(state.srcBlend), unsigned(state.dstBlend));

    float test[2] = { 0.0f, 0.0f };
    state.alphaTest = alphaEnabled && alphaTest(rs[D3DRS_ALPHAFUNC], alphaRef, test) ? 1 : 0;
    // A test that discards nothing: the program without it (the draw becomes opaque to the GPU).
    const bool s_elide = NFSU_RENDER_FAST_CACHED("skip_unused_alpha_test");
    if (state.alphaTest && (s_elide || pf::perf::enabled()))
    {
        const AlphaResult result = alphaTestResult(record, draw, test, own, shared, b);
        ++m_alphaCounts[result];
        if (s_elide && result == AlphaElided)
            state.alphaTest = 0;
    }

    const render::FragmentProgramInfo& fragment =
        render::fragmentProgram(render::FragmentProgram(state.fragmentProgram));
    BlendFactorDraw factor = {};
    if (state.blendEnable)
    {
        factor = resolveBlendFactor(state.srcBlend, state.dstBlend, rs[D3DRS_BLENDOP], rs[D3DRS_BLENDFACTOR],
                                    fragment.blendFactor && !state.alphaTest, (state.colorMask & 8) != 0);
        state.srcBlend = uint8_t(factor.src);
        state.dstBlend = uint8_t(factor.dst);
        state.factorOutput = factor.output;
    }

    ConstantsKey key;
    memset(&key, 0, sizeof(key));
    key.source = source;
    key.version = version;
    key.alphaFunc = rs[D3DRS_ALPHAFUNC];
    key.alphaRef = alphaRef;
    if (effect.kind != NoEffect)
    {
        key.fog = states[PassFog];
        key.fogColor = states[PassFogColor];
    }
    memcpy(key.blendFactor, factor.factor, sizeof(key.blendFactor));
    key.vertexProgram = state.vertexProgram;
    key.fragmentProgram = state.fragmentProgram;
    key.texComponents = uint8_t(record.texComponents | (positionT ? 0x80 : 0));
    key.alphaTest = state.alphaTest;
    if (m_keyValid && sameWords(&key, &m_key, sizeof(key) / 4))
    {
        draw.vertexConstants = m_vertexConstants;
        draw.fragmentConstants = m_fragmentConstants;
        return true;
    }

    const uint32_t vertexFloats = render::vertexProgram(render::VertexProgram(state.vertexProgram)).constantFloats;
    const uint32_t fragmentFloats = fragment.constantFloats;
    float* vc = nullptr;
    float* fc = nullptr;
    draw.vertexConstants = draw.fragmentConstants = render::NoConstants;
    if (vertexFloats && !(vc = render::allocateConstants(f, vertexFloats, &draw.vertexConstants)))
        return false;
    if (fragmentFloats && !(fc = render::allocateConstants(f, fragmentFloats, &draw.fragmentConstants)))
        return false;
#ifdef DEV_TOOLS
    // Unwritten constant floats (alpha row 2-3, unused blend factor) would make the draw hash depend on arena reuse.
    static const bool s_zero = pf::Config::getBool("debug_constants_zero", false);
    if (s_zero)
    {
        if (vc)
            memset(vc, 0, vertexFloats * 4);
        if (fc)
            memset(fc, 0, fragmentFloats * 4);
    }
#endif

    if (programs)
    {
        if (vc)
            effectVertexConstants(record, m_state, own, shared, *b, vc);
        if (fc)
            effectFragmentConstants(record, own, shared, *b, fc);
    }
    else
    {
        render::FixedVertexConstants& v = *reinterpret_cast<render::FixedVertexConstants*>(vc);
        float wv[16];
        multiply(m_state.transforms[TransformWorld].m, m_state.transforms[TransformView].m, wv);
        multiply(wv, m_state.transforms[TransformProjection].m, v.wvp);
        // Window coordinates of the viewport to clip space.
        const D3DVIEWPORT9& vp = m_state.viewport;
        const float w = float(vp.Width ? vp.Width : 1), h = float(vp.Height ? vp.Height : 1);
        v.screen[0] = 2.0f / w;
        v.screen[1] = -2.0f / h;
        v.screen[2] = -1.0f - 2.0f * float(vp.X) / w;
        v.screen[3] = 1.0f + 2.0f * float(vp.Y) / h;
        const DWORD flags = m_state.stageStates[0][D3DTSS_TEXTURETRANSFORMFLAGS];
        memcpy(v.texMatrix, (flags & 0xff) ? m_state.transforms[TransformTexture0].m : s_identity, sizeof(v.texMatrix));
        if ((flags & D3DTTFF_PROJECTED) && (flags & 0xff) == D3DTTFF_COUNT4)
        {
            // Divided by the fourth coordinate: move it to the third, which the program divides by.
            for (int row = 0; row < 4; ++row)
                v.texMatrix[row * 4 + 2] = v.texMatrix[row * 4 + 3];
        }
        texturePad(record.texComponents, v.texPad);
        memcpy(v.diffuseMask, fixed.diffuseMask, sizeof(v.diffuseMask));
        memcpy(v.factorMask, fixed.factorMask, sizeof(v.factorMask));
        color(rs[D3DRS_TEXTUREFACTOR], v.factor);
        render::FixedFragmentConstants& p = *reinterpret_cast<render::FixedFragmentConstants*>(fc);
        memset(&p, 0, sizeof(p));
        memcpy(p.textureMask, fixed.textureMask, sizeof(p.textureMask));
        memcpy(p.scale, fixed.scale, sizeof(p.scale));
        memcpy(p.addMask, fixed.addMask, sizeof(p.addMask));
    }
    if (fc && fragmentFloats >= 2)
    {
        fc[0] = test[0];
        fc[1] = test[1];
    }
    if (fc && state.factorOutput)
        memcpy(fc + fragment.blendFactor, factor.factor, sizeof(factor.factor));
    m_key = key;
    m_keyValid = true;
    m_vertexConstants = draw.vertexConstants;
    m_fragmentConstants = draw.fragmentConstants;
    return true;
}

// Moves a draw's geometry down over unused reserved slots.
__attribute__((noinline)) render::DrawCall& Resolver::moved(render::Frame& f, uint32_t slot)
{
    const render::DrawCall& from = f.draws[slot];
    render::DrawCall& to = f.draws[slot - m_shift];
    to.vertices = from.vertices;
    to.indices = from.indices;
    to.count = from.count;
    to.vertexCount = from.vertexCount;
    to.viewport = from.viewport;
    to.tag = from.tag;
#ifdef DEV_TOOLS
    if (bridge::s_clock)
        ++bridge::s_totals.shifted;
#endif
    return to;
}

void Resolver::splice(render::Frame& f, const SpliceRecord& record, uint32_t depth)
{
    if (depth > 8)
        pf::fatal("d3d9: frame %u: segments nested too deep", f.index);
    const Segment& segment = *record.segment;
    if (__builtin_expect(segment.ready.load(std::memory_order_acquire) == 0, 0))
    {
#ifdef DEV_TOOLS
        const uint64_t start = bridge::s_clock ? bridge::s_clock() : 0;
#endif
        segment.await(segment.job);
#ifdef DEV_TOOLS
        if (bridge::s_clock)
            bridge::s_totals.waitInstructions += bridge::s_clock() - start;
#endif
        if (segment.ready.load(std::memory_order_acquire) == 0)
            pf::fatal("d3d9: frame %u: a pending segment was not finished", f.index);
    }
    const uint32_t saved = m_skip;
    const size_t base = m_undo.size();
    const bool logging = m_logging;
    const uint32_t touched = m_touched;
    m_skip = record.flags;
    m_logging = true;
    m_touched = 0;
    run(f, segment.words, segment.words + segment.size, depth + 1);
    m_skip = saved;
    m_logging = logging;
    m_touched = touched;
#ifdef DEV_TOOLS
    if (bridge::s_clock)
    {
        ++bridge::s_totals.splices;
        bridge::s_totals.segmentWords += segment.size;
        bridge::s_totals.restoreWords += m_undo.size() - base;
    }
#endif
    uint32_t* words = reinterpret_cast<uint32_t*>(&m_state);
    for (size_t i = m_undo.size(); i > base; --i)
        words[m_undo[i - 1] >> 32] = uint32_t(m_undo[i - 1]);
    m_undo.resize(base);
    if (__builtin_expect(segment.drawCount != record.reserved, 0))
    {
        if (segment.drawCount > record.reserved || record.scene >= f.sceneCount)
            pf::fatal("d3d9: frame %u: a segment drew %u in %u slots", f.index, segment.drawCount, record.reserved);
        const uint32_t unused = record.reserved - segment.drawCount;
        f.scenes[record.scene].drawCount -= unused;
        for (uint32_t i = record.scene + 1; i < f.sceneCount; ++i)
            f.scenes[i].firstDraw -= unused;
        m_shift += unused;
    }
    ++m_fixedVersion;
    ++m_bindingVersion;
}

void Resolver::resolve(render::Frame& f)
{
    m_keyValid = false;
    m_full = false;
    m_shift = 0;
    m_skip = 0;
#ifdef DEV_TOOLS
    const uint64_t start = bridge::s_clock ? bridge::s_clock() : 0;
#endif
    run(f, f.commands, f.commands + f.commandCount, 0);
    f.drawCount -= m_shift;
#ifdef DEV_TOOLS
    if (bridge::s_clock)
    {
        bridge::ResolveTotals& t = bridge::s_totals;
        t.instructions += bridge::s_clock() - start;
        ++t.frames;
        t.draws += f.drawCount;
        t.words += f.commandCount;
    }
#endif
    if (pf::perf::enabled() && ++m_alphaFrames == 300)
    {
        char line[256];
        int length = snprintf(line, sizeof(line), "perf: alpha-tested draws per frame:");
        for (uint32_t i = 0; i < AlphaResultCount && length > 0 && length < int(sizeof(line)); ++i)
            length += snprintf(line + length, sizeof(line) - length, " %s %.1f", s_alphaNames[i],
                               double(m_alphaCounts[i]) / m_alphaFrames);
        pf::logInfo("%s", line);
        m_alphaFrames = 0;
        memset(m_alphaCounts, 0, sizeof(m_alphaCounts));
    }
    effectFrameStats();
    static const bool s_hash = pf::Config::getBool("debug_draw_hash", false);
    if (s_hash)
        s_drawHash.frame(f);
    s_resolved.store(f.index, std::memory_order_release);
}

void Resolver::run(render::Frame& f, const uint32_t* w, const uint32_t* end, uint32_t depth)
{
    while (w < end)
    {
        const uint32_t header = *w++;
        const uint32_t a = (header >> 8) & 0xff, b = header >> 16;
        switch (Command(header & 0xff))
        {
        case CmdRenderState:
            if (m_logging)
                logWords(&m_state.renderStates[header >> 8 & 0xff], 1);
            m_state.renderStates[header >> 8 & 0xff] = *w++;
            ++m_fixedVersion;
            ++m_bindingVersion;
            break;
        case CmdStageState:
            if (m_logging)
                logWords(&m_state.stageStates[a & 7][b < 33 ? b : 0], 1);
            m_state.stageStates[a & 7][b < 33 ? b : 0] = *w++;
            ++m_fixedVersion;
            ++m_bindingVersion;
            break;
        case CmdSamplerState:
            if (m_logging)
                logWords(&m_state.samplerStates[a & 15][b < 14 ? b : 0], 1);
            m_state.samplerStates[a & 15][b < 14 ? b : 0] = *w++;
            ++m_fixedVersion;
            ++m_bindingVersion;
            break;
        case CmdTransform:
            if (m_logging && !(m_touched >> (a & 3) & 1))
            {
                m_touched |= 1u << (a & 3);
                logWords(&m_state.transforms[a < TransformCount ? a : 0], sizeof(D3DMATRIX) / 4);
            }
            memcpy(&m_state.transforms[a < TransformCount ? a : 0], w, sizeof(D3DMATRIX));
            w += 16;
            ++m_fixedVersion;
            break;
        case CmdViewport:
            if (m_logging && !(m_touched >> 4 & 1))
            {
                m_touched |= 1u << 4;
                logWords(&m_state.viewport, sizeof(D3DVIEWPORT9) / 4);
            }
            memcpy(&m_state.viewport, w, sizeof(D3DVIEWPORT9));
            w += sizeof(D3DVIEWPORT9) / 4;
            ++m_fixedVersion;
            break;
        case CmdFixedState:
            if (m_logging)
            {
                m_touched = 31;
                logWords(&m_state, sizeof(FixedState) / 4);
            }
            memcpy(&m_state, w, sizeof(FixedState));
            w += sizeof(FixedState) / 4;
            ++m_fixedVersion;
            ++m_bindingVersion;
            break;
        case CmdValues:
        {
            const uint32_t id = header >> 8, offset = w[0] & 0xffff, count = w[0] >> 16;
            if (id >= m_blocks.size())
                m_blocks.resize(id + 1);
            std::vector<float>& values = m_blocks[id];
            if (values.empty())
                values.assign(effectBlockFloats(), 0.0f);
            if (offset + count <= values.size())
                memcpy(values.data() + offset, w + 1, count * 4);
            w += 1 + count;
            ++m_effectVersion;
            if (effectBindingReads(offset, count))
                ++m_bindingVersion;
            break;
        }
        case CmdProduct:
        {
            const uint32_t id = header >> 8, offset = w[0] & 0xffff, count = w[0] >> 16;
            if (id >= m_blocks.size())
                m_blocks.resize(id + 1);
            std::vector<float>& values = m_blocks[id];
            if (values.empty())
                values.assign(effectBlockFloats(), 0.0f);
            if (offset + 16 <= values.size())
                matrixProduct(reinterpret_cast<const float*>(w + 2), count, w[1], values.data() + offset);
            w += 2 + 16 * count;
            ++m_effectVersion;
            if (effectBindingReads(offset, 16))
                ++m_bindingVersion;
            break;
        }
        case CmdDraw:
        {
            const DrawRecord& r = *reinterpret_cast<const DrawRecord*>(w);
            w += sizeof(r) / 4;
            render::DrawCall& d = __builtin_expect(m_shift != 0, 0) ? moved(f, r.draw) : f.draws[r.draw];
            if (!m_full && !fillState(f, r, d))
            {
                pf::logError("d3d9: frame %u: constant arena full, draws dropped", f.index);
                m_full = true;
            }
            if (m_full)
            {
                memset(&d.state, 0, sizeof(d.state));
                d.count = 0;
            }
            break;
        }
        case CmdDrawCall:
            if (m_shift)
                f.draws[*w - m_shift] = f.draws[*w];
            ++w;
            break;
        case CmdSplice:
        {
            SpliceRecord r;
            memcpy(&r, w, sizeof(r));
            w += sizeof(r) / 4;
            splice(f, r, depth);
            break;
        }
        case CmdSkipIf:
        {
            const uint32_t words = *w++;
            if ((m_skip >> (a & 31)) & 1)
                w += words;
            break;
        }
        default:
            pf::fatal("d3d9: frame %u: bad command 0x%08x", f.index, header);
        }
    }
}

}

void ValuesShadow::grow(uint32_t block, uint32_t floats)
{
    if (block >= m_blocks.size())
        m_blocks.resize(block + 1);
    std::vector<Entry>& b = m_blocks[block];
    b.resize(std::max(effectBlockFloats(), floats), Entry{ 0, 0 });
}

void Stream::grow(uint32_t words)
{
    uint32_t capacity = this->capacity ? this->capacity : minimum;
    while (capacity < size + words)
        capacity *= 2;
    uint32_t* grown = static_cast<uint32_t*>(realloc(data, capacity * 4));
    if (!grown)
        pf::fatal("d3d9: out of memory for a %u KB frame recording", capacity / 256);
    data = grown;
    this->capacity = capacity;
}

Stream& frameStream(uint32_t frameIndex)
{
    if (frameIndex > StreamCount && s_resolved.load(std::memory_order_acquire) < frameIndex - StreamCount)
        pf::fatal("d3d9: frame %u begun before frame %u was resolved", frameIndex, frameIndex - StreamCount);
    Stream& s = s_streams[frameIndex % StreamCount];
    s.size = 0;
    s_segmentsUsed[frameIndex % StreamCount] = 0;
    return s;
}

void beginSegment(RecordContext& ctx, uint32_t frameIndex)
{
    std::vector<SegmentBuf*>& list = s_segments[frameIndex % StreamCount];
    uint32_t& used = s_segmentsUsed[frameIndex % StreamCount];
    if (used == list.size())
        list.push_back(new SegmentBuf);
    SegmentBuf& buf = *list[used++];
    buf.stream.size = 0;
    buf.stream.minimum = SegmentWords;
    buf.shadow = takeShadow();
    buf.stream.shadow = buf.shadow;
    buf.shadow->begin(++s_serial);
    buf.parent = ctx.stream;
    ctx.open(buf.saved);
    buf.segment.ready.store(1, std::memory_order_relaxed);
    buf.segment.await = nullptr;
    buf.segment.drawCount = 0;
    SpliceRecord r{ &buf.segment, 0, 0, 0 };
    uint32_t* w = ctx.stream->append(1 + sizeof(r) / 4);
    w[0] = CmdSplice;
    memcpy(w + 1, &r, sizeof(r));
    ctx.stream = &buf.stream;
    ctx.segment = &buf;
}

void endSegment(RecordContext& ctx)
{
    SegmentBuf& buf = *ctx.segment;
    buf.segment.words = buf.stream.data;
    buf.segment.size = buf.stream.size;
    ctx.stream = buf.parent;
    ctx.segment = nullptr;
    ctx.close(buf.saved);
    s_mainSerial = ++s_serial;
    s_freeShadows.push_back(buf.shadow);
    buf.shadow = buf.stream.shadow = nullptr;
}

void beginSegmentClone(RecordContext& parent, RecordContext& clone, uint32_t frameIndex, void (*await)(void*),
                       void* job, uint32_t scene, uint32_t reserved)
{
    std::vector<SegmentBuf*>& list = s_segments[frameIndex % StreamCount];
    uint32_t& used = s_segmentsUsed[frameIndex % StreamCount];
    if (used == list.size())
        list.push_back(new SegmentBuf);
    SegmentBuf& buf = *list[used++];
    buf.stream.size = 0;
    buf.stream.minimum = SegmentWords;
    buf.shadow = takeShadow();
    buf.stream.shadow = buf.shadow;
    buf.shadow->begin(++s_serial);
    buf.parent = parent.stream;
    s_mainSerial = ++s_serial;
    clone.openClone(parent);
    buf.published = false;
    buf.segment.words = nullptr;
    buf.segment.size = 0;
    buf.segment.drawCount = reserved;
    buf.segment.await = await;
    buf.segment.job = job;
    buf.segment.ready.store(await ? 0 : 1, std::memory_order_release);
    SpliceRecord r{ &buf.segment, 0, scene, reserved };
    uint32_t* w = parent.stream->append(1 + sizeof(r) / 4);
    w[0] = CmdSplice;
    memcpy(w + 1, &r, sizeof(r));
    clone.stream = &buf.stream;
    clone.segment = &buf;
}

void publishSegmentClone(RecordContext& clone, uint32_t draws)
{
    SegmentBuf& buf = *clone.segment;
    buf.segment.words = buf.stream.data;
    buf.segment.size = buf.stream.size;
    buf.segment.drawCount = draws;
    buf.published = true;
    buf.segment.ready.store(1, std::memory_order_release);
}

void endSegmentClone(RecordContext& clone, uint32_t draws)
{
    if (!clone.segment->published)
        publishSegmentClone(clone, draws);
    SegmentBuf& buf = *clone.segment;
    clone.stream = nullptr;
    clone.segment = nullptr;
    s_mainSerial = ++s_serial;
    s_freeShadows.push_back(buf.shadow);
    buf.shadow = buf.stream.shadow = nullptr;
}

uint32_t effectRecordSerial(const Stream* stream)
{
    return stream && stream->shadow ? stream->shadow->serial : s_mainSerial;
}

void resolveFrame(render::Frame& frame)
{
    s_resolver.resolve(frame);
}

void resetResolver()
{
    s_resolver = Resolver();
    s_resolved.store(0xffffffffu, std::memory_order_release);
}

}}
