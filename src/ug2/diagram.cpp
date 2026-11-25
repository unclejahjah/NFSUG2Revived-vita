// The Joypad Configuration picture composed from its layers, and the label texts (diagram.h).

#include "diagram.h"
#include "diagram_art.h"

#include <algorithm>
#include <cstring>

namespace ug2 { namespace diagram {

namespace
{

using namespace controlsdata;

const Text NoText = { 0, nullptr };

Text actionText(uint32_t action)
{
    switch (action)
    {
    case Accelerate:        return { 0x619b23b0, "Accelerate" };
    case Brake:             return { 0, "Brake" };     // the game only has "Brake / Reverse"
    case SteerLeft:         return { 0, "Steer Left" };
    case SteerRight:        return { 0, "Steer Right" };
    case Handbrake:         return { 0xb70ff4a4, "Handbrake" };
    case Nitrous:           return { 0x5e5c5f2e, "Nitrous" };
    case ShiftUp:           return { 0xdc1cd97d, "Shift Up" };
    case ShiftDown:         return { 0x56afe190, "Shift Down" };
    case LookBack:          return { 0x13121940, "Look Back" };
    case ChangeCamera:      return { 0xd38df194, "Change Camera" };
    case Pause:             return { 0x6369f4b8, "Pause" };
    case ResetCar:          return { 0x1cedc132, "Reset Car" };
    case EnterShop:         return { 0x4a1341b3, "Enter Race / Shop" };
    case WorldMap:          return { 0x6026c91f, "World Map" };
    case ReadSms:           return { 0x6ef09d0d, "Display SMS" };
    case CareerStats:       return { 0x70a30056, "Career Statistics" };
    case HydraulicsBounce:  return { 0x64730aac, "Hydraulics Bounce" };
    case Hydraulics:        return { 0xeac74624, "Hydraulics" };
    default:                return NoText;
    }
}

Label stickLabel(StickRole role)
{
    switch (role)
    {
    case RoleSteer:     return { { 0x63aa639d, "Steer" }, NoText };
    case RoleThrottle:  return { actionText(Accelerate), actionText(Brake) };
    case RoleShift:     return { actionText(ShiftUp), actionText(ShiftDown) };
    case RoleLook:      return { { 0, "Free Look" }, NoText };
    default:            return { NoText, NoText };
    }
}

uint32_t buttonAction(const Binds& b, Slot slot)
{
    switch (slot)
    {
    case SlotTriangle:  return b.map[Triangle];
    case SlotCircle:    return b.map[Circle];
    case SlotCross:     return b.map[Cross];
    case SlotSquare:    return b.map[Square];
    case SlotL:         return b.map[L1];
    case SlotR:         return b.map[R1];
    case SlotDpadUp:    return b.map[DpadUp];
    case SlotDpadDown:  return b.map[DpadDown];
    case SlotDpadLeft:  return b.map[DpadLeft];
    case SlotDpadRight: return b.map[DpadRight];
    default:            return 0;
    }
}

bool onDrawnButton(const Binds& b, uint32_t action)
{
    for (int i = 0; i < ControlSlots; ++i)
    {
        if (buttonAction(b, Slot(i)) == action)
            return true;
    }
    return false;
}

Label touchLabel(const Binds& b, uint32_t action)
{
    return onDrawnButton(b, action) ? Label{ NoText, NoText } : Label{ actionText(action), NoText };
}

std::string textString(const Text& t, const Lookup& lookup)
{
    const std::string game = t.hash ? lookup(t.hash) : std::string();
    return !game.empty() ? game : t.plain ? std::string(t.plain) : std::string();
}

// Decodes a make_layers.py run-length coded layer.
std::vector<uint8_t> decode(const diagramart::Layer& layer, int bpp)
{
    std::vector<uint8_t> out(size_t(layer.w) * layer.h * bpp, 0);
    size_t at = 0;
    for (int i = 0; i < layer.bytes && at < out.size();)
    {
        const int control = layer.data[i++];
        if (control & 0x80)
            at += size_t(control - 127) * bpp;
        else
        {
            const size_t bytes = size_t(control + 1) * bpp;
            memcpy(&out[at], &layer.data[i], std::min(bytes, out.size() - at));
            at += bytes;
            i += int(bytes);
        }
    }
    return out;
}

void blend(std::vector<uint8_t>& canvas, const diagramart::Layer& layer)
{
    const int bpp = layer.rgba ? 4 : 2;
    const std::vector<uint8_t> pixels = decode(layer, bpp);
    for (int row = 0; row < layer.h; ++row)
    {
        const int cy = layer.y + row;
        if (cy < 0 || cy >= diagramlayout::Height)
            continue;
        for (int col = 0; col < layer.w; ++col)
        {
            const int cx = layer.x + col;
            if (cx < 0 || cx >= diagramlayout::Width)
                continue;
            const uint8_t* s = &pixels[(size_t(row) * layer.w + col) * bpp];
            const int sa = layer.rgba ? s[3] : s[1];
            if (!sa)
                continue;
            const int sr = s[0], sg = layer.rgba ? s[1] : s[0], sb = layer.rgba ? s[2] : s[0];
            uint8_t* d = &canvas[(size_t(cy) * diagramlayout::Width + cx) * 4];
            const int da = d[3];
            const int weightD = (da * (255 - sa) + 127) / 255;
            const int outA = sa + weightD;
            for (int c = 0; c < 3; ++c)
            {
                const int sc = c == 0 ? sr : c == 1 ? sg : sb;
                d[c] = uint8_t(outA ? (sc * sa + d[c] * weightD + outA / 2) / outA : 0);
            }
            d[3] = uint8_t(outA);
        }
    }
}


void composeInto(const Plan& plan, std::vector<uint8_t>* canvas)
{
    canvas->assign(size_t(diagramlayout::Width) * diagramlayout::Height * 4, 0);
    blend(*canvas, diagramart::Layers[diagramart::BodyLayer]);
    for (int i = 0; i < SlotCount; ++i)
    {
        if (plan.leader[i])
            blend(*canvas, diagramart::Layers[diagramart::LeaderLayer + i]);
    }
}

// Writes w * h bytes of DXT3 blocks, row by row.
void encodeDxt3Into(const uint8_t* rgba, int w, int h, uint8_t* out)
{
    for (int by = 0; by < h; by += 4)
    {
        for (int bx = 0; bx < w; bx += 4)
        {
            const uint8_t* px[16];
            for (int i = 0; i < 16; ++i)
                px[i] = rgba + (size_t(by + i / 4) * w + bx + i % 4) * 4;
            for (int i = 0; i < 16; i += 2)
            {
                const int a0 = (px[i][3] * 15 + 127) / 255, a1 = (px[i + 1][3] * 15 + 127) / 255;
                *out++ = uint8_t(a0 | (a1 << 4));
            }
            int lo = -1, hi = -1, lowLum = 1 << 30, highLum = -1;
            for (int i = 0; i < 16; ++i)
            {
                if (!px[i][3])
                    continue;
                const int lum = px[i][0] * 3 + px[i][1] * 6 + px[i][2];
                if (lum < lowLum) { lowLum = lum; lo = i; }
                if (lum > highLum) { highLum = lum; hi = i; }
            }
            auto pack = [](const uint8_t* p) { return uint16_t(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3)); };
            uint16_t c0 = hi >= 0 ? pack(px[hi]) : 0, c1 = lo >= 0 ? pack(px[lo]) : 0;
            if (c0 < c1)
                std::swap(c0, c1);
            auto expand = [](uint16_t v, int c) {
                const int bits = c == 1 ? 6 : 5, shift = c == 0 ? 11 : c == 1 ? 5 : 0;
                const int x = (v >> shift) & ((1 << bits) - 1);
                return (x << (8 - bits)) | (x >> (2 * bits - 8));
            };
            int palette[4][3];
            for (int c = 0; c < 3; ++c)
            {
                palette[0][c] = expand(c0, c);
                palette[1][c] = expand(c1, c);
                palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3;
                palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3;
            }
            uint32_t indices = 0;
            if (c0 != c1)
            {
                for (int i = 0; i < 16; ++i)
                {
                    int best = 0, bestDistance = 1 << 30;
                    for (int k = 0; k < 4; ++k)
                    {
                        int d = 0;
                        for (int c = 0; c < 3; ++c)
                            d += (px[i][c] - palette[k][c]) * (px[i][c] - palette[k][c]);
                        if (d < bestDistance) { bestDistance = d; best = k; }
                    }
                    indices |= uint32_t(best) << (2 * i);
                }
            }
            *out++ = uint8_t(c0); *out++ = uint8_t(c0 >> 8);
            *out++ = uint8_t(c1); *out++ = uint8_t(c1 >> 8);
            for (int i = 0; i < 4; ++i)
                *out++ = uint8_t(indices >> (8 * i));
        }
    }
}

}

const char* slotName(Slot slot)
{
    return slot >= 0 && slot < SlotCount ? diagramlayout::Slots[slot].name : "";
}

bool isTouch(Slot slot)
{
    return slot >= ControlSlots && slot < SlotCount;
}

float labelX(Slot slot) { return diagramlayout::Slots[slot].x; }
float labelY(Slot slot) { return diagramlayout::Slots[slot].y; }
bool rightColumn(Slot slot) { return diagramlayout::Slots[slot].right; }
float labelRoom() { return diagramlayout::LabelRoom; }

Label labelFor(const Binds& b, Slot slot)
{
    switch (slot)
    {
    case SlotLeftStick:     return stickLabel(stickRole(b, 0));
    case SlotRightStick:    return stickLabel(stickRole(b, 1));
    case SlotTouchMap:      return touchLabel(b, WorldMap);
    case SlotTouchSms:      return touchLabel(b, ReadSms);
    case SlotTouchShop:     return touchLabel(b, EnterShop);
    case SlotTouchCareer:   return touchLabel(b, CareerStats);
    default:                return { actionText(buttonAction(b, slot)), NoText };
    }
}

bool assigned(const Label& label)
{
    return label.first.plain != nullptr;
}

Plan planFor(const Binds& b)
{
    Plan plan;
    for (int i = 0; i < SlotCount; ++i)
        plan.leader[i] = assigned(labelFor(b, Slot(i)));
    return plan;
}

std::string cacheKey(const Plan& plan)
{
    std::string key;
    for (bool l : plan.leader)
        key += l ? 'L' : '-';
    return key;
}

int layerCount(const Plan& plan)
{
    int n = 1;
    for (bool l : plan.leader)
        n += l ? 1 : 0;
    return n;
}

std::string labelString(const Label& label, const Lookup& lookup)
{
    const std::string first = textString(label.first, lookup), second = textString(label.second, lookup);
    return second.empty() ? first : first + " / " + second;
}

std::string fitLine(const std::string& text, double limit, const WidthFn& width)
{
    if (width(text) <= limit)
        return text;
    std::string s = text;
    while (!s.empty() && width(s + "...") > limit)
        s.pop_back();
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s + "...";
}

int width() { return diagramlayout::Width; }
int height() { return diagramlayout::Height; }
float originX() { return diagramlayout::OriginX; }
float originY() { return diagramlayout::OriginY; }
float scale() { return diagramlayout::Scale; }

std::vector<uint8_t> compose(const Plan& plan)
{
    std::vector<uint8_t> canvas;
    composeInto(plan, &canvas);
    return canvas;
}

std::vector<uint8_t> encodeDxt3(const uint8_t* rgba, int w, int h)
{
    std::vector<uint8_t> out(size_t(w) * h);
    encodeDxt3Into(rgba, w, h, out.data());
    return out;
}

const std::vector<uint8_t>& Composer::compose(const Plan& plan)
{
    composeInto(plan, &m_rgba);
    m_dxt.resize(size_t(diagramlayout::Width) * diagramlayout::Height);
    encodeDxt3Into(m_rgba.data(), diagramlayout::Width, diagramlayout::Height, m_dxt.data());
    return m_dxt;
}

}}