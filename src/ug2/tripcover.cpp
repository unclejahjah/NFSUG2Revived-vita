// The trip's cover (tripcover.h): composed on the CPU from the bundled font when its text changes, shown by the GXM
// device over every image (Device::overlayPixels); only the spinner is drawn again each frame.

#include "tripcover.h"
#include "font_atlas.h"
#include <platform/gxm.h>
#include <platform/log.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <psp2/kernel/processmgr.h>

namespace ug2 { namespace tripcover {

namespace
{

const int W = 960, H = 544;
// 0xAABBGGRR; the browser's colours (style.css: --text, --muted, --lime, --line; the cover's radial gradient)
const uint32_t Text = 0xffffffffu, Muted = 0xffc4c4c4u, Lime = 0xff88f0c8u, Line = 0xff3a3a3au;
const int Inner = 0x20, Outer = 0x0a;
const int Small = 0, Normal = 1, Large = 2;     // atlas::Sizes
const int CardW = 454;                          // 400 of the browser's units at this height (544 / 480 px each)
const int Gap = 9;                              // a block's padding (0.5 em)

std::string s_lines;            // shown now ("" none)
std::vector<uint32_t> s_image;  // composed (the overlay's copy)
int s_spinX = 0, s_spinY = 0, s_spinD = 0;
bool s_shown = false;

uint32_t backdrop(int x, int y)
{
    const float dx = (x - 0.5f * W) / (0.6f * W), dy = (y - 0.45f * H) / (0.6f * H);
    const float t = std::min(1.0f, std::sqrt(dx * dx + dy * dy));
    const unsigned c = unsigned(Inner + (Outer - Inner) * t + 0.5f);
    return 0xff000000u | c << 16 | c << 8 | c;
}

uint32_t blend(uint32_t d, uint32_t c, unsigned a)
{
    uint32_t out = 0xff000000u;
    for (int sh = 0; sh < 24; sh += 8)
        out |= ((((d >> sh) & 0xff) * (255 - a) + ((c >> sh) & 0xff) * a) / 255) << sh;
    return out;
}

int textWidth(const std::string& s, int size)
{
    int pen = 0;
    for (unsigned char ch : s)
        if (ch >= 32 && ch <= 126)
            pen += atlas::Sizes[size].glyphs[ch - 32].advance64;
    return (pen + 32) >> 6;
}

// (cut to width with "...")
std::string fit(const std::string& s, int size, int width)
{
    if (textWidth(s, size) <= width)
        return s;
    std::string t = s;
    while (!t.empty() && textWidth(t + "...", size) > width)
        t.pop_back();
    return t + "...";
}

void text(int x, int y, const std::string& s, uint32_t color, int size)
{
    int pen = x << 6;
    const int baseline = y + atlas::Sizes[size].ascent;
    for (unsigned char ch : s)
    {
        if (ch < 32 || ch > 126)
            continue;
        const atlas::GlyphData& g = atlas::Sizes[size].glyphs[ch - 32];
        const uint8_t* alpha = atlas::Alpha + g.offset;
        const int pitch = (g.width + 1) / 2, gx = ((pen + 32) >> 6) + g.left, gy = baseline - g.top;
        for (int row = 0; row < g.height; ++row)
            for (int col = 0; col < g.width; ++col)
            {
                const int px = gx + col, py = gy + row;
                const unsigned a = ((alpha[row * pitch + (col >> 1)] >> ((col & 1) ? 0 : 4)) & 15) * 17;
                if (a && px >= 0 && py >= 0 && px < W && py < H)
                    s_image[size_t(py) * W + px] = blend(s_image[size_t(py) * W + px], color, a);
            }
        pen += g.advance64;
    }
}

int lineHeight(int size) { return atlas::Sizes[size].pixels * 7 / 5; }

struct Row { char look; std::string left, right; };

void compose(const std::string& lines)
{
    s_image.resize(size_t(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            s_image[size_t(y) * W + x] = backdrop(x, y);
    std::vector<Row> rows;
    size_t at = 0;
    while (at < lines.size())
    {
        size_t end = lines.find('\n', at);
        if (end == std::string::npos)
            end = lines.size();
        if (end > at)
        {
            const std::string l = lines.substr(at + 1, end - at - 1);
            const size_t tab = l.find('\t');
            rows.push_back(Row{ lines[at], tab == std::string::npos ? l : l.substr(0, tab),
                                tab == std::string::npos ? std::string() : l.substr(tab + 1) });
        }
        at = end + 1;
    }
    auto sizeOf = [](char look) { return look == '#' ? Large : look == '~' ? Small : Normal; };
    int height = 0;
    for (const Row& r : rows)
        height += r.look == '-' ? 2 * Gap + 1 : lineHeight(sizeOf(r.look));
    const int x0 = (W - CardW) / 2;
    int y = std::max(16, (H - height) / 2);
    s_spinD = 0;
    for (const Row& r : rows)
    {
        if (r.look == '-')
        {
            for (int x = x0; x < x0 + CardW; ++x)
                s_image[size_t(y + Gap) * W + x] = Line;
            y += 2 * Gap + 1;
            continue;
        }
        const int size = sizeOf(r.look), lh = lineHeight(size);
        const int top = y + (lh - atlas::Sizes[size].pixels) / 2 - 2;
        const uint32_t color = r.look == '#' || r.look == '!' ? Lime : r.look == '~' || r.look == '>' ? Muted : Text;
        int x = x0;
        if (r.look == '>')
        {
            // the spinner, an em wide, then the step
            s_spinD = atlas::Sizes[size].pixels;
            s_spinX = x0;
            s_spinY = y + (lh - s_spinD) / 2;
            x += s_spinD + 11;
        }
        const int rightW = r.right.empty() ? 0 : textWidth(r.right, size);
        text(x, top, fit(r.left, size, x0 + CardW - x - (rightW ? rightW + 12 : 0)), color, size);
        if (rightW)
            text(x0 + CardW - rightW, top, r.right, r.look == '*' ? Text : Muted, size);
        y += lh;
    }
}

// The spinner (style.css .lobby-spin: a ring, white at 20 %, a quarter of it lime, round in 0.9 s), into the overlay.
void spin(uint32_t* overlay)
{
    if (!s_spinD)
        return;
    const float turn = float(sceKernelGetProcessTimeWide() % 900000) / 900000.0f;
    const float r = 0.5f * s_spinD, cx = s_spinX + r, cy = s_spinY + r;
    for (int y = s_spinY; y < s_spinY + s_spinD; ++y)
        for (int x = s_spinX; x < s_spinX + s_spinD; ++x)
        {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = std::sqrt(dx * dx + dy * dy);
            const uint32_t bg = s_image[size_t(y) * W + x];
            const float edge = std::min(r - d, d - (r - 2.0f));          // inside the ring when > 0
            uint32_t out = bg;
            if (edge > -1.0f)
            {
                const unsigned cover = unsigned(255.0f * std::min(1.0f, edge + 1.0f));
                float a = std::atan2(dx, -dy) / 6.2831853f - turn;        // from the top, clockwise
                a -= std::floor(a);
                out = a < 0.25f ? blend(bg, Lime, cover) : blend(bg, Text, cover * 51 / 255);
            }
            overlay[size_t(y) * W + x] = out;
        }
}

}

void set(const char* lines)
{
    pf::gxm::Device& device = pf::gxm::Device::get();
    if (!lines || !*lines)
    {
        if (s_shown)
        {
            device.showOverlay(false);
            s_shown = false;
            s_lines.clear();
            pf::logInfo("trip cover: off");
        }
        return;
    }
    uint32_t* overlay = device.overlayPixels();
    if (!overlay)
        return;
    if (s_lines != lines)
    {
        s_lines = lines;
        compose(s_lines);
        memcpy(overlay, s_image.data(), s_image.size() * sizeof(uint32_t));
    }
    spin(overlay);
    if (!s_shown)
    {
        device.showOverlay(true);
        s_shown = true;
        pf::logInfo("trip cover: on");
    }
}

}}
