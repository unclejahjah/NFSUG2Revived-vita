// Frame plan (render thread, after resolve): the same image from fewer scenes. Merges scenes of one target, drops
// whole back buffer copies and draws nothing reads.

#include <platform/switches.h>
#include <platform/render.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/texture.h>
#include <algorithm>
#include <cstring>

namespace pf { namespace render {

namespace
{

enum : uint8_t
{
    Rgb = 1,
    Alpha = 2
};

const uint32_t MaxReads = 6;
const uint32_t MaxTracked = 24;
const uint32_t MaxStale = 32;

// A scene's sampled targets and depth use.
struct Summary
{
    const TextureStorage*   reads[MaxReads];
    uint8_t                 readCount;
    bool                    readsAny;       // more targets than reads holds
    bool                    depthUsed, depthWritten;
};

bool tracked(const TextureStorage* t)
{
    return t && (t->desc.target || t == backBufferTexture());
}

const TextureStorage* colorOf(const Scene& s)
{
    return s.target.color ? s.target.color : backBufferTexture();
}

Summary summarize(const Frame& f, const Scene& s)
{
    Summary m;
    memset(&m, 0, sizeof(m));
    m.depthUsed = m.depthWritten = s.target.depth && s.depthCleared;
    for (uint32_t n = 0; n < s.drawCount; ++n)
    {
        const DrawState& d = f.draws[s.firstDraw + n].state;
        if (s.target.depth && d.depthEnable)
        {
            m.depthUsed = true;
            m.depthWritten = m.depthWritten || d.depthWrite;
        }
        for (const TextureStorage* t : d.textures)
        {
            if (!tracked(t))
                continue;
            bool known = false;
            for (uint32_t i = 0; i < m.readCount && !known; ++i)
                known = m.reads[i] == t;
            if (known)
                continue;
            if (m.readCount == MaxReads)
                m.readsAny = true;
            else
                m.reads[m.readCount++] = t;
        }
    }
    return m;
}

bool reads(const Summary& m, const TextureStorage* t)
{
    if (m.readsAny)
        return true;
    for (uint32_t i = 0; i < m.readCount; ++i)
        if (m.reads[i] == t)
            return true;
    return false;
}

void join(Summary& to, const Summary& from)
{
    for (uint32_t i = 0; i < from.readCount; ++i)
    {
        if (reads(to, from.reads[i]))
            continue;
        if (to.readCount == MaxReads)
            to.readsAny = true;
        else
            to.reads[to.readCount++] = from.reads[i];
    }
    to.readsAny = to.readsAny || from.readsAny;
    to.depthUsed = to.depthUsed || from.depthUsed;
    to.depthWritten = to.depthWritten || from.depthWritten;
}

// Either order of the two scenes gives the same targets.
bool independent(const Scene& a, const Summary& ma, const Scene& x, const Summary& mx)
{
    if (a.target.color == x.target.color || reads(ma, colorOf(x)) || reads(mx, colorOf(a)))
        return false;
    if (ma.depthUsed && mx.depthUsed && a.target.depth == x.target.depth && (ma.depthWritten || mx.depthWritten))
        return false;
    return true;
}

// Scene b's draws can follow a's in one scene.
bool joins(const Scene& a, const Summary& ma, const Scene& b, const Summary& mb)
{
    if (a.target.color != b.target.color || a.target.face != b.target.face || a.width != b.width
        || a.height != b.height || reads(mb, colorOf(b)))
        return false;
    return !mb.depthUsed || !ma.depthUsed || (a.target.depth == b.target.depth && !b.depthCleared);
}

void removeScene(Frame& f, Summary* m, uint32_t index)
{
    memmove(f.scenes + index, f.scenes + index + 1, (f.sceneCount - index - 1) * sizeof(Scene));
    memmove(m + index, m + index + 1, (f.sceneCount - index - 1) * sizeof(Summary));
    --f.sceneCount;
}

// Merges scenes j < i when the scenes between allow it.
bool mergePair(Frame& f, Summary* m, uint32_t j, uint32_t i)
{
    Scene& a = f.scenes[j];
    Scene& b = f.scenes[i];
    if (!joins(a, m[j], b, m[i]))
        return false;
    bool back = true, forward = true;
    for (uint32_t k = j + 1; k < i; ++k)
    {
        back = back && independent(b, m[i], f.scenes[k], m[k]);
        forward = forward && independent(a, m[j], f.scenes[k], m[k]);
    }
    if (!back && !forward)
        return false;
    Scene merged = m[j].depthUsed ? a : b;
    merged.target.color = a.target.color;
    if (back)
    {
        const uint32_t end = a.firstDraw + a.drawCount;
        std::rotate(f.draws + end, f.draws + b.firstDraw, f.draws + b.firstDraw + b.drawCount);
        for (uint32_t k = j + 1; k < i; ++k)
            f.scenes[k].firstDraw += b.drawCount;
        merged.firstDraw = a.firstDraw;
        merged.drawCount = a.drawCount + b.drawCount;
        a = merged;
        join(m[j], m[i]);
        removeScene(f, m, i);
    }
    else
    {
        std::rotate(f.draws + a.firstDraw, f.draws + a.firstDraw + a.drawCount, f.draws + b.firstDraw);
        for (uint32_t k = j + 1; k < i; ++k)
            f.scenes[k].firstDraw -= a.drawCount;
        merged.firstDraw = b.firstDraw - a.drawCount;
        merged.drawCount = a.drawCount + b.drawCount;
        b = merged;
        join(m[i], m[j]);
        removeScene(f, m, j);
    }
    return true;
}

uint32_t mergeScenes(Frame& f, Summary* m)
{
    uint32_t merges = 0;
    for (uint32_t i = 1; i < f.sceneCount;)
    {
        uint32_t j = i;
        while (j-- > 0)
        {
            if (f.scenes[j].target.color == f.scenes[i].target.color)
                break;
        }
        static const uint32_t s_limit = uint32_t(NFSU_DEV_INT("debug_merge_limit", 1000));
        if (j < i && merges < s_limit && mergePair(f, m, j, i))
        {
            ++merges;
            i = j ? j : 1;
            continue;
        }
        ++i;
    }
    return merges;
}

// One rectangle writing every pixel of its target, unconditionally.
bool wholeTarget(const Frame& f, const Scene& s, const DrawCall& d)
{
    const DrawState& st = d.state;
    if (st.blendEnable || st.alphaTest || st.depthEnable || d.count != 4 || d.indices || !d.vertices
        || st.primitive == PrimitiveTriangles || (s.target.color && s.target.color->desc.cube))
        return false;
    // The GPU snaps positions to 1/16 pixel: an edge within 1/64 of a pixel centre covers it.
    const float slack = 1.0f / 64.0f;
    float x0, y0, x1, y1;
    return quadBounds(f, d, &x0, &y0, &x1, &y1) && x0 <= slack && y0 <= slack && x1 >= float(s.width) - 0.75f
        && y1 >= float(s.height) - 0.75f;
}

// StretchRect's whole back buffer copy, texel for texel.
bool wholeBackCopy(const Frame& f, const Scene& s)
{
    if (s.drawCount != 1 || !s.target.color || s.width != f.width || s.height != f.height)
        return false;
    const DrawCall& d = f.draws[s.firstDraw];
    const DrawState& st = d.state;
    const TextureDesc& desc = s.target.color->desc;
    if (st.textures[0] != backBufferTexture() || st.vertexProgram != VpFixedScreen || st.fragmentProgram != FpFixedTexture
        || st.samplers[0].filter || st.colorMask != 0xf || desc.cube || desc.levels != 1
        || desc.format != texture::FormatARGB8888 || desc.width != f.width || desc.height != f.height
        || !wholeTarget(f, s, d))
        return false;
    struct Vertex
    {
        float       x, y, z, rhw;
        uint32_t    color;
        float       u, v;
    };
    const Vertex* v = static_cast<const Vertex*>(d.vertices);
    if (layout(st.layout).stride != sizeof(Vertex))
        return false;
    for (uint32_t i = 0; i < 4; ++i)
    {
        const bool right = v[i].x > 0.0f, bottom = v[i].y > 0.0f;
        if (v[i].x != (right ? float(s.width) : 0.0f) - 0.5f || v[i].y != (bottom ? float(s.height) : 0.0f) - 0.5f
            || v[i].u != (right ? 1.0f : 0.0f) || v[i].v != (bottom ? 1.0f : 0.0f) || v[i].color != 0xffffffffu)
            return false;
    }
    return true;
}

// Drops back buffer copies whose readers can sample it directly.
uint32_t elideCopies(Frame& f, Summary* m)
{
    uint32_t elided = 0;
    for (uint32_t c = 0; c < f.sceneCount; ++c)
    {
        if (!wholeBackCopy(f, f.scenes[c]))
            continue;
        const TextureStorage* copy = f.scenes[c].target.color;
        const TextureStorage* back = backBufferTexture();
        bool ok = true, redefined = false;
        uint32_t last = c;
        for (uint32_t k = c + 1; k < f.sceneCount && ok && !redefined; ++k)
        {
            const Scene& s = f.scenes[k];
            const bool readsCopy = reads(m[k], copy);
            if (s.target.color == copy)
            {
                redefined = !readsCopy && s.drawCount && f.draws[s.firstDraw].state.colorMask == 0xf
                         && wholeTarget(f, s, f.draws[s.firstDraw]);
                ok = redefined;
            }
            else if (!s.target.color)
            {
                ok = false;
            }
            else if (readsCopy && m[k].readsAny)
            {
                ok = false;
            }
            last = k;
        }
        if (!ok || !redefined)
            continue;
        for (uint32_t k = c + 1; k < last; ++k)
        {
            const Scene& s = f.scenes[k];
            if (!reads(m[k], copy))
                continue;
            for (uint32_t n = 0; n < s.drawCount; ++n)
                for (const TextureStorage*& t : f.draws[s.firstDraw + n].state.textures)
                    if (t == copy)
                        t = back;
            for (uint32_t i = 0; i < m[k].readCount; ++i)
                if (m[k].reads[i] == copy)
                    m[k].reads[i] = back;
        }
        removeScene(f, m, c);
        --c;
        ++elided;
    }
    return elided;
}

struct Live
{
    const TextureStorage*   texture;
    uint8_t                 channels;
};

struct LiveSet
{
    Live        entries[MaxTracked];
    uint32_t    count = 0;
    bool        full = false;

    uint8_t& of(const TextureStorage* t)
    {
        for (uint32_t i = 0; i < count; ++i)
            if (entries[i].texture == t)
                return entries[i].channels;
        if (count == MaxTracked)
        {
            full = true;
            return entries[0].channels;
        }
        entries[count] = Live{ t, 0 };
        return entries[count++].channels;
    }
};

bool usesSourceAlpha(uint8_t factor)
{
    return factor == 5 || factor == 6 || factor == 11 || factor == 12 || factor == 13;
}

bool usesDestAlpha(uint8_t factor)
{
    return factor == 7 || factor == 8 || factor == 11;
}

// Texture channels a draw needs for its output channels.
uint8_t textureNeed(const DrawState& st, uint8_t output)
{
    if (!output)
        return 0;
    const uint8_t program = st.fragmentProgram;
    const bool filter = program >= FpFilter1 && program <= FpFilter4;
    if (!filter && program != FpOverBright)
        return Rgb | Alpha;
    if (st.factorOutput == FactorColor)
        return 0;
    uint8_t need = (output & Rgb) ? Rgb : 0;
    if (filter && (output & Alpha) && st.factorOutput != FactorAlpha)
        need |= Alpha;
    return need;
}

// Targets whose unread alpha a frame left unwritten.
const TextureStorage*   s_stale[MaxStale];
uint32_t                s_staleCount = 0;

// Marks draws whose output nothing reads; false: not analysed.
bool findDead(const Frame& f, bool* dead, uint32_t* deadCount, PlanStats& stats)
{
    const TextureStorage* back = backBufferTexture();
    const TextureStorage* endAlpha[MaxTracked];
    uint32_t endAlphaCount = 0;
    for (uint32_t round = 0; round <= MaxTracked; ++round)
    {
        LiveSet live;
        live.of(back) = Rgb;
        for (uint32_t i = 0; i < f.sceneCount; ++i)
        {
            const Scene& s = f.scenes[i];
            if (s.target.color)
                live.of(s.target.color) = Rgb;
            for (uint32_t n = 0; n < s.drawCount; ++n)
                for (const TextureStorage* t : f.draws[s.firstDraw + n].state.textures)
                    if (tracked(t) && t != back)
                        live.of(t) = Rgb;
        }
        for (uint32_t i = 0; i < endAlphaCount; ++i)
            live.of(endAlpha[i]) |= Alpha;
        if (live.full)
            return false;

        *deadCount = 0;
        for (uint32_t i = f.sceneCount; i-- > 0;)
        {
            const Scene& s = f.scenes[i];
            const TextureStorage* target = colorOf(s);
            for (uint32_t n = s.drawCount; n-- > 0;)
            {
                const uint32_t index = s.firstDraw + n;
                const DrawCall& d = f.draws[index];
                const DrawState& st = d.state;
                const uint8_t written = uint8_t(((st.colorMask & 7) ? Rgb : 0) | ((st.colorMask & 8) ? Alpha : 0));
                uint8_t& targetLive = live.of(target);
                const uint8_t wanted = targetLive & written;
                dead[index] = !wanted && !(s.target.depth && st.depthEnable && st.depthWrite);
                if (dead[index])
                {
                    ++*deadCount;
                    continue;
                }
                uint8_t source = wanted;
                if (st.blendEnable)
                {
                    if ((wanted & Rgb) && (usesSourceAlpha(st.srcBlend) || usesSourceAlpha(st.dstBlend)))
                        source |= Alpha;
                    if ((wanted & Rgb) && (usesDestAlpha(st.srcBlend) || usesDestAlpha(st.dstBlend)))
                        targetLive |= Alpha;
                }
                else if (wholeTarget(f, s, d))
                {
                    targetLive &= uint8_t(~(((st.colorMask & 7) == 7 ? Rgb : 0) | ((st.colorMask & 8) ? Alpha : 0)));
                }
                if (st.alphaTest)
                    source |= Alpha;
                const uint8_t need = textureNeed(st, source);
                if (!need)
                    continue;
                for (const TextureStorage* t : st.textures)
                    if (tracked(t))
                        live.of(t) |= need;
            }
        }
        if (live.full)
            return false;

        bool grown = false;
        for (uint32_t i = 0; i < live.count; ++i)
        {
            const Live& l = live.entries[i];
            if (l.texture == back || !(l.channels & Alpha))
                continue;
            bool known = false;
            for (uint32_t k = 0; k < endAlphaCount && !known; ++k)
                known = endAlpha[k] == l.texture;
            if (!known)
            {
                endAlpha[endAlphaCount++] = l.texture;
                grown = true;
            }
        }
        if (grown)
            continue;

        for (uint32_t k = 0; k < endAlphaCount; ++k)
            for (uint32_t i = 0; i < s_staleCount; ++i)
                if (s_stale[i] == endAlpha[k])
                {
                    if (!stats.staleReads++)
                        logError("plan: frame %u reads the alpha of a target that earlier frames left unwritten", f.index);
                    s_stale[i] = s_stale[--s_staleCount];
                    break;
                }
        if (*deadCount)
        {
            for (uint32_t i = 0; i < live.count; ++i)
            {
                const TextureStorage* t = live.entries[i].texture;
                bool listed = t == back;
                for (uint32_t k = 0; k < endAlphaCount && !listed; ++k)
                    listed = endAlpha[k] == t;
                for (uint32_t k = 0; k < s_staleCount && !listed; ++k)
                    listed = s_stale[k] == t;
                if (!listed && s_staleCount < MaxStale)
                    s_stale[s_staleCount++] = t;
            }
        }
        return true;
    }
    return false;
}

// Removes the dead draws and emptied scenes.
void removeDead(Frame& f, Summary* m, const bool* dead, PlanStats& stats)
{
    for (uint32_t i = 0; i < f.sceneCount; ++i)
    {
        Scene& s = f.scenes[i];
        uint32_t kept = 0;
        for (uint32_t n = 0; n < s.drawCount; ++n)
        {
            if (dead[s.firstDraw + n])
                continue;
            if (kept != n)
                f.draws[s.firstDraw + kept] = f.draws[s.firstDraw + n];
            ++kept;
        }
        if (kept == s.drawCount)
            continue;
        if (kept || (s.target.depth && s.depthCleared))
        {
            stats.deadDraws += s.drawCount - kept;
            s.drawCount = kept;
            continue;
        }
        ++stats.deadScenes;
        removeScene(f, m, i);
        --i;
    }
}

PlanStats s_stats;

}

// Keeps the predecessor's blend operation the renderer drew with.
void keepBlendOps(Frame& f)
{
    static const bool s_exact = NFSU_DEV_BOOL("blend_op_exact", true);
    if (s_exact)
        return;
    const DrawState* previous = nullptr;
    for (uint32_t i = 0; i < f.drawCount; ++i)
    {
        if (!f.draws[i].count)
            continue;
        DrawState& s = f.draws[i].state;
        if (previous && s.vertexProgram == previous->vertexProgram && s.fragmentProgram == previous->fragmentProgram
            && s.alphaTest == previous->alphaTest && s.factorOutput == previous->factorOutput
            && s.blendEnable == previous->blendEnable && s.srcBlend == previous->srcBlend
            && s.dstBlend == previous->dstBlend && s.colorMask == previous->colorMask
            && s.blendOp != previous->blendOp)
        {
            s.blendOp = previous->blendOp;
            ++s_stats.inheritedOps;
        }
        previous = &s;
    }
}

bool quadBounds(const Frame& frame, const DrawCall& draw, float* x0, float* y0, float* x1, float* y1)
{
    const DrawState& st = draw.state;
    const VertexLayout& l = layout(st.layout);
    const bool screen = st.vertexProgram == VpFixedScreen;
    const bool fixed = st.vertexProgram == VpFixed && draw.vertexConstants != NoConstants;
    if ((!screen && !fixed && st.vertexProgram != VpFilter && st.vertexProgram != VpClear) || draw.count != 4
        || !draw.vertices || draw.indices || !l.count || l.elements[0].usage != UsagePosition
        || (l.elements[0].type != TypeFloat3 && l.elements[0].type != TypeFloat4))
        return false;
    const Viewport& vp = frame.viewports[draw.viewport];
    const float* m = fixed ? frame.constants + draw.vertexConstants : nullptr;
    float x[4], y[4];
    for (uint32_t i = 0; i < 4; ++i)
    {
        float p[3];
        memcpy(p, static_cast<const uint8_t*>(draw.vertices) + i * l.stride + l.elements[0].offset, sizeof(p));
        if (fixed)
        {
            const float w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
            if (!(w > 0.0f))
                return false;
            const float cx = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
            const float cy = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
            const float cz = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
            p[0] = cx / w;
            p[1] = cy / w;
            p[2] = cz / w;
        }
        if (!screen && !(p[2] >= 0.0f && p[2] <= 1.0f))
            return false;
        x[i] = screen ? p[0] : vp.x + (p[0] + 1.0f) * 0.5f * vp.width;
        y[i] = screen ? p[1] : vp.y + (1.0f - p[1]) * 0.5f * vp.height;
    }
    *x0 = std::min(std::min(x[0], x[1]), std::min(x[2], x[3]));
    *x1 = std::max(std::max(x[0], x[1]), std::max(x[2], x[3]));
    *y0 = std::min(std::min(y[0], y[1]), std::min(y[2], y[3]));
    *y1 = std::max(std::max(y[0], y[1]), std::max(y[2], y[3]));
    const float slack = 1.0f / 64.0f;
    uint32_t corners = 0;
    for (uint32_t i = 0; i < 4; ++i)
    {
        const bool right = x[i] > *x1 - slack, bottom = y[i] > *y1 - slack;
        if ((!right && x[i] > *x0 + slack) || (!bottom && y[i] > *y0 + slack))
            return false;
        corners |= 1u << ((right ? 1 : 0) | (bottom ? 2 : 0));
    }
    if (corners != 0xf || *x1 - *x0 < 1.0f || *y1 - *y0 < 1.0f)
        return false;
    return true;
}

void optimizeFrame(Frame& frame)
{
    struct Options
    {
        bool merge, copies, dead;
    };
    static const Options s_options = { NFSU_RENDER_FAST("scene_merge"), NFSU_RENDER_FAST("copy_elide"),
                                       NFSU_RENDER_FAST("dead_passes") };
    const uint32_t MaxScenes = 64;
    if ((!s_options.merge && !s_options.copies && !s_options.dead) || frame.sceneCount < 2
        || frame.sceneCount > MaxScenes)
        return;
    Summary summaries[MaxScenes];
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
        summaries[i] = summarize(frame, frame.scenes[i]);
    ++s_stats.frames;
    if (s_options.merge)
        s_stats.merged += mergeScenes(frame, summaries);
    if (s_options.copies)
        s_stats.copies += elideCopies(frame, summaries);
    if (s_options.dead)
    {
        static bool* s_dead = nullptr;
        static uint32_t s_capacity = 0;
        if (s_capacity < frame.drawCapacity)
        {
            delete[] s_dead;
            s_dead = new bool[frame.drawCapacity];
            s_capacity = frame.drawCapacity;
        }
        uint32_t count = 0;
        if (findDead(frame, s_dead, &count, s_stats) && count)
            removeDead(frame, summaries, s_dead, s_stats);
    }
    if (perf::enabled() && s_stats.frames >= 600)
    {
        const double n = s_stats.frames;
        logInfo("plan: per frame: %.2f scenes merged, %.2f back buffer copies dropped, %.2f unread scenes and %.2f "
                "unread draws dropped, %.2f draws with their predecessor's blend operation; %u stale reads",
                s_stats.merged / n, s_stats.copies / n, s_stats.deadScenes / n, s_stats.deadDraws / n,
                s_stats.inheritedOps / n, unsigned(s_stats.staleReads));
        const uint32_t stale = s_stats.staleReads;
        s_stats = PlanStats();
        s_stats.staleReads = stale;
    }
}

} }
