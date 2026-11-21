// The Vita's own system font through libpgf, picked as vita2d_load_default_pgf (VitaShell) picks it. Rows use its
// native size; small text and the title are scaled from it.

#include "cfgui.h"
#include <platform/log.h>
#ifdef __vita__
#include <psp2/pgf.h>
#include <psp2/sysmodule.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <map>
#endif

namespace ug2 { namespace cfgui {

#ifdef __vita__
namespace
{

const float Scales[FontSizes] = { 0.8f, 1.0f, 1.5f };

void* fontAlloc(void*, unsigned int size)
{
    return memalign(8, size);
}

void fontFree(void*, void* p)
{
    free(p);
}

// Bilinear resampling of an 8-bit alpha bitmap.
std::vector<uint8_t> scaled(const std::vector<uint8_t>& src, int w, int h, int ow, int oh)
{
    std::vector<uint8_t> out(size_t(ow) * oh, 0);
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x)
        {
            const float sx = (x + 0.5f) * w / ow - 0.5f, sy = (y + 0.5f) * h / oh - 0.5f;
            const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
            const float fx = sx - x0, fy = sy - y0;
            float sum = 0.0f;
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i)
                {
                    const int xx = x0 + i, yy = y0 + j;
                    if (xx >= 0 && yy >= 0 && xx < w && yy < h)
                        sum += src[size_t(yy) * w + xx] * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                }
            out[size_t(y) * ow + x] = uint8_t(std::min(255.0f, sum + 0.5f));
        }
    return out;
}

class SystemFont : public Font
{
public:
    bool open()
    {
        if (sceSysmoduleLoadModule(SCE_SYSMODULE_PGF) < 0)
            return false;
        m_moduleLoaded = true;
        SceFontNewLibParams params;
        memset(&params, 0, sizeof(params));
        params.numFonts = 1;
        params.allocFunc = &fontAlloc;
        params.freeFunc = &fontFree;
        unsigned int error = 1;
        m_lib = sceFontNewLib(&params, &error);
        if (error != 0 || !m_lib)
        {
            m_lib = nullptr;
            return false;
        }
        SceFontStyle style;
        memset(&style, 0, sizeof(style));
        style.fontLanguage = SCE_FONT_LANGUAGE_DEFAULT;
        error = 1;
        const int index = sceFontFindOptimumFont(m_lib, &style, &error);
        if (error != 0 || index < 0)
            return false;
        error = 1;
        m_font = sceFontOpen(m_lib, index, 0, &error);
        if (error != 0 || !m_font)
        {
            m_font = nullptr;
            return false;
        }
        SceFontCharInfo info;
        if (sceFontGetCharInfo(m_font, 'H', &info) < 0 || info.bitmapHeight < 6 || info.bitmapHeight > 60)
            return false;
        m_capHeight = int(info.bitmapHeight);
        SceFontInfo font;
        memset(&font, 0, sizeof(font));
        sceFontGetFontInfo(m_font, &font);
        font.fontStyle.fontName[63] = font.fontStyle.fontFileName[63] = 0;
        pf::logInfo("configurator: system font %d, %s (%s), family %u, style %u, language %u, capital height %d px",
                    index, font.fontStyle.fontName, font.fontStyle.fontFileName, font.fontStyle.fontFamily,
                    font.fontStyle.fontStyle, font.fontStyle.fontLanguage, m_capHeight);
        return true;
    }

    void close()
    {
        if (m_font)
            sceFontClose(m_font);
        if (m_lib)
            sceFontDoneLib(m_lib);
        if (m_moduleLoaded)
            sceSysmoduleUnloadModule(SCE_SYSMODULE_PGF);
        m_font = nullptr;
        m_lib = nullptr;
        m_moduleLoaded = false;
        for (auto& cache : m_cache)
            cache.clear();
    }

    bool glyph(FontSize size, unsigned code, Glyph* out) const override
    {
        if (size >= FontSizes || code < 32 || code > 126)
            return false;
        auto found = m_cache[size].find(code);
        if (found == m_cache[size].end())
            found = m_cache[size].emplace(code, render(size, code)).first;
        const Cached& c = found->second;
        if (!c.ok)
            return bundledFont().glyph(size, code, out);
        *out = c.glyph;
        out->alpha = c.alpha.data();
        return true;
    }

    int ascent(FontSize size) const override
    {
        return centredAscent(bundledFont(), size, int(std::lround(m_capHeight * Scales[size])));
    }

private:
    struct Cached
    {
        bool ok = false;
        std::vector<uint8_t> alpha;
        Glyph glyph = {};
    };

    Cached render(FontSize size, unsigned code) const
    {
        Cached c;
        SceFontCharInfo info;
        if (sceFontGetCharInfo(m_font, code, &info) < 0 || info.bitmapWidth > 128 || info.bitmapHeight > 128)
            return c;
        const int w = int(info.bitmapWidth), h = int(info.bitmapHeight);
        const int pitch = (w + 3) & ~3;
        std::vector<uint8_t> image(size_t(pitch) * std::max(h, 1), 0);
        if (w && h)
        {
            SceFontGlyphImage glyph;
            memset(&glyph, 0, sizeof(glyph));
            glyph.pixelFormat = SCE_FONT_PIXELFORMAT_8;
            glyph.bufWidth = (unsigned short)pitch;
            glyph.bufHeight = (unsigned short)h;
            glyph.bytesPerLine = (unsigned short)pitch;
            glyph.bufferPtr = (unsigned int)(uintptr_t)image.data();
            if (sceFontGetCharGlyphImage(m_font, code, &glyph) < 0)
                return c;
        }
        std::vector<uint8_t> tight(size_t(w) * h);
        for (int y = 0; y < h; ++y)
            memcpy(tight.data() + size_t(y) * w, image.data() + size_t(y) * pitch, size_t(w));
        const float s = Scales[size];
        const int ow = w ? std::max(1, int(std::lround(w * s))) : 0, oh = h ? std::max(1, int(std::lround(h * s))) : 0;
        c.alpha = s == 1.0f ? tight : scaled(tight, w, h, ow, oh);
        c.glyph.pitch = ow;
        c.glyph.bits = 8;
        c.glyph.width = ow;
        c.glyph.height = oh;
        c.glyph.left = int(std::lround(int(info.bitmapLeft) * s));
        c.glyph.top = int(std::lround(int(info.bitmapTop) * s));
        c.glyph.advance64 = int(std::lround(info.sfp26AdvanceH * s));
        c.ok = true;
        return c;
    }

    bool m_moduleLoaded = false;
    SceFontLibHandle m_lib = nullptr;
    SceFontHandle m_font = nullptr;
    int m_capHeight = 0;
    mutable std::map<unsigned, Cached> m_cache[FontSizes];
};

SystemFont* s_font = nullptr;

}

const Font* openSystemFont()
{
    if (!s_font)
    {
        s_font = new SystemFont;
        if (!s_font->open())
        {
            pf::logInfo("configurator: no system font (libpgf), using the bundled one");
            s_font->close();
            delete s_font;
            s_font = nullptr;
        }
    }
    return s_font;
}

void closeSystemFont()
{
    if (s_font)
    {
        s_font->close();
        delete s_font;
        s_font = nullptr;
    }
}
#else
const Font* openSystemFont()
{
    return nullptr;
}

void closeSystemFont()
{
}
#endif

}}
