// Direct3D frames on SceGxm: GPU heaps, resources, frame slots and scene drawing (gxm_render.h).

#include "gxm_render.h"
#include <platform/switches.h>
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/jobs.h>
#include <platform/log.h>
#include <platform/limiter.h>
#include <platform/perf.h>
#include <platform/stall.h>
#include <platform/texture.h>
#include <psp2/kernel/threadmgr.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#define NFSU_SHADERS(X) \
    X(ff_v) X(ff_screen_v) X(world_v) X(world_proj_v) X(worldheadlight_v) X(worldreflect_v) \
    X(worldheadlightreflect_v) X(worldbone_v) X(car_v) X(filter_v) X(clearz_v) \
    X(ff_color_f) X(ff_color_f_atest) X(ff_tex_f) X(ff_tex_f_atest) X(ff_proj_f) X(ff_proj_f_atest) \
    X(world_f) X(world_f_atest) X(world_proj_f) X(world_proj_f_atest) X(worldheadlight_f) X(worldheadlight_f_atest) \
    X(worldreflect_f) X(worldreflect_f_atest) X(worldheadlightreflect_f) X(worldheadlightreflect_f_atest) X(car_f) \
    X(car_f_atest) X(skymorph_f) X(skymorph_f_atest) \
    X(filter1_f) X(filter1_f_atest) X(filter2_f) X(filter2_f_atest) X(filter3_f) X(filter3_f_atest) \
    X(filter4_f) X(filter4_f_atest) X(clear_f) \
    X(filter1_f_bfcolor) X(filter1_f_bfalpha) X(filter1_f_bfscale) X(filter2_f_bfcolor) X(filter2_f_bfalpha) \
    X(filter2_f_bfscale) X(filter3_f_bfcolor) X(filter3_f_bfalpha) X(filter3_f_bfscale) X(filter4_f_bfcolor) \
    X(filter4_f_bfalpha) X(filter4_f_bfscale) \
    X(worldbone_f) X(worldbone_f_atest) X(worldreflect_fog_v) X(worldheadlight_fog_v) X(worldheadlightreflect_fog_v) \
    X(worldreflect_fog_f) X(worldreflect_fog_f_atest) X(worldheadlight_fog_f) X(worldheadlight_fog_f_atest) \
    X(worldheadlightreflect_fog_f) X(worldheadlightreflect_fog_f_atest) \
    NFSU_SCREEN_SHADERS(X, overbright_f) NFSU_SCREEN_SHADERS(X, screengain_f) NFSU_SCREEN_SHADERS(X, screentint_f) \
    NFSU_SCREEN_SHADERS(X, screenramp_f) NFSU_SCREEN_SHADERS(X, raindrop_f)
#define NFSU_SCREEN_SHADERS(X, name) X(name) X(name##_atest) X(name##_bfcolor) X(name##_bfalpha) X(name##_bfscale)

// Weak: cmake/shaders.cmake leaves PENDING programs out, and their passes then fall back (render::missing).
extern "C" {
#define NFSU_DECLARE_SHADER(name) extern const unsigned char shader_##name[] __attribute__((weak));
NFSU_SHADERS(NFSU_DECLARE_SHADER)
}

namespace pf { namespace gxm {

namespace
{

struct ShaderBinary
{
    const char*             name;
    const unsigned char*    data;
};

#define NFSU_SHADER_ENTRY(name) { #name, shader_##name },
const ShaderBinary s_binaries[] = { NFSU_SHADERS(NFSU_SHADER_ENTRY) };

const unsigned char* binary(const char* name)
{
    for (const ShaderBinary& b : s_binaries)
    {
        if (strcmp(b.name, name) == 0)
            return b.data;
    }
    return nullptr;
}

uint32_t alignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

// Hitch log: a pool or heap grew by bytes in us.
void growth(uint32_t bytes, uint32_t us)
{
    perf::event(perf::EventGpuGrowths, 1);
    perf::event(perf::EventGpuGrowthKb, bytes / 1024);
    perf::event(perf::EventGpuGrowthUs, us);
}

const uint32_t SlotVertexBytes = 4 * 1024 * 1024;
const uint32_t SlotDraws = 16384;
const uint32_t SlotConstants = 512 * 1024;      // floats
const uint32_t SlotScenes = 128;
const uint32_t SlotViewports = 512;
const uint32_t NotificationBase = 8;            // notification words 0..7 belong to the present path

SceGxmTextureFormat textureFormat(texture::Format format)
{
    switch (format)
    {
    case texture::FormatBC1: return SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR;
    case texture::FormatBC2: return SCE_GXM_TEXTURE_FORMAT_UBC2_ABGR;
    default:                 return SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB;
    }
}

// One level of a swizzled texture from rows of pitch bytes.
void writeSwizzled(const Texture& t, uint32_t face, uint32_t level, const void* data, uint32_t pitch)
{
    const render::TextureDesc& d = t.desc;
    uint8_t* base = static_cast<uint8_t*>(t.data) + face * t.faceStride;
    texture::swizzleLevel(d.format, d.width, d.height, level, data, pitch,
                          base + texture::levelOffset(d.format, d.width, d.height, level));
}

// The smallest alpha of a written level of a 2D texture (render::TextureStorage::levelAlpha).
void noteAlpha(Texture& t, uint32_t level, const void* data, uint32_t pitch)
{
    const render::TextureDesc& d = t.desc;
    if (d.cube || d.target || level >= 16)
        return;
    t.levelAlpha[level].store(texture::minAlpha(d.format, d.width, d.height, level, data, pitch),
                              std::memory_order_relaxed);
}

void copyAlpha(Texture& to, const Texture& from)
{
    for (uint32_t level = 0; level < 16; ++level)
        to.levelAlpha[level].store(from.levelAlpha[level].load(std::memory_order_relaxed), std::memory_order_relaxed);
}

SceGxmBlendFactor blendFactor(uint8_t factor)
{
    switch (factor)
    {
    case 1:  return SCE_GXM_BLEND_FACTOR_ZERO;
    case 3:  return SCE_GXM_BLEND_FACTOR_SRC_COLOR;
    case 4:  return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5:  return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    case 6:  return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7:  return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
    case 8:  return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9:  return SCE_GXM_BLEND_FACTOR_DST_COLOR;
    case 10: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return SCE_GXM_BLEND_FACTOR_ONE;       // ONE (the layer resolves BLENDFACTOR, INVBLENDFACTOR)
    }
}

// The alpha channel reads the alpha of a colour factor, as in Direct3D.
SceGxmBlendFactor alphaFactor(SceGxmBlendFactor factor)
{
    switch (factor)
    {
    case SCE_GXM_BLEND_FACTOR_SRC_COLOR:           return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case SCE_GXM_BLEND_FACTOR_DST_COLOR:           return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE:  return SCE_GXM_BLEND_FACTOR_ONE;
    default:                                       return factor;
    }
}

// Program variant: 0 plain, 1 alpha test, 2.. blend factor outputs (FrameRenderer::Variants).
uint32_t variant(const render::DrawState& state)
{
    return state.factorOutput ? 1u + state.factorOutput : state.alphaTest ? 1u : 0u;
}

SceGxmAttributeFormat attributeFormat(uint8_t type, uint8_t* components)
{
    switch (type)
    {
    case render::TypeFloat1:  *components = 1; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    case render::TypeFloat2:  *components = 2; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    case render::TypeFloat3:  *components = 3; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    case render::TypeFloat4:  *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    case render::TypeColor:
    case render::TypeUByte4N: *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_U8N;
    case render::TypeUByte4:  *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_U8;
    case render::TypeShort2:  *components = 2; return SCE_GXM_ATTRIBUTE_FORMAT_S16;
    case render::TypeShort4:  *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_S16;
    default:                  *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    }
}

SceGxmPrimitiveType primitiveType(uint8_t primitive)
{
    return primitive == render::PrimitiveStrip ? SCE_GXM_PRIMITIVE_TRIANGLE_STRIP
         : primitive == render::PrimitiveFan ? SCE_GXM_PRIMITIVE_TRIANGLE_FAN : SCE_GXM_PRIMITIVE_TRIANGLES;
}

bool powerOfTwo(uint32_t v)
{
    return v && !(v & (v - 1));
}

// Default uniform buffer bytes of a program: its own size, and at least what the fields write.
uint32_t uniformBytes(const SceGxmProgram* program, const render::UniformField* fields,
                      const std::vector<const SceGxmProgramParameter*>& parameters)
{
    uint32_t bytes = sceGxmProgramGetDefaultUniformBufferSize(program);
    for (size_t i = 0; i < parameters.size(); ++i)
    {
        if (parameters[i])
            bytes = std::max<uint32_t>(bytes, (sceGxmProgramParameterGetResourceIndex(parameters[i]) + fields[i].count) * 4);
    }
    return alignUp(bytes, 16);
}

}

// ---- HostArena -----------------------------------------------------------------------------------------------------

HostArena::~HostArena()
{
    for (uint8_t* b : m_blocks)
        delete[] b;
}

void* HostArena::allocate(uint32_t size)
{
    size = alignUp(size, 16);
    if (m_block < m_blocks.size() && m_used + size > BlockSize)
    {
        ++m_block;
        m_used = 0;
    }
    if (m_block == m_blocks.size())
        m_blocks.push_back(new uint8_t[BlockSize]);
    void* p = m_blocks[m_block] + m_used;
    m_used += size;
    return p;
}

// ---- FramePool -----------------------------------------------------------------------------------------------------

FramePool::~FramePool()
{
    for (Chunk& c : m_chunks)
    {
        sceGxmUnmapMemory(c.base);
        sceKernelFreeMemBlock(c.uid);
    }
}

void FramePool::begin(uint32_t frameIndex, Finished finished, void* context)
{
    m_frame = frameIndex;
    m_finished = finished;
    m_context = context;
    if (m_current >= 0)
        m_chunks[m_current].lastUse = frameIndex;
}

int FramePool::freeChunk(uint32_t minimum)
{
    for (size_t i = 0; i < m_chunks.size(); ++i)
    {
        Chunk& c = m_chunks[i];
        if (int(i) != m_current && c.size >= minimum && (c.lastUse == 0 || m_finished(m_context, c.lastUse)))
        {
            c.used = 0;
            return int(i);
        }
    }
    const uint32_t size = alignUp(std::max(minimum, m_chunkSize), 4096);
    const bool timed = perf::hitch();
    const uint32_t start = timed ? perf::now() : 0;
    const SceUID uid = sceKernelAllocMemBlock(m_name, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, size, nullptr);
    if (uid < 0)
        fatal("gxm: %s: out of memory for %u KB: 0x%08x", m_name, size / 1024, uid);
    void* base = nullptr;
    sceKernelGetMemBlockBase(uid, &base);
    if (sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_READ) < 0)
        fatal("gxm: %s: sceGxmMapMemory failed", m_name);
    m_chunks.push_back(Chunk{ uid, static_cast<uint8_t*>(base), size, 0, 0 });
    if (timed)
        growth(size, perf::now() - start);
    logInfo("gxm: %s pool +%u KB", m_name, size / 1024);
    return int(m_chunks.size() - 1);
}

void* FramePool::allocate(uint32_t size, uint32_t alignment)
{
    if (m_current >= 0)
    {
        Chunk& c = m_chunks[m_current];
        const uint32_t start = alignUp(c.used, alignment);
        if (start + size <= c.size)
        {
            c.used = start + size;
            c.lastUse = m_frame;
            return c.base + start;
        }
    }
    m_current = freeChunk(size + alignment);
    Chunk& c = m_chunks[m_current];
    c.used = size;
    c.lastUse = m_frame;
    return c.base;
}

void* FramePool::take(uint32_t minimum, uint32_t* size)
{
    m_current = freeChunk(minimum);
    Chunk& c = m_chunks[m_current];
    c.used = c.size;
    c.lastUse = m_frame;
    *size = c.size;
    return c.base;
}

// ---- GpuHeap -------------------------------------------------------------------------------------------------------

GpuHeap::GpuHeap(const char* name, SceKernelMemBlockType type, uint32_t chunkSize, uint32_t attributes)
    :   m_name(name), m_type(type), m_chunkSize(chunkSize), m_attributes(attributes)
{
}

GpuHeap::~GpuHeap()
{
    for (Chunk* c : m_chunks)
    {
        sceGxmUnmapMemory(c->base);
        sceKernelFreeMemBlock(c->uid);
        delete c;
    }
}

bool GpuHeap::addChunk(uint32_t minimum)
{
    const uint32_t granularity = m_type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 256 * 1024 : 4096;
    const uint32_t least = alignUp(minimum, granularity);
    uint32_t size = alignUp(std::max(minimum, m_chunkSize), granularity);
    if (m_limit && m_totalBytes + size > m_limit)
    {
        const uint32_t left = m_limit > m_totalBytes ? (m_limit - m_totalBytes) / granularity * granularity : 0;
        m_full = left < least;
        if (m_full)
            return false;
        size = left;
    }
    const bool timed = perf::hitch();
    const uint32_t start = timed ? perf::now() : 0;
    const uint32_t smallest = std::max<uint32_t>(least, 1024 * 1024);
    SceUID uid = sceKernelAllocMemBlock(m_name, m_type, size, nullptr);
    while (uid < 0 && size > smallest)
    {
        size = std::max(smallest, alignUp(size / 2, granularity));
        uid = sceKernelAllocMemBlock(m_name, m_type, size, nullptr);
    }
    m_full = uid < 0;
    if (m_full)
        return false;
    void* base = nullptr;
    sceKernelGetMemBlockBase(uid, &base);
    if (sceGxmMapMemory(base, size, SceGxmMemoryAttribFlags(m_attributes)) < 0)
    {
        sceKernelFreeMemBlock(uid);
        return false;
    }
    Chunk* c = new Chunk{ uid, static_cast<uint8_t*>(base), size, {} };
    c->holes[0] = size;
    m_chunks.push_back(c);
    m_totalBytes += size;
    if (timed)
        growth(size, perf::now() - start);
    logInfo("gxm: %s heap +%u KB, %u of %u KB in use", m_name, size / 1024, m_usedBytes / 1024, m_totalBytes / 1024);
    return true;
}

void* GpuHeap::allocate(uint32_t size, uint32_t alignment, bool quiet)
{
    // Whole alignment units: smaller sizes leave a sliver hole per block that every search walks.
    size = alignUp(size ? size : 1, std::max<uint32_t>(alignment, 16));
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        for (Chunk* c : m_chunks)
        {
            for (auto it = c->holes.begin(); it != c->holes.end(); ++it)
            {
                const uint32_t start = alignUp(it->first, alignment);
                if (start + size > it->first + it->second)
                    continue;
                const uint32_t holeStart = it->first, holeEnd = it->first + it->second;
                c->holes.erase(it);
                if (start > holeStart)
                    c->holes[holeStart] = start - holeStart;
                if (start + size < holeEnd)
                    c->holes[start + size] = holeEnd - (start + size);
                void* p = c->base + start;
                m_used[p] = std::make_pair(c, size);
                m_usedBytes += size;
                return p;
            }
        }
        if (attempt == 0 && !addChunk(size + alignment))
            break;
    }
    if (!quiet)
        failed(size);
    return nullptr;
}

void GpuHeap::failed(uint32_t size)
{
    // A log line costs about 20 ms on the Vita without log_async: the first few, then every 64th.
    if (++m_failures <= 4 || m_failures % 64 == 0)
        logError("gxm: %s heap: no memory for %u KB (%u of %u KB in use, largest hole %u KB, failure %u)", m_name,
                 size / 1024, m_usedBytes / 1024, m_totalBytes / 1024, largestHole() / 1024, m_failures);
}

uint32_t GpuHeap::largestHole() const
{
    uint32_t largest = 0;
    for (const Chunk* c : m_chunks)
        for (const auto& hole : c->holes)
            largest = std::max(largest, hole.second);
    return largest;
}

uint32_t GpuHeap::sizeOf(void* pointer) const
{
    const auto it = m_used.find(pointer);
    return it == m_used.end() ? 0 : it->second.second;
}

void GpuHeap::free(void* pointer)
{
    auto it = m_used.find(pointer);
    if (it == m_used.end())
        return;
    Chunk* c = it->second.first;
    uint32_t start = uint32_t(static_cast<uint8_t*>(pointer) - c->base);
    uint32_t size = it->second.second;
    m_usedBytes -= size;
    m_used.erase(it);
    auto next = c->holes.lower_bound(start);
    if (next != c->holes.end() && next->first == start + size)
    {
        size += next->second;
        next = c->holes.erase(next);
    }
    if (next != c->holes.begin())
    {
        auto previous = std::prev(next);
        if (previous->first + previous->second == start)
        {
            start = previous->first;
            size += previous->second;
            c->holes.erase(previous);
        }
    }
    c->holes[start] = size;
    if (m_shrink && size == c->size)
        trim(c);
}

// Frees an empty chunk when another empty one stays as a spare.
void GpuHeap::trim(Chunk* chunk)
{
    bool spare = false;
    for (const Chunk* c : m_chunks)
        spare = spare || (c != chunk && c->holes.size() == 1 && c->holes.begin()->second == c->size);
    if (!spare)
        return;
    sceGxmUnmapMemory(chunk->base);
    sceKernelFreeMemBlock(chunk->uid);
    m_totalBytes -= chunk->size;
    m_full = false;
    m_chunks.erase(std::find(m_chunks.begin(), m_chunks.end(), chunk));
    logInfo("gxm: %s heap -%u KB, %u of %u KB in use", m_name, chunk->size / 1024, m_usedBytes / 1024,
            m_totalBytes / 1024);
    delete chunk;
}

void GpuHeap::trimEmpty()
{
    for (size_t i = m_chunks.size(); m_shrink && i-- > 0;)
    {
        Chunk* c = m_chunks[i];
        if (c->holes.size() != 1 || c->holes.begin()->second != c->size)
            continue;
        sceGxmUnmapMemory(c->base);
        sceKernelFreeMemBlock(c->uid);
        m_totalBytes -= c->size;
        m_full = false;
        m_chunks.erase(m_chunks.begin() + i);
        logInfo("gxm: %s heap -%u KB, %u of %u KB in use", m_name, c->size / 1024, m_usedBytes / 1024,
                m_totalBytes / 1024);
        delete c;
    }
}

// ---- Initialisation ------------------------------------------------------------------------------------------------

bool FrameRenderer::init(SceGxmContext* context, SceGxmShaderPatcher* patcher, volatile unsigned int* notifications)
{
    m_context = context;
    m_patcher = patcher;
    m_main.context = context;
    m_uploadLock = new Mutex("gxm_uploads");
    m_uploadDone = new CondVar("gxm_uploads", *m_uploadLock);
    m_precomputed = NFSU_RENDER_FAST("gxm_precomputed");
    logInfo("gxm: %s draws", m_precomputed ? "precomputed" : "immediate");
    NFSU_DEV_SET(m_cullSwap, NFSU_DEV_BOOL("cull_swap", false));
    if (m_cullSwap)
        logInfo("gxm: cull winding swapped (cull_swap=on)");
    static const char* const DrawOrders[] = { "sorted", "game" };
    NFSU_DEV_SET(m_drawSort, NFSU_DEV_CHOICE("alpha_draw_order", DrawOrders, 2, 0) == 0);
    NFSU_DEV_SET(m_skipVp, uint32_t(NFSU_DEV_INT("debug_skip_vp", 0)));
    NFSU_DEV_SET(m_skipFp, uint32_t(NFSU_DEV_INT("debug_skip_fp", 0)));
    NFSU_DEV_SET(m_maxDraws, uint32_t(std::max(0, NFSU_DEV_INT("debug_max_draws", 0))));
    NFSU_DEV_SET(m_sceneScale, float(std::min(100, std::max(10, NFSU_DEV_INT("debug_scene_scale", 100)))) / 100.0f);
    if (m_sceneScale != 1.0f)
        logInfo("gxm: debug: every scene drawn at %.0f%% of its size, in its target's corner (debug_scene_scale)",
                double(m_sceneScale * 100.0f));
    if (m_skipVp || m_skipFp || m_maxDraws)
        logInfo("gxm: debug: vertex programs 0x%x and fragment programs 0x%x skipped, at most %u draws a frame",
                unsigned(m_skipVp), unsigned(m_skipFp), unsigned(m_maxDraws));
    logInfo("gxm: alpha-tested draws %s (alpha_draw_order=%s)", m_drawSort ? "after opaque draws" : "in game order",
            m_drawSort ? "sorted" : "game");
    m_textureHeap = new GpuHeap("gxm_textures", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 16 * 1024 * 1024,
                                SCE_GXM_MEMORY_ATTRIB_RW);
    m_heapLock = new LwMutex("gxm_texture_heap");
    NFSU_DEV_SET(m_instantFree, NFSU_DEV_BOOL("texture_instant_free", true));
    const int heapMb = std::min(std::max(Config::getInt("texture_heap_mb", 0), 0), 1024);
    m_textureHeap->setLimit(uint32_t(heapMb) * 1024 * 1024);
    // Only with instant frees is a freed block known to be out of the GPU's hands.
    m_textureHeap->setShrink(m_instantFree && NFSU_DEV_BOOL("texture_heap_shrink", true));
    logInfo("gxm: released textures freed %s (texture_instant_free=%s); texture heap in 16 MB chunks, then smaller "
            "ones down to 1 MB for what CDRAM has left, capped at %d MB (texture_heap_mb, 0: no cap)", m_instantFree
            ? "at once when their frames are finished, and finished ones before an allocation fails"
            : "at a frame start after the next frame", m_instantFree ? "on" : "off", heapMb);
    m_bufferHeap = new GpuHeap("gxm_buffers", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, 8 * 1024 * 1024,
                               SCE_GXM_MEMORY_ATTRIB_READ);

    // Programs.
    auto registerProgram = [this](const char* name, SceGxmShaderPatcherId* id) -> const SceGxmProgram* {
        const unsigned char* data = binary(name);
        if (!data)
            return nullptr;
        const SceGxmProgram* program = reinterpret_cast<const SceGxmProgram*>(data);
        if (sceGxmProgramCheck(program) < 0 || sceGxmShaderPatcherRegisterProgram(m_patcher, program, id) < 0)
            fatal("shader %s cannot be registered", name);
        return program;
    };
    for (int v = 0; v < render::VpCount; ++v)
    {
        const render::VertexProgramInfo& info = render::vertexProgram(render::VertexProgram(v));
        VertexProgram& p = m_vertexPrograms[v];
        p.program = registerProgram(info.name, &p.id);
        if (!p.program)
        {
            logInfo("gxm: vertex program %s not linked (compile pending): its passes fall back", info.name);
            render::setMissing(render::VertexProgram(v));
            p.program = m_vertexPrograms[render::VpWorld].program;
            continue;
        }
        for (uint8_t i = 0; i < info.fieldCount; ++i)
            p.fields.push_back(sceGxmProgramFindParameterByName(p.program, info.fields[i].name));
        p.uniformBytes = uniformBytes(p.program, info.fields, p.fields);
    }
    for (int f = 0; f < render::FpCount; ++f)
    {
        const render::FragmentProgramInfo& info = render::fragmentProgram(render::FragmentProgram(f));
        for (int v = 0; v < Variants; ++v)
        {
            // Without the variant: the plain program (the layer never selects it).
            if ((v == 1 && f == render::FpClear) || (v >= 2 && !info.blendFactor))
            {
                m_fragmentPrograms[f][v] = m_fragmentPrograms[f][0];
                m_fragmentPrograms[f][v].owned = false;
                continue;
            }
            char name[64];
            snprintf(name, sizeof(name), "%s%s", info.name,
                     v == 1 ? "_atest" : render::factorSuffix(render::FactorOutput(v >= 2 ? v - 1 : 0)));
            FragmentProgram& p = m_fragmentPrograms[f][v];
            p.owned = true;
            p.program = registerProgram(name, &p.id);
            if (!p.program)
            {
                if (v != 0 && !render::missing(render::FragmentProgram(f)))
                    fatal("shader %s is not linked", name);
                if (v == 0)
                    logInfo("gxm: fragment program %s not linked (compile pending): its passes fall back", name);
                render::setMissing(render::FragmentProgram(f));
                p = m_fragmentPrograms[render::FpWorld][v < 2 ? v : 0];
                p.owned = false;
                continue;
            }
            for (uint8_t i = 0; i < info.fieldCount; ++i)
                p.fields.push_back(sceGxmProgramFindParameterByName(p.program, info.fields[i].name));
            p.uniformBytes = uniformBytes(p.program, info.fields, p.fields);
            for (uint8_t s = 0; s < info.samplers; ++s)
            {
                char sampler[8] = "uTex0";
                sampler[4] = char('0' + s);
                const SceGxmProgramParameter* parameter = sceGxmProgramFindParameterByName(p.program, sampler);
                p.units[s] = parameter ? sceGxmProgramParameterGetResourceIndex(parameter) : 0;
            }
        }
    }

    // 0..65535 for draws without indices.
    m_sequenceUid = sceKernelAllocMemBlock("gxm_sequence", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, 128 * 1024, nullptr);
    if (m_sequenceUid < 0)
        return false;
    void* sequence = nullptr;
    sceKernelGetMemBlockBase(m_sequenceUid, &sequence);
    sceGxmMapMemory(sequence, 128 * 1024, SCE_GXM_MEMORY_ATTRIB_READ);
    m_sequence = static_cast<uint16_t*>(sequence);
    for (uint32_t i = 0; i < 65536; ++i)
        m_sequence[i] = uint16_t(i);

    // Frame slots: host arrays, a GPU-read vertex arena each.
    m_arenaUid = sceKernelAllocMemBlock("gxm_frames", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                        Slots * SlotVertexBytes, nullptr);
    if (m_arenaUid < 0)
        return false;
    void* arena = nullptr;
    sceKernelGetMemBlockBase(m_arenaUid, &arena);
    sceGxmMapMemory(arena, Slots * SlotVertexBytes, SCE_GXM_MEMORY_ATTRIB_READ);
    static_assert(Slots <= stall::MaxSlots, "stall::Gxm slot notifications");
    for (uint32_t i = 0; i < Slots; ++i)
    {
        Slot& s = m_slots[i];
        s.scenes.resize(SlotScenes);
        s.draws.resize(SlotDraws);
        s.viewports.resize(SlotViewports);
        s.constants.resize(SlotConstants);
        s.vertices = static_cast<uint8_t*>(arena) + i * SlotVertexBytes;
        s.done.address = notifications + NotificationBase + i;
        *s.done.address = 0;
        s.done.value = 0;
        stall::g_gxm.slotDone[i].store(s.done.address, std::memory_order_relaxed);
    }

    render::TextureDesc black = { texture::FormatARGB8888, 1, false, false, 8, 8 };
    m_black = static_cast<Texture*>(createTexture(black));
    if (!m_black)
        return false;
    memset(m_black->data, 0, m_black->bytes);
    for (uint32_t i = 0; i < 64; ++i)
        static_cast<uint8_t*>(m_black->data)[i * 4 + 3] = 0xff;
    initLists();
    return true;
}

void FrameRenderer::shutdown()
{
    if (m_uploadLock)
    {
        LockGuard guard(*m_uploadLock);
        while (!m_uploading.empty())
            m_uploadDone->wait();
    }
    shutdownLists();
    for (auto& v : m_vertexCache)
        sceGxmShaderPatcherReleaseVertexProgram(m_patcher, v.second.program);
    for (auto& f : m_fragmentCache)
        sceGxmShaderPatcherReleaseFragmentProgram(m_patcher, f.second.program);
    for (auto& t : m_targets)
        sceGxmDestroyRenderTarget(t.second);
    for (VertexProgram& p : m_vertexPrograms)
        if (p.id)
            sceGxmShaderPatcherUnregisterProgram(m_patcher, p.id);
    for (int f = 0; f < render::FpCount; ++f)
        for (int v = 0; v < Variants; ++v)
            if (m_fragmentPrograms[f][v].owned && m_fragmentPrograms[f][v].id)
                sceGxmShaderPatcherUnregisterProgram(m_patcher, m_fragmentPrograms[f][v].id);
    delete m_textureHeap;
    delete m_bufferHeap;
    delete m_heapLock;
    delete m_uploadDone;
    delete m_uploadLock;
    if (m_arenaUid >= 0)
        sceKernelFreeMemBlock(m_arenaUid);
    if (m_sequenceUid >= 0)
        sceKernelFreeMemBlock(m_sequenceUid);
}

// ---- Frames (game thread) ------------------------------------------------------------------------------------------

uint32_t FrameRenderer::completedFrame()
{
    for (Slot& s : m_slots)
    {
        if (s.inFlight && *s.done.address == s.done.value)
        {
            s.inFlight = false;
            m_completed = std::max(m_completed, s.frame.index);
        }
    }
    stall::set(stall::g_gxm.completed, m_completed);
    return m_completed;
}

render::Frame* FrameRenderer::beginFrame(uint32_t width, uint32_t height)
{
    const uint32_t index = m_lastIndex + 1;
    Slot& s = m_slots[index % Slots];
    if (s.inFlight)
    {
        const bool timed = perf::timed() || limiter::on();
        const uint32_t start = timed ? perf::now() : 0;
        stall::mark(stall::g_gxm.frames, "fence_wait");
        sceGxmNotificationWait(&s.done);
        if (timed)
        {
            const uint32_t waited = perf::now() - start;
            perf::event(perf::EventFenceWaitUs, waited);
            perf::add(perf::FenceWait, waited);
            if (limiter::on())
                limiter::add(limiter::GameFenceWait, waited);
        }
        s.inFlight = false;
        stall::mark(stall::g_gxm.frames, "wait_recorders");
        // A worker whose job was taken over may still read the slot's draws.
        waitRecorders();
        m_completed = std::max(m_completed, s.frame.index);
    }
    completedFrame();
    {
        LwLockGuard guard(*m_heapLock);
        freePending();
    }

    // Back buffers of another size than the display are drawn offscreen and scaled when shown.
    const bool direct = width == 960 && height == 544;
    if (!direct && (!s.offscreen || s.offscreen->desc.width != width || s.offscreen->desc.height != height))
    {
        if (s.offscreen)
            release(s.offscreen);
        render::TextureDesc desc = { texture::FormatARGB8888, 1, false, true, uint16_t(width), uint16_t(height) };
        s.offscreen = static_cast<Texture*>(createTexture(desc));
    }

    m_lastIndex = index;
    stall::set(stall::g_gxm.begun, index);
    stall::mark(stall::g_gxm.frames, "recording");
    render::Frame& f = s.frame;
    f.index = index;
    f.width = uint16_t(width);
    f.height = uint16_t(height);
    f.scenes = s.scenes.data();
    f.sceneCount = 0;
    f.sceneCapacity = uint32_t(s.scenes.size());
    f.draws = s.draws.data();
    f.drawCount = 0;
    f.drawCapacity = uint32_t(s.draws.size());
    f.viewports = s.viewports.data();
    f.viewportCount = 0;
    f.viewportCapacity = uint32_t(s.viewports.size());
    f.constants = s.constants.data();
    f.constantCount = 0;
    f.constantCapacity = uint32_t(s.constants.size());
    f.vertices = static_cast<uint8_t*>(s.vertices);
    f.vertexBytes = 0;
    f.vertexCapacity = SlotVertexBytes;
    f.resolve = nullptr;
    f.commands = nullptr;
    f.commandCount = 0;
    f.uploads = false;
    f.msaaSamples = 1;
    return &f;
}

void FrameRenderer::submitted(render::Frame* frame, uint32_t vblanks)
{
    Slot& s = m_slots[frame->index % Slots];
    m_lastSubmitted = frame->index;
    s.vblanks = vblanks;
    s.done.value = frame->index;
    s.inFlight = true;
    stall::set(stall::g_gxm.slotValue[frame->index % Slots], frame->index);
    stall::set(stall::g_gxm.submitted, frame->index);
    stall::mark(stall::g_gxm.frames, "submitted");
    if (s.offscreen)
        s.offscreen->lastUse.store(frame->index, std::memory_order_relaxed);
}

void FrameRenderer::freePending()
{
    size_t kept = 0;
    for (size_t i = 0; i < m_pending.size(); ++i)
    {
        // A texture is freed once no queued upload touches it either.
        if (m_pending[i].after <= m_completed
            && (m_pending[i].kind != 0 || uploadDone(static_cast<Texture*>(m_pending[i].object)->lastTicket)))
        {
            if (m_pending[i].kind != 2)
                m_pendingBytes -= m_pending[i].bytes;
            destroy(m_pending[i].kind, m_pending[i].object);
        }
        else
            m_pending[kept++] = m_pending[i];
    }
    m_pending.resize(kept);
}

void FrameRenderer::destroy(int kind, void* object)
{
    if (kind == 0)
    {
        Texture* t = static_cast<Texture*>(object);
        m_textureHeap->free(t->data);
        delete t;
    }
    else if (kind == 1)
    {
        Depth* d = static_cast<Depth*>(object);
        m_textureHeap->free(d->data);
        m_textureHeap->free(d->stencil);
        delete d;
    }
    else
    {
        Buffer* b = static_cast<Buffer*>(object);
        m_bufferHeap->free(b->data);
        delete b;
    }
}

void FrameRenderer::queueRelease(int kind, void* object, uint32_t after, uint32_t bytes)
{
    m_pending.push_back(Pending{ kind, object, after, bytes });
    if (kind != 2)
    {
        m_pendingBytes += bytes;
        m_peakPending = std::max(m_peakPending, m_pendingBytes);
        ++m_queuedFrees;
    }
}

void FrameRenderer::release(render::TextureStorage* texture)
{
    if (!texture)
        return;
    Texture* t = static_cast<Texture*>(texture);
    LwLockGuard guard(*m_heapLock);
    if (!m_instantFree)
    {
        queueRelease(0, t, m_lastIndex + 1, t->bytes);
        return;
    }
    // lastUse is 0 for a texture never drawn; the frame being recorded is never finished yet.
    if (t->lastUse.load(std::memory_order_relaxed) <= completedFrame() && uploadDone(t->lastTicket))
    {
        destroy(0, t);
        ++m_instantFrees;
        return;
    }
    queueRelease(0, t, t->lastUse.load(std::memory_order_relaxed), t->bytes);
}

void FrameRenderer::release(render::DepthStorage* depth)
{
    if (!depth)
        return;
    Depth* d = static_cast<Depth*>(depth);
    LwLockGuard guard(*m_heapLock);
    const uint32_t bytes = m_textureHeap->sizeOf(d->data) + (d->stencil ? m_textureHeap->sizeOf(d->stencil) : 0);
    if (!m_instantFree)
    {
        queueRelease(1, d, m_lastIndex + 1, bytes);
        return;
    }
    if (d->lastUse.load(std::memory_order_relaxed) <= completedFrame())
    {
        destroy(1, d);
        ++m_instantFrees;
        return;
    }
    queueRelease(1, d, d->lastUse.load(std::memory_order_relaxed), bytes);
}

void FrameRenderer::release(render::BufferStorage* buffer)
{
    if (!buffer)
        return;
    LwLockGuard guard(*m_heapLock);
    queueRelease(2, buffer, m_lastIndex + 1, 0);
}

void* FrameRenderer::allocateTexture(uint32_t bytes, uint32_t alignment)
{
    LwLockGuard guard(*m_heapLock);
    void* p = m_textureHeap->allocate(bytes, alignment, true);
    if (!p && m_instantFree && !m_pending.empty())
    {
        completedFrame();
        freePending();
        p = m_textureHeap->allocate(bytes, alignment, true);
        if (!p && waitForPending())
        {
            freePending();
            p = m_textureHeap->allocate(bytes, alignment, true);
        }
        if (p)
            ++m_rescued;
    }
    if (!p)
    {
        m_textureHeap->failed(bytes);
        ++m_textureFailures;
        return nullptr;
    }
    m_peakUsed = std::max(m_peakUsed, m_textureHeap->usedBytes());
    return p;
}

void FrameRenderer::collectTextures()
{
    LwLockGuard guard(*m_heapLock);
    completedFrame();
    freePending();
    if (waitForPending())
        freePending();
    m_textureHeap->trimEmpty();
}

bool FrameRenderer::waitForPending()
{
    // The newest frame pending textures wait for that is submitted; the frame being recorded cannot finish.
    uint32_t frame = 0;
    for (const Pending& p : m_pending)
        if (p.kind != 2 && p.after > m_completed && p.after <= m_lastSubmitted)
            frame = std::max(frame, p.after);
    if (!frame)
        return false;
    // Polled with the heap unlocked and for at most 100 ms: the caller may hold the execution lock.
    m_heapLock->unlock();
    for (int i = 0; i < 100 && !finished(this, frame); ++i)
        sceKernelDelayThread(1000);
    m_heapLock->lock();
    return completedFrame() >= frame;
}

TextureMemory FrameRenderer::textureMemory()
{
    TextureMemory m = {};
    if (!m_heapLock)
        return m;
    LwLockGuard guard(*m_heapLock);
    m.totalBytes = m_textureHeap->totalBytes();
    m.usedBytes = m_textureHeap->usedBytes();
    m.pendingBytes = m_pendingBytes;
    for (const Pending& p : m_pending)
        m.pendingCount += p.kind != 2 ? 1 : 0;
    m.largestHole = m_textureHeap->largestHole();
    m.failures = m_textureFailures;
    m.instantFrees = m_instantFrees;
    m.queuedFrees = m_queuedFrees;
    m.rescued = m_rescued;
    m.peakUsedBytes = std::max(m_peakUsed, m.usedBytes);
    m.peakPendingBytes = std::max(m_peakPending, m.pendingBytes);
    m_peakUsed = m.usedBytes;
    m_peakPending = m.pendingBytes;
    return m;
}

// ---- Resources (game thread) ---------------------------------------------------------------------------------------

render::TextureStorage* FrameRenderer::createTexture(const render::TextureDesc& desc)
{
    Texture* t = new Texture();
    t->desc = desc;
    const SceGxmTextureFormat format = textureFormat(desc.format);
    if (desc.target && !desc.cube)
    {
        // Linear: rendered to as a colour surface, rows of 8-pixel multiples.
        const uint32_t strideInPixels = alignUp(desc.width, 8);
        t->stride = strideInPixels * 4;
        t->bytes = t->stride * desc.height;
        t->faceStride = t->bytes;
        t->data = allocateTexture(t->bytes, 256);
        if (!t->data)
        {
            delete t;
            return nullptr;
        }
        memset(t->data, 0, t->bytes);
        sceGxmTextureInitLinear(&t->texture, t->data, format, desc.width, desc.height, 1);
        sceGxmColorSurfaceInit(&t->surfaces[0], SCE_GXM_COLOR_FORMAT_A8R8G8B8, SCE_GXM_COLOR_SURFACE_LINEAR,
                               SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, desc.width,
                               desc.height, strideInPixels, t->data);
        return t;
    }
    if (desc.linear && !desc.cube && desc.levels == 1 && !texture::isBlock(desc.format))
    {
        // Streamed: linear rows (8-pixel multiples) the CPU rewrites with a row copy instead of a swizzle.
        t->stride = alignUp(desc.width, 8) * texture::unitBytes(desc.format);
        t->bytes = t->stride * desc.height;
        t->faceStride = t->bytes;
        t->data = allocateTexture(t->bytes, 256);
        if (!t->data)
        {
            delete t;
            return nullptr;
        }
        sceGxmTextureInitLinear(&t->texture, t->data, format, desc.width, desc.height, 1);
        return t;
    }
    t->stride = 0;
    t->faceStride = texture::faceStride(desc.format, desc.width, desc.height, desc.levels);
    t->bytes = texture::totalBytes(desc.format, desc.width, desc.height, desc.levels, desc.cube);
    t->data = allocateTexture(t->bytes, 256);
    if (!t->data)
    {
        delete t;
        return nullptr;
    }
    // Cube mip count 0: one level per face; any other count: faces with mips down to 1x1.
    if (desc.cube)
        sceGxmTextureInitCube(&t->texture, t->data, format, desc.width, desc.height, desc.levels > 1 ? desc.levels : 0);
    else
        sceGxmTextureInitSwizzled(&t->texture, t->data, format, desc.width, desc.height, desc.levels);
    // LOD bias 0, the Direct3D default: the field holds bias * 8 + 31.
    sceGxmTextureSetLodBias(&t->texture, 31);
    if (desc.target)
    {
        // Cube target: each face a swizzled colour surface, black until the game draws it.
        memset(t->data, 0, t->bytes);
        for (uint32_t face = 0; face < 6; ++face)
            sceGxmColorSurfaceInit(&t->surfaces[face], SCE_GXM_COLOR_FORMAT_A8R8G8B8, SCE_GXM_COLOR_SURFACE_SWIZZLED,
                                   SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, desc.width,
                                   desc.height, desc.width, static_cast<uint8_t*>(t->data) + face * t->faceStride);
    }
    return t;
}

void FrameRenderer::writeTexture(render::TextureStorage* storage, uint32_t face, uint32_t level, const void* data,
                                 uint32_t pitch)
{
    Texture* t = static_cast<Texture*>(storage);
    const render::TextureDesc& d = t->desc;
    if (level >= d.levels || face >= (d.cube ? 6u : 1u))
        return;
    settle(t);
    if (perf::timed())
    {
        perf::upload(perf::UploadLevels, 1);
        perf::upload(perf::UploadBytes, texture::levelBytes(d.format, d.width, d.height, level));
    }
    uint8_t* base = static_cast<uint8_t*>(t->data) + face * t->faceStride;
    noteAlpha(*t, level, data, pitch);
    if (t->stride)
    {
        const uint32_t rowBytes = d.width * texture::unitBytes(d.format);
        if (pitch == t->stride)
            memcpy(base, data, size_t(rowBytes) + size_t(t->stride) * (d.height - 1));
        else
            for (uint32_t y = 0; y < d.height; ++y)
                memcpy(base + y * t->stride, static_cast<const uint8_t*>(data) + y * pitch, rowBytes);
        return;
    }
    writeSwizzled(*t, face, level, data, pitch);
}

void FrameRenderer::readTexture(const render::TextureStorage* storage, uint32_t face, uint32_t level, void* data,
                                uint32_t pitch)
{
    const Texture* t = static_cast<const Texture*>(storage);
    const render::TextureDesc& d = t->desc;
    if (level >= d.levels || face >= (d.cube ? 6u : 1u))
        return;
    settle(t);
    const uint8_t* base = static_cast<const uint8_t*>(t->data) + face * t->faceStride;
    if (t->stride)
    {
        const uint32_t rowBytes = d.width * texture::unitBytes(d.format);
        for (uint32_t y = 0; y < d.height; ++y)
            memcpy(static_cast<uint8_t*>(data) + y * pitch, base + y * t->stride, rowBytes);
        return;
    }
    texture::unswizzleLevel(d.format, d.width, d.height, level,
                            base + texture::levelOffset(d.format, d.width, d.height, level), data, pitch);
}

void FrameRenderer::copyTexture(render::TextureStorage* to, const render::TextureStorage* from)
{
    Texture* dst = static_cast<Texture*>(to);
    const Texture* src = static_cast<const Texture*>(from);
    settle(dst);
    settle(src);
    if (dst->bytes == src->bytes)
    {
        memcpy(dst->data, src->data, dst->bytes);
        copyAlpha(*dst, *src);
    }
}

uint32_t FrameRenderer::writeTextureAsync(render::TextureStorage* storage, uint32_t face, uint32_t level,
                                          const void* data, uint32_t pitch)
{
    Texture* t = static_cast<Texture*>(storage);
    const render::TextureDesc& d = t->desc;
    if (level >= d.levels || face >= (d.cube ? 6u : 1u))
        return 0;
    if (t->stride)
    {
        writeTexture(storage, face, level, data, pitch);
        return 0;
    }
    if (perf::timed())
    {
        perf::upload(perf::UploadLevels, 1);
        perf::upload(perf::UploadBytes, texture::levelBytes(d.format, d.width, d.height, level));
    }
    t->uploads.fetch_add(1, std::memory_order_relaxed);
    const uint32_t ticket = queueUpload(new Upload{ this, 0, t->lastTicket, t, nullptr, face, level, pitch, data });
    t->lastTicket = ticket;
    return ticket;
}

uint32_t FrameRenderer::copyTextureAsync(render::TextureStorage* to, const render::TextureStorage* from)
{
    Texture* dst = static_cast<Texture*>(to);
    Texture* src = const_cast<Texture*>(static_cast<const Texture*>(from));
    if (dst->bytes != src->bytes)
        return 0;
    dst->uploads.fetch_add(1, std::memory_order_relaxed);
    const uint32_t ticket = queueUpload(new Upload{ this, 0, std::max(dst->lastTicket, src->lastTicket), dst, src,
                                                    0, 0, 0, nullptr });
    dst->lastTicket = ticket;
    src->lastTicket = ticket;
    return ticket;
}

uint32_t FrameRenderer::queueUpload(Upload* upload)
{
    const uint32_t ticket = m_nextTicket++;
    upload->ticket = ticket;
    stall::set(stall::g_gxm.uploadsQueued, ticket);
    if (perf::timed())
        perf::upload(perf::UploadAsync, 1);
    if (m_batching)
    {
        if (m_batch.empty())
            m_batchFirst.store(ticket, std::memory_order_release);
        m_batch.push_back(upload);
        return ticket;
    }
    {
        LockGuard guard(*m_uploadLock);
        m_uploading.push_back(ticket);
        m_firstUploading.store(m_uploading.front(), std::memory_order_relaxed);
    }
    jobs::post(&runUpload, upload);
    return ticket;
}

void FrameRenderer::beginUploads()
{
    m_batching = true;
}

void FrameRenderer::endUploads()
{
    m_batching = false;
    flushUploads();
}

void FrameRenderer::flushUploads()
{
    if (m_batch.empty())
        return;
    UploadBatch* batch = new UploadBatch{ this, {} };
    batch->uploads.swap(m_batch);
    {
        LockGuard guard(*m_uploadLock);
        for (const Upload* u : batch->uploads)
            m_uploading.push_back(u->ticket);
        m_firstUploading.store(m_uploading.front(), std::memory_order_relaxed);
    }
    m_batchFirst.store(0xffffffffu, std::memory_order_release);
    jobs::post(&runUploadBatch, batch);
}

void FrameRenderer::write(const Upload& u)
{
    if (u.from)
    {
        memcpy(u.to->data, u.from->data, u.to->bytes);
        copyAlpha(*u.to, *u.from);
    }
    else
    {
        writeSwizzled(*u.to, u.face, u.level, u.data, u.pitch);
        noteAlpha(*u.to, u.level, u.data, u.pitch);
    }
}

void FrameRenderer::runUpload(void* argument)
{
    Upload* u = static_cast<Upload*>(argument);
    FrameRenderer* self = u->owner;
    self->waitUpload(u->after);
    const bool timed = perf::timed();
    const uint32_t start = timed ? perf::now() : 0;
    self->write(*u);
    __asm__ volatile("dsb" : : : "memory");
    if (timed)
        perf::upload(perf::UploadWorkerUs, perf::now() - start);
    {
        LockGuard guard(*self->m_uploadLock);
        const auto it = std::lower_bound(self->m_uploading.begin(), self->m_uploading.end(), u->ticket);
        if (it != self->m_uploading.end() && *it == u->ticket)
            self->m_uploading.erase(it);
        self->m_firstUploading.store(self->m_uploading.empty() ? 0xffffffffu : self->m_uploading.front(),
                                     std::memory_order_release);
        u->to->uploads.fetch_sub(1, std::memory_order_release);
        stall::bump(stall::g_gxm.uploadsDone);
        self->m_uploadDone->broadcast();
    }
    delete u;
}

void FrameRenderer::runUploadBatch(void* argument)
{
    UploadBatch* batch = static_cast<UploadBatch*>(argument);
    FrameRenderer* self = batch->owner;
    const uint32_t first = batch->uploads.front()->ticket;
    const bool timed = perf::timed();
    uint32_t us = 0;
    for (const Upload* u : batch->uploads)
    {
        if (u->after < first)
            self->waitUpload(u->after);
        const uint32_t start = timed ? perf::now() : 0;
        self->write(*u);
        if (timed)
            us += perf::now() - start;
    }
    __asm__ volatile("dsb" : : : "memory");
    if (timed)
        perf::upload(perf::UploadWorkerUs, us);
    {
        LockGuard guard(*self->m_uploadLock);
        for (const Upload* u : batch->uploads)
        {
            const auto it = std::lower_bound(self->m_uploading.begin(), self->m_uploading.end(), u->ticket);
            if (it != self->m_uploading.end() && *it == u->ticket)
                self->m_uploading.erase(it);
            u->to->uploads.fetch_sub(1, std::memory_order_release);
            stall::bump(stall::g_gxm.uploadsDone);
        }
        self->m_firstUploading.store(self->m_uploading.empty() ? 0xffffffffu : self->m_uploading.front(),
                                     std::memory_order_release);
        self->m_uploadDone->broadcast();
    }
    for (Upload* u : batch->uploads)
        delete u;
    delete batch;
}

bool FrameRenderer::uploadDone(uint32_t ticket)
{
    if (ticket >= m_batchFirst.load(std::memory_order_acquire))
        return false;
    if (ticket < m_firstUploading.load(std::memory_order_acquire))
        return true;
    LockGuard guard(*m_uploadLock);
    return !std::binary_search(m_uploading.begin(), m_uploading.end(), ticket);
}

void FrameRenderer::waitUpload(uint32_t ticket)
{
    if (!ticket)
        return;
    // Only the game thread holds tickets of its open batch: safe to post it here.
    if (ticket >= m_batchFirst.load(std::memory_order_acquire))
        flushUploads();
    LockGuard guard(*m_uploadLock);
    while (std::binary_search(m_uploading.begin(), m_uploading.end(), ticket))
        m_uploadDone->wait();
}

void FrameRenderer::waitWrites(const render::TextureStorage* texture)
{
    if (!texture->uploads.load(std::memory_order_acquire))
        return;
    LockGuard guard(*m_uploadLock);
    while (texture->uploads.load(std::memory_order_acquire))
        m_uploadDone->wait();
}

void FrameRenderer::settle(const Texture* texture)
{
    if (uploadDone(texture->lastTicket))
        return;
    const bool timed = perf::timed();
    const uint32_t start = timed ? perf::now() : 0;
    const char* const stage = stall::g_gxm.frames.load(std::memory_order_relaxed);
    stall::mark(stall::g_gxm.frames, "upload_wait");
    waitUpload(texture->lastTicket);
    stall::mark(stall::g_gxm.frames, stage);
    if (timed)
    {
        perf::upload(perf::UploadGameWaits, 1);
        perf::upload(perf::UploadGameWaitUs, perf::now() - start);
    }
}

render::DepthStorage* FrameRenderer::createDepth(uint32_t width, uint32_t height, uint32_t samples)
{
    const uint32_t w = alignUp(width, SCE_GXM_TILE_SIZEX), h = alignUp(height, SCE_GXM_TILE_SIZEY);
    samples = samples == 4 ? 4 : samples == 2 ? 2 : 1;
    Depth* d = new Depth();
    d->width = uint16_t(width);
    d->height = uint16_t(height);
    // Depth plane only, no stencil plane; msaa keeps a value per sample (2x: twice the rows, 4x: also twice the row).
    const uint32_t bytes = w * h * 4 * samples;
    d->data = allocateTexture(bytes, SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT);
    d->stencil = nullptr;
    if (!d->data)
    {
        delete d;
        return nullptr;
    }
    sceGxmDepthStencilSurfaceInit(&d->surface, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
                                  w, d->data, nullptr);
    if (samples > 1)
    {
        d->samples = uint8_t(samples);
        sceGxmDepthStencilSurfaceInit(&d->msaaSurface, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M,
                                      SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, samples == 4 ? w * 2 : w, d->data, nullptr);
        if (m_depthLogs < 16)
        {
            ++m_depthLogs;
            logInfo("gxm: depth %ux%u for msaa %ux: %u bytes (%u without msaa)", unsigned(width), unsigned(height),
                    unsigned(samples), unsigned(bytes), unsigned(w * h * 4));
        }
    }
    return d;
}

render::BufferStorage* FrameRenderer::createBuffer(uint32_t size)
{
    Buffer* b = new Buffer();
    b->size = size;
    {
        LwLockGuard guard(*m_heapLock);
        b->data = m_bufferHeap->allocate(size, 16);
    }
    if (!b->data)
    {
        delete b;
        return nullptr;
    }
    return b;
}

// ---- Drawing (render thread) ---------------------------------------------------------------------------------------

const SceGxmTexture* FrameRenderer::offscreen(const render::Frame& frame) const
{
    const Slot& s = m_slots[frame.index % Slots];
    return s.direct || !s.offscreen ? nullptr : &s.offscreen->texture;
}

bool FrameRenderer::presentScene(const render::Frame& frame) const
{
    if (offscreen(frame))
        return true;
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        if (!frame.scenes[i].target.color)
            return false;
    }
    return true;
}

SceGxmRenderTarget* FrameRenderer::renderTarget(uint32_t width, uint32_t height, bool msaa)
{
    const uint32_t key = width << 16 | height | (msaa ? uint32_t(m_msaa) << 30 : 0);
    auto it = m_targets.find(key);
    if (it != m_targets.end())
        return it->second;
    SceGxmRenderTargetParams params;
    memset(&params, 0, sizeof(params));
    params.width = uint16_t(width);
    params.height = uint16_t(height);
    params.scenesPerFrame = 8;
    params.multisampleMode = msaa ? m_msaa : SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;
    SceGxmRenderTarget* target = nullptr;
    const int result = sceGxmCreateRenderTarget(&params, &target);
    if (result < 0)
        fatal("sceGxmCreateRenderTarget(%ux%u%s) failed: 0x%08x", width, height, msaa ? ", msaa" : "", result);
    if (msaa)
    {
        unsigned int driverBytes = 0, plainBytes = 0;
        sceGxmGetRenderTargetMemSize(&params, &driverBytes);
        params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
        sceGxmGetRenderTargetMemSize(&params, &plainBytes);
        logInfo("gxm: render target %ux%u msaa %ux: driver memory %u bytes (%u without msaa)", width, height,
                unsigned(m_msaaSamples), driverBytes, plainBytes);
    }
    m_targets[key] = target;
    return target;
}

const FrameRenderer::VertexEntry* FrameRenderer::vertexProgram(uint8_t program, uint8_t layoutId)
{
    const uint32_t key = uint32_t(program) << 8 | layoutId;
    auto it = m_vertexCache.find(key);
    if (it != m_vertexCache.end())
        return &it->second;
    const VertexProgram& p = m_vertexPrograms[program < render::VpCount ? program : 0];
    const render::VertexLayout& layout = render::layout(layoutId);
    SceGxmVertexAttribute attributes[render::UsageCount];
    unsigned int count = 0;
    for (uint32_t u = 0; u < render::UsageCount; ++u)
    {
        const SceGxmProgramParameter* parameter = sceGxmProgramFindParameterByName(p.program,
                                                                                    render::inputName(render::Usage(u)));
        if (!parameter)
            continue;
        const render::VertexElement* element = nullptr;
        for (uint32_t i = 0; i < layout.count; ++i)
        {
            if (layout.elements[i].usage == u)
                element = &layout.elements[i];
        }
        SceGxmVertexAttribute& a = attributes[count++];
        a.streamIndex = 0;
        a.regIndex = uint16_t(sceGxmProgramParameterGetResourceIndex(parameter));
        if (element)
        {
            a.offset = element->offset;
            a.format = uint8_t(attributeFormat(element->type, &a.componentCount));
        }
        else
        {
            // The layout lacks the input: read the first element (unused by the program's result).
            if (program != render::VpFilter || u < render::UsageTex1)
                logError("gxm: vertex program %s: layout %u has no %s",
                         render::vertexProgram(render::VertexProgram(program)).name, layoutId,
                         render::inputName(render::Usage(u)));
            a.offset = layout.count ? layout.elements[0].offset : 0;
            a.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
            a.componentCount = 1;
        }
    }
    SceGxmVertexStream stream = { layout.stride, SCE_GXM_INDEX_SOURCE_INDEX_16BIT };
    SceGxmVertexProgram* result = nullptr;
    const bool timed = perf::hitch() && !m_loadingPrograms;
    const uint32_t start = timed ? perf::now() : 0;
    const int error = sceGxmShaderPatcherCreateVertexProgram(m_patcher, p.id, attributes, count, &stream, 1, &result);
    if (timed)
    {
        perf::renderPart(perf::RenderProgramCreates, 1);
        perf::renderPart(perf::RenderProgramCreateUs, perf::now() - start);
    }
    if (error < 0)
        fatal("gxm: vertex program %s for layout %u: 0x%08x", render::vertexProgram(render::VertexProgram(program)).name,
              layoutId, error);
    VertexEntry& entry = m_vertexCache[key];
    entry.program = result;
    entry.source = uint8_t(program < render::VpCount ? program : 0);
    entry.drawSize = sceGxmGetPrecomputedDrawSize(result);
    entry.stateSize = sceGxmGetPrecomputedVertexStateSize(result);
    entry.uniformBytes = p.uniformBytes;
    return &entry;
}

// Fragment program cache key: the programs, the variant, blending, the colour mask and the multisample mode.
static uint64_t fragmentKey(uint8_t vertexProgram, uint8_t fragmentProgram, uint32_t variant, bool blend, uint8_t mask,
                            uint8_t srcBlend, uint8_t dstBlend, uint8_t blendOp, uint32_t multisample)
{
    return uint64_t(vertexProgram) | uint64_t(fragmentProgram) << 8 | uint64_t(variant) << 16 | uint64_t(blend) << 19
         | uint64_t(mask) << 20
         | (blend ? uint64_t(srcBlend) << 24 | uint64_t(dstBlend) << 32 | uint64_t(blendOp & 7) << 40 : 0)
         | uint64_t(multisample & 3) << 43;
}

const FrameRenderer::FragmentEntry* FrameRenderer::fragmentProgram(const render::DrawState& state, bool msaa)
{
    const uint64_t key = fragmentKey(state.vertexProgram, state.fragmentProgram, variant(state),
                                     state.blendEnable != 0, state.colorMask, state.srcBlend, state.dstBlend,
                                     state.blendOp, msaa ? uint32_t(m_msaa) : 0);
    auto it = m_fragmentCache.find(key);
    if (it != m_fragmentCache.end())
        return &it->second;
    return createFragment(key);
}

const FrameRenderer::FragmentEntry* FrameRenderer::createFragment(uint64_t key)
{
    const uint8_t vertexProgram = uint8_t(key);
    const uint8_t fragmentProgram = uint8_t(key >> 8);
    const uint32_t v = uint32_t(key >> 16) & 7;
    const bool blend = (key >> 19) & 1;
    // With blending the factors follow the mask's 4 bits.
    const uint8_t mask = blend ? uint8_t(key >> 20) & 0xf : uint8_t(key >> 20);
    const uint8_t srcBlend = uint8_t(key >> 24), dstBlend = uint8_t(key >> 32), op = uint8_t(key >> 40) & 7;
    const SceGxmMultisampleMode multisample = SceGxmMultisampleMode((key >> 43) & 3);
    const FragmentProgram& p = m_fragmentPrograms[fragmentProgram < render::FpCount ? fragmentProgram : 0][v];
    SceGxmBlendInfo info;
    memset(&info, 0, sizeof(info));
    info.colorMask = uint8_t(((mask & 1) ? SCE_GXM_COLOR_MASK_R : 0) | ((mask & 2) ? SCE_GXM_COLOR_MASK_G : 0)
                           | ((mask & 4) ? SCE_GXM_COLOR_MASK_B : 0) | ((mask & 8) ? SCE_GXM_COLOR_MASK_A : 0));
    static const SceGxmBlendFunc s_funcs[6] = { SCE_GXM_BLEND_FUNC_ADD, SCE_GXM_BLEND_FUNC_ADD, SCE_GXM_BLEND_FUNC_SUBTRACT,
                                                SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT, SCE_GXM_BLEND_FUNC_MIN,
                                                SCE_GXM_BLEND_FUNC_MAX };
    info.colorFunc = info.alphaFunc = s_funcs[op < 6 ? op : 0];
    uint8_t src = blend ? srcBlend : 2, dst = blend ? dstBlend : 1;
    if (src == 12 || src == 13)
    {
        // BOTHSRCALPHA, BOTHINVSRCALPHA.
        dst = src == 12 ? 6 : 5;
        src = src == 12 ? 5 : 6;
    }
    const SceGxmBlendFactor colorSrc = blendFactor(src), colorDst = blendFactor(dst);
    info.colorSrc = colorSrc;
    info.colorDst = colorDst;
    info.alphaSrc = alphaFactor(colorSrc);
    info.alphaDst = alphaFactor(colorDst);
    const bool plain = !blend && mask == 0xf;
    const SceGxmProgram* vertex = m_vertexPrograms[vertexProgram < render::VpCount ? vertexProgram : 0].program;
    SceGxmFragmentProgram* result = nullptr;
    const bool timed = perf::hitch() && !m_loadingPrograms;
    const uint32_t start = timed ? perf::now() : 0;
    const int error = sceGxmShaderPatcherCreateFragmentProgram(m_patcher, p.id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                               multisample, plain ? nullptr : &info, vertex, &result);
    if (timed)
    {
        perf::renderPart(perf::RenderProgramCreates, 1);
        perf::renderPart(perf::RenderProgramCreateUs, perf::now() - start);
    }
    if (error < 0)
        fatal("gxm: fragment program %u (vertex %u): 0x%08x", fragmentProgram, vertexProgram, error);
    FragmentEntry& entry = m_fragmentCache[key];
    entry.program = result;
    entry.stateSize = sceGxmGetPrecomputedFragmentStateSize(result);
    entry.uniformBytes = p.uniformBytes;
    return &entry;
}

namespace
{

// Program list file (loadPrograms, savePrograms): the header, the vertex entries, then the fragment cache keys.
const char ProgramListMagic[4] = { 'G', 'X', 'P', 'L' };
const uint32_t ProgramListVersion = 1;
const uint32_t ProgramListMaxEntries = 4096;

struct ProgramListHeader
{
    char        magic[4];
    uint32_t    version;
    uint32_t    vertexPrograms, fragmentPrograms, variants, layoutBytes;   // of the build that wrote it
    uint32_t    vertexCount, fragmentCount;
};

struct ProgramListVertex
{
    uint8_t                 program;
    uint8_t                 unused[3];
    render::VertexLayout    layout;
};

}

uint32_t FrameRenderer::loadPrograms(const char* path)
{
    FILE* file = fopen(path, "rb");
    if (!file)
        return 0;
    ProgramListHeader h;
    uint32_t created = 0;
    if (fread(&h, sizeof(h), 1, file) != 1 || memcmp(h.magic, ProgramListMagic, 4) != 0
        || h.version != ProgramListVersion || h.vertexPrograms != render::VpCount
        || h.fragmentPrograms != render::FpCount || h.variants != Variants
        || h.layoutBytes != sizeof(render::VertexLayout) || h.vertexCount > ProgramListMaxEntries
        || h.fragmentCount > ProgramListMaxEntries)
    {
        logInfo("gxm: %s is not a program list of this build, not used", path);
        fclose(file);
        return 0;
    }
    m_loadingPrograms = true;
    for (uint32_t i = 0; i < h.vertexCount; ++i)
    {
        ProgramListVertex e;
        if (fread(&e, sizeof(e), 1, file) != 1)
            break;
        if (e.program >= render::VpCount || e.layout.count > render::MaxVertexElements
            || render::missing(render::VertexProgram(e.program)))
            continue;
        const uint8_t layout = render::registerLayout(e.layout);
        if (m_vertexCache.find(uint32_t(e.program) << 8 | layout) == m_vertexCache.end())
        {
            vertexProgram(e.program, layout);
            ++created;
        }
    }
    for (uint32_t i = 0; i < h.fragmentCount; ++i)
    {
        uint64_t key;
        if (fread(&key, sizeof(key), 1, file) != 1)
            break;
        const uint8_t vertex = uint8_t(key), fragment = uint8_t(key >> 8);
        const uint32_t v = uint32_t(key >> 16) & 7;
        const bool blend = (key >> 19) & 1;
        const uint8_t mask = blend ? uint8_t(key >> 20) & 0xf : uint8_t(key >> 20);
        // Multisampled entries of the 2x and 4x modes too: the mode can change at run time.
        const uint32_t multisample = uint32_t(key >> 43) & 3;
        if (vertex >= render::VpCount || fragment >= render::FpCount || v >= Variants
            || render::missing(render::VertexProgram(vertex)) || render::missing(render::FragmentProgram(fragment))
            || multisample > uint32_t(SCE_GXM_MULTISAMPLE_4X)
            || fragmentKey(vertex, fragment, v, blend, mask, uint8_t(key >> 24), uint8_t(key >> 32),
                           uint8_t(key >> 40), multisample) != key
            || m_fragmentCache.find(key) != m_fragmentCache.end())
            continue;
        createFragment(key);
        ++created;
    }
    m_loadingPrograms = false;
    fclose(file);
    return created;
}

void FrameRenderer::savePrograms(const char* path) const
{
    FILE* file = fopen(path, "wb");
    if (!file)
    {
        logError("gxm: %s cannot be written", path);
        return;
    }
    ProgramListHeader h = {};
    memcpy(h.magic, ProgramListMagic, 4);
    h.version = ProgramListVersion;
    h.vertexPrograms = render::VpCount;
    h.fragmentPrograms = render::FpCount;
    h.variants = Variants;
    h.layoutBytes = sizeof(render::VertexLayout);
    h.vertexCount = uint32_t(std::min<size_t>(m_vertexCache.size(), ProgramListMaxEntries));
    h.fragmentCount = uint32_t(std::min<size_t>(m_fragmentCache.size(), ProgramListMaxEntries));
    bool ok = fwrite(&h, sizeof(h), 1, file) == 1;
    uint32_t written = 0;
    for (const auto& v : m_vertexCache)
    {
        if (written++ == h.vertexCount)
            break;
        ProgramListVertex e = {};
        e.program = uint8_t(v.first >> 8);
        e.layout = render::layout(uint8_t(v.first));
        ok = ok && fwrite(&e, sizeof(e), 1, file) == 1;
    }
    written = 0;
    for (const auto& f : m_fragmentCache)
    {
        if (written++ == h.fragmentCount)
            break;
        const uint64_t key = f.first;
        ok = ok && fwrite(&key, sizeof(key), 1, file) == 1;
    }
    ok = fclose(file) == 0 && ok;
    if (ok)
        logInfo("gxm: %u vertex and %u fragment programs listed in %s", unsigned(h.vertexCount),
                unsigned(h.fragmentCount), path);
    else
        logError("gxm: writing %s failed", path);
}

bool FrameRenderer::finished(void* self, uint32_t frameIndex)
{
    const Slot& s = static_cast<FrameRenderer*>(self)->m_slots[frameIndex % Slots];
    return *s.done.address >= frameIndex;
}

void FrameRenderer::preparePrograms(const render::Frame& frame)
{
    m_programs.resize(frame.drawCount);
    bool msaa = false;
    for (uint32_t i = 0; i < frame.sceneCount && !msaa; ++i)
        msaa = m_plan[i].msaa;
    if (msaa)
    {
        m_drawMsaa.assign(frame.drawCount, 0);
        for (uint32_t i = 0; i < frame.sceneCount; ++i)
        {
            const render::Scene& scene = frame.scenes[i];
            if (m_plan[m_plan[i].skipped ? m_plan[i].carriedTo : i].msaa && scene.firstDraw < frame.drawCount)
                memset(m_drawMsaa.data() + scene.firstDraw, 1, std::min(scene.drawCount,
                                                                        frame.drawCount - scene.firstDraw));
        }
    }
    DrawPrograms last = { nullptr, nullptr };
    const render::DrawState* previous = nullptr;
    bool previousMsaa = false;
    for (uint32_t i = 0; i < frame.drawCount; ++i)
    {
        const render::DrawState& s = frame.draws[i].state;
        const bool drawMsaa = msaa && m_drawMsaa[i];
        if (!frame.draws[i].count)
        {
            m_programs[i] = DrawPrograms{ nullptr, nullptr };
            continue;
        }
        // Programs looked up where the state differs from the previous draw's.
        if (!previous || s.vertexProgram != previous->vertexProgram || s.layout != previous->layout)
            last.vertex = vertexProgram(s.vertexProgram, s.layout);
        if (!previous || s.vertexProgram != previous->vertexProgram || s.fragmentProgram != previous->fragmentProgram
            || s.alphaTest != previous->alphaTest || s.factorOutput != previous->factorOutput
            || s.blendEnable != previous->blendEnable || s.srcBlend != previous->srcBlend
            || s.dstBlend != previous->dstBlend || s.colorMask != previous->colorMask
            || s.blendOp != previous->blendOp || drawMsaa != previousMsaa)
            last.fragment = fragmentProgram(s, drawMsaa);
        m_programs[i] = last;
        previous = &s;
        previousMsaa = drawMsaa;
    }
}

void FrameRenderer::upload(void* buffer, const render::UniformField* fields, uint8_t count,
                           const std::vector<const SceGxmProgramParameter*>& parameters, const float* constants)
{
    for (uint8_t i = 0; i < count && i < parameters.size(); ++i)
    {
        if (parameters[i])
            sceGxmSetUniformDataF(buffer, parameters[i], 0, fields[i].count, constants + fields[i].offset);
    }
}

// Texture of sampler i of a draw with the sampler's modes; black without a texture.
SceGxmTexture FrameRenderer::samplerTexture(const render::DrawState& s, uint32_t i) const
{
    const Texture* t = !s.textures[i] ? m_black
                     : s.textures[i] == m_backSentinel ? &m_backBuffer : static_cast<const Texture*>(s.textures[i]);
    SceGxmTexture texture = t->texture;
    const render::Sampler& sm = s.samplers[i];
    const bool pow2 = powerOfTwo(t->desc.width) && powerOfTwo(t->desc.height);
    auto mode = [pow2](uint8_t a) {
        return !pow2 || a == render::AddressClamp ? SCE_GXM_TEXTURE_ADDR_CLAMP
             : a == render::AddressMirror ? SCE_GXM_TEXTURE_ADDR_MIRROR : SCE_GXM_TEXTURE_ADDR_REPEAT;
    };
    if (!t->desc.cube)
    {
        sceGxmTextureSetUAddrMode(&texture, mode(sm.addressU));
        sceGxmTextureSetVAddrMode(&texture, mode(sm.addressV));
    }
    sceGxmTextureSetMinFilter(&texture, (sm.filter & 1) ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&texture, (sm.filter & 2) ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
    if (t->desc.levels > 1)
    {
        if (sm.mip == render::MipNone)
            sceGxmTextureSetMipmapCount(&texture, 1);
        sceGxmTextureSetMipFilter(&texture, sm.mip == render::MipLinear ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED
                                                                         : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
    }
    return texture;
}

// Viewport, depth test and culling of a draw, where they changed.
void FrameRenderer::setFixedState(ContextState& c, SceGxmContext* context, const render::Frame& frame,
                                  const render::DrawCall& d, bool first)
{
    const render::DrawState& s = d.state;
    if (first || d.viewport != c.viewport)
    {
        // Direct3D window coordinates moved by half a pixel, scaled by the scene's display scale.
        const render::Viewport& v = frame.viewports[d.viewport];
        const float sx = c.scaleX, sy = c.scaleY;
        sceGxmSetViewport(context, sx * (v.x + 0.5f * v.width + 0.5f), sx * 0.5f * v.width,
                          sy * (v.y + 0.5f * v.height + 0.5f), sy * -0.5f * v.height, v.minZ, v.maxZ - v.minZ);
        c.viewport = d.viewport;
    }
    if (first || s.depthEnable != c.last.depthEnable || s.depthWrite != c.last.depthWrite
        || s.depthFunc != c.last.depthFunc)
    {
        const uint32_t func = s.depthEnable && s.depthFunc >= 1 && s.depthFunc <= 8 ? s.depthFunc - 1u : 7u;
        sceGxmSetFrontDepthFunc(context, SceGxmDepthFunc(func << 22));
        sceGxmSetFrontDepthWriteEnable(context, s.depthEnable && s.depthWrite ? SCE_GXM_DEPTH_WRITE_ENABLED
                                                                               : SCE_GXM_DEPTH_WRITE_DISABLED);
    }
    if (first || s.cullMode != c.last.cullMode)
    {
        const SceGxmCullMode cw = m_cullSwap ? SCE_GXM_CULL_CCW : SCE_GXM_CULL_CW;
        const SceGxmCullMode ccw = m_cullSwap ? SCE_GXM_CULL_CW : SCE_GXM_CULL_CCW;
        sceGxmSetCullMode(context, s.cullMode == 2 ? cw : s.cullMode == 3 ? ccw : SCE_GXM_CULL_NONE);
    }
}

void FrameRenderer::draw(Recorder& r, const render::Frame& frame, uint32_t index)
{
    const render::DrawCall& d = frame.draws[index];
    if (!d.count)
        return;     // dropped by the layer
    if (!d.indices && d.count > 65536)
    {
        logError("gxm: draw of %u vertices without indices skipped", d.count);
        return;
    }
    if (r.precomputed)
    {
        drawPrecomputed(r, frame, index);
        return;
    }
    ContextState& c = r.state;
    SceGxmContext* context = r.context;
    const DrawPrograms& pr = m_programs[index];
    const render::DrawState& s = d.state;
    const bool first = !c.valid;
    const bool programs = first || pr.vertex != c.vertex || pr.fragment != c.fragment;
    if (programs)
    {
        sceGxmSetVertexProgram(context, pr.vertex->program);
        sceGxmSetFragmentProgram(context, pr.fragment->program);
    }
    const render::VertexProgramInfo& vinfo = render::vertexProgram(render::VertexProgram(s.vertexProgram));
    const render::FragmentProgramInfo& finfo = render::fragmentProgram(render::FragmentProgram(s.fragmentProgram));
    if ((programs || d.vertexConstants != c.vertexConstants) && vinfo.fieldCount && d.vertexConstants != render::NoConstants)
    {
        void* buffer = nullptr;
        sceGxmReserveVertexDefaultUniformBuffer(context, &buffer);
        upload(buffer, vinfo.fields, vinfo.fieldCount, m_vertexPrograms[s.vertexProgram].fields,
               frame.constants + d.vertexConstants);
    }
    if ((programs || d.fragmentConstants != c.fragmentConstants) && finfo.fieldCount
        && d.fragmentConstants != render::NoConstants)
    {
        void* buffer = nullptr;
        sceGxmReserveFragmentDefaultUniformBuffer(context, &buffer);
        upload(buffer, finfo.fields, finfo.fieldCount, m_fragmentPrograms[s.fragmentProgram][variant(s)].fields,
               frame.constants + d.fragmentConstants);
    }
    c.vertexConstants = d.vertexConstants;
    c.fragmentConstants = d.fragmentConstants;

    const FragmentProgram& fp = m_fragmentPrograms[s.fragmentProgram][variant(s)];
    for (uint8_t i = 0; i < finfo.samplers; ++i)
    {
        if (!programs && s.textures[i] == c.last.textures[i]
            && memcmp(&s.samplers[i], &c.last.samplers[i], sizeof(render::Sampler)) == 0)
            continue;
        const SceGxmTexture texture = samplerTexture(s, i);
        sceGxmSetFragmentTexture(context, fp.units[i], &texture);
    }
    setFixedState(c, context, frame, d, first);
    c.last = s;
    c.valid = true;
    c.vertex = pr.vertex;
    c.fragment = pr.fragment;

    sceGxmSetVertexStream(context, 0, d.vertices);
    sceGxmDraw(context, primitiveType(s.primitive), SCE_GXM_INDEX_FORMAT_U16, d.indices ? d.indices : m_sequence, d.count);
}

// The same draw with precomputed objects: a vertex state when the vertex constants change, a fragment state when
// its constants, textures or samplers change, and a precomputed draw. Their memory and uniform buffers come from the
// recorder's pool and live until the frame is finished.
void FrameRenderer::drawPrecomputed(Recorder& r, const render::Frame& frame, uint32_t index)
{
    const render::DrawCall& d = frame.draws[index];
    ContextState& c = r.state;
    SceGxmContext* context = r.context;
    FramePool& pool = r.data;
    const DrawPrograms& pr = m_programs[index];
    const render::DrawState& s = d.state;
    const bool first = !c.valid;
    const bool programs = first || pr.vertex != c.vertex || pr.fragment != c.fragment;
    if (programs)
    {
        sceGxmSetVertexProgram(context, pr.vertex->program);
        sceGxmSetFragmentProgram(context, pr.fragment->program);
    }
    const render::VertexProgramInfo& vinfo = render::vertexProgram(render::VertexProgram(s.vertexProgram));
    const render::FragmentProgramInfo& finfo = render::fragmentProgram(render::FragmentProgram(s.fragmentProgram));
    int error = 0;

    if (programs || d.vertexConstants != c.vertexConstants)
    {
        auto* state = static_cast<SceGxmPrecomputedVertexState*>(r.words.allocate(sizeof(SceGxmPrecomputedVertexState)));
        void* memory = pr.vertex->stateSize ? pool.allocate(pr.vertex->stateSize, 16) : nullptr;
        error = sceGxmPrecomputedVertexStateInit(state, pr.vertex->program, memory);
        if (!error && pr.vertex->uniformBytes)
        {
            void* buffer = pool.allocate(pr.vertex->uniformBytes, 16);
            if (vinfo.fieldCount && d.vertexConstants != render::NoConstants)
                upload(buffer, vinfo.fields, vinfo.fieldCount, m_vertexPrograms[s.vertexProgram].fields,
                       frame.constants + d.vertexConstants);
            else
                memset(buffer, 0, pr.vertex->uniformBytes);
            sceGxmPrecomputedVertexStateSetDefaultUniformBuffer(state, buffer);
        }
        if (!error)
            sceGxmSetPrecomputedVertexState(context, state);
    }

    bool textures = programs || d.fragmentConstants != c.fragmentConstants;
    for (uint8_t i = 0; i < finfo.samplers && !textures; ++i)
        textures = s.textures[i] != c.last.textures[i]
                || memcmp(&s.samplers[i], &c.last.samplers[i], sizeof(render::Sampler)) != 0;
    if (!error && textures)
    {
        const FragmentProgram& fp = m_fragmentPrograms[s.fragmentProgram][variant(s)];
        auto* state = static_cast<SceGxmPrecomputedFragmentState*>(r.words.allocate(sizeof(SceGxmPrecomputedFragmentState)));
        void* memory = pr.fragment->stateSize ? pool.allocate(pr.fragment->stateSize, 16) : nullptr;
        error = sceGxmPrecomputedFragmentStateInit(state, pr.fragment->program, memory);
        for (uint8_t i = 0; i < finfo.samplers && !error; ++i)
        {
            const SceGxmTexture texture = samplerTexture(s, i);
            error = sceGxmPrecomputedFragmentStateSetTexture(state, fp.units[i], &texture);
        }
        if (!error && pr.fragment->uniformBytes)
        {
            void* buffer = pool.allocate(pr.fragment->uniformBytes, 16);
            if (finfo.fieldCount && d.fragmentConstants != render::NoConstants)
                upload(buffer, finfo.fields, finfo.fieldCount, fp.fields, frame.constants + d.fragmentConstants);
            else
                memset(buffer, 0, pr.fragment->uniformBytes);
            sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer(state, buffer);
        }
        if (!error)
            sceGxmSetPrecomputedFragmentState(context, state);
    }

    SceGxmPrecomputedDraw* draw = nullptr;
    if (!error)
    {
        draw = static_cast<SceGxmPrecomputedDraw*>(r.words.allocate(sizeof(SceGxmPrecomputedDraw)));
        void* memory = pr.vertex->drawSize ? pool.allocate(pr.vertex->drawSize, 16) : nullptr;
        error = sceGxmPrecomputedDrawInit(draw, pr.vertex->program, memory);
        const void* streams[1] = { d.vertices };
        if (!error)
            error = sceGxmPrecomputedDrawSetAllVertexStreams(draw, streams);
    }
    if (error)
    {
        // Unbinds the precomputed states and draws this and the following draws immediately.
        logError("gxm: precomputed objects failed (0x%08x): immediate draws from now on", error);
        m_precomputed.store(false, std::memory_order_relaxed);
        r.precomputed = false;
        sceGxmSetPrecomputedVertexState(context, nullptr);
        sceGxmSetPrecomputedFragmentState(context, nullptr);
        c.reset(c.scaleX, c.scaleY);
        this->draw(r, frame, index);
        return;
    }
    c.vertexConstants = d.vertexConstants;
    c.fragmentConstants = d.fragmentConstants;
    setFixedState(c, context, frame, d, first);
    c.last = s;
    c.valid = true;
    c.vertex = pr.vertex;
    c.fragment = pr.fragment;

    sceGxmPrecomputedDrawSetParams(draw, primitiveType(s.primitive), SCE_GXM_INDEX_FORMAT_U16,
                                   d.indices ? d.indices : m_sequence, d.count);
    sceGxmDrawPrecomputed(context, draw);
}

void FrameRenderer::planFrame(const render::Frame& frame, bool stretchDirect)
{
    m_plan.resize(frame.sceneCount);
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        const render::Scene& scene = frame.scenes[i];
        bool used = scene.target.depth && scene.depthCleared;
        for (uint32_t n = 0; n < scene.drawCount && scene.target.depth && !used; ++n)
            used = frame.draws[scene.firstDraw + n].state.depthEnable != 0;
        m_plan[i].depthUsed = used;
        m_plan[i].skipped = false;
        m_plan[i].carriedTo = i;
    }

    // A scene of colour clears alone (the game clears the back buffer right after Present) moves to the start of
    // the next scene on its target when nothing in between samples that target and its depth clear is dead.
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        const render::Scene& scene = frame.scenes[i];
        bool clears = scene.drawCount != 0;
        for (uint32_t n = 0; n < scene.drawCount && clears; ++n)
        {
            const render::DrawState& s = frame.draws[scene.firstDraw + n].state;
            clears = s.vertexProgram == render::VpClear && !s.depthEnable;
        }
        if (!clears)
            continue;
        uint32_t next = i + 1;
        while (next < frame.sceneCount && (frame.scenes[next].target.color != scene.target.color
                                           || frame.scenes[next].target.face != scene.target.face))
            ++next;
        if (next == frame.sceneCount)
            continue;
        bool sampled = false;
        const render::TextureStorage* read = scene.target.color ? scene.target.color : m_backSentinel;
        for (uint32_t k = i + 1; k < next && !sampled; ++k)
        {
            const render::Scene& between = frame.scenes[k];
            for (uint32_t n = 0; n < between.drawCount && !sampled; ++n)
                for (const render::TextureStorage* t : frame.draws[between.firstDraw + n].state.textures)
                    sampled = sampled || t == read;
        }
        if (sampled)
            continue;
        if (scene.target.depth && scene.depthCleared)
        {
            bool dead = true;
            for (uint32_t k = i + 1; k < frame.sceneCount; ++k)
            {
                if (frame.scenes[k].target.depth == scene.target.depth && m_plan[k].depthUsed)
                {
                    dead = frame.scenes[k].depthCleared;
                    break;
                }
            }
            if (!dead)
                continue;
        }
        m_plan[i].skipped = true;
        m_plan[i].carriedTo = next;
        m_plan[i].depthUsed = false;
    }
    // A carried scene that is itself carried passes its clears on.
    for (uint32_t i = frame.sceneCount; i-- > 0;)
    {
        if (m_plan[i].skipped && m_plan[m_plan[i].carriedTo].skipped)
            m_plan[i].carriedTo = m_plan[m_plan[i].carriedTo].carriedTo;
    }

    // A stretched back buffer whose scenes need no depth is drawn to the display at its resolution: no offscreen
    // store and no scaling pass.
    Slot& slot = m_slots[frame.index % Slots];
    slot.direct = frame.width == 960 && frame.height == 544;
    if (!slot.direct && stretchDirect)
    {
        bool any = false, depth = false;
        for (uint32_t i = 0; i < frame.sceneCount; ++i)
        {
            if (frame.scenes[i].target.color || m_plan[i].skipped)
                continue;
            any = true;
            depth = depth || m_plan[i].depthUsed;
        }
        slot.direct = any && !depth;
    }
    planMsaa(frame);
}

// Setting msaa: a scene drawing with depth into a 2D target of the back buffer's size, or into the back buffer, is
// multisampled when its depth holds the mode's samples. A depth continued by a later scene keeps its sample layout,
// so both scenes are multisampled or neither is.
void FrameRenderer::planMsaa(const render::Frame& frame)
{
    m_msaaSamples = frame.msaaSamples == 4 ? 4 : frame.msaaSamples == 2 ? 2 : 1;
    m_msaa = m_msaaSamples == 4 ? SCE_GXM_MULTISAMPLE_4X
           : m_msaaSamples == 2 ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE;
    if (m_msaaSamples != m_msaaModeShown)
    {
        if (m_msaaSamples > 1)
            logInfo("gxm: msaa %ux from frame %u: scenes drawing with depth into a 2D target of the back buffer's size "
                    "or into the back buffer are multisampled, colour downscaled when stored", unsigned(m_msaaSamples),
                    unsigned(frame.index));
        else
            logInfo("gxm: msaa off from frame %u", unsigned(frame.index));
        m_msaaModeShown = m_msaaSamples;
        m_msaaSignature = 0;
        m_msaaFailed = false;
    }
    const Slot& slot = m_slots[frame.index % Slots];
    const auto nextDepthScene = [&](uint32_t i) {
        uint32_t j = i + 1;
        while (j < frame.sceneCount
               && (frame.scenes[j].target.depth != frame.scenes[i].target.depth || !m_plan[j].depthUsed))
            ++j;
        return j;
    };
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        const render::Scene& scene = frame.scenes[i];
        const render::TextureStorage* color = scene.target.color;
        const Depth* depth = static_cast<const Depth*>(scene.target.depth);
        const bool backBuffer = !color;
        const uint32_t w = backBuffer && slot.direct ? 960 : scene.width;
        const uint32_t h = backBuffer && slot.direct ? 544 : scene.height;
        m_plan[i].msaa = m_msaaSamples > 1 && !m_msaaFailed && m_plan[i].depthUsed
                      && (backBuffer || (color->desc.target && !color->desc.cube))
                      && scene.width == frame.width && scene.height == frame.height
                      && depth->samples == m_msaaSamples && depth->width >= w && depth->height >= h;
        m_plan[i].storesDepth = false;
    }
    for (bool changed = m_msaaSamples > 1; changed;)
    {
        changed = false;
        for (uint32_t i = 0; i < frame.sceneCount; ++i)
        {
            if (!m_plan[i].depthUsed)
                continue;
            const uint32_t j = nextDepthScene(i);
            if (j < frame.sceneCount && !frame.scenes[j].depthCleared && m_plan[i].msaa != m_plan[j].msaa)
            {
                m_plan[i].msaa = m_plan[j].msaa = false;
                changed = true;
            }
        }
    }
    uint32_t msaaScenes = 0, signature = 2166136261u;
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        if (!m_plan[i].depthUsed)
            continue;
        const uint32_t j = nextDepthScene(i);
        m_plan[i].storesDepth = j < frame.sceneCount && !frame.scenes[j].depthCleared;
        msaaScenes += m_plan[i].msaa;
        const uint32_t key = i << 8 | uint32_t(m_plan[i].msaa) << 1 | m_plan[i].storesDepth;
        signature = (signature ^ key ^ uint32_t(frame.scenes[i].width) << 16 ^ frame.scenes[i].height) * 16777619u;
    }
    if (m_msaaSamples == 1 || signature == m_msaaSignature || m_msaaLogs >= 32)
        return;
    m_msaaSignature = signature;
    ++m_msaaLogs;
    char line[900];
    int n = snprintf(line, sizeof(line), "gxm: msaa %ux, frame %u: %u of %u scenes multisampled, back buffer %ux%u %s "
                     "(t texture, b back buffer, z depth used, s depth stored):", unsigned(m_msaaSamples),
                     unsigned(frame.index), unsigned(msaaScenes), unsigned(frame.sceneCount), unsigned(frame.width),
                     unsigned(frame.height), slot.direct ? "on the display" : "offscreen");
    for (uint32_t i = 0; i < frame.sceneCount && n < int(sizeof(line)) - 40; ++i)
    {
        const render::Scene& scene = frame.scenes[i];
        if (m_plan[i].skipped)
            continue;
        n += snprintf(line + n, sizeof(line) - n, " %s%ux%u%s%s%s", scene.target.color ? "t" : "b",
                      unsigned(scene.width), unsigned(scene.height), m_plan[i].msaa ? m_msaaSamples == 4 ? "/4x" : "/2x"
                      : "", m_plan[i].depthUsed ? "z" : "", m_plan[i].storesDepth ? "s" : "");
    }
    logInfo("%s", line);
}

namespace
{

enum SortClass
{
    SortFixed,          // stays in place; no draw moves across it
    SortOpaque,
    SortDiscard
};

SortClass sortClass(const render::DrawCall& d, const render::TextureStorage* target)
{
    if (!d.count)
        return SortOpaque;
    const render::DrawState& s = d.state;
    // D3DCMP_LESS 2, D3DCMP_LESSEQUAL 4.
    if (s.vertexProgram == render::VpClear || s.blendEnable || s.colorMask != 0xf || !s.depthEnable || !s.depthWrite
        || (s.depthFunc != 2 && s.depthFunc != 4))
        return SortFixed;
    for (const render::TextureStorage* t : s.textures)
        if (t && t == target)
            return SortFixed;
    return s.alphaTest ? SortDiscard : SortOpaque;
}

// Hidden-surface pass of a draw: 2 alpha-tested, 1 blended, 0 opaque.
uint32_t hsrPass(const render::DrawState& s)
{
    return s.alphaTest ? 2 : s.blendEnable ? 1 : 0;
}

uint32_t flushTransitions(const render::Frame& frame, const uint32_t* order, uint32_t count)
{
    uint32_t transitions = 0, previous = 0;
    for (uint32_t n = 0; n < count; ++n)
    {
        const uint32_t pass = hsrPass(frame.draws[order[n]].state);
        transitions += previous == 2 && pass < 2;
        previous = pass;
    }
    return transitions;
}

uint64_t geometryBit(const void* vertices)
{
    return uint64_t(1) << ((uintptr_t(vertices) >> 4) & 63);
}

}

void FrameRenderer::sortScene(const render::Frame& frame, uint32_t scene)
{
    const ScenePlan& p = m_plan[scene];
    if (p.skipped || p.orderCount < 2)
        return;
    uint32_t* order = m_order.data() + p.firstOrder;
    const bool timed = perf::enabled();
    const uint32_t before = timed ? flushTransitions(frame, order, p.orderCount) : 0;
    if (m_drawSort && p.depthUsed)
    {
        const render::TextureStorage* target = frame.scenes[scene].target.color;
        m_held.clear();
        uint64_t held = 0;
        uint32_t out = 0;
        for (uint32_t n = 0; n < p.orderCount; ++n)
        {
            const uint32_t index = order[n];
            const render::DrawCall& d = frame.draws[index];
            const SortClass c = sortClass(d, target);
            if (c == SortDiscard)
            {
                m_held.push_back(index);
                held |= geometryBit(d.vertices);
                continue;
            }
            bool release = c == SortFixed;
            // The same geometry drawn again stays after its alpha-tested draw.
            if (!release && (held & geometryBit(d.vertices)))
            {
                for (uint32_t h : m_held)
                {
                    const render::DrawCall& e = frame.draws[h];
                    if (e.vertices == d.vertices && e.indices == d.indices && e.count == d.count)
                    {
                        release = true;
                        break;
                    }
                }
            }
            if (release)
            {
                for (uint32_t h : m_held)
                    order[out++] = h;
                m_held.clear();
                held = 0;
            }
            order[out++] = index;
        }
        for (uint32_t h : m_held)
            order[out++] = h;
        m_held.clear();
    }
    if (!timed)
        return;
    const uint32_t after = flushTransitions(frame, order, p.orderCount);
    SortStats& s = m_sortStats;
    s.allBefore += before;
    s.allAfter += after;
    if (p.orderCount > s.largest)
    {
        uint32_t discards = 0;
        for (uint32_t n = 0; n < p.orderCount; ++n)
            discards += frame.draws[order[n]].state.alphaTest ? 1 : 0;
        s.largest = p.orderCount;
        s.largestDiscards = discards;
        s.largestBefore = before;
        s.largestAfter = after;
    }
}

FrameRenderer::Shown FrameRenderer::drawFrame(const render::Frame& frame, const SceGxmColorSurface* display,
                                              SceGxmSyncObject* displaySync, bool stretchDirect)
{
    Slot& slot = m_slots[frame.index % Slots];
    // Hitch log: the time since the last mark goes to a part.
    const bool parts = perf::hitch();
    const bool limited = limiter::on();
    uint32_t mark = parts ? perf::now() : 0;
    const auto lap = [parts, &mark](perf::RenderPart part)
    {
        if (!parts)
            return;
        const uint32_t now = perf::now();
        perf::renderPart(part, now - mark);
        mark = now;
    };
    uint32_t limitMark = limited ? perf::now() : 0;
    const auto limit = [limited, &limitMark](limiter::Sum sum)
    {
        if (!limited)
            return;
        const uint32_t now = perf::now();
        limiter::add(sum, now - limitMark);
        limitMark = now;
    };
    if (parts)
    {
        perf::renderPart(perf::RenderSceneCount, frame.sceneCount);
        perf::renderPart(perf::RenderDrawCount, frame.drawCount);
    }
    stall::set(stall::g_gxm.drawing, frame.index);
    stall::set(stall::g_gxm.draws, frame.drawCount);
    stall::mark(stall::g_gxm.render, "wait_recorders");
    // A worker whose job the previous frame took over may still read the plan, order and programs changed below.
    waitRecorders();
    if (parts)
    {
        const uint32_t now = perf::now();
        perf::renderPart(perf::RenderLists, now - mark);
        perf::renderPart(perf::RenderListWait, now - mark);
        mark = now;
    }
    stall::mark(stall::g_gxm.render, "plan");
    planFrame(frame, stretchDirect);
    Shown shown = {};
    shown.presentScene = presentScene(frame);
    if (const SceGxmTexture* texture = offscreen(frame))
    {
        shown.offscreen = true;
        shown.texture = *texture;
    }
    shown.done = &slot.done;
    shown.vblanks = slot.vblanks;
    if (!slot.direct && slot.offscreen)
    {
        m_backBuffer.desc = slot.offscreen->desc;
        m_backBuffer.texture = slot.offscreen->texture;
    }
    else
    {
        const uint32_t w = slot.direct ? 960 : frame.width, h = slot.direct ? 544 : frame.height;
        m_backBuffer.desc = render::TextureDesc{ texture::FormatARGB8888, 1, false, true, uint16_t(w), uint16_t(h) };
        sceGxmTextureInitLinearStrided(&m_backBuffer.texture, sceGxmColorSurfaceGetData(display),
                                       SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, w, h,
                                       sceGxmColorSurfaceGetStrideInPixels(display) * 4);
    }
    const uint32_t sceneCount = frame.sceneCount;
    lap(perf::RenderPlan);
    stall::mark(stall::g_gxm.render, "programs");
    preparePrograms(frame);
    lap(perf::RenderPrograms);
    stall::mark(stall::g_gxm.render, "sort");
    m_main.data.begin(frame.index, &finished, this);
    m_main.words.reset();
    m_main.precomputed = m_precomputed.load(std::memory_order_relaxed);
    for (Worker* w : m_workers)
        w->recorder.precomputed = m_main.precomputed;
    const bool lastWritesDone = !shown.presentScene;

    // Where each scene draws, and its draws in order: the colour clears carried to it first.
    m_order.clear();
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        ScenePlan& p = m_plan[i];
        const render::Scene& scene = frame.scenes[i];
        p.color = display;
        p.sync = nullptr;
        p.width = scene.width;
        p.height = scene.height;
        p.scaleX = p.scaleY = 1.0f;
        if (scene.target.color)
        {
            const Texture* t = static_cast<const Texture*>(scene.target.color);
            p.color = &t->surfaces[scene.target.face < 6 ? scene.target.face : 0];
        }
        else if (slot.direct)
        {
            p.sync = displaySync;
            p.width = 960;
            p.height = 544;
            p.scaleX = 960.0f / float(scene.width);
            p.scaleY = 544.0f / float(scene.height);
        }
        else if (slot.offscreen)
        {
            p.color = &slot.offscreen->surfaces[0];
        }
        else
        {
            p.sync = displaySync;
        }
        if (p.msaa)
        {
            p.msaaColor = *p.color;
            sceGxmColorSurfaceSetScaleMode(&p.msaaColor, SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE);
            p.color = &p.msaaColor;
        }
        p.scaleX *= m_sceneScale;
        p.scaleY *= m_sceneScale;
        p.firstOrder = uint32_t(m_order.size());
        p.firstJob = p.jobCount = 0;
        if (!p.skipped)
        {
            for (uint32_t c = 0; c < i; ++c)
            {
                if (m_plan[c].skipped && m_plan[c].carriedTo == i)
                    for (uint32_t n = 0; n < frame.scenes[c].drawCount; ++n)
                        m_order.push_back(frame.scenes[c].firstDraw + n);
            }
            for (uint32_t n = 0; n < scene.drawCount; ++n)
            {
                const render::DrawState& ds = frame.draws[scene.firstDraw + n].state;
                if ((m_skipVp >> ds.vertexProgram & 1) || (m_skipFp >> ds.fragmentProgram & 1)
                    || (m_maxDraws && m_order.size() >= m_maxDraws))
                    continue;
                m_order.push_back(scene.firstDraw + n);
            }
        }
        p.orderCount = uint32_t(m_order.size()) - p.firstOrder;
        sortScene(frame, i);
    }
    if (perf::enabled())
    {
        SortStats& s = m_sortStats;
        s.draws += s.largest;
        s.discards += s.largestDiscards;
        s.before += s.largestBefore;
        s.after += s.largestAfter;
        s.largest = s.largestDiscards = s.largestBefore = s.largestAfter = 0;
        if (++s.frames == 300)
        {
            const double n = s.frames;
            logInfo("perf: draw sort %s, per frame: largest scene %.0f draws, %.1f alpha-tested, HSR flush "
                    "transitions %.1f -> %.1f; all scenes %.1f -> %.1f", m_drawSort ? "on" : "off", s.draws / n,
                    s.discards / n, s.before / n, s.after / n, s.allBefore / n, s.allAfter / n);
            s = SortStats();
        }
    }
    lap(perf::RenderPlan);
    limit(limiter::RenderPlan);
    stall::mark(stall::g_gxm.render, "record_lists");
    const bool lists = recordLists(frame);
    lap(perf::RenderLists);
    limit(limiter::RenderLists);
    stall::mark(stall::g_gxm.render, "upload_wait");
    if (frame.uploads)
        waitFrameUploads(frame);
    lap(perf::RenderUploadWait);
    stall::set(stall::g_gxm.scenes, sceneCount);
    if (perf::enabled())
        logScenes(frame);
    const uint32_t scenesStart = perf::enabled() ? perf::now() : 0;
    limitMark = limited ? perf::now() : 0;
    uint32_t beginUs = 0, endUs = 0;
    const bool sceneFinish = limited && limiter::sceneFinish();
    uint32_t sceneKinds[16][3], sceneKindCount = 0;
    if (sceneFinish)
        sceGxmFinish(m_context);

    for (uint32_t i = 0; i < sceneCount; ++i)
    {
        const ScenePlan& p = m_plan[i];
        if (p.skipped)
            continue;
        const render::Scene& scene = frame.scenes[i];
        // Scenes whose draws never test depth (filter passes, the HUD) neither load nor store it.
        SceGxmDepthStencilSurface depth;
        const Depth* d = m_plan[i].depthUsed ? static_cast<const Depth*>(scene.target.depth) : nullptr;
        if (d)
        {
            depth = p.msaa ? d->msaaSurface : d->surface;
            if (scene.depthCleared)
                sceGxmDepthStencilSurfaceSetBackgroundDepth(&depth, scene.clearDepth);
            sceGxmDepthStencilSurfaceSetForceLoadMode(&depth, scene.depthCleared ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED
                                                                                 : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
            // Stored only when the next scene of the frame using this depth continues with it.
            sceGxmDepthStencilSurfaceSetForceStoreMode(&depth, p.storesDepth
                                                                   ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED
                                                                   : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
        }
        stall::set(stall::g_gxm.scene, i);
        stall::mark(stall::g_gxm.render, "begin_scene");
        const uint32_t beginStart = limited ? perf::now() : 0;
        const int begun = sceGxmBeginScene(m_context, 0, renderTarget(p.width, p.height, p.msaa), nullptr, nullptr,
                                           p.sync, p.color, d ? &depth : nullptr);
        if (begun < 0 && p.msaa && !m_msaaFailed)
        {
            m_msaaFailed = true;
            logError("gxm: sceGxmBeginScene of a %ux%u msaa scene failed: 0x%08x; msaa off from the next frame until "
                     "the mode changes", unsigned(p.width), unsigned(p.height), begun);
        }
        if (limited)
            beginUs += perf::now() - beginStart;
        bool recorded = lists && p.jobCount;
        for (uint32_t j = 0; j < p.jobCount && recorded; ++j)
            recorded = m_jobs[p.firstJob + j].ok;
        stall::mark(stall::g_gxm.render, recorded ? "execute_lists" : "draw");
        if (recorded)
        {
            for (uint32_t j = 0; j < p.jobCount; ++j)
            {
                const int error = sceGxmExecuteCommandList(m_context, &m_jobs[p.firstJob + j].list);
                if (error < 0)
                    logError("gxm: sceGxmExecuteCommandList failed: 0x%08x", error);
            }
        }
        else
        {
            sceGxmSetViewportEnable(m_context, SCE_GXM_VIEWPORT_ENABLED);
            sceGxmSetRegionClip(m_context, SCE_GXM_REGION_CLIP_NONE, 0, 0, p.width - 1, p.height - 1);
            sceGxmSetTwoSidedEnable(m_context, SCE_GXM_TWO_SIDED_DISABLED);
            m_main.state.reset(p.scaleX, p.scaleY);
            for (uint32_t n = 0; n < p.orderCount; ++n)
                draw(m_main, frame, m_order[p.firstOrder + n]);
        }
        const bool last = i + 1 == sceneCount;
        stall::mark(stall::g_gxm.render, "end_scene");
        const uint32_t endStart = limited ? perf::now() : 0;
        sceGxmEndScene(m_context, nullptr, last && lastWritesDone && !m_checkPass ? &slot.done : nullptr);
        if (limited)
        {
            const uint32_t now = perf::now();
            endUs += now - endStart;
            if (!shown.firstEndUs)
                shown.firstEndUs = now | 1;
        }
        if (sceneFinish)
        {
            sceGxmFinish(m_context);
            const uint32_t kind[3] = { p.width, p.height, uint32_t(scene.target.color ? 0 : 1) };
            uint32_t ordinal = 0;
            for (uint32_t k = 0; k < sceneKindCount; ++k)
                ordinal += memcmp(sceneKinds[k], kind, sizeof(kind)) == 0;
            if (sceneKindCount < 16)
                memcpy(sceneKinds[sceneKindCount++], kind, sizeof(kind));
            limiter::scene(p.width, p.height, kind[2] != 0, ordinal, perf::now() - endStart, p.orderCount);
        }
    }
    stall::mark(stall::g_gxm.render, "scenes_done");
    // Unbinds the precomputed states.
    sceGxmSetPrecomputedVertexState(m_context, nullptr);
    sceGxmSetPrecomputedFragmentState(m_context, nullptr);
    lap(perf::RenderScenes);
    limit(limiter::RenderScenes);
    if (limited)
    {
        limiter::add(limiter::RenderBeginScene, beginUs);
        limiter::add(limiter::RenderEndScene, endUs);
    }
    if (perf::enabled())
        perf::add(perf::SceneSubmit, perf::now() - scenesStart);
    return shown;
}

// Timing log: scene sums and each new scene layout.
void FrameRenderer::logScenes(const render::Frame& frame)
{
    uint32_t scenes = 0, pixels = 0, displayPixels = 0, signature = 2166136261u;
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        const ScenePlan& p = m_plan[i];
        if (p.skipped)
            continue;
        const uint32_t area = uint32_t(float(p.width) * m_sceneScale) * uint32_t(float(p.height) * m_sceneScale);
        ++scenes;
        pixels += area;
        if (!frame.scenes[i].target.color)
            displayPixels += area;
        const uint32_t key = uint32_t(p.width) << 16 ^ p.height ^ (frame.scenes[i].target.color ? 0x80000000u : 0)
                           ^ (p.orderCount == 1 ? 0x8000u : 0) ^ (p.msaa ? 0x40000000u : 0);
        signature = (signature ^ key) * 16777619u;
    }
    perf::gpuWork(scenes, pixels, displayPixels);
    if (signature == m_sceneSignature || m_sceneLogs >= 24 || frame.index < m_sceneLogFrame)
        return;
    m_sceneSignature = signature;
    ++m_sceneLogs;
    m_sceneLogFrame = frame.index + 120;
    char line[900];
    int n = snprintf(line, sizeof(line), "perf: scenes of frame %u (target, draws, z = depth used):", unsigned(frame.index));
    for (uint32_t i = 0; i < frame.sceneCount && n < int(sizeof(line)) - 40; ++i)
    {
        const ScenePlan& p = m_plan[i];
        if (p.skipped)
            continue;
        const render::Scene& scene = frame.scenes[i];
        n += snprintf(line + n, sizeof(line) - n, " %s%ux%u", scene.target.color ? "t" : "display ", unsigned(p.width),
                      unsigned(p.height));
        if (scene.target.color && scene.target.face < 6 && static_cast<const Texture*>(scene.target.color)->desc.cube)
            n += snprintf(line + n, sizeof(line) - n, "/face%u", unsigned(scene.target.face));
        n += snprintf(line + n, sizeof(line) - n, ":%u%s%s", unsigned(p.orderCount), p.depthUsed ? "z" : "",
                      p.msaa ? m_msaaSamples == 4 ? "/4x" : "/2x" : "");
    }
    logInfo("%s", line);
}

void FrameRenderer::waitFrameUploads(const render::Frame& frame)
{
    const bool timed = perf::timed();
    for (uint32_t i = 0; i < frame.drawCount; ++i)
    {
        for (const render::TextureStorage* t : frame.draws[i].state.textures)
        {
            if (!t || !t->uploads.load(std::memory_order_acquire))
                continue;
            const uint32_t start = timed ? perf::now() : 0;
            waitWrites(t);
            if (timed)
            {
                perf::upload(perf::UploadRenderWaits, 1);
                perf::upload(perf::UploadRenderWaitUs, perf::now() - start);
            }
        }
    }
}

// ---- Command lists -------------------------------------------------------------------------------------------------

namespace
{

const uint32_t ListMinimumDraws = 96;   // smaller frames are drawn on the immediate context
const uint32_t JobMinimumDraws = 32;    // smallest range a list records
const uint32_t WorkerHostMem = 16 * 1024;
// 80: below the job workers (76), level with guest threads at THREAD_PRIORITY_HIGHEST, above the other guest threads.
const int ListWorkerPriority = Thread::PriorityHigh + 16;

}

// Memory callbacks of the deferred contexts: a whole chunk of the worker's pool, kept until every frame that used it
// is finished. The chunk a context writes into is its pool's current one.
void* FrameRenderer::listMemory(FramePool& pool, SceSize requested, SceSize* size)
{
    uint32_t granted = 0;
    void* memory = pool.take(requested, &granted);
    *size = granted;
    return memory;
}

void* FrameRenderer::vdmMemory(void* worker, SceSize requested, SceSize* size)
{
    return listMemory(static_cast<Worker*>(worker)->vdm, requested, size);
}

void* FrameRenderer::vertexMemory(void* worker, SceSize requested, SceSize* size)
{
    return listMemory(static_cast<Worker*>(worker)->vertex, requested, size);
}

void* FrameRenderer::fragmentMemory(void* worker, SceSize requested, SceSize* size)
{
    return listMemory(static_cast<Worker*>(worker)->fragment, requested, size);
}

bool FrameRenderer::createWorker(Worker& w)
{
    w.owner = this;
    w.hostMem = malloc(WorkerHostMem);
    SceGxmDeferredContextParams params;
    memset(&params, 0, sizeof(params));
    params.hostMem = w.hostMem;
    params.hostMemSize = WorkerHostMem;
    params.vdmCallback = &vdmMemory;
    params.vertexCallback = &vertexMemory;
    params.fragmentCallback = &fragmentMemory;
    params.callbackData = &w;
    uint32_t size = 0;
    params.vdmRingBufferMem = w.vdm.take(0, &size);
    params.vdmRingBufferMemSize = size;
    params.vertexRingBufferMem = w.vertex.take(0, &size);
    params.vertexRingBufferMemSize = size;
    params.fragmentRingBufferMem = w.fragment.take(0, &size);
    params.fragmentRingBufferMemSize = size;
    const int error = sceGxmCreateDeferredContext(&params, &w.recorder.context);
    if (error < 0)
    {
        logError("gxm: sceGxmCreateDeferredContext failed: 0x%08x", error);
        w.recorder.context = nullptr;
        return false;
    }
    return true;
}

void FrameRenderer::initLists()
{
    if (!NFSU_DEV_BOOL("gxm_lists", false))
    {
        logInfo("gxm: command lists off (gxm_lists=off)");
        return;
    }
    // The render thread records too and takes whatever the helpers have not claimed.
    std::vector<int> cores;
    if (cores::core3Waited())
        cores.push_back(3);
    if (NFSU_DEV_BOOL("gxm_core2", true))
        cores.push_back(2);
    for (size_t i = 0; i <= cores.size(); ++i)
    {
        Worker* w = new Worker();
        if (!createWorker(*w))
        {
            delete w;
            break;
        }
        w->index = uint8_t(m_workers.size());
        m_workers.push_back(w);
    }
    if (m_workers.empty())
    {
        logError("gxm: command lists off: no deferred context");
        return;
    }
    m_jobLock = new Mutex("gxm_jobs", false);
    m_jobsDone = new CondVar("gxm_jobs_done", *m_jobLock);
    m_wake = new Semaphore("gxm_wake", 0);
    static const char* const names[] = { "gxm_list_core3", "gxm_list_core2" };
    for (size_t i = 1; i < m_workers.size(); ++i)
    {
        Worker* w = m_workers[i];
        const int core = cores[i - 1];
        w->thread = new Thread(core == 3 ? names[0] : names[1], &workerEntry, w, ListWorkerPriority,
                               64 * 1024, Thread::Cpu0 << core);
        w->thread->start();
    }
    char where[32] = "render core";
    for (size_t i = 1; i < m_workers.size(); ++i)
        snprintf(where + strlen(where), sizeof(where) - strlen(where), ", core %d", cores[i - 1]);
    logInfo("gxm: command lists recorded by %u thread(s): %s", uint32_t(m_workers.size()), where);
}

void FrameRenderer::shutdownLists()
{
    if (m_jobLock)
    {
        {
            LockGuard guard(*m_jobLock);
            m_stop = true;
        }
        m_wake->post(int(m_workers.size()));
    }
    for (Worker* w : m_workers)
    {
        if (w->thread)
        {
            w->thread->join();
            delete w->thread;
        }
        if (w->recorder.context)
            sceGxmDestroyDeferredContext(w->recorder.context);
        free(w->hostMem);
        delete w;
    }
    m_workers.clear();
    delete m_jobsDone;
    delete m_jobLock;
    delete m_wake;
    m_jobsDone = nullptr;
    m_jobLock = nullptr;
    m_wake = nullptr;
}

int FrameRenderer::workerEntry(void* worker)
{
    Worker& w = *static_cast<Worker*>(worker);
    FrameRenderer& r = *w.owner;
    static const char* const tracked[] = { "gxm_list0", "gxm_list1", "gxm_list2", "gxm_list3" };
    if (perf::enabled())
        perf::trackThread(tracked[w.index & 3]);
    while (true)
    {
        stall::recorder(w.index, "wake_wait");
        r.m_wake->wait();
        {
            LockGuard guard(*r.m_jobLock);
            if (r.m_stop)
                break;
        }
        r.runJobs(w);
    }
    return 0;
}

void FrameRenderer::beginWorkerFrame(Worker& w, const render::Frame& frame)
{
    if (w.frame == frame.index)
        return;
    w.frame = frame.index;
    w.recorder.data.begin(frame.index, &finished, this);
    w.vdm.begin(frame.index, &finished, this);
    w.vertex.begin(frame.index, &finished, this);
    w.fragment.begin(frame.index, &finished, this);
    w.recorder.words.reset();
}

// Claims jobs until none is left. A worker woken late finds the current frame's jobs, or none.
void FrameRenderer::runJobs(Worker& w)
{
    while (true)
    {
        const render::Frame* frame;
        uint32_t index, generation;
        Job job;
        stall::recorder(w.index, "claim");
        {
            LockGuard guard(*m_jobLock);
            if (m_stop || m_nextJob >= m_jobs.size())
                return;
            index = m_nextJob++;
            stall::recorderJob(w.index, index);
            frame = m_jobFrame;
            generation = m_jobGeneration;
            m_jobs[index].state = JobClaimed;
            m_jobs[index].worker = w.index;
            job = m_jobs[index];
            w.abandon.store(false, std::memory_order_relaxed);
            ++m_recording;
        }
        stall::recorder(w.index, "record");
        beginWorkerFrame(w, *frame);
        SceGxmCommandList list;
        const uint32_t jobStart = w.index && limiter::on() ? perf::now() : 0;
        const bool ok = recordJob(w, *frame, job, &list);
        if (w.index && limiter::on())
            limiter::add(limiter::WorkerJobs, perf::now() - jobStart);
        stall::recorder(w.index, "finish");
        finishJob(w, generation, index, ok, list);
    }
}

void FrameRenderer::finishJob(Worker& w, uint32_t generation, uint32_t index, bool ok, const SceGxmCommandList& list)
{
    LockGuard guard(*m_jobLock);
    --m_recording;
    const bool stolen = m_stealing == index;
    if (w.index == 0 && stolen)
        m_stealing = NoJob;
    if (generation == m_jobGeneration && m_jobs[index].state != JobDone)
    {
        Job& job = m_jobs[index];
        job.ok = ok;
        job.list = list;
        job.state = JobDone;
        // The other recorder of the job stops: the claiming worker, or the render thread stealing it.
        if (stolen)
            m_workers[w.index == 0 ? job.worker : 0]->abandon.store(true, std::memory_order_relaxed);
        ++m_doneJobs;
        stall::set(stall::g_gxm.jobsDone, m_doneJobs);
    }
    m_jobsDone->broadcast();
}

void FrameRenderer::stealJobs(const render::Frame& frame)
{
    Worker& self = *m_workers[0];
    const bool timed = perf::hitch();
    while (true)
    {
        uint32_t index = NoJob, generation;
        Job job;
        stall::recorder(0, "steal");
        {
            LockGuard guard(*m_jobLock);
            if (m_doneJobs >= m_jobs.size())
                return;
            for (uint32_t i = 0; i < m_jobs.size() && index == NoJob; ++i)
            {
                if (m_jobs[i].state == JobClaimed && m_jobs[i].worker != 0)
                    index = i;
            }
            if (index == NoJob)
            {
                stall::recorder(0, "steal_wait");
                m_jobsDone->wait();
                continue;
            }
            m_stealing = index;
            stall::recorderJob(0, index);
            generation = m_jobGeneration;
            job = m_jobs[index];
            self.abandon.store(false, std::memory_order_relaxed);
            ++m_recording;
        }
        stall::recorder(0, "steal_record");
        beginWorkerFrame(self, frame);
        SceGxmCommandList list;
        const bool ok = recordJob(self, frame, job, &list);
        stall::recorder(0, "finish");
        if (timed)
        {
            perf::renderPart(perf::RenderListSteals, 1);
            if (!self.abandon.load(std::memory_order_relaxed))
                perf::renderPart(perf::RenderListStealsWon, 1);
        }
        finishJob(self, generation, index, ok, list);
    }
}

void FrameRenderer::waitRecorders()
{
    if (!m_jobLock)
        return;
    LockGuard guard(*m_jobLock);
    while (m_recording)
        m_jobsDone->wait();
}

bool FrameRenderer::recordJob(Worker& w, const render::Frame& frame, const Job& job, SceGxmCommandList* list)
{
    SceGxmContext* context = w.recorder.context;
    const ScenePlan& p = m_plan[job.scene];
    int error = sceGxmBeginCommandList(context);
    if (error < 0)
    {
        logError("gxm: sceGxmBeginCommandList failed: 0x%08x", error);
        return false;
    }
    // A list starts from a reset GPU state.
    sceGxmSetDefaultRegionClipAndViewport(context, p.width - 1, p.height - 1);
    sceGxmSetViewportEnable(context, SCE_GXM_VIEWPORT_ENABLED);
    sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_NONE, 0, 0, p.width - 1, p.height - 1);
    sceGxmSetTwoSidedEnable(context, SCE_GXM_TWO_SIDED_DISABLED);
    w.recorder.state.reset(p.scaleX, p.scaleY);
    if (!w.recorder.precomputed)
    {
        sceGxmSetPrecomputedVertexState(context, nullptr);
        sceGxmSetPrecomputedFragmentState(context, nullptr);
    }
    for (uint32_t n = job.begin; n < job.end; ++n)
    {
        // Every 8 draws: another recorder finished the job, this list is not used.
        if (((n - job.begin) & 7) == 7 && w.abandon.load(std::memory_order_relaxed))
            break;
        draw(w.recorder, frame, m_order[n]);
    }
    error = sceGxmEndCommandList(context, list);
    if (error < 0)
    {
        logError("gxm: sceGxmEndCommandList failed: 0x%08x", error);
        return false;
    }
    return true;
}

// Splits the frame's scenes into jobs and records them on the workers and this thread; false: draw immediately.
bool FrameRenderer::recordLists(const render::Frame& frame)
{
    if (m_workers.empty() || m_order.size() < ListMinimumDraws)
        return false;
    const uint32_t threads = uint32_t(m_workers.size());
    {
        LockGuard guard(*m_jobLock);
        m_jobs.clear();
        for (uint32_t i = 0; i < frame.sceneCount; ++i)
        {
            ScenePlan& p = m_plan[i];
            p.firstJob = uint32_t(m_jobs.size());
            if (p.skipped || !p.orderCount)
                continue;
            // About two ranges per thread.
            const uint32_t size = std::max(JobMinimumDraws, (p.orderCount + 2 * threads - 1) / (2 * threads));
            for (uint32_t begin = 0; begin < p.orderCount; begin += size)
            {
                const uint32_t end = std::min(p.orderCount, begin + size);
                m_jobs.push_back(Job{ i, p.firstOrder + begin, p.firstOrder + end, false, {}, JobFree, 0 });
            }
            p.jobCount = uint32_t(m_jobs.size()) - p.firstJob;
        }
        m_jobFrame = &frame;
        m_nextJob = 0;
        m_doneJobs = 0;
        ++m_jobGeneration;
        stall::set(stall::g_gxm.jobs, uint32_t(m_jobs.size()));
        stall::set(stall::g_gxm.jobsDone, 0);
        stall::set(stall::g_gxm.jobGeneration, m_jobGeneration);
    }
    if (threads > 1)
        m_wake->post(int(threads - 1));
    runJobs(*m_workers[0]);
    // A worker that stalls in its job (another thread runs on its core) is not waited for: this thread records the
    // job too, and the first to finish gives the job its list.
    const bool timed = perf::hitch(), limited = limiter::on();
    const uint32_t start = timed || limited ? perf::now() : 0;
    stealJobs(frame);
    if (timed)
        perf::renderPart(perf::RenderListWait, perf::now() - start);
    if (limited)
        limiter::add(limiter::RenderListWait, perf::now() - start);
    return true;
}

}}
