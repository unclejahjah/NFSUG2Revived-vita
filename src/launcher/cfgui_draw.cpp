// Drawing of the start-up screen and manager, styled like VitaShell's settings: dark background, centred
// headings, labels left of centre, values right of it, a green selection bar.

#include "cfgui.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ug2 { namespace cfgui {

using pf::Setting;
using pf::SettingType;

namespace
{

uint32_t rgb(uint32_t r, uint32_t g, uint32_t b)
{
    return 0xff000000u | (b << 16) | (g << 8) | r;
}

const uint32_t Background = rgb(0x14, 0x15, 0x17);
const uint32_t Panel      = rgb(0x26, 0x28, 0x2b);
const uint32_t Rule       = rgb(0x3a, 0x3d, 0x42);
const uint32_t White      = rgb(0xff, 0xff, 0xff);
const uint32_t Label      = rgb(0xc8, 0xcb, 0xd0);
const uint32_t Grey       = rgb(0x8c, 0x91, 0x99);
const uint32_t DarkGrey   = rgb(0x5a, 0x5e, 0x66);
const uint32_t Green      = rgb(0x8b, 0xe3, 0x8b);
const uint32_t Bar        = rgb(0x2f, 0x6e, 0x3e);
const uint32_t OnBar      = rgb(0xd8, 0xf0, 0xdc);
const uint32_t Missing    = rgb(0xe0, 0xa0, 0x90);
const uint32_t Orange     = rgb(0xff, 0xa5, 0x3a);
const uint32_t CrossBlue  = rgb(0x8f, 0xb0, 0xff);
const uint32_t CircleRed  = rgb(0xff, 0x78, 0x78);
const uint32_t SquarePink = rgb(0xf0, 0x9c, 0xd8);
const uint32_t TriGreen   = rgb(0x6c, 0xd8, 0xb0);

const int HeaderHeight = 64;
const int ListTop = 76;
const int RowHeight = 26;
const int VisibleRows = 11;
const int DescriptionTop = 374;
const int ConfigDescriptionLines = 2;
const int HintsTop = 446;
const int PanelLineHeight = 21;
const int PathTop = HintsTop - 25;     // the file line, right-justified below the description
const int ButtonsTop = 484;
const int ButtonHeight = 38;
const int Centre = Width / 2;
const int PopupWidth = 620;
const int PopupHeight = 190;
const int PopupText = PopupWidth - 48;

enum class Face { Cross, Circle, Square, Triangle };

float clamp01(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

}

// Anti-aliased drawing into a canvas: shapes by signed distance, text from a Font.
struct Configurator::Painter
{
    Canvas& c;
    const Font& font;

    void blend(int x, int y, uint32_t color, float a)
    {
        if (x < 0 || y < 0 || x >= c.width || y >= c.height || a <= 0.0f)
            return;
        uint32_t& d = c.pixels[size_t(y) * c.stride + x];
        if (a >= 1.0f)
        {
            d = color | 0xff000000u;
            return;
        }
        const uint32_t ia = uint32_t(a * 256.0f);
        uint32_t out = 0xff000000u;
        for (int s = 0; s < 24; s += 8)
        {
            const uint32_t dc = (d >> s) & 0xff, sc = (color >> s) & 0xff;
            out |= ((dc * (256 - ia) + sc * ia) >> 8) << s;
        }
        d = out;
    }

    void rect(int x, int y, int w, int h, uint32_t color)
    {
        const int x0 = std::max(0, x), y0 = std::max(0, y);
        const int x1 = std::min(c.width, x + w), y1 = std::min(c.height, y + h);
        for (int yy = y0; yy < y1; ++yy)
            std::fill(c.pixels + size_t(yy) * c.stride + x0, c.pixels + size_t(yy) * c.stride + std::max(x0, x1), color);
    }

    // Fills where sd < 0, blending one edge pixel.
    template< class F >
    void shape(float bx0, float by0, float bx1, float by1, uint32_t color, F sd)
    {
        for (int y = int(std::floor(by0)) - 1; y <= int(std::ceil(by1)); ++y)
            for (int x = int(std::floor(bx0)) - 1; x <= int(std::ceil(bx1)); ++x)
                blend(x, y, color, clamp01(0.5f - sd(x + 0.5f, y + 0.5f)));
    }

    void roundRect(float x, float y, float w, float h, float r, uint32_t color)
    {
        const float cx = x + w / 2, cy = y + h / 2, hx = w / 2 - r, hy = h / 2 - r;
        shape(x, y, x + w, y + h, color, [=](float px, float py) {
            const float qx = std::fabs(px - cx) - hx, qy = std::fabs(py - cy) - hy;
            const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
            return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
        });
    }

    void circle(float cx, float cy, float r, uint32_t color)
    {
        shape(cx - r, cy - r, cx + r, cy + r, color, [=](float px, float py) {
            return std::hypot(px - cx, py - cy) - r;
        });
    }

    void ring(float cx, float cy, float r, float width, uint32_t color)
    {
        shape(cx - r, cy - r, cx + r, cy + r, color, [=](float px, float py) {
            return std::fabs(std::hypot(px - cx, py - cy) - (r - width / 2)) - width / 2;
        });
    }

    void segment(float ax, float ay, float bx, float by, float width, uint32_t color)
    {
        const float r = width / 2;
        shape(std::min(ax, bx) - r, std::min(ay, by) - r, std::max(ax, bx) + r, std::max(ay, by) + r, color,
              [=](float px, float py) {
                  const float dx = bx - ax, dy = by - ay;
                  const float t = clamp01(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy));
                  return std::hypot(px - ax - t * dx, py - ay - t * dy) - r;
              });
    }

    void triangle(float ax, float ay, float bx, float by, float qx, float qy, uint32_t color)
    {
        const float xs[3] = { ax, bx, qx }, ys[3] = { ay, by, qy };
        const float area = (bx - ax) * (qy - ay) - (by - ay) * (qx - ax);
        shape(std::min({ ax, bx, qx }), std::min({ ay, by, qy }), std::max({ ax, bx, qx }), std::max({ ay, by, qy }),
              color, [&](float px, float py) {
                  float inside = -1e9f;
                  for (int e = 0; e < 3; ++e)
                  {
                      const float ex = xs[(e + 1) % 3] - xs[e], ey = ys[(e + 1) % 3] - ys[e];
                      const float cross = (ex * (py - ys[e]) - ey * (px - xs[e])) / std::hypot(ex, ey);
                      inside = std::max(inside, area > 0 ? -cross : cross);
                  }
                  return inside;
              });
    }

    int textWidth(const std::string& s, FontSize size) const
    {
        int pen = 0;
        Glyph g;
        for (unsigned char ch : s)
            if (font.glyph(size, ch, &g) || font.glyph(size, '?', &g))
                pen += g.advance64;
        return (pen + 32) >> 6;
    }

    // y is the top of the line; returns the width.
    int text(int x, int y, const std::string& s, uint32_t color, FontSize size)
    {
        int pen = x << 6;
        const int baseline = y + font.ascent(size);
        Glyph g;
        for (unsigned char ch : s)
        {
            if (!font.glyph(size, ch, &g) && !font.glyph(size, '?', &g))
                continue;
            const int gx = ((pen + 32) >> 6) + g.left, gy = baseline - g.top;
            for (int row = 0; row < g.height; ++row)
            {
                const uint8_t* line = g.alpha + row * g.pitch;
                for (int col = 0; col < g.width; ++col)
                {
                    const int a = g.bits == 8 ? line[col] : ((line[col >> 1] >> ((col & 1) ? 0 : 4)) & 15) * 17;
                    if (a)
                        blend(gx + col, gy + row, color, a / 255.0f);
                }
            }
            pen += g.advance64;
        }
        return ((pen + 32) >> 6) - x;
    }

    void centred(int y, const std::string& s, uint32_t color, FontSize size)
    {
        text((c.width - textWidth(s, size)) / 2, y, s, color, size);
    }

    void face(float cx, float cy, float r, Face kind)
    {
        circle(cx, cy, r, rgb(0x33, 0x36, 0x3b));
        const float s = r * 0.42f, w = std::max(1.6f, r * 0.17f);
        switch (kind)
        {
        case Face::Cross:
            segment(cx - s, cy - s, cx + s, cy + s, w, CrossBlue);
            segment(cx - s, cy + s, cx + s, cy - s, w, CrossBlue);
            break;
        case Face::Circle:
            ring(cx, cy, s * 1.2f, w, CircleRed);
            break;
        case Face::Square:
            segment(cx - s, cy - s, cx + s, cy - s, w, SquarePink);
            segment(cx + s, cy - s, cx + s, cy + s, w, SquarePink);
            segment(cx + s, cy + s, cx - s, cy + s, w, SquarePink);
            segment(cx - s, cy + s, cx - s, cy - s, w, SquarePink);
            break;
        case Face::Triangle:
            segment(cx, cy - s * 1.15f, cx + s * 1.1f, cy + s * 0.75f, w, TriGreen);
            segment(cx + s * 1.1f, cy + s * 0.75f, cx - s * 1.1f, cy + s * 0.75f, w, TriGreen);
            segment(cx - s * 1.1f, cy + s * 0.75f, cx, cy - s * 1.15f, w, TriGreen);
            break;
        }
    }

    // A START, L or R badge; returns its width.
    int badge(int x, int y, const std::string& label, int height)
    {
        const int w = std::max(height + 4, textWidth(label, FontSmall) + 14);
        roundRect(x, y, w, height, height / 2.0f, rgb(0x3a, 0x3d, 0x42));
        text(x + (w - textWidth(label, FontSmall)) / 2, y + (height - 22) / 2, label, White, FontSmall);
        return w;
    }

    void dpad(float cx, float cy, bool vertical)
    {
        const uint32_t lit = White, unlit = DarkGrey;
        const float a = 6.5f, t = 2.6f;
        roundRect(cx - t, cy - a - t + 0.5f, 2 * t, a + t - 1.5f, 1.2f, vertical ? lit : unlit);
        roundRect(cx - t, cy + 1.0f, 2 * t, a + t - 1.5f, 1.2f, vertical ? lit : unlit);
        roundRect(cx - a - t + 0.5f, cy - t, a + t - 1.5f, 2 * t, 1.2f, vertical ? unlit : lit);
        roundRect(cx + 1.0f, cy - t, a + t - 1.5f, 2 * t, 1.2f, vertical ? unlit : lit);
    }

    void fileIcon(float x, float y)
    {
        const uint32_t c = DarkGrey;
        segment(x + 1, y + 2, x + 8, y + 2, 1.4f, c);
        segment(x + 8, y + 2, x + 12, y + 6, 1.4f, c);
        segment(x + 12, y + 6, x + 12, y + 17, 1.4f, c);
        segment(x + 12, y + 17, x + 1, y + 17, 1.4f, c);
        segment(x + 1, y + 17, x + 1, y + 2, 1.4f, c);
        segment(x + 8, y + 2, x + 8, y + 6, 1.2f, c);
        segment(x + 8, y + 6, x + 12, y + 6, 1.2f, c);
    }

    void stickIcon(float cx, float cy)
    {
        ring(cx, cy, 8.5f, 1.6f, DarkGrey);
        circle(cx, cy, 4.5f, White);
    }

    void arrow(float x, float cy, bool right, uint32_t color)
    {
        const float w = 6.0f, h = 10.0f;
        if (right)
            triangle(x, cy - h / 2, x + w, cy, x, cy + h / 2, color);
        else
            triangle(x + w, cy - h / 2, x, cy, x + w, cy + h / 2, color);
    }

    void darken()
    {
        for (int y = 0; y < c.height; ++y)
        {
            uint32_t* row = c.pixels + size_t(y) * c.stride;
            for (int x = 0; x < c.width; ++x)
                row[x] = 0xff000000u | ((row[x] >> 1) & 0x007f7f7fu);
        }
    }
};

namespace
{

std::string clip(const std::string& text, size_t chars)
{
    return text.size() <= chars ? text : text.substr(0, chars - 3) + "...";
}

int firstVisible(int selection, int count, int visible = VisibleRows)
{
    return std::max(0, std::min(selection - visible / 2, count - visible));
}

std::vector<std::string> wrap(const Configurator::Painter& p, const std::string& text, int width, FontSize size)
{
    std::vector<std::string> lines;
    std::string current;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find(' ', pos);
        if (end == std::string::npos)
            end = text.size();
        const std::string word = text.substr(pos, end - pos);
        const std::string tried = current.empty() ? word : current + " " + word;
        if (!current.empty() && p.textWidth(tried, size) > width)
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

void description(Configurator::Painter& p, const std::string& text, uint32_t color)
{
    const std::vector<std::string> lines = wrap(p, text, Width - 56, FontSmall);
    for (size_t l = 0; l < std::min<size_t>(2, lines.size()); ++l)
        p.text(28, DescriptionTop + 6 + int(l) * 20, lines[l], color, FontSmall);
}

}

Rect buttonRect(int which)
{
    const int w = 176, between = 20;
    return Rect{ (Width - 2 * w - between) / 2 + which * (w + between), ButtonsTop, w, ButtonHeight };
}

Rect choiceRect(int which)
{
    const int w = 190, h = 44, between = 40;
    return Rect{ (Width - 2 * w - between) / 2 + which * (w + between), (Height - 190) / 2 + 190 - h - 24, w, h };
}

Rect okRect(int)
{
    const Rect r = choiceRect(0);
    return Rect{ (Width - r.w) / 2, r.y, r.w, r.h };
}

void drawMessage(Canvas& canvas, const Font& font, const std::vector<std::string>& lines)
{
    Configurator::Painter p{ canvas, font };
    p.rect(0, 0, canvas.width, canvas.height, Background);
    int y = (Height - int(lines.size()) * 32) / 2;
    for (size_t i = 0; i < lines.size(); ++i, y += 32)
        p.centred(y, lines[i], i ? Grey : White, FontNormal);
}

void Configurator::draw(Canvas& canvas, const Font& font) const
{
    Painter p{ canvas, font };
    p.rect(0, 0, canvas.width, canvas.height, Background);
    switch (m_screen)
    {
    case Screen::Splash:
        drawSplash(p);
        break;
    case Screen::Manager:
        drawManager(p);
        break;
    case Screen::ConfirmCancel:
        drawManager(p);
        drawPopup(p, "Any mod and configuration changes will not be saved");
        break;
    case Screen::ConfirmSave:
        drawManager(p);
        drawPopup(p, "Save these mod and configuration changes?");
        break;
    case Screen::Launch:
        break;
    case Screen::ConfirmDownload:
    case Screen::DownloadError:
        drawManager(p);
        drawPopupFrame(p, wrap(p, popupText(), PopupText, FontNormal), m_screen == Screen::ConfirmDownload);
        break;
    case Screen::Downloading:
        drawManager(p);
        drawProgress(p);
        break;
    case Screen::ConfirmStop:
        drawManager(p);
        drawProgress(p);
        drawPopupFrame(p, wrap(p, popupText(), PopupText, FontNormal), true);
        break;
    }
}

void Configurator::drawSplash(Painter& p) const
{
    p.centred(140, "Need for Speed Underground 2", White, FontLarge);
    p.centred(186, "for the PS Vita" + (m_version.empty() ? std::string() : "  " + m_version), Grey, FontSmall);
    const std::string before = "Press ", after = " to configure.";
    const int badgeWidth = p.textWidth("START", FontSmall) + 14;
    const int total = p.textWidth(before, FontNormal) + badgeWidth + 8 + p.textWidth(after, FontNormal);
    int x = (Width - total) / 2;
    x += p.text(x, 262, before, White, FontNormal);
    x += p.badge(x, 261, "START", 28) + 8;
    p.text(x, 262, after, White, FontNormal);
    p.centred(300, "Any other button to skip.", Grey, FontNormal);
    p.centred(372, "Game will launch in 3 Seconds", Grey, FontSmall);
}

void Configurator::drawManager(Painter& p) const
{
    const char* names[2] = { "Mods", "Config" };
    const int gap = 48;
    const int w0 = p.textWidth(names[0], FontNormal), w1 = p.textWidth(names[1], FontNormal);
    const int x0 = Centre - gap / 2 - w0, x1 = Centre + gap / 2;
    p.badge(x0 - 46, 19, "L", 24);
    p.badge(x1 + w1 + 18, 19, "R", 24);
    for (int t = 0; t < 2; ++t)
    {
        const bool active = int(m_tab) == t;
        const int x = t ? x1 : x0, w = t ? w1 : w0;
        p.text(x, 18, names[t], active ? White : Grey, FontNormal);
        if (active)
            p.roundRect(x - 4, 50, w + 8, 3, 1.5f, Green);
    }
    const int changes = changeCount();
    if (changes)
    {
        const std::string note = std::to_string(changes) + (changes == 1 ? " change" : " changes");
        p.text(Width - 24 - p.textWidth(note, FontSmall), 22, note, Green, FontSmall);
    }
    p.rect(0, HeaderHeight, Width, 1, Rule);
    if (m_tab == Tab::Config)
        drawConfigRows(p);
    else
        drawModRows(p);

    drawHints(p, true);
    const BottomRow row = bottomRow(p.font);
    if (!row.path.empty())
    {
        p.fileIcon(row.pathX, PathTop + 2);
        p.text(row.pathX + 18, PathTop, row.path, DarkGrey, FontSmall);
    }

    for (int b = 0; b < 2; ++b)
    {
        const Rect r = buttonRect(b);
        const int left = r.x, buttonWidth = r.w;
        const bool focused = onButtons() && m_buttonFocus == b;
        p.roundRect(left, ButtonsTop, buttonWidth, ButtonHeight, 10, focused ? Bar : Panel);
        const std::string label = b ? "Save" : "Cancel";
        const int iconWidth = b ? p.textWidth("START", FontSmall) + 14 : 22;
        const int contentX = left + (buttonWidth - iconWidth - 10 - p.textWidth(label, FontNormal)) / 2;
        if (b)
            p.badge(contentX, ButtonsTop + 6, "START", 26);
        else
            p.face(contentX + 11, ButtonsTop + ButtonHeight / 2.0f, 11, Face::Circle);
        p.text(contentX + iconWidth + 10, ButtonsTop + 6, label, White, FontNormal);
    }
}

int Configurator::drawHints(Painter& p, bool draw) const
{
    int x = 28;
    const float hy = HintsTop + 11;
    auto label = [&](int at, const char* text) {
        return draw ? p.text(at, HintsTop, text, Grey, FontSmall) : p.textWidth(text, FontSmall);
    };
    if (draw)
    {
        p.dpad(x + 9, hy, true);
        p.stickIcon(x + 31, hy);
    }
    x += label(x + 46, "Navigate") + 70;
    const bool config = m_tab == Tab::Config;
    if (draw)
    {
        p.dpad(x + 9, hy, false);
        if (config)
            p.stickIcon(x + 31, hy);
    }
    x += label(x + (config ? 46 : 24), "Change") + (config ? 70 : 48);
    if (config)
    {
        if (draw)
            p.face(x + 9, hy, 9, Face::Square);
        x += label(x + 24, "Default") + 48;
    }
    if (draw)
        p.face(x + 9, hy, 9, Face::Triangle);
    const bool shown = m_show[int(m_tab)];
    x += label(x + 24, config ? (shown ? "Hide dev settings" : "Show dev settings")
                              : (shown ? "Hide unsupported" : "Show unsupported")) + 48;
    const int badge = draw ? p.badge(x, HintsTop, "SELECT", 22) : p.textWidth("SELECT", FontSmall) + 14;
    const bool flash = m_nowUs < m_flashUntil;
    const char* text = flash ? "Set to default" : "Change all to default (Hold 3 sec.)";
    const int width = draw ? p.text(x + badge + 6, HintsTop, text, flash ? Green : Grey, FontSmall)
                           : p.textWidth("Change all to default (Hold 3 sec.)", FontSmall);
    return x + badge + 6 + width;
}

Configurator::BottomRow Configurator::bottomRow(const Font& font) const
{
    Canvas none = { nullptr, 0, 0, 0 };
    Painter p{ none, font };
    BottomRow row;
    row.pathY = PathTop;
    row.hintsEnd = drawHints(p, false);
    row.path = fileLine();
    const int room = Width - 56 - 18;
    if (row.path.empty())
    {
        row.pathX = Width - 28;
        return row;
    }
    const size_t slash = row.path.find_last_of('/');
    for (size_t cut = 0; p.textWidth(row.path, FontSmall) > room && slash != std::string::npos && cut < slash; ++cut)
        row.path = "..." + fileLine().substr(cut + 1);
    row.pathX = Width - 28 - p.textWidth(row.path, FontSmall) - 18;
    return row;
}

void Configurator::drawConfigRows(Painter& p) const
{
    const int count = rowCount(), sel = selection();
    const int top = firstVisible(sel < count ? sel : count - 1, count);
    for (int i = top; i < std::min(count, top + VisibleRows); ++i)
    {
        const Row& row = configRow(i);
        const int y = ListTop + (i - top) * RowHeight, ty = y + 1;
        const float cy = y + RowHeight / 2.0f;
        if (!row.setting)
        {
            p.centred(ty, pf::settingGroupName(row.group), White, FontNormal);
            continue;
        }
        const Setting& s = *row.setting;
        const bool selected = i == sel, changedRow = row.current != row.initial;
        if (selected)
            p.rect(0, y, Width, RowHeight, Bar);
        const std::string label = clip(s.key, 40);
        const int labelWidth = p.textWidth(label, FontNormal);
        p.text(Centre - 18 - labelWidth, ty, label, selected ? White : Label, FontNormal);
        if (changedRow)
            p.circle(Centre - 30 - labelWidth, cy, 3, selected ? White : Green);
        int x = Centre + 14;
        if (s.type == SettingType::String)
        {
            const std::string shown = row.current.empty() ? "(empty)"
                                    : (s.flags & pf::SettingSecret) ? "(set)"
                                    : "\"" + clip(row.current, 28) + "\"";
            x += p.text(x, ty, shown, selected ? OnBar : Grey, FontNormal);
        }
        else
        {
            bool canLeft = true, canRight = true;
            int n = 0;
            if (s.type == SettingType::Int && pf::cfgfile::parseInt(row.current.c_str(), &n))
            {
                canLeft = n > s.min || (s.flags & (pf::SettingOff | pf::SettingAuto));
                canRight = n < s.max;
            }
            else if (s.type == SettingType::Int)
                canLeft = false;
            const uint32_t on = selected ? White : Green, off = selected ? rgb(0x5c, 0x8f, 0x68) : DarkGrey;
            p.arrow(x, cy, false, canLeft ? on : off);
            x += 14;
            x += p.text(x, ty, clip(row.current, 24), selected ? White : Green, FontNormal);
            p.arrow(x + 8, cy, true, canRight ? on : off);
            x += 14;
        }
        const std::string note = atDefault(s, row.current) ? "(default)" : "default " + clip(pf::defaultText(s), 24);
        p.text(x + 16, ty + 3, note, selected ? OnBar : DarkGrey, FontSmall);
    }
    if (count > VisibleRows)
    {
        const int trackHeight = VisibleRows * RowHeight;
        const int thumb = std::max(16, trackHeight * VisibleRows / count);
        p.roundRect(Width - 8, ListTop + (trackHeight - thumb) * top / (count - VisibleRows), 3, thumb, 1.5f, DarkGrey);
    }
    p.rect(0, DescriptionTop - 4, Width, 1, Rule);
    if (onButtons())
        description(p, m_buttonFocus ? "Save: write the changes to NeedForSpeedUnderground2.cfg and start the game."
                                     : "Cancel: start the game without saving the changes.", Grey);
    else
    {
        const Setting& s = *configRow(sel).setting;
        std::string text = s.description;
        if (s.type == SettingType::String)
            text += " Edit it in the cfg file.";
        if (const char* old = pf::oldKeyOf(s.key))
            text += std::string(" Old name: ") + old + ".";
        const std::string warning = fileWarning();
        if (warning.empty())
            description(p, text, Label);
        else
        {
            p.text(28, DescriptionTop + 6, warning, Missing, FontSmall);
            const std::vector<std::string> lines = wrap(p, text, Width - 56, FontSmall);
            p.text(28, DescriptionTop + 26, lines.size() > 1 ? lines[0] + " ..." : lines[0], Label, FontSmall);
        }
    }
}

std::vector<Configurator::PanelLine> Configurator::panelLines(const Painter& p, const Mod* mod, bool buttons) const
{
    std::vector<PanelLine> lines;
    auto add = [&](const std::string& text, uint32_t color, int indent) {
        for (const std::string& line : wrap(p, text, Width - 56 - indent, FontSmall))
            lines.push_back({ line, color, indent });
    };
    if (buttons)
        add(m_buttonFocus ? "Save: write the changes (cfg and mod settings files) and start the game."
                          : "Cancel: start the game without saving the changes.", Grey, 0);
    if (!mod)
        return lines;
    if (!mod->view.warning.empty())
        add(mod->view.warning, Missing, 0);
    for (const std::string& detail : mod->view.details)
        add(detail, Label, 0);
    if (!mod->view.requirements.empty())
        add("Requirements: " + mod->view.requirements, Orange, 0);
    return lines;
}

std::vector<Configurator::PanelLine> Configurator::modPanel(const Font& font, int mod) const
{
    Canvas none = { nullptr, 0, 0, 0 };
    const Painter p{ none, font };
    return panelLines(p, &m_mods[mod], false);
}

uint32_t Configurator::requirementColor()
{
    return Orange;
}

int Configurator::lineHeight()
{
    return PanelLineHeight;
}

void Configurator::drawModRows(Painter& p) const
{
    const int count = int(m_modRows.size()), sel = selection();
    if (!count)
        p.centred(ListTop + 4, "No mods are available in this build.", Grey, FontNormal);
    auto panel = [&](const Mod* mod, bool buttons) { return panelLines(p, mod, buttons); };
    const int lineHeight = PanelLineHeight;
    int needed = ConfigDescriptionLines;
    for (const Mod& mod : m_mods)
        if (mod.view.supported)
            needed = std::max(needed, int(panel(&mod, false).size()));
    const int panelTop = PathTop - 4 - needed * lineHeight - 6;
    const int modRows = std::max(1, (panelTop - 8 - ListTop) / RowHeight);
    const int top = firstVisible(sel < count ? sel : count - 1, count, modRows);
    for (int i = top; i < std::min(count, top + modRows); ++i)
    {
        const int y = ListTop + (i - top) * RowHeight, ty = y + 1;
        const float cy = y + RowHeight / 2.0f;
        if (heading(i))
        {
            p.centred(ty, m_modRows[i].group, White, FontNormal);
            continue;
        }
        const Mod& mod = m_mods[modAt(i)];
        if (!mod.view.supported)
        {
            const int labelWidth = p.textWidth(mod.view.name, FontSmall);
            p.text(Centre - 18 - labelWidth, ty + 3, mod.view.name, DarkGrey, FontSmall);
            const int w = p.text(Centre + 14, ty + 3, clip(mod.view.value, 20), DarkGrey, FontSmall);
            p.text(Centre + 30 + w, ty + 3, "(unsupported)", DarkGrey, FontSmall);
            continue;
        }
        const bool selected = i == sel;
        if (selected)
            p.rect(0, y, Width, RowHeight, Bar);
        const int labelWidth = p.textWidth(mod.view.name, FontNormal);
        p.text(Centre - 18 - labelWidth, ty, mod.view.name, selected ? White : Label, FontNormal);
        if (mod.current != mod.view.enabled)
            p.circle(Centre - 30 - labelWidth, cy, 3, selected ? White : Green);
        int x = Centre + 14;
        p.arrow(x, cy, false, selected ? White : Green);
        x += 14;
        x += p.text(x, ty, mod.current ? "on" : "off", selected ? White : Green, FontNormal);
        p.arrow(x + 8, cy, true, selected ? White : Green);
        x += 14;
        if (!mod.current)
            p.text(x + 16, ty + 3, "(vanilla)", selected ? OnBar : DarkGrey, FontSmall);
    }
    p.rect(0, panelTop - 4, Width, 1, Rule);
    const std::vector<PanelLine> lines = onButtons() || !count ? panel(nullptr, onButtons()) : panel(&m_mods[modAt(sel)], false);
    for (int l = 0; l < std::min(int(lines.size()), needed); ++l)
    {
        std::string text = lines[l].text;
        if (l == needed - 1 && int(lines.size()) > needed)
        {
            while (!text.empty() && p.textWidth(text + "...", FontSmall) > Width - 56 - lines[l].indent)
                text.pop_back();
            text += "...";
        }
        p.text(28 + lines[l].indent, panelTop + 6 + l * lineHeight, text, lines[l].color, FontSmall);
    }
}

void Configurator::drawPopupFrame(Painter& p, const std::vector<std::string>& lines, bool choices) const
{
    p.darken();
    const int x = (Width - PopupWidth) / 2, y = (Height - PopupHeight) / 2;
    p.roundRect(x, y, PopupWidth, PopupHeight, 16, Panel);
    const int shown = std::min<int>(3, int(lines.size()));
    for (int l = 0; l < shown; ++l)
        p.centred(y + 24 + l * 28 + (3 - shown) * 14, lines[l], White, FontNormal);
    for (int c = choices ? 0 : 1; c < 2; ++c)
    {
        const Rect r = choices ? choiceRect(c) : okRect(0);
        p.roundRect(r.x, r.y, r.w, r.h, 10, rgb(0x34, 0x37, 0x3d));
        const std::string label = c ? "OK" : "Go Back";
        const int content = 24 + 10 + p.textWidth(label, FontNormal);
        const int left = r.x + (r.w - content) / 2;
        p.face(left + 12, r.y + r.h / 2.0f, 12, c ? Face::Cross : Face::Circle);
        p.text(left + 34, r.y + (r.h - 26) / 2, label, c ? Green : White, FontNormal);
    }
}

void Configurator::drawProgress(Painter& p) const
{
    p.darken();
    const int x = (Width - PopupWidth) / 2, y = (Height - PopupHeight) / 2;
    p.roundRect(x, y, PopupWidth, PopupHeight, 16, Panel);
    p.centred(y + 28, "Files for " + m_mods[m_download].view.downloadName, White, FontNormal);
    const int barX = x + 40, barY = y + 78, barW = PopupWidth - 80, barH = 16;
    p.roundRect(barX, barY, barW, barH, barH / 2.0f, Rule);
    const float fraction = std::min(1.0f, std::max(0.0f, m_progress.fraction));
    if (fraction > 0.0f)
        p.roundRect(barX, barY, std::max(float(barH), barW * fraction), barH, barH / 2.0f, Green);
    p.centred(y + 104, progressText(), Label, FontSmall);
    if (!progressPrompt())
        return;
    const int width = 24 + 8 + p.textWidth("Cancel", FontSmall), left = (Width - width) / 2;
    p.face(left + 10, y + 155, 10, Face::Circle);
    p.text(left + 28, y + 144, "Cancel", Grey, FontSmall);
}

void Configurator::drawPopup(Painter& p, const char* title) const
{
    p.darken();
    const int w = 620, h = 190, x = (Width - w) / 2, y = (Height - h) / 2;
    p.roundRect(x, y, w, h, 16, Panel);
    p.centred(y + 38, title, White, FontNormal);
    const int changes = changeCount();
    p.centred(y + 72, changes ? std::to_string(changes) + (changes == 1 ? " change" : " changes") : "No changes",
              changes ? Green : Grey, FontSmall);
    for (int c = 0; c < 2; ++c)
    {
        const Rect r = choiceRect(c);
        p.roundRect(r.x, r.y, r.w, r.h, 10, rgb(0x34, 0x37, 0x3d));
        const std::string label = c ? "OK" : "Go Back";
        const int content = 24 + 10 + p.textWidth(label, FontNormal);
        const int left = r.x + (r.w - content) / 2;
        p.face(left + 12, r.y + r.h / 2.0f, 12, c ? Face::Cross : Face::Circle);
        p.text(left + 34, r.y + (r.h - 26) / 2, label, c ? Green : White, FontNormal);
    }
}

}}
