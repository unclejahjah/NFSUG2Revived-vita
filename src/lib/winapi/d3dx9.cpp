// D3DX 9.0c natively: the effects of SPEED2.EXE's compiled resources (winapi/d3d9_fxtables.h) drawn with the
// port's programs, effect pools, and the matrix functions the game calls. See include/winapi/d3dx9.h.

#include <winapi/d3dx9.h>
#include <winapi/d3d9_internal.h>
#include <winapi/d3d9_native.h>
#include <winapi/d3d9_fxtables.h>
#include <winapi/d3d9_probe.h>
#include <winapi/kernel32.h>
#include <lib/heap.h>
#include <platform/switches.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/sha256.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>
#include <unordered_map>

#if defined(__ARM_NEON) && defined(__arm__)
#include <arm_neon.h>
#endif

namespace win32 { namespace d3d9
{

namespace
{

using fx::ParamCount;
using fx::EffectCount;

const uint32_t MaxPasses = 4;       // of technique 0 (FILTER has 4)
const uint32_t MaxRestore = 64;     // states technique 0 sets and End restores

bool isNumeric(uint32_t p)  { return fx::s_params[p].type >= 1 && fx::s_params[p].type <= 3 && fx::s_params[p].cls < 4; }
bool isFloat(uint32_t p)    { return isNumeric(p) && fx::s_params[p].type == 3; }
bool isInteger(uint32_t p)  { return isNumeric(p) && fx::s_params[p].type != 3; }
bool isTexture(uint32_t p)  { return fx::s_params[p].type >= 5 && fx::s_params[p].type <= 9; }

// Handles: parameters (the same for every effect), techniques, passes and technique annotations, all below any
// guest address (a larger handle is a name).
enum : x86::reg32
{
    HandleParam = 0x1000,
    HandleTechnique = 0x2000,
    HandlePass = 0x3000,
    HandleAnnotation = 0x5000
};

// The port's programs for a pass; stages: the sampler stage of each program sampler.
struct PassPrograms
{
    uint8_t             vertex, fragment;
    int8_t              stages[render::MaxSamplers];
    uint8_t             cubeSlots;
    const PassPrograms* fallback;   // used while these programs are missing (render::missing)
    bool                fixed;      // fixed-function pass: the device's pipeline draws it
};

const PassPrograms s_worldPrograms = { render::VpWorld, render::FpWorld, { 0, -1, -1, -1 }, 0, nullptr, false };
const PassPrograms s_skyMorphPrograms = { render::VpWorld, render::FpSkyMorph, { 0, 1, -1, -1 }, 0, nullptr, false };
// ps t0 is the cube map (stage 0), t1 the diffuse map (stage 1).
const PassPrograms s_carPrograms = { render::VpCar, render::FpCar, { 1, 0, -1, -1 }, 0x2, nullptr, false };
// UG2's reflection and headlight passes; the programs without fog stand in until the fog ones are compiled.
const PassPrograms s_reflectNoFog = { render::VpWorldReflect, render::FpWorldReflect, { 0, 1, -1, -1 }, 0, nullptr,
                                      false };
const PassPrograms s_reflectPrograms = { render::VpWorldReflectFog, render::FpWorldReflectFog, { 0, 1, -1, -1 }, 0,
                                         &s_reflectNoFog, false };
const PassPrograms s_headlightNoFog = { render::VpWorldHeadlight, render::FpWorldHeadlight, { 0, 1, 2, -1 }, 0,
                                        nullptr, false };
const PassPrograms s_headlightPrograms = { render::VpWorldHeadlightFog, render::FpWorldHeadlightFog, { 0, 1, 2, -1 },
                                           0, &s_headlightNoFog, false };
const PassPrograms s_headlightReflectNoFog = { render::VpWorldHeadlightReflect, render::FpWorldHeadlightReflect,
                                               { 0, 1, 2, 3 }, 0, nullptr, false };
const PassPrograms s_headlightReflectPrograms = { render::VpWorldHeadlightReflectFog,
                                                  render::FpWorldHeadlightReflectFog, { 0, 1, 2, 3 }, 0,
                                                  &s_headlightReflectNoFog, false };
const PassPrograms s_worldBonePrograms = { render::VpWorldBone, render::FpWorldBone, { 0, -1, -1, -1 }, 0, nullptr,
                                           false };
const PassPrograms s_fixedPrograms = { render::VpFixed, render::FpFixedTexture, { 0, -1, -1, -1 }, 0, nullptr, true };
// IDI_FILTER_FX pass n: n + 1 weighted taps, the vertex shader passes clip positions through.
const PassPrograms s_filterPrograms[4] = {
    { render::VpFilter, render::FpFilter1, { 0, -1, -1, -1 }, 0, nullptr, false },
    { render::VpFilter, render::FpFilter2, { 0, 1, -1, -1 }, 0, nullptr, false },
    { render::VpFilter, render::FpFilter3, { 0, 1, 2, -1 }, 0, nullptr, false },
    { render::VpFilter, render::FpFilter4, { 0, 1, 2, 3 }, 0, nullptr, false },
};
// OVERBRIGHT, SCREENFILTER passes 0-2 and RAIN_DROP: screen_f.cg, positions passed through as in FILTER.
const PassPrograms s_overBrightPrograms = { render::VpFilter, render::FpOverBright, { 0, -1, -1, -1 }, 0, nullptr,
                                            false };
const PassPrograms s_screenFilterPrograms[3] = {
    { render::VpFilter, render::FpScreenGain, { 0, 1, 2, -1 }, 0, nullptr, false },
    { render::VpFilter, render::FpScreenTint, { 0, -1, -1, -1 }, 0, nullptr, false },
    { render::VpFilter, render::FpScreenRamp, { 0, 1, -1, -1 }, 0, nullptr, false },
};
const PassPrograms s_rainDropPrograms = { render::VpFilter, render::FpRainDrop, { 0, 1, -1, -1 }, 0, nullptr, false };

// The first of a chain whose programs the device has.
const PassPrograms* available(const PassPrograms* p)
{
    while (p && !p->fixed && (render::missing(render::VertexProgram(p->vertex))
                              || render::missing(render::FragmentProgram(p->fragment))))
        p = p->fallback;
    return p;
}

const render::TextureStorage* s_blackCube = nullptr;

// Technique 0's programs of a pass; null: none.
const PassPrograms* passPrograms(uint32_t kind, uint32_t pass)
{
    if (kind == fx::EffectFilter)
        return pass < 4 ? &s_filterPrograms[pass] : nullptr;
    if (kind == fx::EffectScreenFilter)
        return pass < 3 ? available(&s_screenFilterPrograms[pass]) : nullptr;
    if (pass != 0)
        return nullptr;
    switch (kind)
    {
    case fx::EffectWorld:
    case fx::EffectWorldNoFog:
    case fx::EffectFe:
    case fx::EffectWorldNoDepth:
    case fx::EffectRunwayLight:
        return &s_worldPrograms;
    case fx::EffectSkyMorph:
        return &s_skyMorphPrograms;
    case fx::EffectCar:
        return &s_carPrograms;
    case fx::EffectWorldReflect:
        return available(&s_reflectPrograms);
    case fx::EffectWorldHeadlight:
    case fx::EffectWorldHeadlightNoDepth:
        return available(&s_headlightPrograms);
    case fx::EffectWorldHeadlightReflect:
        return available(&s_headlightReflectPrograms);
    case fx::EffectWorldBone:
        return available(&s_worldBonePrograms);
    case fx::EffectHorizonFog:
    case fx::EffectWorldMin:
        return &s_fixedPrograms;
    case fx::EffectOverBright:
        return available(&s_overBrightPrograms);
    case fx::EffectRainDrop:
        return available(&s_rainDropPrograms);
    default:
        return nullptr;
    }
}

const fx::Pass* passOf(uint32_t kind, uint32_t pass)
{
    const fx::Technique& t = fx::s_effects[kind].techniques[0];
    return t.passes && pass < t.passCount ? &t.passes[pass] : nullptr;
}

// COLORWRITEENABLE 8 only.
bool alphaOnly(const fx::Pass& pass)
{
    const fx::Value& v = pass.states[PassColorWrite];
    return v.source == fx::SourceConst && v.value == 8;
}

const fx::Sampler* samplerAt(const fx::Pass& pass, int stage)
{
    for (uint32_t i = 0; i < pass.samplerCount; ++i)
        if (pass.samplers[i].stage == stage)
            return &pass.samplers[i];
    return nullptr;
}

// Effects the first milestone draws: their technique 0 must validate.
const uint8_t s_required[] = { fx::EffectWorld, fx::EffectCar, fx::EffectWorldMin, fx::EffectWorldNoFog, fx::EffectFe,
                               fx::EffectWorldNoDepth, fx::EffectSkyMorph, fx::EffectRunwayLight };

enum : uint8_t
{
    FastFloat = 1,                  // one of the effect's parameters, float
    FastInt = 2                     // one of the effect's parameters, integer
};

struct KindInfo
{
    int8_t  local[ParamCount];      // index in the effect's parameters, -1: not one of them
    bool    shared[ParamCount];
    uint8_t fast[ParamCount];       // FastFloat, FastInt or 0
    bool    valid;                  // technique 0 validates
};

KindInfo s_kinds[EffectCount];
uint32_t s_offsets[ParamCount];
uint32_t s_totalFloats = 0;
const float s_zeros[256] = {};
bool s_bound[ParamCount];           // a technique 0 pass state or sampler field reads it
std::vector<uint8_t> s_paramAt;     // the parameter of each block float

void markBound(const fx::Value& v)
{
    if (v.source == fx::SourceParam)
        s_bound[v.param] = true;
}

void buildPassInfos();

void initTables()
{
    if (s_totalFloats)
        return;
    for (uint32_t p = 0; p < ParamCount; ++p)
    {
        s_offsets[p] = s_totalFloats;
        s_totalFloats += fx::s_params[p].floats;
        s_paramAt.resize(s_totalFloats, uint8_t(p));
    }
    for (uint32_t k = 0; k < EffectCount; ++k)
    {
        const fx::Technique& t = fx::s_effects[k].techniques[0];
        for (uint32_t p = 0; t.passes && p < t.passCount; ++p)
        {
            for (const fx::Value& v : t.passes[p].states)
                markBound(v);
            for (uint32_t i = 0; i < t.passes[p].samplerCount; ++i)
                for (const fx::Value& v : t.passes[p].samplers[i].fields)
                    markBound(v);
        }
    }
    for (uint32_t k = 0; k < EffectCount; ++k)
    {
        const fx::EffectInfo& e = fx::s_effects[k];
        KindInfo& info = s_kinds[k];
        memset(info.local, -1, sizeof(info.local));
        for (uint32_t i = 0; i < e.paramCount; ++i)
        {
            info.local[e.params[i].param] = int8_t(i);
            info.shared[e.params[i].param] = e.params[i].shared;
        }
        for (uint32_t p = 0; p < ParamCount; ++p)
            info.fast[p] = info.local[p] < 0 ? 0 : isFloat(p) ? FastFloat : isInteger(p) ? FastInt : 0;
        const fx::Technique& t = e.techniques[0];
        info.valid = t.passes != nullptr && t.passCount <= MaxPasses;
        for (uint32_t p = 0; info.valid && p < t.passCount; ++p)
            info.valid = alphaOnly(t.passes[p]) || passPrograms(k, p);
        if (t.restoreRender + t.restoreStage + t.restoreSampler > MaxRestore)
            pf::fatal("d3dx9: %s restores %u states", e.resource,
                      unsigned(t.restoreRender + t.restoreStage + t.restoreSampler));
    }
    for (uint8_t k : s_required)
        if (!s_kinds[k].valid)
            pf::fatal("d3dx9: %s technique 0 has no programs", fx::s_effects[k].resource);
    buildPassInfos();
}

// What a draw of technique 0's pass records; built once per kind and pass.
struct PassInfo
{
    const PassPrograms* programs;
    bool                built, checked;     // checked: the programs can be missing
    uint8_t             count, deviceSlots;
    uint8_t             texture[render::MaxSamplers];   // parameter of each sampler slot, fx::NoParam: none
};

PassInfo s_passInfo[2][EffectCount][MaxPasses];   // game thread, segment runner: one writer each

__attribute__((noinline)) void buildPassInfo(uint32_t kind, uint32_t pass, PassInfo& info)
{
    const fx::Pass* fxPass = passOf(kind, pass);
    const PassPrograms* programs = fxPass ? passPrograms(kind, pass) : nullptr;
    info.programs = programs;
    info.checked = programs && !programs->fixed;
    info.count = 0;
    info.deviceSlots = programs && programs->fixed ? 1 : 0;
    memset(info.texture, fx::NoParam, sizeof(info.texture));
    if (programs && !programs->fixed)
    {
        const uint32_t count = render::fragmentProgram(render::FragmentProgram(programs->fragment)).samplers;
        info.count = uint8_t(count < render::MaxSamplers ? count : render::MaxSamplers);
        for (uint32_t i = 0; i < info.count; ++i)
        {
            const fx::Sampler* s = samplerAt(*fxPass, programs->stages[i]);
            if (!s || s->texture == fx::NoParam)
                info.deviceSlots = uint8_t(info.deviceSlots | 1u << i);
            else
                info.texture[i] = s->texture;
        }
    }
    info.built = true;
#ifdef DEV_TOOLS
    probe::count(probe::g_inSegment ? probe::PassInfoClone : probe::PassInfoParent);
#endif
}

void buildPassInfos()
{
    for (uint32_t side = 0; side < 2; ++side)
        for (uint32_t k = 0; k < EffectCount; ++k)
            for (uint32_t p = 0; p < MaxPasses; ++p)
                buildPassInfo(k, p, s_passInfo[side][k][p]);
}

// Programs only go missing, so a built choice stays valid.
inline const PassInfo& passInfo(PassInfo& info, uint32_t kind, uint32_t pass)
{
    if (__builtin_expect(!info.built || (info.checked && (render::missing(render::VertexProgram(info.programs->vertex))
                                                          || render::missing(render::FragmentProgram(info.programs->fragment)))), 0))
        buildPassInfo(kind, pass, info);
    return info;
}

bool fastEffects()
{
    return NFSU_RENDER_FAST_CACHED("d3d_fast_effects");
}

bool fastLayer()
{
    return NFSU_RENDER_FAST_CACHED("d3d_fast_layer");
}

struct Values;
// Value blocks by id (the render thread mirrors their floats), and the blocks to send whole.
std::vector<Values*>    s_blocks;
std::vector<uint16_t>   s_dirty;

// A matrix parameter recorded as the product of 2 or 3 matrices (CmdProduct); its floats are computed when the game
// thread reads them.
struct Pending
{
    int         param;
    uint32_t    count;
    uint32_t    fpscr;
    uint32_t    serial;     // the context that recorded it (effectRecordSerial)
    float       matrices[48];
};

// Parameter values: floats (ints stored as their bits) and textures.
struct Values
{
    enum { PendingSlots = 4 };

    uint16_t            id;                 // id, dirty and pendingCount first: every write reads them
    bool                dirty = false;
    uint32_t            pendingCount = 0;
    uint32_t            unknownCount = 0;   // floats a segment overwrote on the render thread's copy
    std::vector<float>  floats;
    Object*             textures[ParamCount] = {};
    Pending             pending[PendingSlots];
    std::vector<uint64_t> unknown;          // a bit per float

    void markUnknown(uint32_t offset, uint32_t n)
    {
        if (unknown.empty())
            unknown.assign((s_totalFloats + 63) / 64, 0);
        for (uint32_t i = offset; i < offset + n; ++i)
        {
            const uint64_t bit = uint64_t(1) << (i & 63);
            unknownCount += !(unknown[i >> 6] & bit);
            unknown[i >> 6] |= bit;
        }
    }
    // Whether any of the floats was unknown; they are known again.
    bool takeUnknown(uint32_t offset, uint32_t n)
    {
        bool any = false;
        for (uint32_t i = offset; i < offset + n; ++i)
        {
            const uint64_t bit = uint64_t(1) << (i & 63);
            if (unknown[i >> 6] & bit)
            {
                unknown[i >> 6] &= ~bit;
                --unknownCount;
                any = true;
            }
        }
        return any;
    }
    // The store must be recorded even when the value is equal: a segment pends or may have written the floats.
    bool staleStore(uint32_t offset, uint32_t n)
    {
        bool stale = bridge::g_pendingSegments != 0;
        if (unknownCount)
            stale |= takeUnknown(offset, n);
        return stale;
    }
    void clearUnknown()
    {
        std::fill(unknown.begin(), unknown.end(), 0);
        unknownCount = 0;
    }

    Pending* findPending(int p)
    {
        for (uint32_t i = 0; i < pendingCount; ++i)
            if (pending[i].param == p)
                return &pending[i];
        return nullptr;
    }
    // Computes the floats of a pending parameter.
    void settle(int p)
    {
        for (uint32_t i = 0; i < pendingCount; ++i)
        {
            if (pending[i].param != p)
                continue;
            matrixProduct(pending[i].matrices, pending[i].count, pending[i].fpscr, floats.data() + s_offsets[p]);
            pending[i] = pending[--pendingCount];
            return;
        }
    }
    void settleAll()
    {
        while (pendingCount)
            settle(pending[0].param);
    }

    Values()
    {
        initTables();
        floats.assign(s_totalFloats, 0.0f);
        id = uint16_t(std::find(s_blocks.begin(), s_blocks.end(), nullptr) - s_blocks.begin());
        if (id == s_blocks.size())
            s_blocks.push_back(this);
        else
            s_blocks[id] = this;
        markDirty();
    }
    ~Values()
    {
        for (Object* t : textures)
            if (t)
                t->unbind();
        s_blocks[id] = nullptr;
        if (dirty)
            s_dirty.erase(std::find(s_dirty.begin(), s_dirty.end(), id));
    }
    void markDirty()
    {
        if (!dirty)
            s_dirty.push_back(id);
        dirty = true;
    }
};

// A draw's parameter values (render thread, mirrored blocks); zeros for a parameter the effect lacks.
struct View
{
    uint32_t        kind;
    const float*    own;
    const float*    shared;     // null without a pool

    const float* floats(uint32_t p) const
    {
        const KindInfo& k = s_kinds[kind];
        if (k.local[p] < 0)
            return s_zeros;
        return (k.shared[p] && shared ? shared : own) + s_offsets[p];
    }
    x86::sreg32 intValue(uint32_t p, uint32_t element = 0) const
    {
        x86::sreg32 v;
        memcpy(&v, floats(p) + element, 4);
        return v;
    }
    // A state value; float parameters truncated (ftoi).
    DWORD value(const fx::Value& v) const
    {
        if (v.source == fx::SourceConst)
            return v.value;
        if (isFloat(v.param))
            return DWORD(x86::sreg32(floats(v.param)[v.element]));
        return DWORD(intValue(v.param, v.element));
    }
};

// Sampler fields as D3DSAMP states and Device::deviceSamplers bits.
const uint8_t s_fieldState[fx::FieldCount] = { 1, 2, 5, 6, 7 };
const uint8_t s_fieldBit[fx::FieldCount] = { DeviceAddressU, DeviceAddressV, DeviceMagFilter, DeviceMinFilter,
                                             DeviceMipFilter };

// Render thread: the last binding and its inputs (version 0: none).
struct BindingKey
{
    EffectDraw  effect;
    uint32_t    deviceStates, deviceSamplers, texComponents, version;
};
static_assert(sizeof(BindingKey) % 4 == 0, "BindingKey is whole words");
BindingKey s_bindingKey = {};
EffectBinding s_binding;

// Diagnostics: effect_stats counts (draws and bindings made on the render thread, dropped draws and bad handles on
// the game thread).
uint32_t s_draws[EffectCount][MaxPasses];
uint32_t s_bindingCalls = 0, s_bindingsMade = 0;
std::atomic<uint32_t> s_dropped[EffectCount][MaxPasses];
std::atomic<uint32_t> s_badHandles{0};
uint32_t s_statFrames = 0;

bool statsEnabled()
{
    return NFSU_DEV_BOOL_CACHED("effect_stats", false);
}

#ifdef DEV_SETTINGS
// The first effect_trace parameter writes are logged; -1: not read yet.
int s_traceLeft = -1;

bool tracing()
{
    if (s_traceLeft < 0)
        s_traceLeft = std::max(pf::Config::getInt("effect_trace", 0), 0);
    bridge::g_hot.trace = s_traceLeft != 0;
    return s_traceLeft > 0;
}
#endif

// Guest copy of a name or semantic, kept.
x86::reg32 guestString(const char* text)
{
    static std::unordered_map<const char*, x86::reg32> s_strings;
    auto it = s_strings.find(text);
    if (it != s_strings.end())
        return it->second;
    const size_t n = strlen(text) + 1;
    const x86::reg32 address = heap::allocate(x86::reg32(n), true);
    if (!address)
        pf::fatal("d3dx9: out of guest memory for the string %s", text);
    memcpy(g_guestBase + address, text, n);
    s_strings.emplace(text, address);
    return address;
}

void color(DWORD argb, float out[4])
{
    out[0] = float((argb >> 16) & 255) / 255.0f;
    out[1] = float((argb >> 8) & 255) / 255.0f;
    out[2] = float(argb & 255) / 255.0f;
    out[3] = float(argb >> 24) / 255.0f;
}

}

class EffectPool : public Object
{
public:
    EffectPool() : Object(KindEffectPool) {}
    Values values;
};

class Effect;

// The pass an effect has open in one context: the game thread's, or a segment's.
struct OpenPass final : EffectPass
{
    Effect*         effect = nullptr;
    PassInfo*       info = nullptr;         // of the current pass; null: recordSlow
    uint32_t        pass = 0;
    uint32_t        stamp = 0;              // segment state: serial it was set up for
    Stream*         segment = nullptr;      // segment state: its stream
    mutable uint8_t textureStages = 0;      // stages whose texture parameter changed in this pass (CommitChanges)
    bool            saved = false;          // Begin saved the states technique 0 sets
    DWORD           restore[MaxRestore];
    mutable bool    recorded = false;       // the record fields hold the last record()
    mutable uint8_t recordSlots = 0;
    mutable uint32_t recordEpoch = 0, recordWrites = 0;     // g_hot.textureEpoch, the segment's texture writes
    mutable EffectDraw recordEffect = {};
    mutable const render::TextureStorage* recordTextures[render::MaxSamplers] = {};

    bool drawn() const override;
    void record(EffectDraw& effect, const render::TextureStorage* textures[render::MaxSamplers],
                uint8_t& deviceSlots) const override;
};

class Effect : public Object
{
    friend struct OpenPass;

public:
    Effect(Device* device, EffectPool* pool, uint32_t kind)
        :   Object(KindEffect), m_device(device), m_pool(pool), m_kind(kind)
    {
        device->addRef();
        if (pool)
            pool->addRef();
        m_fast = fastEffects();
        m_reuseRecord = m_fast && fastLayer();
        for (uint32_t i = 0; i < MaxPasses && m_fast; ++i)
        {
            const fx::Pass* pass = passOf(kind, i);
            m_always = uint8_t(m_always | ((!pass || !alphaOnly(*pass)) ? 1u << i : 0u));
        }
        for (uint32_t p = 0; p < ParamCount; ++p)
        {
            Values& v = values(int(p));
            const bool isFloat = (s_kinds[kind].fast[p] & FastFloat) && m_fast
                                 && device == g_streamDevice;
            const bool isInt = (s_kinds[kind].fast[p] & FastInt) && m_fast && device == g_streamDevice;
            m_slots[p] = Slot{ &v, v.floats.data() + s_offsets[p], uint16_t(s_offsets[p]),
                               uint16_t(isFloat ? fx::s_params[p].floats : 0),
                               uint16_t(isInt ? fx::s_params[p].floats : 0) };
        }
        m_ps[0].effect = m_ps[1].effect = this;
        techniqueChanged();
    }
    ~Effect() override
    {
        if (m_device->effectPass() == &m_ps[0])
            m_device->setEffectPass(nullptr);
        if (m_pool)
            m_pool->release();
        m_device->release();
    }

    bool drawnFor(const OpenPass& ps) const;
    bool passDrawn(uint32_t pass) const;
    void recordFor(const OpenPass& ps, EffectDraw& effect, const render::TextureStorage* textures[render::MaxSamplers],
                   uint8_t& deviceSlots) const;

    HRESULT GetDesc(WinApplication* app, x86::CPU& cpu, DWORD* desc);
    HRESULT GetParameterDesc(WinApplication* app, x86::CPU& cpu, x86::reg32 param, DWORD* desc);
    x86::reg32 GetParameter(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, UINT index);
    x86::reg32 GetParameterByName(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, const char* name);
    x86::reg32 GetParameterBySemantic(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, const char* semantic);
    x86::reg32 GetTechnique(WinApplication* app, x86::CPU& cpu, UINT index);
    x86::reg32 GetTechniqueByName(WinApplication* app, x86::CPU& cpu, const char* name);
    x86::reg32 GetPass(WinApplication* app, x86::CPU& cpu, x86::reg32 technique, UINT index);
    x86::reg32 GetAnnotationByName(WinApplication* app, x86::CPU& cpu, x86::reg32 object, const char* name);
    HRESULT SetValue(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const void* data, UINT bytes);
    HRESULT GetValue(WinApplication* app, x86::CPU& cpu, x86::reg32 param, void* data, UINT bytes);
    HRESULT SetBool(WinApplication* app, x86::CPU& cpu, x86::reg32 param, BOOL value);
    HRESULT SetInt(WinApplication* app, x86::CPU& cpu, x86::reg32 param, x86::sreg32 value);
    HRESULT GetInt(WinApplication* app, x86::CPU& cpu, x86::reg32 param, x86::sreg32* value);
    HRESULT SetIntArray(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const x86::sreg32* values, UINT count);
    HRESULT GetIntArray(WinApplication* app, x86::CPU& cpu, x86::reg32 param, x86::sreg32* values, UINT count);
    HRESULT SetFloat(WinApplication* app, x86::CPU& cpu, x86::reg32 param, float value);
    HRESULT GetFloat(WinApplication* app, x86::CPU& cpu, x86::reg32 param, float* value);
    HRESULT SetFloatArray(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const float* values, UINT count);
    HRESULT SetVector(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const float* vector);
    HRESULT GetVector(WinApplication* app, x86::CPU& cpu, x86::reg32 param, float* vector);
    HRESULT SetVectorArray(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const float* vectors, UINT count);
    HRESULT SetMatrix(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const float* matrix);
    HRESULT GetMatrix(WinApplication* app, x86::CPU& cpu, x86::reg32 param, float* matrix);
    HRESULT SetMatrixArray(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const float* matrices, UINT count);
    HRESULT SetMatrixTranspose(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const float* matrix);
    HRESULT SetMatrixTransposeArray(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const float* matrices,
                                    UINT count);
    HRESULT SetTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 param, Object* texture);
    HRESULT GetTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 param, x86::reg32* texture);
    HRESULT GetPool(WinApplication* app, x86::CPU& cpu, x86::reg32* pool);
    HRESULT SetTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique);
    x86::reg32 GetCurrentTechnique(WinApplication* app, x86::CPU& cpu);
    HRESULT ValidateTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique);
    HRESULT FindNextValidTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique, x86::reg32* next);
    BOOL IsParameterUsed(WinApplication* app, x86::CPU& cpu, x86::reg32 param, x86::reg32 technique);
    HRESULT Begin(WinApplication* app, x86::CPU& cpu, UINT* passes, DWORD flags);
    HRESULT BeginPass(WinApplication* app, x86::CPU& cpu, UINT pass);
    HRESULT CommitChanges(WinApplication* app, x86::CPU& cpu);
    HRESULT EndPass(WinApplication* app, x86::CPU& cpu);
    HRESULT End(WinApplication* app, x86::CPU& cpu);
    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    HRESULT OnLostDevice(WinApplication* app, x86::CPU& cpu);
    HRESULT OnResetDevice(WinApplication* app, x86::CPU& cpu);

    // SetMatrix of the product of count (2 or 3) matrices, recorded unmultiplied; the render thread multiplies with
    // fpscr.
    HRESULT setProduct(x86::reg32 handle, const float* const* matrices, uint32_t count, uint32_t fpscr, Stream* stream);
    // The pass count Begin stores.
    UINT passCount() const                  { return technique().passCount; }
    // The type of each parameter handle in this effect (bridge::effect::paramTypes).
    const uint8_t* paramTypes() const;
    HRESULT checkPass(UINT pass) const      { return pass >= technique().passCount ? D3DERR_INVALIDCALL : D3D_OK; }
    // Writes N floats by handle; false: not taken.
    template< uint32_t N >
    bool setFast(x86::reg32 handle, const void* data, Stream* stream);
    // SetFloat, SetVector, SetMatrix and SetVectorArray for bridge::effect.
    HRESULT setFloat(x86::reg32 handle, float value, Stream* stream);
    HRESULT setVector(x86::reg32 handle, const float* vector, Stream* stream);
    HRESULT setMatrix(x86::reg32 handle, const float* matrix, Stream* stream);
    HRESULT setVectors(x86::reg32 handle, const float* vectors, uint32_t count, Stream* stream);
    // SetIntArray for bridge::effectSetIntArray.
    HRESULT setInts(x86::reg32 handle, const x86::sreg32* data, uint32_t count, Stream* stream);
    // CommitChanges without its arguments.
    void commit(const x86::CPU& cpu);
    // BeginPass without its arguments.
    HRESULT beginPass(const x86::CPU& cpu, UINT pass);
    Stream* streamFor(const x86::CPU& cpu) const
    {
        return cpu.og ? bridge::deviceOf(cpu, 0)->stream() : m_device->stream();
    }
    Device& deviceFor(const x86::CPU& cpu) const
    {
        return cpu.og ? *bridge::deviceOf(cpu, 0) : *m_device;
    }

private:
    // A float parameter of this effect by handle; floats 0: none.
    struct Slot
    {
        Values*     values;
        float*      to;
        uint16_t    offset, floats;
        uint16_t    words;          // of an integer parameter of the effect, else 0
    };

    // After a pass or m_technique changed.
    void passChanged(OpenPass& ps)
    {
        ps.recorded = false;
        ps.alwaysDrawn = m_technique != 0 ? m_fast : ps.pass < MaxPasses && ((m_always >> ps.pass) & 1);
        ps.info = m_fast && m_technique == 0 && ps.pass < MaxPasses ? &s_passInfo[ps.segment != nullptr][m_kind][ps.pass] : nullptr;
    }
    void techniqueChanged()
    {
        m_passCount = technique().passCount;
        m_ps[1].stamp = 0;
        passChanged(m_ps[0]);
    }
    // A segment's pass state is set up on first use.
    OpenPass& passFor(const x86::CPU& cpu)
    {
        return __builtin_expect(cpu.og == nullptr, 1) ? m_ps[0] : segmentPass(cpu);
    }
    OpenPass& segmentPass(const x86::CPU& cpu);
public:
    // Fixes the texture parameters a segment reads to the values now.
    void snapshotTextures(ValuesShadow& shadow) const
    {
        for (uint32_t p = 0; p < ParamCount; ++p)
            if (isTexture(p) && s_kinds[m_kind].local[p] >= 0)
                shadow.addBase(m_slots[p].values->id, p, m_slots[p].values->textures[p]);
    }
private:
    // The pass's texture: a segment's own, else the block's.
    const Object* textureOf(const OpenPass& ps, const Values& v, uint32_t p) const
    {
        if (__builtin_expect(ps.segment != nullptr, 0))
            if (Object* const* t = ps.segment->shadow->textureOf(v.id, p))
                return *t;
        return v.textures[p];
    }
    void recordSlow(const OpenPass& ps, EffectDraw& effect, const render::TextureStorage* textures[render::MaxSamplers],
                    uint8_t& deviceSlots) const;
    template< uint32_t N >
    void storeSlot(const Slot& s, const void* data, uint32_t n, Stream* stream);
    __attribute__((noinline)) HRESULT setFloatSlow(x86::reg32 handle, float value, Stream* stream);
    __attribute__((noinline)) HRESULT setIntsSlow(x86::reg32 handle, const x86::sreg32* data, uint32_t count,
                                                  Stream* stream);

    const fx::EffectInfo& info() const      { return fx::s_effects[m_kind]; }
    const fx::Technique& technique() const  { return info().techniques[m_technique]; }
    // The parameter of a handle or name in this effect, or -1.
    int param(x86::reg32 handle) const;
    Values& values(int p)                   { return s_kinds[m_kind].shared[p] && m_pool ? m_pool->values : m_values; }
    const Values& values(int p) const
    {
        return s_kinds[m_kind].shared[p] && m_pool ? m_pool->values : m_values;
    }
    float* floats(int p)
    {
        Values& v = values(p);
        if (v.pendingCount)
            v.settle(p);
        return v.floats.data() + s_offsets[p];
    }
    const float* floats(int p) const        { return const_cast<Effect*>(this)->floats(p); }
    x86::sreg32 intValue(int p, uint32_t i = 0) const { x86::sreg32 v; memcpy(&v, floats(p) + i, 4); return v; }
    // Handle's parameter when its type is in mask; else -1.
    int fastParam(x86::reg32 handle, uint8_t mask) const
    {
        const uint32_t p = handle - HandleParam;
        return p < ParamCount && (s_kinds[m_kind].fast[p] & mask) ? int(p) : -1;
    }
    int fastFloat(x86::reg32 handle) const  { return fastParam(handle, FastFloat); }
    HRESULT setFloats(const char* method, x86::reg32 handle, const float* data, uint32_t count, Stream* stream);
    // Writes n floats of a parameter; a change is sent to the render thread.
    void store(int p, const void* data, uint32_t n, Stream* stream);
    template< uint32_t N >
    void storeWords(int p, const void* data, uint32_t n, Stream* stream);
    void trace(const char* method, int p, const void* data, uint32_t n) const
    {
#ifdef DEV_SETTINGS
        if (__builtin_expect(bridge::g_hot.trace, 0))
            traceSlow(method, p, data, n);
#else
        (void)method; (void)p; (void)data; (void)n;
#endif
    }
#ifdef DEV_SETTINGS
    void traceSlow(const char* method, int p, const void* data, uint32_t n) const;
#endif

    Device*         m_device;
    EffectPool*     m_pool;
    uint32_t        m_kind;
    Values          m_values;
    Slot            m_slots[ParamCount];
    bool            m_fast = false;         // d3d_fast_effects
    uint8_t         m_always = 0;           // technique 0 passes whose draws are never dropped, a bit each
    bool            m_reuseRecord = false;  // d3d_fast_layer: record() reuses its last result
    uint32_t        m_passCount = 0;        // of the current technique
    uint32_t        m_technique = 0;
    OpenPass       m_ps[2];                // game thread, segment
};

}}

namespace win32 { namespace d3d9
{

// lookup, caching the last effect until objects change.
inline Effect* effectObject(x86::reg32 guest, bool segment = false)
{
    if (__builtin_expect(segment, 0))
        return static_cast<Effect*>(lookup(guest));
    if (__builtin_expect(guest == bridge::g_hot.effectGuest, 1))
        return static_cast<Effect*>(bridge::g_hot.effect);
    Effect* e = static_cast<Effect*>(lookup(guest));
    if (e)
    {
        bridge::g_hot.effectGuest = guest;
        bridge::g_hot.effect = e;
    }
    return e;
}

}}

namespace win32
{

template<> struct Unpacker<d3d9::Effect*>
{
    static inline d3d9::Effect* unpack(WinApplication* app, x86::reg32 address)
    {
        return d3d9::effectObject(app->getMemory<x86::reg32>(address));
    }
};
D3D9_UNPACKER(d3d9::EffectPool)

}

namespace win32 { namespace d3d9
{

int Effect::param(x86::reg32 handle) const
{
    int p = -1;
    if (handle >= HandleParam && handle < HandleParam + ParamCount)
    {
        p = int(handle - HandleParam);
    }
    else if (handle >= 0x10000)
    {
        // A name instead of a handle.
        const char* name = reinterpret_cast<const char*>(g_guestBase + handle);
        for (uint32_t i = 0; i < info().paramCount; ++i)
            if (strcmp(fx::s_params[info().params[i].param].name, name) == 0)
                p = info().params[i].param;
    }
    if (p >= 0 && s_kinds[m_kind].local[p] >= 0)
        return p;
    // The game passes handle 0 for semantics an effect lacks.
    s_badHandles.fetch_add(1, std::memory_order_relaxed);
    if (handle)
        D3D_LOG_ONCE("d3dx9: %s: parameter handle 0x%x is not one of its parameters", info().resource,
                     unsigned(handle));
    return -1;
}

const uint8_t* Effect::paramTypes() const
{
    static_assert(uint32_t(HandleParam) == bridge::effect::ParamHandleBase
                  && uint32_t(ParamCount) <= bridge::effect::ParamHandleCount, "parameter handles");
    static uint8_t s_types[EffectCount][bridge::effect::ParamHandleCount];
    static bool s_built = false;
    if (!s_built)
    {
        for (uint32_t k = 0; k < EffectCount; ++k)
            for (uint32_t i = 0; i < bridge::effect::ParamHandleCount; ++i)
            {
                const bool used = i < ParamCount && s_kinds[k].local[i] >= 0;
                s_types[k][i] = !used ? uint8_t(bridge::effect::ParamNone)
                              : isFloat(i) ? uint8_t(bridge::effect::ParamFloat)
                              : isInteger(i) ? uint8_t(bridge::effect::ParamInt)
                              : isTexture(i) ? uint8_t(bridge::effect::ParamTexture)
                                             : uint8_t(bridge::effect::ParamNone);
            }
        s_built = true;
    }
    return s_types[m_kind];
}

#ifdef DEV_SETTINGS
void Effect::traceSlow(const char* method, int p, const void* data, uint32_t n) const
{
    if (!tracing())
        return;
    --s_traceLeft;
    bridge::g_hot.trace = s_traceLeft != 0;
    char text[160];
    int length = 0;
    for (uint32_t i = 0; i < n && i < 8 && length >= 0 && length < int(sizeof(text)); ++i)
    {
        float f;
        x86::sreg32 v;
        memcpy(&f, static_cast<const uint8_t*>(data) + 4 * i, 4);
        memcpy(&v, &f, 4);
        length += isInteger(uint32_t(p)) ? snprintf(text + length, sizeof(text) - length, " %d", int(v))
                                         : snprintf(text + length, sizeof(text) - length, " %g", double(f));
    }
    if (length == 0)
        text[0] = 0;
    pf::logInfo("d3dx9: %s %s %s =%s%s", info().resource, method, fx::s_params[p].name, text, n > 8 ? " ..." : "");
}
#endif

// 16 words equal, first word already compared.
inline bool sameMatrix(const void* a, const void* b)
{
#if defined(__ARM_NEON) && defined(__arm__)
    const uint8_t* p = static_cast<const uint8_t*>(a);
    const uint8_t* q = static_cast<const uint8_t*>(b);
    const uint8x16_t d0 = veorq_u8(vld1q_u8(p), vld1q_u8(q));
    const uint8x16_t d1 = veorq_u8(vld1q_u8(p + 16), vld1q_u8(q + 16));
    const uint8x16_t d2 = veorq_u8(vld1q_u8(p + 32), vld1q_u8(q + 32));
    const uint8x16_t d3 = veorq_u8(vld1q_u8(p + 48), vld1q_u8(q + 48));
    const uint32x4_t d = vreinterpretq_u32_u8(vorrq_u8(vorrq_u8(d0, d1), vorrq_u8(d2, d3)));
    const uint32x2_t h = vorr_u32(vget_low_u32(d), vget_high_u32(d));
    return (vget_lane_u32(h, 0) | vget_lane_u32(h, 1)) == 0;
#else
    return sameWords(a, b, 16);
#endif
}

// sameWords for long runs that are mostly equal.
inline bool sameRun(const void* a, const void* b, uint32_t count)
{
#if defined(__ARM_NEON) && defined(__arm__)
    const uint8_t* p = static_cast<const uint8_t*>(a);
    const uint8_t* q = static_cast<const uint8_t*>(b);
    uint8x16_t d = vdupq_n_u8(0);
    for (; count >= 4; count -= 4, p += 16, q += 16)
        d = vorrq_u8(d, veorq_u8(vld1q_u8(p), vld1q_u8(q)));
    const uint32x4_t w = vreinterpretq_u32_u8(d);
    const uint32x2_t h = vorr_u32(vget_low_u32(w), vget_high_u32(w));
    return (vget_lane_u32(h, 0) | vget_lane_u32(h, 1)) == 0 && sameWords(p, q, count);
#else
    return sameWords(a, b, count);
#endif
}

template< uint32_t N >
__attribute__((always_inline)) inline void Effect::storeSlot(const Slot& s, const void* data, uint32_t n,
                                                             Stream* stream)
{
    if (N)
        n = N;
    if (__builtin_expect(stream && stream->shadow, 0))
    {
        segmentRecord<N>(*stream, s.values->id, s.offset, data, n);
        return;
    }
    float* to = s.to;
    Values& v = *s.values;
#ifdef DEV_TOOLS
    if (__builtin_expect(probe::g_on, 0))
        probe::parentStore(v.id, s.offset, n, sameRun(to, data, n));
#endif
    const bool stale = __builtin_expect(v.unknownCount | bridge::g_pendingSegments, 0) && v.staleStore(s.offset, n);
    if (!stale && (N == 16 ? (memcmp(to, data, 4) == 0 && sameMatrix(to, data)) : N == 0 ? sameRun(to, data, n)
                                                                                         : sameWords(to, data, n)))
        return;
    memcpy(to, data, n * 4);
    if (__builtin_expect(!stream || v.dirty, 0))
    {
        v.markDirty();
        return;
    }
    uint32_t* w = stream->append(2 + n);
    w[0] = CmdValues | uint32_t(v.id) << 8;
    w[1] = s.offset | n << 16;
    memcpy(w + 2, data, n * 4);
}

template< uint32_t N >
__attribute__((always_inline)) inline bool Effect::setFast(x86::reg32 handle, const void* data, Stream* stream)
{
    const uint32_t i = handle - HandleParam;
    if (__builtin_expect(i >= ParamCount || !data, 0))
        return false;
    const Slot& s = m_slots[i];
    if (__builtin_expect(s.floats < N || bridge::g_hot.trace, 0) ||
        (__builtin_expect(s.values->pendingCount, 0) && !(stream && stream->shadow)))
        return false;
    storeSlot<N>(s, data, N, stream);
    return true;
}

HRESULT Effect::setFloats(const char* method, x86::reg32 handle, const float* data, uint32_t count, Stream* stream)
{
    int p = fastFloat(handle);
    if (__builtin_expect(p < 0 || !data, 0))
    {
        p = param(handle);
        if (p < 0 || !data || !isFloat(uint32_t(p)))
            return D3DERR_INVALIDCALL;
    }
    const uint32_t n = std::min<uint32_t>(count, fx::s_params[p].floats);
    trace(method, p, data, n);
    store(p, data, n, stream);
    return D3D_OK;
}

// N: fixed word count (copies inlined), or 0 for n.
template< uint32_t N >
inline void Effect::storeWords(int p, const void* data, uint32_t n, Stream* stream)
{
    if (N)
        n = N;
    Values& v = values(p);
    if (__builtin_expect(stream && stream->shadow, 0))
    {
        segmentRecord<N>(*stream, v.id, s_offsets[p], data, n);
        return;
    }
    if (v.pendingCount)
        v.settle(p);
    float* to = v.floats.data() + s_offsets[p];
#ifdef DEV_TOOLS
    if (__builtin_expect(probe::g_on, 0))
        probe::parentStore(v.id, s_offsets[p], n, sameWords(to, data, n));
#endif
    const bool stale = __builtin_expect(v.unknownCount | bridge::g_pendingSegments, 0) && v.staleStore(s_offsets[p], n);
    if (!stale && sameWords(to, data, n))
        return;
    memcpy(to, data, n * 4);
    if (!stream || v.dirty)
    {
        v.markDirty();
        return;
    }
    uint32_t* w = stream->append(2 + n);
    w[0] = CmdValues | uint32_t(v.id) << 8;
    w[1] = s_offsets[p] | n << 16;
    memcpy(w + 2, data, n * 4);
}

void Effect::store(int p, const void* data, uint32_t n, Stream* stream)
{
    if (n == 16)
        storeWords<16>(p, data, n, stream);
    else if (n == 4)
        storeWords<4>(p, data, n, stream);
    else
        storeWords<0>(p, data, n, stream);
}

__attribute__((always_inline)) inline HRESULT Effect::setFloat(x86::reg32 handle, float value, Stream* stream)
{
    if (__builtin_expect(setFast<1>(handle, &value, stream), 1))
        return D3D_OK;
    return setFloatSlow(handle, value, stream);
}

__attribute__((always_inline)) inline HRESULT Effect::setVector(x86::reg32 handle, const float* vector, Stream* stream)
{
    if (__builtin_expect(setFast<4>(handle, vector, stream), 1))
        return D3D_OK;
    return setFloats("SetVector", handle, vector, 4, stream);
}

__attribute__((always_inline)) inline HRESULT Effect::setMatrix(x86::reg32 handle, const float* matrix, Stream* stream)
{
    if (__builtin_expect(setFast<16>(handle, matrix, stream), 1))
        return D3D_OK;
    return setFloats("SetMatrix", handle, matrix, 16, stream);
}

__attribute__((always_inline)) inline HRESULT Effect::setVectors(x86::reg32 handle, const float* vectors, uint32_t count, Stream* stream)
{
    const uint32_t i = handle - HandleParam;
    if (__builtin_expect(i < ParamCount && vectors && count * 4 && count <= 64, 1))
    {
        const Slot& s = m_slots[i];
        if (__builtin_expect(s.floats >= count * 4 && !bridge::g_hot.trace &&
                             (!s.values->pendingCount || (stream && stream->shadow)), 1))
        {
            storeSlot<0>(s, vectors, count * 4, stream);
            return D3D_OK;
        }
    }
    return setFloats("SetVectorArray", handle, vectors, count * 4, stream);
}

__attribute__((always_inline)) inline HRESULT Effect::setInts(x86::reg32 handle, const x86::sreg32* data,
                                                              uint32_t count, Stream* stream)
{
    const uint32_t i = handle - HandleParam;
    if (__builtin_expect(i < ParamCount && data && count, 1))
    {
        const Slot& s = m_slots[i];
        if (__builtin_expect(s.words && !bridge::g_hot.trace &&
                             (!s.values->pendingCount || (stream && stream->shadow)), 1))
        {
            storeSlot<0>(s, data, count < s.words ? count : s.words, stream);
            return D3D_OK;
        }
    }
    return setIntsSlow(handle, data, count, stream);
}

HRESULT Effect::setProduct(x86::reg32 handle, const float* const* matrices, uint32_t count, uint32_t fpscr,
                           Stream* stream)
{
    float in[48];
    for (uint32_t i = 0; i < count; ++i)
        memcpy(in + 16 * i, matrices[i], 64);
    const int p = param(handle);
    if (p < 0 || !isFloat(uint32_t(p)) || fx::s_params[p].floats < 16 || !stream || stream->shadow || values(p).dirty)
    {
        float product[16];
        matrixProduct(in, count, fpscr, product);
        return setFloats("SetMatrix", handle, product, 16, stream);
    }
    Values& v = values(p);
#ifdef DEV_TOOLS
    if (__builtin_expect(probe::g_on, 0))
        probe::parentStore(v.id, s_offsets[p], 16, false);
#endif
    Pending* e = v.findPending(p);
    const uint32_t serial = effectRecordSerial(stream);
    const bool stale = __builtin_expect(v.unknownCount | bridge::g_pendingSegments, 0) && v.staleStore(s_offsets[p], 16);
    if (!stale && e && e->serial == serial && e->count == count && e->fpscr == fpscr && sameWords(e->matrices, in, 16 * count))
        return D3D_OK;
    if (!e)
    {
        if (v.pendingCount == Values::PendingSlots)
            v.settle(v.pending[0].param);
        e = &v.pending[v.pendingCount++];
        e->param = p;
    }
    e->count = count;
    e->fpscr = fpscr;
    e->serial = serial;
    memcpy(e->matrices, in, 64 * count);
    uint32_t* w = stream->append(3 + 16 * count);
    w[0] = CmdProduct | uint32_t(v.id) << 8;
    w[1] = s_offsets[p] | count << 16;
    w[2] = fpscr;
    memcpy(w + 3, in, 64 * count);
    return D3D_OK;
}

bool OpenPass::drawn() const
{
    return effect->drawnFor(*this);
}

void OpenPass::record(EffectDraw& draw, const render::TextureStorage* textures[render::MaxSamplers],
                       uint8_t& deviceSlots) const
{
    effect->recordFor(*this, draw, textures, deviceSlots);
}

// False: the pass writes only alpha and has no programs.
bool Effect::passDrawn(uint32_t index) const
{
    const fx::Pass* pass = m_technique == 0 ? passOf(m_kind, index) : nullptr;
    return !pass || !alphaOnly(*pass) || passPrograms(m_kind, index);
}

bool Effect::drawnFor(const OpenPass& ps) const
{
    if (passDrawn(ps.pass))
        return true;
    if (ps.pass < MaxPasses)
    {
        static bool s_logged[EffectCount][MaxPasses];
        s_dropped[m_kind][ps.pass].fetch_add(1, std::memory_order_relaxed);
        if (!s_logged[m_kind][ps.pass])
        {
            s_logged[m_kind][ps.pass] = true;
            pf::logInfo("d3dx9: %s pass %u writes only alpha: its draws are dropped", info().resource, ps.pass);
        }
    }
    return false;
}

void Effect::recordFor(const OpenPass& ps, EffectDraw& effect,
                       const render::TextureStorage* textures[render::MaxSamplers], uint8_t& deviceSlots) const
{
    if (__builtin_expect(!ps.info, 0))
    {
        recordSlow(ps, effect, textures, deviceSlots);
        return;
    }
#ifdef DEV_TOOLS
    if (__builtin_expect(probe::g_on && !ps.segment, 0))
        probe::parentDraw(m_kind, ps.pass, m_values.id, m_pool ? m_pool->values.id : 0xffffu,
                          passInfo(*ps.info, m_kind, ps.pass).texture, render::MaxSamplers);
#endif
    const uint32_t writes = ps.segment ? ps.segment->shadow->textureWrites : 0;
    if (ps.recorded && ps.recordEpoch == bridge::g_hot.textureEpoch && ps.recordWrites == writes)
    {
        effect = ps.recordEffect;
        deviceSlots = ps.recordSlots;
        for (uint32_t i = 0; i < render::MaxSamplers; ++i)
            textures[i] = ps.recordTextures[i];
        return;
    }
    const PassInfo& info = passInfo(*ps.info, m_kind, ps.pass);
    effect.own = m_values.id;
    effect.shared = m_pool ? m_pool->values.id : uint16_t(NoBlock);
    effect.kind = uint8_t(m_kind);
    effect.pass = uint8_t(ps.pass);
    effect.pad[0] = effect.pad[1] = 0;
    deviceSlots = info.deviceSlots;
    for (uint32_t i = 0; i < render::MaxSamplers; ++i)
    {
        const uint32_t p = info.texture[i];
        const Object* t = p != fx::NoParam ? textureOf(ps, *m_slots[p].values, p) : nullptr;
        textures[i] = t && (t->kind() == KindTexture || t->kind() == KindCubeTexture)
                    ? static_cast<const Texture*>(t)->storage() : nullptr;
    }
    if (m_reuseRecord)
    {
        ps.recorded = true;
        ps.recordEpoch = bridge::g_hot.textureEpoch;
        ps.recordWrites = writes;
        ps.recordEffect = effect;
        ps.recordSlots = deviceSlots;
        for (uint32_t i = 0; i < render::MaxSamplers; ++i)
            ps.recordTextures[i] = textures[i];
    }
}

void Effect::recordSlow(const OpenPass& ps, EffectDraw& effect,
                        const render::TextureStorage* textures[render::MaxSamplers], uint8_t& deviceSlots) const
{
    effect.own = m_values.id;
    effect.shared = m_pool ? m_pool->values.id : uint16_t(NoBlock);
    effect.kind = uint8_t(m_kind);
    effect.pass = uint8_t(ps.pass);
    effect.pad[0] = effect.pad[1] = 0;
    deviceSlots = 0;
    for (uint32_t i = 0; i < render::MaxSamplers; ++i)
        textures[i] = nullptr;
    const fx::Pass* pass = m_technique == 0 ? passOf(m_kind, ps.pass) : nullptr;
    const PassPrograms* programs = pass ? passPrograms(m_kind, ps.pass) : nullptr;
    if (!programs)
        return;
    if (programs->fixed)
    {
        deviceSlots = 1;
        return;
    }
    const uint32_t count = render::fragmentProgram(render::FragmentProgram(programs->fragment)).samplers;
    for (uint32_t i = 0; i < count && i < render::MaxSamplers; ++i)
    {
        const fx::Sampler* s = samplerAt(*pass, programs->stages[i]);
        if (!s || s->texture == fx::NoParam)
        {
            deviceSlots = uint8_t(deviceSlots | 1u << i);
            continue;
        }
        const Object* t = textureOf(ps, values(s->texture), s->texture);
        textures[i] = t && (t->kind() == KindTexture || t->kind() == KindCubeTexture)
                    ? static_cast<const Texture*>(t)->storage() : nullptr;
    }
}

OpenPass& Effect::segmentPass(const x86::CPU& cpu)
{
    Stream* stream = bridge::deviceOf(cpu, 0)->stream();
    OpenPass& ps = m_ps[1];
    if (ps.stamp != stream->shadow->serial)
    {
        ps.stamp = stream->shadow->serial;
        ps.segment = stream;
        ps.pass = m_ps[0].pass;
        ps.textureStages = 0;
        ps.saved = false;
        passChanged(ps);
    }
    return ps;
}

void syncEffectValues(Stream& stream)
{
    if (s_dirty.empty())
        return;
    for (uint16_t id : s_dirty)
    {
        Values* v = id < s_blocks.size() ? s_blocks[id] : nullptr;
        if (!v || !v->dirty)
            continue;
        v->settleAll();
#ifdef DEV_TOOLS
        if (__builtin_expect(probe::g_on, 0))
        {
            probe::count(probe::SyncBlocks);
            probe::count(probe::SyncWords, 2 + s_totalFloats);
        }
#endif
        uint32_t* w = stream.append(2 + s_totalFloats);
        w[0] = CmdValues | uint32_t(id) << 8;
        w[1] = s_totalFloats << 16;
        memcpy(w + 2, v->floats.data(), s_totalFloats * 4);
        v->dirty = false;
        if (v->unknownCount)
            v->clearUnknown();
    }
    s_dirty.clear();
}

void markEffectValuesDirty()
{
    for (Values* v : s_blocks)
        if (v)
            v->markDirty();
}

void forgetSegmentEffects(ValuesShadow& shadow)
{
    for (const ValuesShadow::Range& r : shadow.touched)
        if (Values* v = r.block < s_blocks.size() ? s_blocks[r.block] : nullptr)
            v->markUnknown(r.offset, r.count);
}

void mergeSegmentEffects(ValuesShadow& shadow)
{
    for (const ValuesShadow::Range& r : shadow.touched)
    {
        Values* v = r.block < s_blocks.size() ? s_blocks[r.block] : nullptr;
        if (!v)
            continue;
        for (uint32_t i = r.offset; i < uint32_t(r.offset) + r.count; ++i)
        {
            if (v->pendingCount)
                v->settle(s_paramAt[i]);
            const uint32_t bits = shadow.bitsAt(r.block, i);
#ifdef DEV_TOOLS
            if (__builtin_expect(probe::g_on, 0))
                probe::merged(r.block, i, memcmp(&v->floats[i], &bits, 4) != 0, s_paramAt[i]);
#endif
            memcpy(&v->floats[i], &bits, 4);
        }
    }
    for (const ValuesShadow::TextureSet& t : shadow.textures)
    {
        Values* v = t.block < s_blocks.size() ? s_blocks[t.block] : nullptr;
        if (!v)
            continue;
        Object*& slot = v->textures[t.param];
#ifdef DEV_TOOLS
        if (__builtin_expect(probe::g_on, 0))
            probe::mergedTexture(t.block, t.param, slot != t.texture);
#endif
        if (slot == t.texture)
            continue;
        if (t.texture)
            t.texture->bind();
        if (slot)
            slot->unbind();
        slot = t.texture;
        ++bridge::g_hot.textureEpoch;
    }
}

#ifdef DEV_TOOLS
namespace
{
const struct ProbeNames
{
    ProbeNames()
    {
        probe::g_paramName = [](uint32_t p) { return p < ParamCount ? fx::s_params[p].name : "?"; };
        probe::g_effectName = [](uint32_t k) { return k < EffectCount ? fx::s_effects[k].resource : "?"; };
    }
} s_probeNames;
}

const char* effectMergeTest()
{
    initTables();
    s_dirty.clear();
    ValuesShadow shadow;
    const char* failed = nullptr;
    {
        Values a, b;
        a.dirty = b.dirty = false;
        s_dirty.clear();
        Object* texture = new Object(KindTexture);
        const float x[4] = { 1, 2, 3, 4 }, y[4] = { 5, 6, 7, 8 };
        shadow.begin(77);
        shadow.store(a.id, 0, x, 4);
        shadow.store(a.id, 4, y, 4);
        shadow.store(b.id, 0, x, 4);
        shadow.store(a.id, 0, y, 2);
        shadow.setTexture(a.id, 3, texture);
        shadow.setTexture(b.id, 3, texture);
        mergeSegmentEffects(shadow);
        if (a.floats[0] != 5 || a.floats[1] != 6 || a.floats[2] != 3 || a.floats[3] != 4 || a.floats[4] != 5 ||
            a.floats[5] != 6 || a.floats[6] != 7 || a.floats[7] != 8 || b.floats[3] != 4)
            failed = "a merge takes the segment's floats";
        else if (a.textures[3] != texture || b.textures[3] != texture)
            failed = "a merge takes the segment's textures";
        forgetSegmentEffects(shadow);
        if (a.unknownCount != 8 || b.unknownCount != 4)
            failed = "a segment's floats are unknown on the parent";
        else if (!a.takeUnknown(4, 2) || a.takeUnknown(4, 2) || a.unknownCount != 6)
            failed = "a store takes the unknown floats it covers";
        a.dirty = b.dirty = false;
        s_dirty.clear();
        texture->release();
    }
    return failed;
}
#endif

int effectSamplerStage(const EffectDraw& effect, uint32_t slot)
{
    const PassPrograms* programs = effect.kind < EffectCount ? passPrograms(effect.kind, effect.pass) : nullptr;
    if (programs && programs->fixed)
        return slot == 0 ? 0 : -1;
    if (!programs || slot >= render::MaxSamplers
        || slot >= render::fragmentProgram(render::FragmentProgram(programs->fragment)).samplers)
        return -1;
    return programs->stages[slot];
}

const char* effectName(const EffectDraw& effect)
{
    return effect.kind < EffectCount ? fx::s_effects[effect.kind].resource : "fixed function";
}

const render::TextureStorage* effectBlackCube()
{
    return s_blackCube;
}

uint32_t effectBlockFloats()
{
    initTables();
    return s_totalFloats;
}

bool effectBindingReads(uint32_t offset, uint32_t count)
{
    if (offset >= s_paramAt.size())
        return true;
    const uint32_t p = s_paramAt[offset];
    return s_bound[p] || offset + count > s_offsets[p] + fx::s_params[p].floats;
}

namespace
{

void bind(const DrawRecord& record, const FixedState& device, const float* own, const float* shared, EffectBinding& b)
{
    memset(&b, 0, sizeof(b));
    const EffectDraw& effect = record.effect;
    for (uint32_t s = 0; s < PassStateCount; ++s)
        b.states[s] = device.renderStates[g_passRenderState[s]];
    const fx::Pass* pass = effect.kind < EffectCount ? passOf(effect.kind, effect.pass) : nullptr;
    const PassPrograms* programs = pass ? passPrograms(effect.kind, effect.pass) : nullptr;
    if (!programs)
    {
        static bool s_logged[EffectCount][MaxPasses];
        if (effect.kind < EffectCount && effect.pass < MaxPasses && !s_logged[effect.kind][effect.pass])
        {
            s_logged[effect.kind][effect.pass] = true;
            pf::logInfo("d3dx9: %s technique 0 pass %u has no programs: its draws are dropped", effectName(effect),
                        unsigned(effect.pass));
        }
        b.skip = true;
        return;
    }
    const View v{ effect.kind, own, shared };
    for (uint32_t s = 0; s < PassStateCount; ++s)
    {
        if (pass->states[s].source != fx::SourceNone && !((record.deviceStates >> s) & 1))
            b.states[s] = v.value(pass->states[s]);
    }
    if (programs->fixed)
    {
        b.fixed = true;
        return;
    }
    b.vertexProgram = programs->vertex;
    b.fragmentProgram = programs->fragment;
    b.cubeSlots = programs->cubeSlots;
    // A pass without TEXTURETRANSFORMFLAGS[0] keeps the device's PROJECTED.
    const DWORD ttf = (pass->ttfStages & 1) ? pass->ttf0 : device.stageStates[0][D3DTSS_TEXTURETRANSFORMFLAGS];
    if ((ttf & D3DTTFF_PROJECTED) && record.texComponents == 4)
    {
        if (b.vertexProgram == render::VpWorld)
        {
            D3D_LOG_ONCE("d3dx9: %s pass %u: projected texture coordinates (stage 0 PROJECTED)", effectName(effect),
                         unsigned(effect.pass));
            b.vertexProgram = render::VpWorldProjected;
            b.fragmentProgram = render::FpWorldProjected;
        }
        else
        {
            D3D_LOG_ONCE("d3dx9: %s pass %u: projected texture coordinates not emulated", effectName(effect),
                         unsigned(effect.pass));
        }
    }
    b.samplerCount = render::fragmentProgram(render::FragmentProgram(b.fragmentProgram)).samplers;
    for (uint32_t i = 0; i < b.samplerCount && i < render::MaxSamplers; ++i)
    {
        const int stage = programs->stages[i];
        DWORD states[MaxSamplerStates];
        memcpy(states, device.samplerStates[stage], sizeof(states));
        if (const fx::Sampler* s = samplerAt(*pass, stage))
        {
            const uint32_t written = record.deviceSamplers >> (stage * 8);
            for (uint32_t f = 0; f < fx::FieldCount; ++f)
                if (s->fields[f].source != fx::SourceNone && !(written & s_fieldBit[f]))
                    states[s_fieldState[f]] = v.value(s->fields[f]);
        }
        b.samplers[i] = samplerOf(states);
    }
}

}

const EffectBinding& effectBinding(const DrawRecord& record, const FixedState& device, const float* own,
                                   const float* shared, uint32_t version)
{
    const EffectDraw& effect = record.effect;
    const BindingKey key = { effect, record.deviceStates, record.deviceSamplers, record.texComponents, version };
    if (!sameWords(&key, &s_bindingKey, sizeof(key) / 4))
    {
        bind(record, device, own, shared, s_binding);
        s_bindingKey = key;
        ++s_bindingsMade;
    }
    ++s_bindingCalls;
    if (!s_binding.skip && effect.pass < MaxPasses)
        ++s_draws[effect.kind][effect.pass];
    return s_binding;
}

namespace
{

void copyFloats(float* to, const View& v, uint32_t p, uint32_t n)
{
    memcpy(to, v.floats(p), n * sizeof(float));
}

// ps_1_1 constant registers hold [-1, 1].
void copyClamped(float* to, const View& v, uint32_t p, uint32_t n)
{
    const float* from = v.floats(p);
    for (uint32_t i = 0; i < n; ++i)
        to[i] = from[i] < -1.0f ? -1.0f : from[i] > 1.0f ? 1.0f : from[i];
}

// effect_check: logs the first draw of each pass.
void checkDraw(const DrawRecord& record, const EffectBinding& b, const float* vc, const float* fc)
{
    static const bool s_check = NFSU_DEV_BOOL("effect_check", false);
    static bool s_checked[EffectCount][MaxPasses];
    const EffectDraw& e = record.effect;
    if (!s_check || e.kind >= EffectCount || e.pass >= MaxPasses || s_checked[e.kind][e.pass])
        return;
    s_checked[e.kind][e.pass] = true;
    char textures[160];
    int length = 0;
    for (uint32_t i = 0; i < b.samplerCount && length >= 0 && length < int(sizeof(textures)); ++i)
    {
        const render::TextureStorage* t = record.textures[i];
        length += t ? snprintf(textures + length, sizeof(textures) - length, " %u:%ux%u%s", i,
                               unsigned(t->desc.width), unsigned(t->desc.height), t->desc.cube ? " cube" : "")
                    : snprintf(textures + length, sizeof(textures) - length, " %u:none", i);
    }
    if (length <= 0)
        textures[0] = 0;
    const render::VertexProgramInfo& vp = render::vertexProgram(render::VertexProgram(b.vertexProgram));
    const render::FragmentProgramInfo& fp = render::fragmentProgram(render::FragmentProgram(b.fragmentProgram));
    pf::logInfo("d3dx9: check %s pass %u: %s/%s, wvp row 3 (%g %g %g %g), cull %u, z %u/%u func %u, colour mask "
                "0x%x, blend %u (%u, %u), alpha test %u ref %u, fog %u colour 0x%08x, textures%s",
                effectName(e), unsigned(e.pass), vp.name, fp.name, vc ? double(vc[12]) : 0.0, vc ? double(vc[13]) : 0.0,
                vc ? double(vc[14]) : 0.0, vc ? double(vc[15]) : 0.0, unsigned(b.states[PassCull]),
                unsigned(b.states[PassZEnable]), unsigned(b.states[PassZWrite]), unsigned(b.states[PassZFunc]),
                unsigned(b.states[PassColorWrite]), unsigned(b.states[PassBlend]), unsigned(b.states[PassSrcBlend]),
                unsigned(b.states[PassDestBlend]), unsigned(b.states[PassAlphaTest]), unsigned(b.states[PassAlphaRef]),
                unsigned(b.states[PassFog]), unsigned(b.states[PassFogColor]), textures);
    if (vc && (b.vertexProgram == render::VpWorld || b.vertexProgram == render::VpWorldProjected))
    {
        const render::WorldVertexConstants& c = *reinterpret_cast<const render::WorldVertexConstants*>(vc);
        pf::logInfo("d3dx9: check %s pass %u: viewZ (%g %g %g %g), fogValue (%g %g %g %g)", effectName(e),
                    unsigned(e.pass), double(c.viewZ[0]), double(c.viewZ[1]), double(c.viewZ[2]), double(c.viewZ[3]),
                    double(c.fogValue[0]), double(c.fogValue[1]), double(c.fogValue[2]), double(c.fogValue[3]));
    }
    RC_USE(fc);
}

}

namespace
{

// View z and fog terms; (0, 0, 1, 0) without fog.
void fogConstants(const View& v, const EffectBinding& b, float viewZ[4], float fogValue[4])
{
    const float* wv = v.floats(fx::PWorldView);
    viewZ[0] = wv[2];
    viewZ[1] = wv[6];
    viewZ[2] = wv[10];
    viewZ[3] = wv[14];
    if (b.states[PassFog])
    {
        copyFloats(fogValue, v, fx::PFogValue, 4);
        return;
    }
    fogValue[0] = fogValue[1] = fogValue[3] = 0.0f;
    fogValue[2] = 1.0f;
}

}

void effectVertexConstants(const DrawRecord& record, const FixedState& device, const float* own,
                           const float* shared, const EffectBinding& b, float* out)
{
    const View v{ record.effect.kind, own, shared };
    switch (b.vertexProgram)
    {
    case render::VpWorldReflect:
        copyFloats(reinterpret_cast<render::WorldReflectVertexConstants*>(out)->wvp, v, fx::PWorldViewProj, 16);
        break;
    case render::VpWorldReflectFog:
    {
        render::WorldReflectFogVertexConstants& c = *reinterpret_cast<render::WorldReflectFogVertexConstants*>(out);
        copyFloats(c.wvp, v, fx::PWorldViewProj, 16);
        fogConstants(v, b, c.viewZ, c.fogValue);
        break;
    }
    case render::VpWorldHeadlight:
    {
        render::WorldHeadlightVertexConstants& c = *reinterpret_cast<render::WorldHeadlightVertexConstants*>(out);
        copyFloats(c.wvp, v, fx::PWorldViewProj, 16);
        copyFloats(c.headlightWvp, v, fx::PHeadlightWVP, 16);
        copyFloats(c.textureOffset, v, fx::PTextureOffset, 4);
        break;
    }
    case render::VpWorldHeadlightFog:
    {
        render::WorldHeadlightFogVertexConstants& c =
            *reinterpret_cast<render::WorldHeadlightFogVertexConstants*>(out);
        copyFloats(c.wvp, v, fx::PWorldViewProj, 16);
        copyFloats(c.headlightWvp, v, fx::PHeadlightWVP, 16);
        copyFloats(c.textureOffset, v, fx::PTextureOffset, 4);
        fogConstants(v, b, c.viewZ, c.fogValue);
        break;
    }
    case render::VpWorldHeadlightReflect:
    {
        render::WorldHeadlightReflectVertexConstants& c =
            *reinterpret_cast<render::WorldHeadlightReflectVertexConstants*>(out);
        copyFloats(c.wvp, v, fx::PWorldViewProj, 16);
        copyFloats(c.headlightWvp, v, fx::PHeadlightWVP, 16);
        break;
    }
    case render::VpWorldHeadlightReflectFog:
    {
        render::WorldHeadlightReflectFogVertexConstants& c =
            *reinterpret_cast<render::WorldHeadlightReflectFogVertexConstants*>(out);
        copyFloats(c.wvp, v, fx::PWorldViewProj, 16);
        copyFloats(c.headlightWvp, v, fx::PHeadlightWVP, 16);
        fogConstants(v, b, c.viewZ, c.fogValue);
        break;
    }
    case render::VpWorldBone:
    {
        render::WorldBoneVertexConstants& c = *reinterpret_cast<render::WorldBoneVertexConstants*>(out);
        copyFloats(c.wvp, v, fx::PWorldViewProj, 16);
        copyFloats(c.localEyePos, v, fx::PLocalEyePos, 4);
        copyFloats(c.harmonic, v, fx::PHarmonicCoeff, 40);
        copyFloats(c.diffuseMin, v, fx::PDiffuseMin, 4);
        copyFloats(c.diffuseRange, v, fx::PDiffuseRange, 4);
        copyFloats(c.bones, v, fx::PBlendMatrices, 256);
        float fog[4];
        fogConstants(v, b, c.viewZ, fog);
        c.focalRange[0] = v.floats(fx::PFocalRange)[0];
        c.focalRange[1] = c.focalRange[2] = c.focalRange[3] = 0.0f;
        break;
    }
    case render::VpWorld:
    case render::VpWorldProjected:
    {
        render::WorldVertexConstants& c = *reinterpret_cast<render::WorldVertexConstants*>(out);
        copyFloats(c.wvp, v, fx::PWorldViewProj, 16);
        copyFloats(c.textureOffset, v, fx::PTextureOffset, 4);
        // vs_1_1 c6: column 2 of WorldView (registers hold columns).
        const float* wv = v.floats(fx::PWorldView);
        c.viewZ[0] = wv[2];
        c.viewZ[1] = wv[6];
        c.viewZ[2] = wv[10];
        c.viewZ[3] = wv[14];
        if (b.states[PassFog])
        {
            copyFloats(c.fogValue, v, fx::PFogValue, 4);
            const DWORD table = device.renderStates[35], vertex = device.renderStates[140];
            D3D_LOG_ONCE("d3dx9: fog: %s FogValue (%g, %g, %g) FogColor 0x%08x FOGTABLEMODE %u FOGVERTEXMODE %u",
                         effectName(record.effect), double(c.fogValue[0]), double(c.fogValue[1]),
                         double(c.fogValue[2]), unsigned(b.states[PassFogColor]), unsigned(table), unsigned(vertex));
            if (table)
                D3D_LOG_ONCE("d3dx9: pixel fog (FOGTABLEMODE %u) not emulated: vertex fog drawn", unsigned(table));
        }
        else
        {
            c.fogValue[0] = c.fogValue[1] = c.fogValue[3] = 0.0f;
            c.fogValue[2] = 1.0f;
        }
        break;
    }
    case render::VpCar:
    {
        render::CarVertexConstants& c = *reinterpret_cast<render::CarVertexConstants*>(out);
        copyFloats(c.wvp, v, fx::PWorldViewProj, 16);
        copyFloats(c.worldView, v, fx::PWorldView, 16);
        copyFloats(c.localEyePos, v, fx::PLocalEyePos, 4);
        copyFloats(c.harmonic, v, fx::PHarmonicCoeff, 40);
        copyFloats(c.diffuseMin, v, fx::PDiffuseMin, 4);
        copyFloats(c.diffuseRange, v, fx::PDiffuseRange, 4);
        copyFloats(c.specularMin, v, fx::PSpecularMin, 4);
        copyFloats(c.specularRange, v, fx::PSpecularRange, 4);
        c.powers[0] = v.floats(fx::PSpecularPower)[0];
        c.powers[1] = v.floats(fx::PEnvmapPower)[0];
        c.powers[2] = c.powers[3] = 0.0f;
        break;
    }
    default:
        break;
    }
    checkDraw(record, b, out, nullptr);
}

void effectFragmentConstants(const DrawRecord& record, const float* own, const float* shared,
                             const EffectBinding& b, float* out)
{
    const View v{ record.effect.kind, own, shared };
    switch (b.fragmentProgram)
    {
    case render::FpWorld:
    case render::FpWorldProjected:
    {
        render::WorldFragmentConstants& c = *reinterpret_cast<render::WorldFragmentConstants*>(out);
        color(b.states[PassFogColor], c.fogColor);
        break;
    }
    case render::FpSkyMorph:
    {
        render::SkyMorphFragmentConstants& c = *reinterpret_cast<render::SkyMorphFragmentConstants*>(out);
        color(b.states[PassFogColor], c.fogColor);
        copyClamped(c.morph, v, fx::PSkyMorphAlpha, 1);
        c.morph[1] = c.morph[2] = c.morph[3] = 0.0f;
        break;
    }
    case render::FpWorldReflect:
    case render::FpWorldReflectFog:
    case render::FpWorldBone:
        color(b.states[PassFogColor], reinterpret_cast<render::WorldFragmentConstants*>(out)->fogColor);
        break;
    case render::FpWorldHeadlight:
    case render::FpWorldHeadlightReflect:
        copyClamped(reinterpret_cast<render::WorldHeadlightFragmentConstants*>(out)->headlightColor, v,
                    fx::PHeadlightColor, 4);
        break;
    case render::FpWorldHeadlightFog:
    case render::FpWorldHeadlightReflectFog:
    {
        render::WorldHeadlightFogFragmentConstants& c =
            *reinterpret_cast<render::WorldHeadlightFogFragmentConstants*>(out);
        copyClamped(c.headlightColor, v, fx::PHeadlightColor, 4);
        color(b.states[PassFogColor], c.fogColor);
        break;
    }
    case render::FpFilter1:
    case render::FpFilter2:
    case render::FpFilter3:
    case render::FpFilter4:
        copyClamped(reinterpret_cast<render::FilterFragmentConstants*>(out)->weights, v, fx::PFilterWeights, 16);
        break;
    case render::FpOverBright:
    {
        render::ScreenFragmentConstants& c = *reinterpret_cast<render::ScreenFragmentConstants*>(out);
        memset(&c, 0, sizeof(c));
        copyClamped(c.param0, v, fx::POverBrightOffset, 1);
        copyClamped(c.param1, v, fx::POverBrightGreyScale, 4);
        break;
    }
    case render::FpScreenTint:
    case render::FpScreenRamp:
    {
        render::ScreenFragmentConstants& c = *reinterpret_cast<render::ScreenFragmentConstants*>(out);
        memset(&c, 0, sizeof(c));
        copyClamped(c.param0, v, fx::PSaturationScale, 1);
        copyClamped(c.param1, v, fx::PTintColor, 4);
        break;
    }
    case render::FpRainDrop:
    {
        render::ScreenFragmentConstants& c = *reinterpret_cast<render::ScreenFragmentConstants*>(out);
        memset(&c, 0, sizeof(c));
        copyFloats(c.param0, v, fx::PRaindropoffset, 4);
        copyFloats(c.param1, v, fx::PRaindropalpha, 1);
        break;
    }
    case render::FpCar:
    {
        render::CarFragmentConstants& c = *reinterpret_cast<render::CarFragmentConstants*>(out);
        copyClamped(c.envmapMin, v, fx::PEnvmapMin, 4);
        copyClamped(c.envmapRange, v, fx::PEnvmapRange, 4);
        break;
    }
    default:
        break;
    }
}

bool effectVertexAlpha(const DrawRecord& record, const float* own, const float* shared, const EffectBinding& b,
                       float* lowest)
{
    if (b.vertexProgram != render::VpCar)
        return false;
    const View v{ record.effect.kind, own, shared };
    const float min = v.floats(fx::PDiffuseMin)[3], range = v.floats(fx::PDiffuseRange)[3];
    // car_v: alpha = saturate(0.5 * (min + max(0.01, v.n) * range)).
    if (!(range >= 0.0f) || !(min == min))
        return false;
    const float a = 0.5f * (min + 0.01f * range);
    *lowest = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
    return true;
}

void effectFrameStats()
{
    if (!statsEnabled() || ++s_statFrames < 300)
        return;
    char line[1024];
    int length = snprintf(line, sizeof(line), "d3dx9: effect draws per frame:");
    for (uint32_t k = 0; k < EffectCount; ++k)
        for (uint32_t p = 0; p < MaxPasses; ++p)
        {
            const uint32_t drawn = s_draws[k][p], dropped = s_dropped[k][p].exchange(0, std::memory_order_relaxed);
            s_draws[k][p] = 0;
            if ((drawn || dropped) && length > 0 && length < int(sizeof(line)))
                length += snprintf(line + length, sizeof(line) - length, " %s.%u %.1f%s", fx::s_effects[k].resource,
                                   p, double(drawn ? drawn : dropped) / s_statFrames, drawn ? "" : " (dropped)");
        }
    if (length > 0 && length < int(sizeof(line)))
        snprintf(line + length, sizeof(line) - length, ", bindings made %.1f (%.0f%% reused), bad parameter handles "
                 "%.1f", double(s_bindingsMade) / s_statFrames,
                 s_bindingCalls ? 100.0 * double(s_bindingCalls - s_bindingsMade) / s_bindingCalls : 0.0,
                 double(s_badHandles.exchange(0, std::memory_order_relaxed)) / s_statFrames);
    s_bindingCalls = s_bindingsMade = 0;
    pf::logInfo("%s", line);
    s_statFrames = 0;
}

HRESULT Effect::GetDesc(WinApplication* app, x86::CPU& cpu, DWORD* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!desc)
        return D3DERR_INVALIDCALL;
    desc[0] = 0;                    // Creator
    desc[1] = info().paramCount;
    desc[2] = info().techniqueCount;
    desc[3] = 0;                    // Functions
    return D3D_OK;
}

HRESULT Effect::GetParameterDesc(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, DWORD* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !desc)
        return D3DERR_INVALIDCALL;
    const fx::ParamInfo& pi = fx::s_params[p];
    const fx::EffectParam& ep = info().params[s_kinds[m_kind].local[p]];
    // D3DXPARAMETER_DESC (11 dwords).
    desc[0] = guestString(pi.name);
    desc[1] = ep.semantic ? guestString(ep.semantic) : 0;
    desc[2] = pi.cls;
    desc[3] = pi.type;
    desc[4] = pi.rows;
    desc[5] = pi.columns;
    desc[6] = pi.elements;
    desc[7] = 0;                    // Annotations
    desc[8] = 0;                    // StructMembers
    desc[9] = ep.shared ? 1 : 0;    // D3DX_PARAMETER_SHARED
    desc[10] = isNumeric(uint32_t(p)) ? pi.floats * 4u : 4u;
    return D3D_OK;
}

x86::reg32 Effect::GetParameter(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, UINT index)
{
    RC_USE(app);
    RC_USE(cpu);
    if (parent)
    {
        D3D_LOG_ONCE("d3dx9: GetParameter of a structure parameter 0x%x is not supported", unsigned(parent));
        return 0;
    }
    return index < info().paramCount ? HandleParam + info().params[index].param : 0;
}

x86::reg32 Effect::GetParameterByName(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, const char* name)
{
    RC_USE(app);
    RC_USE(cpu);
    if (parent || !name)
        return 0;
    for (uint32_t i = 0; i < info().paramCount; ++i)
        if (strcmp(fx::s_params[info().params[i].param].name, name) == 0)
            return HandleParam + info().params[i].param;
    return 0;
}

x86::reg32 Effect::GetParameterBySemantic(WinApplication* app, x86::CPU& cpu, x86::reg32 parent,
                                          const char* semantic)
{
    RC_USE(app);
    RC_USE(cpu);
    if (parent || !semantic)
        return 0;
    for (uint32_t i = 0; i < info().paramCount; ++i)
        if (info().params[i].semantic && strcasecmp(info().params[i].semantic, semantic) == 0)
            return HandleParam + info().params[i].param;
    return 0;
}

x86::reg32 Effect::GetTechnique(WinApplication* app, x86::CPU& cpu, UINT index)
{
    RC_USE(app);
    RC_USE(cpu);
    return index < info().techniqueCount ? HandleTechnique + index : 0;
}

x86::reg32 Effect::GetTechniqueByName(WinApplication* app, x86::CPU& cpu, const char* name)
{
    RC_USE(app);
    RC_USE(cpu);
    for (uint32_t t = 0; name && t < info().techniqueCount; ++t)
        if (strcmp(info().techniques[t].name, name) == 0)
            return HandleTechnique + t;
    return 0;
}

x86::reg32 Effect::GetPass(WinApplication* app, x86::CPU& cpu, x86::reg32 technique, UINT index)
{
    RC_USE(app);
    RC_USE(cpu);
    const uint32_t t = technique - HandleTechnique;
    if (t >= info().techniqueCount || index >= info().techniques[t].passCount || index >= 16)
        return 0;
    return HandlePass + t * 16 + index;
}

x86::reg32 Effect::GetAnnotationByName(WinApplication* app, x86::CPU& cpu, x86::reg32 object, const char* name)
{
    RC_USE(app);
    RC_USE(cpu);
    const uint32_t t = object - HandleTechnique;
    if (t >= info().techniqueCount || !name)
        return 0;
    const fx::Technique& technique = info().techniques[t];
    for (uint32_t a = 0; a < technique.annotationCount && a < 16; ++a)
        if (strcmp(technique.annotations[a].name, name) == 0)
            return HandleAnnotation + t * 16 + a;
    return 0;
}

HRESULT Effect::SetValue(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const void* data, UINT bytes)
{
    RC_USE(app);
    int p = fastParam(handle, FastFloat | FastInt);
    if (__builtin_expect(p < 0 || !data, 0))
    {
        p = param(handle);
        if (p < 0 || !data || !isNumeric(uint32_t(p)))
            return D3DERR_INVALIDCALL;
    }
    const uint32_t n = std::min<uint32_t>(bytes / 4, fx::s_params[p].floats);
    trace("SetValue", p, data, n);
    store(p, data, n, streamFor(cpu));
    return D3D_OK;
}

HRESULT Effect::GetValue(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, void* data, UINT bytes)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !data || !isNumeric(uint32_t(p)))
        return D3DERR_INVALIDCALL;
#ifdef DEV_TOOLS
    probe::effectRead(values(p).id, s_offsets[p], fx::s_params[p].floats, false);
#endif
    memcpy(data, floats(p), std::min<uint32_t>(bytes / 4, fx::s_params[p].floats) * 4);
    return D3D_OK;
}

HRESULT Effect::SetBool(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, BOOL value)
{
    return SetInt(app, cpu, handle, value ? 1 : 0);
}

HRESULT Effect::SetInt(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::sreg32 value)
{
    return SetIntArray(app, cpu, handle, &value, 1);
}

HRESULT Effect::GetInt(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::sreg32* value)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!value)
        return D3DERR_INVALIDCALL;
    const uint32_t a = handle - HandleAnnotation, t = a / 16;
    if (handle >= HandleAnnotation && t < info().techniqueCount && a % 16 < info().techniques[t].annotationCount)
    {
        *value = info().techniques[t].annotations[a % 16].value;
        return D3D_OK;
    }
    const int p = param(handle);
    if (p < 0 || !isNumeric(uint32_t(p)))
        return D3DERR_INVALIDCALL;
#ifdef DEV_TOOLS
    probe::effectRead(values(p).id, s_offsets[p], 1, false);
#endif
    *value = isInteger(uint32_t(p)) ? intValue(p) : x86::sreg32(floats(p)[0]);
    return D3D_OK;
}

HRESULT Effect::SetIntArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const x86::sreg32* data,
                            UINT count)
{
    RC_USE(app);
    return setIntsSlow(handle, data, count, streamFor(cpu));
}

HRESULT Effect::setIntsSlow(x86::reg32 handle, const x86::sreg32* data, uint32_t count, Stream* stream)
{
    int p = fastParam(handle, FastFloat | FastInt);
    if (__builtin_expect(p < 0 || !data, 0))
    {
        p = param(handle);
        if (p < 0 || !data || !isNumeric(uint32_t(p)))
            return D3DERR_INVALIDCALL;
    }
    const uint32_t n = std::min<uint32_t>(count, fx::s_params[p].floats);
    float converted[256];
    if (isFloat(uint32_t(p)))
    {
        for (uint32_t i = 0; i < n && i < 256; ++i)
            converted[i] = float(data[i]);
        data = reinterpret_cast<const x86::sreg32*>(converted);
    }
    trace(count == 1 ? "SetInt" : "SetIntArray", p, data, n);
    store(p, data, n, stream);
    return D3D_OK;
}

HRESULT Effect::GetIntArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::sreg32* data, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !data || !isNumeric(uint32_t(p)))
        return D3DERR_INVALIDCALL;
#ifdef DEV_TOOLS
    probe::effectRead(values(p).id, s_offsets[p], fx::s_params[p].floats, false);
#endif
    const uint32_t n = std::min<uint32_t>(count, fx::s_params[p].floats);
    for (uint32_t i = 0; i < n; ++i)
        data[i] = isInteger(uint32_t(p)) ? intValue(p, i) : x86::sreg32(floats(p)[i]);
    return D3D_OK;
}

HRESULT Effect::SetFloat(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float value)
{
    RC_USE(app);
    return setFloatSlow(handle, value, streamFor(cpu));
}

HRESULT Effect::setFloatSlow(x86::reg32 handle, float value, Stream* stream)
{
    const int f = fastFloat(handle);
    if (__builtin_expect(f >= 0, 1))
    {
        const uint32_t n = std::min<uint32_t>(1, fx::s_params[f].floats);
        trace("SetFloat", f, &value, n);
        store(f, &value, n, stream);
        return D3D_OK;
    }
    const int p = param(handle);
    if (p >= 0 && isInteger(uint32_t(p)))
    {
        const x86::sreg32 i = x86::sreg32(value);
        trace("SetFloat", p, &i, 1);
        store(p, &i, 1, stream);
        return D3D_OK;
    }
    return setFloats("SetFloat", handle, &value, 1, stream);
}

HRESULT Effect::GetFloat(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* value)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !value || !isNumeric(uint32_t(p)))
        return D3DERR_INVALIDCALL;
#ifdef DEV_TOOLS
    probe::effectRead(values(p).id, s_offsets[p], 1, false);
#endif
    *value = isFloat(uint32_t(p)) ? floats(p)[0] : float(intValue(p));
    return D3D_OK;
}

HRESULT Effect::SetFloatArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* values, UINT count)
{
    RC_USE(app);
    return setFloats("SetFloatArray", handle, values, count, streamFor(cpu));
}

HRESULT Effect::SetVector(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* vector)
{
    RC_USE(app);
    return setFloats("SetVector", handle, vector, 4, streamFor(cpu));
}

HRESULT Effect::GetVector(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* vector)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !vector || !isFloat(uint32_t(p)))
        return D3DERR_INVALIDCALL;
#ifdef DEV_TOOLS
    probe::effectRead(values(p).id, s_offsets[p], 4, false);
#endif
    memcpy(vector, floats(p), std::min<uint32_t>(fx::s_params[p].floats, 4) * 4);
    return D3D_OK;
}

HRESULT Effect::SetVectorArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* vectors,
                               UINT count)
{
    RC_USE(app);
    return setFloats("SetVectorArray", handle, vectors, count * 4, streamFor(cpu));
}

HRESULT Effect::SetMatrix(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* matrix)
{
    RC_USE(app);
    return setFloats("SetMatrix", handle, matrix, 16, streamFor(cpu));
}

HRESULT Effect::GetMatrix(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* matrix)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !matrix || !isFloat(uint32_t(p)) || fx::s_params[p].floats < 16)
        return D3DERR_INVALIDCALL;
#ifdef DEV_TOOLS
    probe::effectRead(values(p).id, s_offsets[p], 16, false);
#endif
    memcpy(matrix, floats(p), 64);
    return D3D_OK;
}

HRESULT Effect::SetMatrixArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* matrices,
                               UINT count)
{
    RC_USE(app);
    return setFloats("SetMatrixArray", handle, matrices, count * 16, streamFor(cpu));
}

HRESULT Effect::SetMatrixTranspose(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* matrix)
{
    return SetMatrixTransposeArray(app, cpu, handle, matrix, 1);
}

HRESULT Effect::SetMatrixTransposeArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle,
                                        const float* matrices, UINT count)
{
    RC_USE(app);
    if (!matrices || count > 16)
        return D3DERR_INVALIDCALL;
    float t[256];
    for (UINT n = 0; n < count; ++n)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                t[n * 16 + i * 4 + j] = matrices[n * 16 + j * 4 + i];
    return setFloats("SetMatrixTranspose", handle, t, count * 16, streamFor(cpu));
}

HRESULT Effect::SetTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, Object* texture)
{
    RC_USE(app);
    const int p = param(handle);
    if (p < 0 || !isTexture(uint32_t(p)))
        return D3DERR_INVALIDCALL;
    if (texture && texture->kind() != KindTexture && texture->kind() != KindCubeTexture)
        return D3DERR_INVALIDCALL;
    Device& device = deviceFor(cpu);
    Values& block = *m_slots[p].values;
    if (cpu.og)
        device.stream()->shadow->setTexture(block.id, uint32_t(p), texture);
    else
    {
        Object*& slot = block.textures[p];
        if (slot != texture || !m_reuseRecord || !bridge::quiet())
        {
            if (texture)
                texture->bind();
            if (slot)
                slot->unbind();
            slot = texture;
            ++bridge::g_hot.textureEpoch;
        }
#ifdef DEV_TOOLS
        if (__builtin_expect(probe::g_on, 0))
            probe::parentTexture(block.id, uint32_t(p));
#endif
    }
    // Applied at the active pass's CommitChanges, as in D3DX 9.0c.
    if (const EffectPass* active = device.effectPass())
    {
        const OpenPass& a = *static_cast<const OpenPass*>(active);
        const Effect* e = a.effect;
        const fx::Pass* pass = e->m_technique == 0 ? passOf(e->m_kind, a.pass) : nullptr;
        if (pass && s_kinds[e->m_kind].local[p] >= 0 && &e->values(p) == &values(p))
            for (uint32_t i = 0; i < pass->samplerCount; ++i)
                if (pass->samplers[i].texture == p && pass->samplers[i].stage < render::MaxSamplers)
                    a.textureStages = uint8_t(a.textureStages | 1u << pass->samplers[i].stage);
    }
    return D3D_OK;
}

HRESULT Effect::GetTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32* texture)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !texture || !isTexture(uint32_t(p)))
        return D3DERR_INVALIDCALL;
#ifdef DEV_TOOLS
    probe::effectRead(values(p).id, uint32_t(p), 1, true);
#endif
    Object* t = values(p).textures[p];
    if (t)
        t->addRef();
    *texture = t ? t->guest() : 0;
    return D3D_OK;
}

HRESULT Effect::GetPool(WinApplication* app, x86::CPU& cpu, x86::reg32* pool)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!pool)
        return D3DERR_INVALIDCALL;
    if (m_pool)
        m_pool->addRef();
    *pool = m_pool ? m_pool->guest() : 0;
    return D3D_OK;
}

HRESULT Effect::SetTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique)
{
    RC_USE(app);
    RC_USE(cpu);
    const uint32_t t = technique - HandleTechnique;
    if (t >= info().techniqueCount)
        return D3DERR_INVALIDCALL;
    static bool s_logged[EffectCount];
    if (!s_logged[m_kind])
    {
        s_logged[m_kind] = true;
        pf::logInfo("d3dx9: %s uses technique %u %s (%u passes)", info().resource, t, info().techniques[t].name,
                    unsigned(info().techniques[t].passCount));
    }
    m_technique = t;
    techniqueChanged();
    return D3D_OK;
}

x86::reg32 Effect::GetCurrentTechnique(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return HandleTechnique + m_technique;
}

HRESULT Effect::ValidateTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique)
{
    RC_USE(app);
    RC_USE(cpu);
    return technique == HandleTechnique && s_kinds[m_kind].valid ? D3D_OK : E_FAIL;
}

HRESULT Effect::FindNextValidTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique, x86::reg32* next)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!next)
        return D3DERR_INVALIDCALL;
    const bool found = technique == 0 && s_kinds[m_kind].valid;
    *next = found ? x86::reg32(HandleTechnique) : 0;
    return found ? D3D_OK : 1;     // S_FALSE
}

BOOL Effect::IsParameterUsed(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32 technique)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(technique);
    return param(handle) >= 0;
}

HRESULT Effect::Begin(WinApplication* app, x86::CPU& cpu, UINT* passes, DWORD flags)
{
    const fx::Technique& t = technique();
    if (passes)
        *passes = t.passCount;
    if (flags & ~7u)
        D3D_LOG_ONCE("d3dx9: Begin flags 0x%x", unsigned(flags));
    // D3DXFX_DONOTSAVESTATE 1, DONOTSAVESAMPLERSTATE 4; End undoes only the game's own writes.
    OpenPass& ps = passFor(cpu);
    ps.saved = !(flags & 1) && t.restore;
    if (!ps.saved)
        return D3D_OK;
    Device& device = deviceFor(cpu);
    uint32_t i = 0;
#ifdef DEV_TOOLS
    struct Scope { Scope() { probe::g_scope = probe::ScopeBegin; } ~Scope() { probe::g_scope = probe::ScopeGuest; } } scope;
#endif
    for (uint32_t n = 0; n < t.restoreRender; ++n, ++i)
        device.GetRenderState(app, cpu, t.restore[i], &ps.restore[i]);
    for (uint32_t n = 0; n < t.restoreStage; ++n, ++i)
        device.GetTextureStageState(app, cpu, t.restore[i] >> 8, t.restore[i] & 0xff, &ps.restore[i]);
    for (uint32_t n = 0; n < t.restoreSampler && !(flags & 4); ++n, ++i)
        device.GetSamplerState(app, cpu, t.restore[i] >> 8, t.restore[i] & 0xff, &ps.restore[i]);
    return D3D_OK;
}

HRESULT Effect::BeginPass(WinApplication* app, x86::CPU& cpu, UINT pass)
{
    RC_USE(app);
    return beginPass(cpu, pass);
}

__attribute__((always_inline)) inline HRESULT Effect::beginPass(const x86::CPU& cpu, UINT pass)
{
    if (pass >= m_passCount)
        return D3DERR_INVALIDCALL;
    OpenPass& ps = passFor(cpu);
    if (pass != ps.pass)
    {
        ps.pass = pass;
        passChanged(ps);
    }
    ps.textureStages = 0;
    Device& device = deviceFor(cpu);
    device.setEffectPass(&ps);
    device.effectPassApplied();
    return D3D_OK;
}

HRESULT Effect::CommitChanges(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    commit(cpu);
    return D3D_OK;
}

__attribute__((always_inline)) inline void Effect::commit(const x86::CPU& cpu)
{
    OpenPass& ps = passFor(cpu);
    if (!ps.textureStages)
        return;
    Device& device = deviceFor(cpu);
    if (device.effectPass() == &ps)
        for (uint32_t s = 0; s < render::MaxSamplers; ++s)
            if (ps.textureStages & (1u << s))
                device.effectTextureApplied(s);
    ps.textureStages = 0;
}

HRESULT Effect::EndPass(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    // The pass stays bound until End or the next BeginPass, as D3DX leaves its shaders set.
    return D3D_OK;
}

HRESULT Effect::End(WinApplication* app, x86::CPU& cpu)
{
    Device& device = deviceFor(cpu);
    OpenPass& ps = passFor(cpu);
    if (device.effectPass() == &ps)
        device.setEffectPass(nullptr);
    if (!ps.saved)
        return D3D_OK;
    ps.saved = false;
    const fx::Technique& t = technique();
    uint32_t i = 0;
    for (uint32_t n = 0; n < t.restoreRender; ++n, ++i)
        device.SetRenderState(app, cpu, t.restore[i], ps.restore[i]);
    for (uint32_t n = 0; n < t.restoreStage; ++n, ++i)
        device.SetTextureStageState(app, cpu, t.restore[i] >> 8, t.restore[i] & 0xff, ps.restore[i]);
    for (uint32_t n = 0; n < t.restoreSampler; ++n, ++i)
    {
        DWORD current = 0;
#ifdef DEV_TOOLS
        probe::g_scope = probe::ScopeEnd;
#endif
        device.GetSamplerState(app, cpu, t.restore[i] >> 8, t.restore[i] & 0xff, &current);
#ifdef DEV_TOOLS
        probe::g_scope = probe::ScopeGuest;
#endif
        if (current != ps.restore[i])
            device.SetSamplerState(app, cpu, t.restore[i] >> 8, t.restore[i] & 0xff, ps.restore[i]);
    }
    return D3D_OK;
}

HRESULT Effect::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

HRESULT Effect::OnLostDevice(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

HRESULT Effect::OnResetDevice(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

// ---- Vtables (ID3DXEffect of D3DX 9.0c: 79 slots, Begin at 65, BeginPass 66, End 69) ------------------------------

namespace
{

const MethodEntry s_effect[] = {
    D3D_NAMED("QueryInterface", Object, QueryInterface), D3D_NAMED("AddRef", Object, AddRef),
    D3D_NAMED("Release", Object, Release),
    D3D_UNBOUND(Effect, GetDesc), D3D_UNBOUND(Effect, GetParameterDesc), D3D_TRAP(GetTechniqueDesc),
    D3D_TRAP(GetPassDesc), D3D_TRAP(GetFunctionDesc), D3D_UNBOUND(Effect, GetParameter),
    D3D_UNBOUND(Effect, GetParameterByName), D3D_UNBOUND(Effect, GetParameterBySemantic),
    D3D_TRAP(GetParameterElement), D3D_UNBOUND(Effect, GetTechnique), D3D_UNBOUND(Effect, GetTechniqueByName),
    D3D_UNBOUND(Effect, GetPass), D3D_TRAP(GetPassByName), D3D_TRAP(GetFunction), D3D_TRAP(GetFunctionByName),
    D3D_TRAP(GetAnnotation), D3D_UNBOUND(Effect, GetAnnotationByName),
    D3D_UNBOUND(Effect, SetValue), D3D_UNBOUND(Effect, GetValue), D3D_UNBOUND(Effect, SetBool), D3D_TRAP(GetBool),
    D3D_TRAP(SetBoolArray), D3D_TRAP(GetBoolArray), D3D_GUEST_NAMED("SetInt", EffectSetInt),
    D3D_UNBOUND(Effect, GetInt), D3D_GUEST_NAMED("SetIntArray", EffectSetIntArray), D3D_UNBOUND(Effect, GetIntArray),
    D3D_GUEST_NAMED("SetFloat", EffectSetFloat),
    D3D_UNBOUND(Effect, GetFloat), D3D_UNBOUND(Effect, SetFloatArray), D3D_TRAP(GetFloatArray),
    D3D_GUEST_NAMED("SetVector", EffectSetVector), D3D_UNBOUND(Effect, GetVector),
    D3D_GUEST_NAMED("SetVectorArray", EffectSetVectorArray), D3D_TRAP(GetVectorArray),
    D3D_GUEST_NAMED("SetMatrix", EffectSetMatrix), D3D_UNBOUND(Effect, GetMatrix),
    D3D_GUEST_NAMED("SetMatrixArray", EffectSetMatrixArray), D3D_TRAP(GetMatrixArray),
    D3D_TRAP(SetMatrixPointerArray), D3D_TRAP(GetMatrixPointerArray), D3D_UNBOUND(Effect, SetMatrixTranspose),
    D3D_TRAP(GetMatrixTranspose), D3D_UNBOUND(Effect, SetMatrixTransposeArray), D3D_TRAP(GetMatrixTransposeArray),
    D3D_TRAP(SetMatrixTransposePointerArray), D3D_TRAP(GetMatrixTransposePointerArray), D3D_TRAP(SetString),
    D3D_TRAP(GetString), D3D_GUEST_NAMED("SetTexture", EffectSetTexture), D3D_UNBOUND(Effect, GetTexture),
    D3D_TRAP(Slot54), D3D_TRAP(Slot55), D3D_TRAP(Slot56), D3D_TRAP(Slot57), D3D_TRAP(SetArrayRange),
    D3D_UNBOUND(Effect, GetPool), D3D_UNBOUND(Effect, SetTechnique), D3D_UNBOUND(Effect, GetCurrentTechnique),
    D3D_UNBOUND(Effect, ValidateTechnique), D3D_UNBOUND(Effect, FindNextValidTechnique),
    D3D_UNBOUND(Effect, IsParameterUsed), D3D_GUEST_NAMED("Begin", EffectBegin),
    D3D_GUEST_NAMED("BeginPass", EffectPass), D3D_UNBOUND(Effect, CommitChanges), D3D_UNBOUND(Effect, EndPass),
    D3D_GUEST_NAMED("End", EffectEnd), D3D_UNBOUND(Effect, GetDevice), D3D_UNBOUND(Effect, OnLostDevice),
    D3D_UNBOUND(Effect, OnResetDevice), D3D_TRAP(SetStateManager), D3D_TRAP(GetStateManager),
    D3D_TRAP(BeginParameterBlock), D3D_TRAP(EndParameterBlock), D3D_TRAP(ApplyParameterBlock),
    D3D_TRAP(CloneEffect),
};
static_assert(sizeof(s_effect) / sizeof(s_effect[0]) == 79, "ID3DXEffect slots");

const MethodEntry s_pool[] = {
    D3D_NAMED("QueryInterface", Object, QueryInterface), D3D_NAMED("AddRef", Object, AddRef),
    D3D_NAMED("Release", Object, Release),
};

}

void initEffectInterfaces(WinApplication* app)
{
    buildInterface(app, KindEffect, "ID3DXEffect", s_effect, sizeof(s_effect) / sizeof(s_effect[0]));
    buildInterface(app, KindEffectPool, "ID3DXEffectPool", s_pool, sizeof(s_pool) / sizeof(s_pool[0]));
}

// ---- Direct calls (d3d9_native.h) ----------------------------------------------------------------------------------

namespace bridge { namespace effect
{

namespace
{

inline Effect* object(reg32 guest)
{
    return effectObject(guest);
}

// A segment's calls skip the cache the game thread writes.
inline Effect* object(const x86::CPU& cpu, reg32 guest)
{
    return effectObject(guest, cpu.og != nullptr);
}

}

HRESULT setInt(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, x86::sreg32 value)
{
    return object(cpu, effect)->SetInt(app, cpu, handle, value);
}

HRESULT setIntArray(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const x86::sreg32* values,
                    UINT count)
{
    return object(cpu, effect)->SetIntArray(app, cpu, handle, values, count);
}

__attribute__((noinline)) HRESULT setFloat(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, float value)
{
    RC_USE(app);
    Effect* e = object(cpu, effect);
    return e->setFloat(handle, value, e->streamFor(cpu));
}

__attribute__((noinline)) HRESULT setVector(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* vector)
{
    RC_USE(app);
    Effect* e = object(cpu, effect);
    return e->setVector(handle, vector, e->streamFor(cpu));
}

__attribute__((noinline)) HRESULT setVectorArray(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* vectors,
                       UINT count)
{
    RC_USE(app);
    Effect* e = object(cpu, effect);
    return e->setVectors(handle, vectors, count, e->streamFor(cpu));
}

__attribute__((noinline)) HRESULT setMatrix(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* matrix)
{
    RC_USE(app);
    Effect* e = object(cpu, effect);
    return e->setMatrix(handle, matrix, e->streamFor(cpu));
}

HRESULT setMatrixArray(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* matrices,
                       UINT count)
{
    return object(cpu, effect)->SetMatrixArray(app, cpu, handle, matrices, count);
}

HRESULT setTexture(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, reg32 texture)
{
    return object(cpu, effect)->SetTexture(app, cpu, handle, lookup(texture));
}

HRESULT setProduct(x86::CPU& cpu, reg32 effect, reg32 handle, const float* const* matrices, UINT count,
                   uint32_t fpscr)
{
    Effect* e = object(cpu, effect);
    return e->setProduct(handle, matrices, count, fpscr, e->streamFor(cpu));
}

UINT passCount(reg32 effect)
{
    return object(effect)->passCount();
}

const uint8_t* paramTypes(reg32 effect)
{
    const Object* o = lookup(effect);
    return o && o->kind() == KindEffect ? static_cast<const Effect*>(o)->paramTypes() : nullptr;
}

HRESULT checkPass(reg32 effect, UINT pass)
{
    return object(effect)->checkPass(pass);
}

HRESULT begin(WinApplication* app, x86::CPU& cpu, reg32 effect, UINT* passes, DWORD flags)
{
    return object(cpu, effect)->Begin(app, cpu, passes, flags);
}

__attribute__((noinline)) HRESULT pass(WinApplication* app, x86::CPU& cpu, reg32 effect, UINT pass)
{
    RC_USE(app);
    return object(cpu, effect)->beginPass(cpu, pass);
}

__attribute__((noinline)) bool commitChanges(x86::CPU& cpu, reg32 effect)
{
    object(cpu, effect)->commit(cpu);
    return true;
}

HRESULT end(WinApplication* app, x86::CPU& cpu, reg32 effect)
{
    return object(cpu, effect)->End(app, cpu);
}

}}

namespace bridge
{

uint32_t effectDrawnPasses(reg32 guest, uint32_t passes)
{
    Object* o = lookup(guest);
    if (!o || o->kind() != KindEffect)
        return passes;
    uint32_t drawn = 0;
    for (uint32_t i = 0; i < passes; ++i)
        drawn += static_cast<Effect*>(o)->passDrawn(i);
    return drawn;
}

void segmentSnapshotEffect(SegmentRef& ref, reg32 guest)
{
    Object* o = lookup(guest);
    if (o && o->kind() == KindEffect)
        static_cast<Effect*>(o)->snapshotTextures(*static_cast<Device*>(ref.device)->stream()->shadow);
}

// Quiet calls run here, one function each; the others go through d3d9_native.cpp.

HRESULT effectSetFloat(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, float value)
{
    if (__builtin_expect(quiet(), 1))
    {
        Effect* e = effectObject(guest, cpu.og != nullptr);
        return e->setFloat(handle, value, cpu.og ? e->streamFor(cpu) : static_cast<Stream*>(g_hot.stream));
    }
    return effectSetFloatSlow(app, cpu, guest, handle, value);
}

HRESULT effectSetVector(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const float* vector)
{
    if (__builtin_expect(quiet(), 1))
    {
        Effect* e = effectObject(guest, cpu.og != nullptr);
        return e->setVector(handle, vector, cpu.og ? e->streamFor(cpu) : static_cast<Stream*>(g_hot.stream));
    }
    return effectSetVectorSlow(app, cpu, guest, handle, vector);
}

HRESULT effectSetIntArray(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const x86::sreg32* values,
                          UINT count)
{
    if (__builtin_expect(quiet(), 1))
    {
        Effect* e = effectObject(guest, cpu.og != nullptr);
        return e->setInts(handle, values, count, cpu.og ? e->streamFor(cpu) : static_cast<Stream*>(g_hot.stream));
    }
    return effectSetIntArraySlow(app, cpu, guest, handle, values, count);
}

HRESULT effectSetVectorArray(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const float* vectors,
                             UINT count)
{
    if (__builtin_expect(quiet(), 1))
    {
        Effect* e = effectObject(guest, cpu.og != nullptr);
        return e->setVectors(handle, vectors, count, cpu.og ? e->streamFor(cpu) : static_cast<Stream*>(g_hot.stream));
    }
    return effectSetVectorArraySlow(app, cpu, guest, handle, vectors, count);
}

HRESULT effectSetMatrix(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const float* matrix)
{
    if (__builtin_expect(quiet(), 1))
    {
        Effect* e = effectObject(guest, cpu.og != nullptr);
        return e->setMatrix(handle, matrix, cpu.og ? e->streamFor(cpu) : static_cast<Stream*>(g_hot.stream));
    }
    return effectSetMatrixSlow(app, cpu, guest, handle, matrix);
}

HRESULT effectSetTexture(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, reg32 texture)
{
    if (__builtin_expect(quiet(), 1))
        return effectObject(guest, cpu.og != nullptr)->SetTexture(app, cpu, handle, lookup(texture));
    return effectSetTextureSlow(app, cpu, guest, handle, texture);
}

HRESULT effectPass(WinApplication* app, x86::CPU& cpu, reg32 guest, UINT pass)
{
    if (__builtin_expect(quiet(), 1))
        return effectObject(guest, cpu.og != nullptr)->beginPass(cpu, pass);
    return effectPassSlow(app, cpu, guest, pass);
}

bool effectCommitChanges(WinApplication* app, x86::CPU& cpu, reg32 guest)
{
    RC_USE(app);
    if (__builtin_expect(!quiet(), 0))
        return false;
    effectObject(guest, cpu.og != nullptr)->commit(cpu);
    return true;
}

}

// ---- Entry points --------------------------------------------------------------------------------------------------

namespace
{

HRESULT createEffectPool(WinApplication* app, x86::CPU& cpu, x86::reg32* pool)
{
    RC_USE(cpu);
    if (!pool)
        return D3DERR_INVALIDCALL;
    *pool = publish(app, new EffectPool());
    pf::logInfo("d3dx9: effect pool created");
    return D3D_OK;
}

HRESULT createEffectFromResource(WinApplication* app, x86::CPU& cpu, Device* device, x86::reg32 module,
                                 x86::reg32 name, x86::reg32 defines, x86::reg32 include, DWORD flags,
                                 EffectPool* pool, x86::reg32* effect, x86::reg32* errors)
{
    RC_USE(defines);
    RC_USE(include);
    RC_USE(flags);
    if (effect)
        *effect = 0;
    if (errors)
        *errors = 0;
    if (!device || !effect)
        return D3DERR_INVALIDCALL;
    initTables();
    if (!s_blackCube)
    {
        const render::TextureDesc desc = { texture::FormatARGB8888, 1, true, false, 8, 8 };
        pf::gxm::Device& gpu = pf::gxm::Device::get();
        render::TextureStorage* cube = gpu.createTexture(desc);
        if (!cube)
            pf::fatal("d3dx9: out of texture memory for the black cube");
        uint32_t pixels[64];
        for (uint32_t& p : pixels)
            p = 0xff000000u;
        for (uint32_t face = 0; face < 6; ++face)
            gpu.writeTexture(cube, face, 0, pixels, 32);
        s_blackCube = cube;
    }
    char label[64];
    if (name >= 0x10000)
        snprintf(label, sizeof(label), "%s", reinterpret_cast<const char*>(g_guestBase + name));
    else
        snprintf(label, sizeof(label), "#%u", unsigned(name));
    const x86::reg32 resource = kernel32::FindResourceA(app, cpu, module, name, 10);     // RT_RCDATA
    const x86::reg32 data = resource ? kernel32::LoadResource(app, cpu, module, resource) : 0;
    const DWORD size = resource ? kernel32::SizeofResource(app, cpu, module, resource) : 0;
    const std::string hash = data ? pf::sha256Hex(g_guestBase + data, size) : std::string("-");
    uint32_t kind = EffectCount;
    for (uint32_t k = 0; k < EffectCount; ++k)
        if (strcasecmp(label, fx::s_effects[k].resource) == 0)
            kind = k;
    // The game ignores the result and uses the effect.
    if (kind == EffectCount || !data)
        pf::fatal("d3dx9: unknown effect resource %s (%u bytes, SHA-256 %s)", label, unsigned(size), hash.c_str());
    if (hash != fx::s_effects[kind].sha256)
        pf::fatal("d3dx9: %s differs from the effect tables (SHA-256 %s)", label, hash.c_str());
    *effect = publish(app, new Effect(device, pool, kind));
    const fx::EffectInfo& e = fx::s_effects[kind];
    pf::logInfo("d3dx9: %s: %u parameters, %u techniques%s", e.resource, unsigned(e.paramCount),
                unsigned(e.techniqueCount),
                s_kinds[kind].valid ? "" : " (no technique has programs: the game turns its features off)");
    return D3D_OK;
}

float* hostFloats(x86::reg32 address)
{
    return reinterpret_cast<float*>(g_guestBase + address);
}

void store(x86::reg32 address, const float* values, uint32_t count)
{
    memcpy(g_guestBase + address, values, count * sizeof(float));
}

x86::reg32 matrixMultiply(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 a, x86::reg32 b)
{
    RC_USE(app);
    RC_USE(cpu);
    float ma[16], mb[16], r[16];
    memcpy(ma, hostFloats(a), 64);
    memcpy(mb, hostFloats(b), 64);
    multiply(ma, mb, r);
    store(out, r, 16);
    return out;
}

x86::reg32 vecTransform(x86::reg32 out, const float v[4], x86::reg32 m)
{
    float mm[16], r[4];
    memcpy(mm, hostFloats(m), 64);
    d3dx9::transform(v, mm, r);
    store(out, r, 4);
    return out;
}

x86::reg32 vec3Transform(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 v, x86::reg32 m)
{
    RC_USE(app);
    RC_USE(cpu);
    float in[4] = { 0, 0, 0, 1 };
    memcpy(in, hostFloats(v), 12);
    return vecTransform(out, in, m);
}

x86::reg32 vec4Transform(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 v, x86::reg32 m)
{
    RC_USE(app);
    RC_USE(cpu);
    float in[4];
    memcpy(in, hostFloats(v), 16);
    return vecTransform(out, in, m);
}

x86::reg32 matrixTranspose(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 m)
{
    RC_USE(app);
    RC_USE(cpu);
    float in[16], r[16];
    memcpy(in, hostFloats(m), 64);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = in[j * 4 + i];
    store(out, r, 16);
    return out;
}

}

}}

namespace win32 { namespace d3dx9
{

void CreateEffectPool(WinApplication* app, x86::CPU& cpu)
{
    d3d9::initInterfaces(app);
    d3d9::bridge::drain();
    Wrapper<decltype(&d3d9::createEffectPool), &d3d9::createEffectPool>::stdcall(app, cpu);
}

void CreateEffectFromResourceA(WinApplication* app, x86::CPU& cpu)
{
    d3d9::initInterfaces(app);
    d3d9::bridge::drain();
    Wrapper<decltype(&d3d9::createEffectFromResource), &d3d9::createEffectFromResource>::stdcall(app, cpu);
}

void MatrixMultiply(WinApplication* app, x86::CPU& cpu)
{
    Wrapper<decltype(&d3d9::matrixMultiply), &d3d9::matrixMultiply>::stdcall(app, cpu);
}

void Vec3Transform(WinApplication* app, x86::CPU& cpu)
{
    Wrapper<decltype(&d3d9::vec3Transform), &d3d9::vec3Transform>::stdcall(app, cpu);
}

void Vec4Transform(WinApplication* app, x86::CPU& cpu)
{
    Wrapper<decltype(&d3d9::vec4Transform), &d3d9::vec4Transform>::stdcall(app, cpu);
}

void MatrixTranspose(WinApplication* app, x86::CPU& cpu)
{
    Wrapper<decltype(&d3d9::matrixTranspose), &d3d9::matrixTranspose>::stdcall(app, cpu);
}

void transform(const float v[4], const float m[16], float r[4])
{
    for (int i = 0; i < 4; ++i)
        r[i] = v[0] * m[i] + v[1] * m[4 + i] + v[2] * m[8 + i] + v[3] * m[12 + i];
}

}}
