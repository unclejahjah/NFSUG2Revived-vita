#include <platform/render.h>
#include <platform/log.h>
#include <atomic>
#include <mutex>
#include <cstring>

namespace pf { namespace render {

namespace
{

#define FIELD(type, member, name) { name, uint16_t(offsetof(type, member) / sizeof(float)), \
                                    uint16_t(sizeof(type::member) / sizeof(float)) }
#define COUNT(fields) uint8_t(sizeof(fields) / sizeof(fields[0]))
#define FLOATS(type) uint16_t(sizeof(type) / sizeof(float))

const UniformField s_fixedVertex[] = {
    FIELD(FixedVertexConstants, wvp, "uWvp"),
    FIELD(FixedVertexConstants, screen, "uScreen"),
    FIELD(FixedVertexConstants, texMatrix, "uTexMatrix"),
    FIELD(FixedVertexConstants, texPad, "uTexPad"),
    FIELD(FixedVertexConstants, diffuseMask, "uDiffuseMask"),
    FIELD(FixedVertexConstants, factorMask, "uFactorMask"),
    FIELD(FixedVertexConstants, factor, "uFactor"),
};
const UniformField s_worldVertex[] = {
    FIELD(WorldVertexConstants, wvp, "uWvp"),
    FIELD(WorldVertexConstants, textureOffset, "uTextureOffset"),
    FIELD(WorldVertexConstants, viewZ, "uViewZ"),
    FIELD(WorldVertexConstants, fogValue, "uFogValue"),
};
const UniformField s_worldHeadlightVertex[] = {
    FIELD(WorldHeadlightVertexConstants, wvp, "uWvp"),
    FIELD(WorldHeadlightVertexConstants, headlightWvp, "uHeadlightWvp"),
    FIELD(WorldHeadlightVertexConstants, textureOffset, "uTextureOffset"),
};
const UniformField s_worldReflectVertex[] = {
    FIELD(WorldReflectVertexConstants, wvp, "uWvp"),
};
const UniformField s_worldHeadlightReflectVertex[] = {
    FIELD(WorldHeadlightReflectVertexConstants, wvp, "uWvp"),
    FIELD(WorldHeadlightReflectVertexConstants, headlightWvp, "uHeadlightWvp"),
};
const UniformField s_worldReflectFogVertex[] = {
    FIELD(WorldReflectFogVertexConstants, wvp, "uWvp"),
    FIELD(WorldReflectFogVertexConstants, viewZ, "uViewZ"),
    FIELD(WorldReflectFogVertexConstants, fogValue, "uFogValue"),
};
const UniformField s_worldHeadlightFogVertex[] = {
    FIELD(WorldHeadlightFogVertexConstants, wvp, "uWvp"),
    FIELD(WorldHeadlightFogVertexConstants, headlightWvp, "uHeadlightWvp"),
    FIELD(WorldHeadlightFogVertexConstants, textureOffset, "uTextureOffset"),
    FIELD(WorldHeadlightFogVertexConstants, viewZ, "uViewZ"),
    FIELD(WorldHeadlightFogVertexConstants, fogValue, "uFogValue"),
};
const UniformField s_worldHeadlightReflectFogVertex[] = {
    FIELD(WorldHeadlightReflectFogVertexConstants, wvp, "uWvp"),
    FIELD(WorldHeadlightReflectFogVertexConstants, headlightWvp, "uHeadlightWvp"),
    FIELD(WorldHeadlightReflectFogVertexConstants, viewZ, "uViewZ"),
    FIELD(WorldHeadlightReflectFogVertexConstants, fogValue, "uFogValue"),
};
const UniformField s_worldBoneVertex[] = {
    FIELD(WorldBoneVertexConstants, wvp, "uWvp"),
    FIELD(WorldBoneVertexConstants, localEyePos, "uLocalEyePos"),
    FIELD(WorldBoneVertexConstants, harmonic, "uHarmonic"),
    FIELD(WorldBoneVertexConstants, diffuseMin, "uDiffuseMin"),
    FIELD(WorldBoneVertexConstants, diffuseRange, "uDiffuseRange"),
    FIELD(WorldBoneVertexConstants, bones, "uBones"),
    FIELD(WorldBoneVertexConstants, viewZ, "uViewZ"),
    FIELD(WorldBoneVertexConstants, focalRange, "uFocalRange"),
};
const UniformField s_carVertex[] = {
    FIELD(CarVertexConstants, wvp, "uWvp"),
    FIELD(CarVertexConstants, worldView, "uWorldView"),
    FIELD(CarVertexConstants, localEyePos, "uLocalEyePos"),
    FIELD(CarVertexConstants, harmonic, "uHarmonic"),
    FIELD(CarVertexConstants, diffuseMin, "uDiffuseMin"),
    FIELD(CarVertexConstants, diffuseRange, "uDiffuseRange"),
    FIELD(CarVertexConstants, specularMin, "uSpecularMin"),
    FIELD(CarVertexConstants, specularRange, "uSpecularRange"),
    FIELD(CarVertexConstants, powers, "uPowers"),
};

const UniformField s_fixedFragment[] = {
    FIELD(FixedFragmentConstants, alphaTest, "uAlphaTest"),
    FIELD(FixedFragmentConstants, textureMask, "uTextureMask"),
    FIELD(FixedFragmentConstants, scale, "uScale"),
    FIELD(FixedFragmentConstants, addMask, "uAddMask"),
};
const UniformField s_worldFragment[] = {
    FIELD(WorldFragmentConstants, alphaTest, "uAlphaTest"),
    FIELD(WorldFragmentConstants, fogColor, "uFogColor"),
};
const UniformField s_skyMorphFragment[] = {
    FIELD(SkyMorphFragmentConstants, alphaTest, "uAlphaTest"),
    FIELD(SkyMorphFragmentConstants, fogColor, "uFogColor"),
    FIELD(SkyMorphFragmentConstants, morph, "uMorph"),
};
const UniformField s_worldHeadlightFragment[] = {
    FIELD(WorldHeadlightFragmentConstants, alphaTest, "uAlphaTest"),
    FIELD(WorldHeadlightFragmentConstants, headlightColor, "uHeadlightColor"),
};
const UniformField s_worldHeadlightFogFragment[] = {
    FIELD(WorldHeadlightFogFragmentConstants, alphaTest, "uAlphaTest"),
    FIELD(WorldHeadlightFogFragmentConstants, headlightColor, "uHeadlightColor"),
    FIELD(WorldHeadlightFogFragmentConstants, fogColor, "uFogColor"),
};
const UniformField s_carFragment[] = {
    FIELD(CarFragmentConstants, alphaTest, "uAlphaTest"),
    FIELD(CarFragmentConstants, envmapMin, "uEnvmapMin"),
    FIELD(CarFragmentConstants, envmapRange, "uEnvmapRange"),
};
const UniformField s_filterFragment[] = {
    FIELD(FilterFragmentConstants, alphaTest, "uAlphaTest"),
    FIELD(FilterFragmentConstants, weights, "uWeights"),
    FIELD(FilterFragmentConstants, blendFactor, "uBlendFactor"),
};
const UniformField s_screenFragment[] = {
    FIELD(ScreenFragmentConstants, alphaTest, "uAlphaTest"),
    FIELD(ScreenFragmentConstants, param0, "uParam0"),
    FIELD(ScreenFragmentConstants, param1, "uParam1"),
    FIELD(ScreenFragmentConstants, blendFactor, "uBlendFactor"),
};
#define BLENDFACTOR(type) uint16_t(offsetof(type, blendFactor) / sizeof(float))
const UniformField s_clearFragment[] = {
    FIELD(ClearFragmentConstants, color, "uColor"),
};

const VertexProgramInfo s_vertexPrograms[VpCount] = {
    { "ff_v", FLOATS(FixedVertexConstants), COUNT(s_fixedVertex), s_fixedVertex },
    { "ff_screen_v", FLOATS(FixedVertexConstants), COUNT(s_fixedVertex), s_fixedVertex },
    { "world_v", FLOATS(WorldVertexConstants), COUNT(s_worldVertex), s_worldVertex },
    { "world_proj_v", FLOATS(WorldVertexConstants), COUNT(s_worldVertex), s_worldVertex },
    { "worldheadlight_v", FLOATS(WorldHeadlightVertexConstants), COUNT(s_worldHeadlightVertex), s_worldHeadlightVertex },
    { "worldreflect_v", FLOATS(WorldReflectVertexConstants), COUNT(s_worldReflectVertex), s_worldReflectVertex },
    { "worldheadlightreflect_v", FLOATS(WorldHeadlightReflectVertexConstants), COUNT(s_worldHeadlightReflectVertex),
      s_worldHeadlightReflectVertex },
    { "worldbone_v", FLOATS(WorldBoneVertexConstants), COUNT(s_worldBoneVertex), s_worldBoneVertex },
    { "car_v", FLOATS(CarVertexConstants), COUNT(s_carVertex), s_carVertex },
    { "filter_v", 0, 0, nullptr },
    { "clearz_v", 0, 0, nullptr },
    { "worldreflect_fog_v", FLOATS(WorldReflectFogVertexConstants), COUNT(s_worldReflectFogVertex),
      s_worldReflectFogVertex },
    { "worldheadlight_fog_v", FLOATS(WorldHeadlightFogVertexConstants), COUNT(s_worldHeadlightFogVertex),
      s_worldHeadlightFogVertex },
    { "worldheadlightreflect_fog_v", FLOATS(WorldHeadlightReflectFogVertexConstants),
      COUNT(s_worldHeadlightReflectFogVertex), s_worldHeadlightReflectFogVertex },
};

const FragmentProgramInfo s_fragmentPrograms[FpCount] = {
    { "ff_color_f", FLOATS(FixedFragmentConstants), COUNT(s_fixedFragment), s_fixedFragment, 0, 0 },
    { "ff_tex_f", FLOATS(FixedFragmentConstants), COUNT(s_fixedFragment), s_fixedFragment, 1, 0 },
    { "ff_proj_f", FLOATS(FixedFragmentConstants), COUNT(s_fixedFragment), s_fixedFragment, 1, 0 },
    { "world_f", FLOATS(WorldFragmentConstants), COUNT(s_worldFragment), s_worldFragment, 1, 0 },
    { "world_proj_f", FLOATS(WorldFragmentConstants), COUNT(s_worldFragment), s_worldFragment, 1, 0 },
    { "worldheadlight_f", FLOATS(WorldHeadlightFragmentConstants), COUNT(s_worldHeadlightFragment),
      s_worldHeadlightFragment, 3, 0 },
    { "worldreflect_f", FLOATS(WorldFragmentConstants), COUNT(s_worldFragment), s_worldFragment, 2, 0 },
    { "worldheadlightreflect_f", FLOATS(WorldHeadlightFragmentConstants), COUNT(s_worldHeadlightFragment),
      s_worldHeadlightFragment, 4, 0 },
    { "worldbone_f", FLOATS(WorldFragmentConstants), COUNT(s_worldFragment), s_worldFragment, 1, 0 },
    { "car_f", FLOATS(CarFragmentConstants), COUNT(s_carFragment), s_carFragment, 2, 0 },
    { "filter1_f", FLOATS(FilterFragmentConstants), COUNT(s_filterFragment), s_filterFragment, 1,
      BLENDFACTOR(FilterFragmentConstants) },
    { "filter2_f", FLOATS(FilterFragmentConstants), COUNT(s_filterFragment), s_filterFragment, 2,
      BLENDFACTOR(FilterFragmentConstants) },
    { "filter3_f", FLOATS(FilterFragmentConstants), COUNT(s_filterFragment), s_filterFragment, 3,
      BLENDFACTOR(FilterFragmentConstants) },
    { "filter4_f", FLOATS(FilterFragmentConstants), COUNT(s_filterFragment), s_filterFragment, 4,
      BLENDFACTOR(FilterFragmentConstants) },
    { "clear_f", FLOATS(ClearFragmentConstants), COUNT(s_clearFragment), s_clearFragment, 0, 0 },
    { "skymorph_f", FLOATS(SkyMorphFragmentConstants), COUNT(s_skyMorphFragment), s_skyMorphFragment, 2, 0 },
    { "worldreflect_fog_f", FLOATS(WorldFragmentConstants), COUNT(s_worldFragment), s_worldFragment, 2, 0 },
    { "worldheadlight_fog_f", FLOATS(WorldHeadlightFogFragmentConstants), COUNT(s_worldHeadlightFogFragment),
      s_worldHeadlightFogFragment, 3, 0 },
    { "worldheadlightreflect_fog_f", FLOATS(WorldHeadlightFogFragmentConstants), COUNT(s_worldHeadlightFogFragment),
      s_worldHeadlightFogFragment, 4, 0 },
    { "overbright_f", FLOATS(ScreenFragmentConstants), COUNT(s_screenFragment), s_screenFragment, 1,
      BLENDFACTOR(ScreenFragmentConstants) },
    { "screengain_f", FLOATS(ScreenFragmentConstants), COUNT(s_screenFragment), s_screenFragment, 3,
      BLENDFACTOR(ScreenFragmentConstants) },
    { "screentint_f", FLOATS(ScreenFragmentConstants), COUNT(s_screenFragment), s_screenFragment, 1,
      BLENDFACTOR(ScreenFragmentConstants) },
    { "screenramp_f", FLOATS(ScreenFragmentConstants), COUNT(s_screenFragment), s_screenFragment, 2,
      BLENDFACTOR(ScreenFragmentConstants) },
    { "raindrop_f", FLOATS(ScreenFragmentConstants), COUNT(s_screenFragment), s_screenFragment, 2,
      BLENDFACTOR(ScreenFragmentConstants) },
};

const char* const s_inputNames[UsageCount] = {
    "aPosition", "aNormal", "aColor", "aTex0", "aTex1", "aTex2", "aTex3", "aWeight", "aIndex"
};

enum
{
    MaxLayouts = 64
};

VertexLayout            s_layouts[MaxLayouts];
bool                    s_missingVertex[VpCount];
bool                    s_missingFragment[FpCount];
std::atomic<uint32_t>   s_layoutCount{0};

}

const VertexProgramInfo& vertexProgram(VertexProgram program)
{
    return s_vertexPrograms[program < VpCount ? program : 0];
}

const FragmentProgramInfo& fragmentProgram(FragmentProgram program)
{
    return s_fragmentPrograms[program < FpCount ? program : 0];
}

void setMissing(VertexProgram program)
{
    if (program < VpCount)
        s_missingVertex[program] = true;
}

void setMissing(FragmentProgram program)
{
    if (program < FpCount)
        s_missingFragment[program] = true;
}

bool missing(VertexProgram program)
{
    return program >= VpCount || s_missingVertex[program];
}

bool missing(FragmentProgram program)
{
    return program >= FpCount || s_missingFragment[program];
}

const char* factorSuffix(FactorOutput output)
{
    static const char* const s_suffixes[FactorCount] = { "", "_bfcolor", "_bfalpha", "_bfscale" };
    return output < FactorCount ? s_suffixes[output] : "";
}

const char* inputName(Usage usage)
{
    return usage < UsageCount ? s_inputNames[usage] : "";
}

uint8_t registerLayout(const VertexLayout& layout)
{
    VertexLayout key;
    memset(&key, 0, sizeof(key));
    key.stride = layout.stride;
    key.count = layout.count < MaxVertexElements ? layout.count : uint8_t(MaxVertexElements);
    key.positionT = layout.positionT;
    memcpy(key.elements, layout.elements, key.count * sizeof(VertexElement));
    const uint32_t count = s_layoutCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (memcmp(&s_layouts[i], &key, sizeof(key)) == 0)
            return uint8_t(i);
    }
    static std::mutex s_lock;
    std::lock_guard<std::mutex> guard(s_lock);
    const uint32_t now = s_layoutCount.load(std::memory_order_relaxed);
    for (uint32_t i = count; i < now; ++i)
    {
        if (memcmp(&s_layouts[i], &key, sizeof(key)) == 0)
            return uint8_t(i);
    }
    if (now == MaxLayouts)
        fatal("too many vertex layouts");
    s_layouts[now] = key;
    s_layoutCount.store(now + 1, std::memory_order_release);
    return uint8_t(now);
}

const VertexLayout& layout(uint8_t id)
{
    return s_layouts[id < MaxLayouts ? id : 0];
}

uint8_t minAlpha(const TextureStorage& texture)
{
    const TextureDesc& d = texture.desc;
    if (d.cube || d.target || !d.levels || d.levels > 16 || texture.uploads.load(std::memory_order_acquire))
        return 0;
    uint8_t lowest = 255;
    for (uint32_t level = 0; level < d.levels; ++level)
    {
        const uint8_t a = texture.levelAlpha[level].load(std::memory_order_relaxed);
        lowest = a < lowest ? a : lowest;
    }
    return lowest;
}

const TextureStorage* backBufferTexture()
{
    struct BackBuffer : TextureStorage
    {
        BackBuffer() { desc = TextureDesc{ texture::FormatARGB8888, 1, false, true, 0, 0 }; }
    };
    static BackBuffer s_backBuffer;
    return &s_backBuffer;
}

float* allocateConstants(Frame& frame, uint32_t count, uint32_t* offset)
{
    if (frame.constantCount + count > frame.constantCapacity)
        return nullptr;
    *offset = frame.constantCount;
    frame.constantCount += count;
    return frame.constants + *offset;
}

void* allocateVertices(Frame& frame, uint32_t bytes)
{
    const uint32_t start = (frame.vertexBytes + 15) & ~15u;
    if (start + bytes > frame.vertexCapacity)
        return nullptr;
    frame.vertexBytes = start + bytes;
    return frame.vertices + start;
}

}}
