// The start-up notice: plain text from the bundled font on a CPU framebuffer, gone before the GXM device starts.

#include "notice.h"
#include "font_atlas.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#ifdef __vita__
#include <psp2/display.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#endif

namespace ug2 { namespace notice {

namespace
{

const uint32_t Background = 0xff171514u;    // 0xAABBGGRR, as the launcher's
const uint32_t White = 0xffffffffu;
const int Size = 1;                         // atlas::Sizes index: Normal
const int Margin = 24;

int advance(unsigned char ch)
{
    return (ch >= 32 && ch <= 126) ? atlas::Sizes[Size].glyphs[ch - 32].advance64 : 0;
}

int textWidth(const std::string& text)
{
    int pen = 0;
    for (unsigned char ch : text)
        pen += advance(ch);
    return (pen + 32) >> 6;
}

void text(uint32_t* pixels, int width, int height, int stride, int x, int y, const std::string& s, uint32_t color)
{
    int pen = x << 6;
    const int baseline = y + atlas::Sizes[Size].ascent;
    for (unsigned char ch : s)
    {
        if (ch < 32 || ch > 126)
            continue;
        const atlas::GlyphData& g = atlas::Sizes[Size].glyphs[ch - 32];
        const uint8_t* alpha = atlas::Alpha + g.offset;
        const int pitch = (g.width + 1) / 2, gx = ((pen + 32) >> 6) + g.left, gy = baseline - g.top;
        for (int row = 0; row < g.height; ++row)
        {
            for (int col = 0; col < g.width; ++col)
            {
                const int px = gx + col, py = gy + row;
                const unsigned a = ((alpha[row * pitch + (col >> 1)] >> ((col & 1) ? 0 : 4)) & 15) * 17;
                if (!a || px < 0 || py < 0 || px >= width || py >= height)
                    continue;
                uint32_t& d = pixels[size_t(py) * stride + px];
                uint32_t out = 0xff000000u;
                for (int sh = 0; sh < 24; sh += 8)
                    out |= ((((d >> sh) & 0xff) * (255 - a) + ((color >> sh) & 0xff) * a) / 255) << sh;
                d = out;
            }
        }
        pen += g.advance64;
    }
}

std::vector<std::string> wrap(const std::string& s, int width)
{
    std::vector<std::string> lines;
    std::string current;
    size_t pos = 0;
    while (pos < s.size())
    {
        size_t end = s.find(' ', pos);
        if (end == std::string::npos)
            end = s.size();
        const std::string word = s.substr(pos, end - pos);
        const std::string tried = current.empty() ? word : current + " " + word;
        if (!current.empty() && textWidth(tried) > width)
        {
            lines.push_back(current);
            current = word;
        }
        else
            current = tried;
        pos = end + 1;
    }
    if (!current.empty())
        lines.push_back(current);
    return lines;
}

}

std::vector<std::string> rows(const std::vector<std::string>& lines, int width)
{
    std::vector<std::string> result;
    for (auto line = lines.rbegin(); line != lines.rend(); ++line)
        for (const std::string& part : wrap(*line, width - 2 * Margin))
            result.push_back(part);
    return result;
}

void draw(uint32_t* pixels, int width, int height, int stride, const std::vector<std::string>& lines)
{
    for (int y = 0; y < height; ++y)
        std::fill(pixels + size_t(y) * stride, pixels + size_t(y) * stride + width, Background);
    const std::vector<std::string> shown = rows(lines, width);
    const int lineHeight = 26;
    int y = height - Margin - int(shown.size()) * lineHeight;
    for (const std::string& row : shown)
    {
        text(pixels, width, height, stride, Margin, y, row, White);
        y += lineHeight;
    }
}

void show(const std::vector<std::string>& lines)
{
#ifdef __vita__
    const int width = 960, height = 544;
    const uint32_t bytes = 2 * 1024 * 1024;
    const SceUID block = sceKernelAllocMemBlock("notice", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, bytes, nullptr);
    void* base = nullptr;
    if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0)
    {
        if (block >= 0)
            sceKernelFreeMemBlock(block);
        return;
    }
    draw(static_cast<uint32_t*>(base), width, height, width, lines);
    SceDisplayFrameBuf frame;
    memset(&frame, 0, sizeof(frame));
    frame.size = sizeof(frame);
    frame.base = base;
    frame.pitch = width;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = width;
    frame.height = height;
    sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceKernelDelayThread(ShowUs);
    sceDisplaySetFrameBuf(nullptr, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
    sceKernelFreeMemBlock(block);
#else
    (void)lines;
#endif
}

}}
