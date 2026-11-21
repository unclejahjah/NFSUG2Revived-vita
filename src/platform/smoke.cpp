// Particle draws: quads cropped to their visible texels (optimised_particles); with DEV_SETTINGS the smoke_* statistics,
// fades and skip.

#include <platform/render.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/texture.h>
#if defined(__vita__)
#include "gxm/gxm_render.h"
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pf { namespace render {

uint8_t (*g_drawTag)() = nullptr;

const ParticleOptions& particleOptions()
{
    static const ParticleOptions o = [] {
        const bool on = Config::getBool("optimised_particles", true);
        ParticleOptions r;
        r.crop = on ? 2 : 0;
        r.thinPercent = on ? 67 : 100;
        return r;
    }();
    return o;
}

namespace
{

struct Clip
{
    float x, y, z, w;
};

// Area of a clip-space polygon inside the view, in viewports.
float clippedArea(const Clip* in, uint32_t count)
{
    Clip a[12], b[12];
    memcpy(a, in, count * sizeof(Clip));
    for (int plane = 0; plane < 5 && count; ++plane)
    {
        const auto distance = [plane](const Clip& c) {
            switch (plane)
            {
            case 0:  return c.w - 1e-4f;
            case 1:  return c.w + c.x;
            case 2:  return c.w - c.x;
            case 3:  return c.w + c.y;
            default: return c.w - c.y;
            }
        };
        uint32_t out = 0;
        for (uint32_t i = 0; i < count && out < 11; ++i)
        {
            const Clip& p = a[i];
            const Clip& q = a[(i + 1) % count];
            const float dp = distance(p), dq = distance(q);
            if (dp >= 0.0f)
                b[out++] = p;
            if ((dp >= 0.0f) != (dq >= 0.0f))
            {
                const float t = dp / (dp - dq);
                b[out++] = Clip{ p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t,
                                 p.w + (q.w - p.w) * t };
            }
        }
        memcpy(a, b, out * sizeof(Clip));
        count = out;
    }
    if (count < 3)
        return 0.0f;
    float area = 0.0f;
    for (uint32_t i = 0; i < count; ++i)
    {
        const Clip& p = a[i];
        const Clip& q = a[(i + 1) % count];
        area += p.x / p.w * (q.y / q.w) - q.x / q.w * (p.y / p.w);
    }
    return std::fabs(area) * 0.125f;
}

struct Settings
{
    bool    log, skip, mipPoint;
    bool    diag;                   // statistics, fade, skip or mip settings in use (DEV_SETTINGS)
    int     crop;                   // quads cut to their texels of at least this alpha (15ths); 0 off
    float   fadeFrom, fadeTo;       // viewports: quads above fadeFrom fade out, gone at fadeTo
    float   nearFrom, nearTo;       // view depth: quads nearer than nearTo fade out, gone at nearFrom
    bool    any;
};

Settings load()
{
    Settings s = {};
    s.crop = particleOptions().crop;
#ifdef DEV_SETTINGS
    s.log = Config::getBool("smoke_log", false);
    s.skip = Config::getBool("dev_smoke_skip", false);
    s.mipPoint = Config::getString("dev_smoke_mip", "")[0] == 'p';
    int a = 0, b = 0;
    const char* fade = Config::getString("dev_smoke_fade", "");
    if (fade && sscanf(fade, "%d,%d", &a, &b) == 2 && b > a)
    {
        s.fadeFrom = float(a) / 100.0f;
        s.fadeTo = float(b) / 100.0f;
    }
    const char* near = Config::getString("dev_smoke_near", "");
    float n0 = 0.0f, n1 = 0.0f;
    if (near && sscanf(near, "%f,%f", &n0, &n1) == 2 && n1 > n0)
    {
        s.nearFrom = n0;
        s.nearTo = n1;
    }
#endif
    s.diag = s.log || s.skip || s.mipPoint || s.fadeTo > 0.0f || s.nearTo > 0.0f;
    s.any = s.diag || s.crop;
    if (s.any)
        logInfo("smoke: log %d, skip %d, crop %d, fade %.2f..%.2f viewports, near %.1f..%.1f", int(s.log),
                int(s.skip), s.crop, double(s.fadeFrom), double(s.fadeTo), double(s.nearFrom), double(s.nearTo));
    return s;
}

const Settings& settings()
{
    static const Settings s = load();
    return s;
}

const float AreaBounds[] = { 0.01f, 0.05f, 0.15f, 0.40f };
const float NearBounds[] = { 1.0f, 3.0f, 10.0f, 30.0f };
enum { Buckets = 5 };

struct Stats
{
    uint32_t    frames = 0, draws = 0, quads = 0, culled = 0, faded = 0, dropped = 0;
    double      area = 0.0, drawn = 0.0, maxFrame = 0.0, maxQuad = 0.0, alphaArea = 0.0;
    uint32_t    areaCount[Buckets] = {};
    double      areaSum[Buckets] = {};
    double      nearSum[Buckets] = {};
};

Stats       s_stats;

// Draw kinds seen: texture and blend, with their area since the last report.
struct Kind
{
    const TextureStorage*   texture;
    uint8_t                 src, dst;
    uint32_t                draws, quads;
    double                  area;
    float                   u0, v0, u1, v1;     // texture coordinates the quads use
};
std::vector<Kind> s_kinds;

uint32_t bucket(const float* bounds, float value)
{
    uint32_t i = 0;
    while (i < Buckets - 1 && value >= bounds[i])
        ++i;
    return i;
}

Kind& kindOf(const Frame& f, uint32_t index, bool log);

void describe(const Frame& f, uint32_t index)
{
    const DrawCall& d = f.draws[index];
    const DrawState& s = d.state;
    uint32_t scene = 0;
    while (scene < f.sceneCount && index >= f.scenes[scene].firstDraw + f.scenes[scene].drawCount)
        ++scene;
    const Scene* sc = scene < f.sceneCount ? &f.scenes[scene] : nullptr;
    const TextureStorage* t = s.textures[0];
    const Viewport& vp = f.viewports[d.viewport];
    logInfo("smoke: draw %u of frame %u in scene %u (%s %ux%u, %u draws): vp %u fp %u layout stride %u, primitive %u, "
            "%u indices %u vertices, blend %u %u/%u op %u, colour mask 0x%x, alpha test %u, z %u/%u func %u, cull %u, "
            "factor %u, texture %ux%u format %u levels %u, filter %u, viewport %.0f,%.0f %.0fx%.0f",
            index, f.index, scene, sc && sc->target.color ? "texture" : "back buffer", sc ? sc->width : 0,
            sc ? sc->height : 0, sc ? sc->drawCount : 0, s.vertexProgram, s.fragmentProgram, layout(s.layout).stride,
            s.primitive, d.count, d.vertexCount, s.blendEnable, s.srcBlend, s.dstBlend, s.blendOp, s.colorMask,
            s.alphaTest, s.depthEnable, s.depthWrite, s.depthFunc, s.cullMode, s.factorOutput, t ? t->desc.width : 0,
            t ? t->desc.height : 0, t ? unsigned(t->desc.format) : 0, t ? t->desc.levels : 0, s.samplers[0].filter,
            double(vp.x), double(vp.y), double(vp.width), double(vp.height));
}

// Level 0 alpha of a DXT3 texture; empty otherwise.
std::vector<uint8_t> decodeAlpha(const TextureStorage* storage)
{
    std::vector<uint8_t> alpha;
#if defined(__vita__)
    const gxm::Texture* t = static_cast<const gxm::Texture*>(storage);
    const TextureDesc& d = t->desc;
    if (d.format != texture::FormatBC2 || d.cube || t->stride || d.width < 32 || d.height < 32 || d.width > 256
        || d.height > 256)
        return alpha;
    const uint32_t bw = d.width / 4, bh = d.height / 4;
    std::vector<uint8_t> blocks(bw * bh * 16);
    texture::unswizzleLevel(d.format, d.width, d.height, 0, t->data, blocks.data(), bw * 16);
    alpha.resize(d.width * d.height);
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx)
            for (uint32_t i = 0; i < 16; ++i)
            {
                const uint8_t b = blocks[(by * bw + bx) * 16 + i / 2];
                alpha[(by * 4 + i / 4) * d.width + bx * 4 + i % 4] = (i & 1) ? b >> 4 : b & 15;
            }
#else
    (void)storage;
#endif
    return alpha;
}

// A texture's alpha and the crops of the texture rectangles quads used.
struct Crop
{
    float   s0, t0, s1, t1;     // rectangle a quad uses
    float   c0, d0, c1, d1;     // its texels of enough alpha; c1 <= c0: none
};
struct AlphaCache
{
    const TextureStorage*   texture;
    const void*             data;
    std::vector<uint8_t>    alpha;
    std::vector<Crop>       crops;
};
std::vector<AlphaCache> s_alpha;

const Crop* cropOf(const TextureStorage* t, float s0, float t0, float s1, float t1, int threshold)
{
#if defined(__vita__)
    const void* data = static_cast<const gxm::Texture*>(t)->data;
#else
    const void* data = nullptr;
#endif
    AlphaCache* cache = nullptr;
    for (AlphaCache& a : s_alpha)
        if (a.texture == t && a.data == data)
            cache = &a;
    if (!cache)
    {
        s_alpha.push_back(AlphaCache{ t, data, decodeAlpha(t), {} });
        cache = &s_alpha.back();
    }
    if (cache->alpha.empty())
        return nullptr;
    for (const Crop& c : cache->crops)
        if (c.s0 == s0 && c.t0 == t0 && c.s1 == s1 && c.t1 == t1)
            return &c;
    if (cache->crops.size() >= 64)
        return nullptr;
    const uint32_t w = t->desc.width, h = t->desc.height;
    const auto texel = [](float a, uint32_t size) {
        return uint32_t(std::min(float(size), std::max(0.0f, std::floor(a * float(size) + 0.5f))));
    };
    const uint32_t x0 = texel(std::min(s0, s1), w), x1 = texel(std::max(s0, s1), w);
    const uint32_t y0 = texel(std::min(t0, t1), h), y1 = texel(std::max(t0, t1), h);
    uint32_t bx0 = x1, by0 = y1, bx1 = x0, by1 = y0;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
            if (cache->alpha[y * w + x] >= threshold)
            {
                bx0 = std::min(bx0, x);
                bx1 = std::max(bx1, x + 1);
                by0 = std::min(by0, y);
                by1 = std::max(by1, y + 1);
            }
    Crop c = { s0, t0, s1, t1, float(bx0) / float(w), float(by0) / float(h), float(bx1) / float(w),
               float(by1) / float(h) };
    cache->crops.push_back(c);
    logInfo("smoke: crop of texture %ux%u rectangle %.3f,%.3f..%.3f,%.3f: %.3f,%.3f..%.3f,%.3f (%.0f%% of its area)", w,
            h, double(s0), double(t0), double(s1), double(t1), double(c.c0), double(c.d0), double(c.c1), double(c.d1),
            bx1 > bx0 ? 100.0 * (bx1 - bx0) * (by1 - by0) / (double(x1 - x0) * (y1 - y0)) : 0.0);
    return &cache->crops.back();
}

// Cuts a quad to its crop; false: nothing left.
bool cropQuad(uint8_t* v, uint32_t stride, const TextureStorage* t, int threshold)
{
    float p[4][3], uv[4][2];
    for (uint32_t k = 0; k < 4; ++k)
    {
        memcpy(p[k], v + k * stride, 12);
        memcpy(uv[k], v + k * stride + 16, 8);
    }
    const float s0 = uv[0][0], t0 = uv[0][1], s1 = uv[1][0], t1 = uv[3][1];
    if (uv[1][1] != t0 || uv[3][0] != s0 || uv[2][0] != s1 || uv[2][1] != t1 || s0 == s1 || t0 == t1)
        return true;
    const Crop* c = cropOf(t, s0, t0, s1, t1, threshold);
    if (!c)
        return true;
    if (c->c1 <= c->c0 || c->d1 <= c->d0)
        return false;
    float ua = (c->c0 - s0) / (s1 - s0), ub = (c->c1 - s0) / (s1 - s0);
    float va = (c->d0 - t0) / (t1 - t0), vb = (c->d1 - t0) / (t1 - t0);
    if (ua > ub)
        std::swap(ua, ub);
    if (va > vb)
        std::swap(va, vb);
    const float us[4] = { ua, ub, ub, ua }, vs[4] = { va, va, vb, vb };
    for (uint32_t k = 0; k < 4; ++k)
    {
        float q[3], tc[2];
        for (int i = 0; i < 3; ++i)
            q[i] = p[0][i] + us[k] * (p[1][i] - p[0][i]) + vs[k] * (p[3][i] - p[0][i]);
        tc[0] = s0 + us[k] * (s1 - s0);
        tc[1] = t0 + vs[k] * (t1 - t0);
        memcpy(v + k * stride, q, 12);
        memcpy(v + k * stride + 16, tc, 8);
    }
    return true;
}

// Logs a DXT3 texture's alpha as hex rows.
void alphaMap(const TextureStorage* storage)
{
#if defined(__vita__)
    const TextureDesc& d = storage->desc;
    const std::vector<uint8_t> alpha = decodeAlpha(storage);
    if (alpha.empty())
        return;
    uint32_t histogram[16] = {};
    for (uint8_t a : alpha)
        ++histogram[a];
    logInfo("smoke: alpha of texture %ux%u (4 bits) by value: %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u", d.width,
            d.height, histogram[0], histogram[1], histogram[2], histogram[3], histogram[4], histogram[5], histogram[6],
            histogram[7], histogram[8], histogram[9], histogram[10], histogram[11], histogram[12], histogram[13],
            histogram[14], histogram[15]);
    const uint32_t cw = d.width / 32, ch = d.height / 32;
    for (uint32_t y = 0; y < 32; ++y)
    {
        char row[33];
        for (uint32_t x = 0; x < 32; ++x)
        {
            uint8_t m = 0;
            for (uint32_t j = 0; j < ch; ++j)
                for (uint32_t i = 0; i < cw; ++i)
                    m = std::max(m, alpha[(y * ch + j) * d.width + x * cw + i]);
            row[x] = "0123456789abcdef"[m];
        }
        row[32] = 0;
        logInfo("smoke: alpha %s", row);
    }
#else
    (void)storage;
#endif
}

Kind& kindOf(const Frame& f, uint32_t index, bool log)
{
    const DrawState& s = f.draws[index].state;
    for (Kind& k : s_kinds)
        if (k.texture == s.textures[0] && k.src == s.srcBlend && k.dst == s.dstBlend)
            return k;
    if (log)
    {
        describe(f, index);
        if (s.textures[0])
            alphaMap(s.textures[0]);
    }
    s_kinds.push_back(Kind{ s.textures[0], s.srcBlend, s.dstBlend, 0, 0, 0.0, 1e9f, 1e9f, -1e9f, -1e9f });
    return s_kinds.back();
}

void report()
{
    Stats& s = s_stats;
    const double n = s.frames ? s.frames : 1;
    char text[400];
    int len = 0;
    for (uint32_t i = 0; i < Buckets; ++i)
        len += snprintf(text + len, sizeof(text) - len, " %u:%.2f", unsigned(s.areaCount[i] / n), s.areaSum[i] / n);
    len += snprintf(text + len, sizeof(text) - len, " | by view depth <1 <3 <10 <30 more:");
    for (uint32_t i = 0; i < Buckets; ++i)
        len += snprintf(text + len, sizeof(text) - len, " %.2f", s.nearSum[i] / n);
    logInfo("smoke: per frame %.1f draws, %.0f quads (%.0f culled by the game), area %.2f viewports (max frame %.2f, "
            "max quad %.2f), drawn %.2f after %.0f faded and %.0f dropped, mean alpha %.2f | quads and area by quad "
            "size <1%% <5%% <15%% <40%% more:%s", s.draws / n, s.quads / n, s.culled / n, s.area / n, s.maxFrame,
            s.maxQuad, s.drawn / n, s.faded / n, s.dropped / n, s.area > 0.0 ? s.alphaArea / s.area : 0.0, text);
    s = Stats();
    for (Kind& k : s_kinds)
    {
        if (k.draws)
            logInfo("smoke: kind texture %ux%u blend %u/%u: %.2f draws, %.0f quads, area %.2f viewports per frame, "
                    "texture coordinates %.3f,%.3f..%.3f,%.3f", k.texture ? k.texture->desc.width : 0,
                    k.texture ? k.texture->desc.height : 0, k.src, k.dst, k.draws / n, k.quads / n, k.area / n,
                    double(k.u0), double(k.v0), double(k.u1), double(k.v1));
        k.draws = k.quads = 0;
        k.area = 0.0;
    }
}

struct Quad
{
    uint8_t*    v;              // first of 4 vertices
    float       area, depth;    // viewports; nearest view depth
};

std::vector<Quad> s_quads;
std::vector<const void*> s_seen;

void fade(const Quad& q, float factor, uint32_t stride, Stats& st)
{
    if (factor >= 1.0f)
        return;
    bool gone = factor <= 0.0f;
    if (!gone)
    {
        for (uint32_t k = 0; k < 4; ++k)
        {
            uint32_t c;
            memcpy(&c, q.v + k * stride + 12, 4);
            const uint32_t a = uint32_t(float(c >> 24) * factor);
            gone = gone || a <= 0x14;
            c = (c & 0x00ffffffu) | a << 24;
            memcpy(q.v + k * stride + 12, &c, 4);
        }
    }
    if (gone)
    {
        for (uint32_t k = 0; k < 4; ++k)
            memset(q.v + k * stride, 0, 12);
        ++st.dropped;
    }
    else
    {
        ++st.faded;
    }
}

void cropParticles(Frame& f, int threshold)
{
    s_seen.clear();
    for (uint32_t i = 0; i < f.drawCount; ++i)
    {
        const DrawCall& d = f.draws[i];
        if (!d.tag || !d.count || !d.state.textures[0] || !d.vertices || layout(d.state.layout).stride != 24
            || std::find(s_seen.begin(), s_seen.end(), d.vertices) != s_seen.end())
            continue;
        s_seen.push_back(d.vertices);
        uint8_t* base = static_cast<uint8_t*>(const_cast<void*>(d.vertices));
        for (uint32_t q = 0; q + 4 <= d.vertexCount; q += 4)
        {
            uint8_t* v = base + q * 24;
            float p[4][3];
            bool zero = true;
            for (uint32_t k = 0; k < 4; ++k)
            {
                memcpy(p[k], v + k * 24, 12);
                zero = zero && p[k][0] == 0.0f && p[k][1] == 0.0f && p[k][2] == 0.0f;
            }
            if (!zero && !cropQuad(v, 24, d.state.textures[0], threshold))
                for (uint32_t k = 0; k < 4; ++k)
                    memset(v + k * 24, 0, 12);
        }
    }
}

}

bool particleTagNeeded()
{
    return settings().any;
}

void smoke(Frame& f)
{
    const Settings& cfg = settings();
    if (!cfg.any)
        return;
    if (!cfg.diag)
    {
        cropParticles(f, cfg.crop);
        return;
    }
    Stats& st = s_stats;
    double frameArea = 0.0, frameDrawn = 0.0;
    s_quads.clear();
    s_seen.clear();
    for (uint32_t i = 0; i < f.drawCount; ++i)
    {
        DrawCall& d = f.draws[i];
        if (!d.tag || !d.count)
            continue;
        ++st.draws;
        Kind& kind = kindOf(f, i, cfg.log);
        ++kind.draws;
        if (cfg.mipPoint && d.state.samplers[0].mip == MipLinear)
            d.state.samplers[0].mip = MipPoint;
        if (cfg.skip)
        {
            d.count = 0;
            continue;
        }
        const uint32_t stride = layout(d.state.layout).stride;
        if (stride != 24 || d.vertexConstants == NoConstants || !d.vertices
            || std::find(s_seen.begin(), s_seen.end(), d.vertices) != s_seen.end())
            continue;
        s_seen.push_back(d.vertices);
        const float* m = f.constants + d.vertexConstants;
        uint8_t* base = static_cast<uint8_t*>(const_cast<void*>(d.vertices));
        for (uint32_t q = 0; q + 4 <= d.vertexCount; q += 4)
        {
            uint8_t* v = base + q * stride;
            Clip c[4];
            bool zero = true;
            for (uint32_t k = 0; k < 4; ++k)
            {
                float p[3];
                memcpy(p, v + k * stride, 12);
                zero = zero && p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f;
                c[k].x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
                c[k].y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
                c[k].z = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
                c[k].w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
            }
            ++st.quads;
            ++kind.quads;
            if (zero)
            {
                ++st.culled;
                continue;
            }
            const float area = clippedArea(c, 4);
            const float depth = std::min(std::min(c[0].w, c[1].w), std::min(c[2].w, c[3].w));
            uint32_t colour;
            memcpy(&colour, v + 12, 4);
            st.alphaArea += double(area) * double(colour >> 24) / 255.0;
            frameArea += area;
            kind.area += area;
            for (uint32_t k = 0; k < 4; ++k)
            {
                float uv[2];
                memcpy(uv, v + k * stride + 16, 8);
                kind.u0 = std::min(kind.u0, uv[0]);
                kind.v0 = std::min(kind.v0, uv[1]);
                kind.u1 = std::max(kind.u1, uv[0]);
                kind.v1 = std::max(kind.v1, uv[1]);
            }
            st.maxQuad = std::max(st.maxQuad, double(area));
            const uint32_t b = bucket(AreaBounds, area);
            ++st.areaCount[b];
            st.areaSum[b] += area;
            st.nearSum[bucket(NearBounds, depth)] += area;
            float drawn = area;
            if (cfg.crop && d.state.textures[0])
            {
                if (!cropQuad(v, stride, d.state.textures[0], cfg.crop))
                {
                    for (uint32_t k = 0; k < 4; ++k)
                        memset(v + k * stride, 0, 12);
                    ++st.dropped;
                    continue;
                }
                for (uint32_t k = 0; k < 4; ++k)
                {
                    float p[3];
                    memcpy(p, v + k * stride, 12);
                    c[k].x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
                    c[k].y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
                    c[k].z = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
                    c[k].w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
                }
                drawn = clippedArea(c, 4);
            }
            s_quads.push_back(Quad{ v, drawn, depth });
        }
    }
    for (const Quad& q : s_quads)
    {
        float factor = 1.0f;
        if (cfg.fadeTo > 0.0f && q.area > cfg.fadeFrom)
            factor = std::max(0.0f, (cfg.fadeTo - q.area) / (cfg.fadeTo - cfg.fadeFrom));
        if (cfg.nearTo > 0.0f && q.depth < cfg.nearTo)
            factor = std::min(factor, std::max(0.0f, (q.depth - cfg.nearFrom) / (cfg.nearTo - cfg.nearFrom)));
        fade(q, factor, 24, st);
        if (factor > 0.0f)
            frameDrawn += q.area;
    }
    st.area += frameArea;
    st.drawn += frameDrawn;
    st.maxFrame = std::max(st.maxFrame, frameArea);
    if (cfg.log && ++st.frames == 300)
        report();
}

}}
