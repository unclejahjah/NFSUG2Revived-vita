// The bundled fallback font of the start-up screen.

#include "cfgui.h"
#include "font_atlas.h"

namespace ug2 { namespace cfgui {

namespace
{

class Bundled : public Font
{
public:
    bool glyph(FontSize size, unsigned code, Glyph* out) const override
    {
        if (code < 32 || code > 126 || size >= FontSizes)
            return false;
        const atlas::GlyphData& g = atlas::Sizes[size].glyphs[code - 32];
        out->alpha = atlas::Alpha + g.offset;
        out->pitch = (g.width + 1) / 2;
        out->bits = 4;
        out->width = g.width;
        out->height = g.height;
        out->left = g.left;
        out->top = g.top;
        out->advance64 = g.advance64;
        return true;
    }

    int ascent(FontSize size) const override
    {
        return atlas::Sizes[size].ascent;
    }
};

}

int centredAscent(const Font& reference, FontSize size, int capHeight)
{
    Glyph h;
    const int cap = reference.glyph(size, 'H', &h) ? h.height : capHeight;
    return reference.ascent(size) - cap / 2 + capHeight / 2;
}

const Font& bundledFont()
{
    static const Bundled s_font;
    return s_font;
}

}}
