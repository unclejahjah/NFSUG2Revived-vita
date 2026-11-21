// Native sub_48bdf0: particle draw pass (thiscall: view, ret 4). Each particle's colour, size, frame and corners go
// to the batch (pass 0, sub_5c0e10 inline) or an oriented quad (pass 1).

#include <native/common.h>
#include <native/particle_batch.h>
#include <native/physics.h>
#include <platform/switches.h>
#include <platform/cached.h>
#include <platform/config.h>
#include <offload.h>

namespace ug2
{

void sub_48bdf0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_43bdf0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5b7de0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5b7e80(win32::WinApplication* app, x86::CPU& cpu);
void sub_5c0e10(win32::WinApplication* app, x86::CPU& cpu);
void sub_5c7e80(win32::WinApplication* app, x86::CPU& cpu);
void sub_5ce120(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

using x86::reg32;
using x86::sreg32;

using batch::ftol2;

inline Real integer(reg32 value) { return Real(sreg32(value)); }

#if defined(__ARM_FP) && !defined(NFSU_FPU_DOUBLE)

// One instruction each, operands in x87 order.
#ifdef __aarch64__
#define UG2_FLOAT_OP(name, a64, a32) \
    inline float name(float a, float b) { float r; __asm__(a64 " %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b)); return r; }
#else
#define UG2_FLOAT_OP(name, a64, a32) \
    inline float name(float a, float b) { float r; __asm__(a32 " %0, %1, %2" : "=t"(r) : "t"(a), "t"(b)); return r; }
#endif
UG2_FLOAT_OP(add, "fadd", "vadd.f32")
UG2_FLOAT_OP(sub, "fsub", "vsub.f32")
UG2_FLOAT_OP(mul, "fmul", "vmul.f32")
UG2_FLOAT_OP(div, "fdiv", "vdiv.f32")
#undef UG2_FLOAT_OP

#else

inline Real add(Real a, Real b) { return a + b; }
inline Real sub(Real a, Real b) { return a - b; }
inline Real mul(Real a, Real b) { return a * b; }
inline Real div(Real a, Real b) { return a / b; }

#endif

// Forward copy of 16 words.
inline void copyMatrix(reg32 destination, reg32 source)
{
    if (destination - source + 63u > 126u)
    {
        memcpy(rc::host(destination), rc::host(source), 64);
        return;
    }
    for (reg32 i = 0; i < 64; i += 4)
        rc::st32(destination + i, rc::ld32(source + i));
}

inline void store(reg32 address, Real value) { rc::stf32a(address, float(value)); }

// Guest callee with the registers the guest holds.
inline bool call(win32::WinApplication* app, x86::CPU& cpu, void (*child)(win32::WinApplication*, x86::CPU&),
                 reg32 esp, reg32 ecx, reg32 ebx, reg32 esi, reg32 edi)
{
    cpu.ecx = ecx;
    cpu.ebx = ebx;
    cpu.esi = esi;
    cpu.edi = edi;
    cpu.esp = esp;
    child(app, cpu);
    return !cpu.terminate;
}

typedef float __attribute__((may_alias)) FrameFloat;

// sub_4881d0, first output: cubic with matrix rows as coefficients.
inline Real cubicFirst(const FrameFloat* m, Real t, Real t2, Real t3)
{
    Real x = t2 * Real(m[4]);
    x += t * Real(m[8]);
    x += t3 * Real(m[0]);
    x += Real(m[12]);
    return Real(float(x));
}

// sub_4881d0, outputs 1 to 3.
inline Real cubicOther(const FrameFloat* m, int i, Real t, Real t2, Real t3)
{
    Real y = t2 * Real(m[4 + i]);
    y += t3 * Real(m[i]);
    y += t * Real(m[8 + i]);
    y += Real(m[12 + i]);
    return Real(float(y));
}

// Clamp to 0..max of one colour channel.
inline Real channel(Real value, Real zero, Real maximum)
{
    if (!(value > zero))
        return zero;
    return maximum < value ? maximum : value;
}

struct Shared
{
    Real one, zero, age, maximum, scale, timeScale;
    Real viewZ[4]; // third column of the view matrix
};

struct Group
{
    const FrameFloat* colour; // cubic coefficients, in the guest frame
    const FrameFloat* extent;
    Real inverseSize, limit, columns, rows;
    reg32 shiftA, maskA, maskB;
};

struct Sample
{
    reg32 following, packed, angle, orient, tableIndex;
    Real t, px, py, pz, red, green, blue, alpha, depth, extentX, extentY, size, weight;
    Real u0, v0, u1, v1, across, down;
};

// Colour, depth, size, angle and texture frame of one particle.
[[gnu::always_inline]] inline void sample(reg32 particle, const Shared& k, const Group& g, Sample& s)
{
    s.following = rc::ld32(particle);
    const sreg32 age = x86::sreg16(rc::ld16(particle + 8));
    const Real t = sub(k.one, mul(mul(integer(reg32(age)), k.age), g.inverseSize));
    const Real t2 = t * t;
    const Real t3 = t * t2;
    s.t = t;
    s.px = ld(particle + 0x10);
    s.py = ld(particle + 0x14);
    s.pz = ld(particle + 0x18);
    s.red = channel(cubicFirst(g.colour, t, t2, t3), k.zero, k.maximum);
    s.green = channel(cubicOther(g.colour, 1, t, t2, t3), k.zero, k.maximum);
    s.blue = channel(cubicOther(g.colour, 2, t, t2, t3), k.zero, k.maximum);
    const Real alpha = cubicOther(g.colour, 3, t, t2, t3);
    s.alpha = !(alpha > k.zero) ? k.zero : k.maximum < alpha ? Real(255.0f) : alpha;
    s.packed = ftol2(s.alpha);
    Real depth = add(mul(k.viewZ[0], s.px), mul(k.viewZ[1], s.py));
    depth = add(depth, mul(k.viewZ[2], s.pz));
    s.depth = Real(float(add(depth, k.viewZ[3])));
    s.extentX = cubicFirst(g.extent, t, t2, t3);
    s.extentY = cubicOther(g.extent, 1, t, t2, t3);
    s.size = s.extentX;
    s.weight = k.zero;
    if (s.extentX > k.zero && s.depth > k.zero)
        s.weight = mul(div(s.extentX, s.depth), k.timeScale);
    if (s.weight > g.limit)
    {
        s.size = Real(float(mul(div(s.depth, k.timeScale), g.limit)));
        s.weight = g.limit;
    }
    const reg32 angle = (bits(float(t)) & 0xffff0000u) | rc::ld16(particle + 0xa);
    const reg32 swing = ftol2(s.extentY);
    s.angle = (angle & 1) == 0 ? angle + swing : angle - swing;
    const reg32 frame = rc::ld8(particle + 0x1f);
    s.u0 = Real(float(mul(integer(frame & g.maskA), g.columns)));
    s.v0 = Real(float(mul(integer((frame >> g.shiftA) & g.maskB), g.rows)));
    s.u1 = Real(float(add(s.u0, g.columns)));
    s.v1 = Real(float(add(s.v0, g.rows)));
    const reg32 flags = rc::ld8(particle + 0xf);
    s.orient = flags & 0xf;
    s.tableIndex = flags >> 4;
    s.across = Real(rc::ldf32a(0x7f3944 + 8 * s.tableIndex));
    s.down = Real(rc::ldf32a(0x7f3948 + 8 * s.tableIndex));
}

// Frame slots every particle leaves: next node, depth, extent.
inline void storeExtent(reg32 S, const Group& g, const Sample& s)
{
    const Real t2 = s.t * s.t;
    const Real t3 = s.t * t2;
    rc::st32(S + 0xe8, s.following);
    rc::stf32a(S + 0x1c8, float(s.depth));
    rc::stf32a(S + 0x1d0, float(s.extentX));
    rc::stf32a(S + 0x1d4, float(s.extentY));
    rc::stf32a(S + 0x1d8, float(cubicOther(g.extent, 2, s.t, t2, t3)));
    rc::stf32a(S + 0x1dc, float(cubicOther(g.extent, 3, s.t, t2, t3)));
}

// The batch record of one particle.
inline void storeRecord(reg32 record, const Shared& k, const Sample& s)
{
    store(record + 0x10, s.px);
    store(record + 0x14, s.py);
    store(record + 0x18, s.pz);
    rc::st32(record + 0x1c, 0x3f800000u);
    store(record + 0x20, s.red);
    store(record + 0x24, s.green);
    store(record + 0x28, s.blue);
    store(record + 0x2c, s.alpha);
    store(record + 0x30, mul(mul(s.size, s.across), k.scale));
    store(record + 0x34, mul(mul(s.size, k.scale), s.down));
    store(record + 0x38, mul(s.weight, s.across));
    store(record + 0x3c, mul(s.weight, s.down));
    store(record + 0x40, s.u0);
    store(record + 0x44, s.v0);
    store(record + 0x48, s.u1);
    store(record + 0x4c, s.v1);
    rc::st16(record + 0x50, x86::reg16(s.angle));
}

const unsigned char kindAction[26] = {0, 3, 3, 3, 0, 0, 3, 3, 1, 2, 1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1};

void body(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!NFSU_NATIVE("particle_draw_native"))
    {
        sub_48bdf0_guest(app, cpu);
        return;
    }
    const reg32 entry = cpu.esp;
    const reg32 savedEbx = cpu.ebx, savedEbp = cpu.ebp, savedEsi = cpu.esi, savedEdi = cpu.edi;
    const reg32 self = cpu.ecx;
    rc::st32(entry - 4, savedEbp);
    cpu.ebp = entry - 4;
    const reg32 S = ((entry - 4) & 0xfffffff0u) - 0x3f0;
    const reg32 view = rc::ld32(entry + 4);

    const auto finish = [&]()
    {
        cpu.ebx = savedEbx;
        cpu.ebp = savedEbp;
        cpu.esi = savedEsi;
        cpu.edi = savedEdi;
        cpu.esp = entry + 8;
    };

    if (!call(app, cpu, sub_43bdf0, S - 4, self, savedEbx, savedEsi, self))
        return;
    if (rc::ld32(view + 4) == 1)
        rc::st32(self + 0x54, 0);
    if (rc::ld32(self) == self)
    {
        finish();
        return;
    }

    // sub_43be80 only walks the list: skipped.
    const reg32 camera = rc::ld32(view + 0x40);
    const reg32 current = rc::ld32(camera + 0x280);
    rc::st32(S + 0x230, view);
    copyMatrix(S + 0x320, rc::ld32(view));
    const reg32 viewBase = S + 0x320;
    rc::st32(S + 0x180, rc::ld32(viewBase));
    rc::st32(S + 0x184, rc::ld32(viewBase + 0x10));
    rc::st32(S + 0x188, rc::ld32(viewBase + 0x20));
    rc::st32(S + 0x18c, 0);
    rc::st32(S + 0x1e0, rc::ld32(viewBase + 4));
    rc::st32(S + 0x1e4, rc::ld32(viewBase + 0x14));
    rc::st32(S + 0x1e8, rc::ld32(viewBase + 0x24));
    rc::st32(S + 0x1ec, 0);
    static const reg32 one = 0x3f800000u;
    static const reg32 tableA[3][4] = {{one, 0, 0, 0}, {one, 0, 0, 0}, {0, one, 0, 0}};
    static const reg32 tableB[3][4] = {{0, one, 0, 0}, {0, 0, one, 0}, {0, 0, one, 0}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
        {
            rc::st32(S + 0x190 + 16 * i + 4 * j, tableA[i][j]);
            rc::st32(S + 0x1f0 + 16 * i + 4 * j, tableB[i][j]);
        }

    Shared k;
    k.one = ld(0x784250);
    k.zero = ld(0x7d7d3c);
    k.age = ld(0x78d964);
    k.maximum = ld(0x784544);
    k.scale = ld(0x78d76c);
    k.timeScale = ld(view + 0xc);
    for (int i = 0; i < 4; ++i)
        k.viewZ[i] = ld(viewBase + 8 + 16 * i);
    const Real kLimit = ld(0x78d768);
    const reg32 limitCamera = rc::ld8(self + 0x4d) != 0 ? rc::ld32(self + 0x40) : 0;
    const reg32 mode = rc::ld32(view + 4);
    const bool inlineBatch = NFSU_NATIVE("particles_native");
    const batch::Thinning& thinning = batch::thinning();
    const reg32 record = S + 0x230;
    reg32 total = 0;
    reg32 guestEdi = self;

    for (reg32 pass = 0; pass < 2; ++pass)
    {
        if (pass == 0)
        {
            rc::st32(S - 4, view);
            if (!call(app, cpu, sub_5b7de0, S - 8, self, 0, view, self))
                return;
        }
        for (reg32 node = rc::ld32(self); node != self;)
        {
            const reg32 next = rc::ld32(node);
            const reg32 listCount = rc::ld32(node + pass * 4 + 0x24);
            bool draw = sreg32(listCount) > 0;
            const reg32 emitter = rc::ld32(node + 0x10);
            if (draw && emitter != 0)
            {
                if ((rc::ld32(emitter + 0x5b4) & (1u << (mode & 31))) == 0)
                    draw = false;
                else
                {
                    const reg32 kind = rc::ld32(rc::ld32(node + 8) + 0x5c) - 1;
                    if (kind <= 0x19)
                    {
                        switch (kindAction[kind])
                        {
                        case 0:
                            if (current == emitter && rc::ld32(camera + 0x284) != 0)
                                draw = false;
                            break;
                        case 1:
                            if (mode != 3 && current == emitter)
                                draw = false;
                            break;
                        case 2:
                            if (mode == 3 || current == emitter)
                                draw = false;
                            break;
                        }
                    }
                }
            }
            const reg32 desc = draw ? rc::ld32(node + 8) : 0;
            const reg32 owner = draw ? rc::ld32(node + 0xc) : 0;
            const reg32 batchTexture = draw ? rc::ld32(desc + 0x14) : 0;
            if (draw)
                guestEdi = owner;
            if (draw && (batchTexture == 0 || (limitCamera != 0 && owner != limitCamera)))
                draw = false;
            if (draw)
            {
                Group g;
                g.inverseSize = div(k.one, ld(desc + 0x3c));
                if (pass == 0)
                {
                    rc::st32(S + 0x234, batchTexture);
                    rc::st32(S + 0x238, rc::ld32(node + 0x24));
                    if (rc::ld32(0x87074c) != 0)
                        rc::st32(0x87074c, 0);
                }
                reg32 source = 0;
                if (owner != 0)
                    source = rc::ld32(owner + 0x30);
                copyMatrix(S + 0x370, source != 0 ? source : desc + 0x100);
                copyMatrix(S + 0x3b0, desc + 0x140);
                const Real rawLimit = ld(desc + 0x8c);
                g.limit = kLimit;
                if (!(rawLimit == k.zero) && rawLimit < kLimit)
                    g.limit = rawLimit;
                rc::stf32a(S + 0x3c, float(g.limit));

                const sreg32 shiftA = x86::sreg8(rc::ld8(desc + 0x19)), shiftB = x86::sreg8(rc::ld8(desc + 0x1a));
                guestEdi = reg32(shiftA);
                reg32 guestEbx = node, guestEsi = rc::ld32(node + 0x14 + pass * 8);
                g.shiftA = reg32(shiftA) & 31;
                g.maskA = ~(0xffffffffu << (shiftA & 31));
                g.maskB = ~(0xffffffffu << (shiftB & 31));
                g.columns = div(k.one, integer(1u << (shiftA & 31)));
                g.rows = div(k.one, integer(1u << (shiftB & 31)));
                reg32 count = 0;
                Real sum = Real(0.0f);
                const reg32 head = node + 0x14 + pass * 8;
                reg32 particle = rc::ld32(head);
                if (particle != head)
                {
                    g.colour = reinterpret_cast< const FrameFloat* >(rc::host(S + 0x370));
                    g.extent = reinterpret_cast< const FrameFloat* >(rc::host(S + 0x3b0));
                    Sample s;
                    for (;;)
                    {
                        sample(particle, k, g, s);
                        reg32 packed = s.packed;
                        guestEbx = s.angle;
                        guestEsi = s.tableIndex;
                        guestEdi = packed;

                        bool more = true;
                        if (pass == 0 && inlineBatch)
                        {
                            if (rc::ld32(0x87074c) == 0)
                            {
                                cpu.ecx = self;
                                cpu.ebx = s.angle;
                                cpu.esi = s.tableIndex;
                                cpu.edi = packed;
                                cpu.esp = S - 8;
                                if (!particleBatchBegin(app, cpu, record))
                                    return;
                            }
                            batch::Quad q;
                            q.x = Real(float(s.px));
                            q.y = Real(float(s.py));
                            q.z = Real(float(s.pz));
                            q.width = Real(float(mul(mul(s.size, s.across), k.scale)));
                            q.height = Real(float(mul(mul(s.size, k.scale), s.down)));
                            q.alpha = Real(float(s.alpha));
                            q.red = Real(float(s.red));
                            q.green = Real(float(s.green));
                            q.blue = Real(float(s.blue));
                            q.s0 = bits(float(s.u1));
                            q.t0 = bits(float(s.v1));
                            q.s1 = bits(float(s.u0));
                            q.t1 = bits(float(s.v0));
                            batch::append(q, thinning, [&s] { return s.following; });
                        }
                        else if (pass == 0)
                        {
                            storeExtent(S, g, s);
                            storeRecord(record, k, s);
                            rc::st32(S - 4, record);
                            if (!call(app, cpu, sub_5c0e10, S - 8, self, s.angle, s.tableIndex, packed))
                                return;
                            if (cpu.eax == 0)
                                more = false;
                        }
                        else
                        {
                            storeExtent(S, g, s);
                            // Orient 4 and above reads stale frame slots, as the guest does.
                            const reg32 rowA = S + 0x180 + 16 * s.orient, rowB = S + 0x1e0 + 16 * s.orient;
                            Real ax = ld(rowA), ay = ld(rowA + 4), az = ld(rowA + 8), aw = ld(rowA + 12);
                            Real bx = ld(rowB), by = ld(rowB + 4), bz = ld(rowB + 8), bw = ld(rowB + 12);
                            Real leading = ax;
                            if ((s.angle & 0xffff) != 0)
                            {
                                if (s.orient == 0)
                                {
                                    const Real cosine = Real(float(physics::sine16(cpu, s.angle + 0x4000)));
                                    const Real sine = Real(float(physics::sine16(cpu, s.angle)));
                                    const Real r[16] = {cosine, sine, 0.0f, 0.0f, -sine, cosine, 0.0f, 0.0f,
                                                        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
                                    Real a1 = mul(r[12], aw);
                                    a1 = add(a1, mul(r[8], az));
                                    a1 = add(a1, mul(r[4], ay));
                                    a1 = add(a1, mul(r[0], ax));
                                    Real a2 = mul(r[13], aw);
                                    a2 = add(a2, mul(r[9], az));
                                    a2 = add(a2, mul(r[5], ay));
                                    a2 = add(a2, mul(r[1], ax));
                                    Real a3 = mul(r[14], aw);
                                    a3 = add(a3, mul(r[10], az));
                                    a3 = add(a3, mul(r[6], ay));
                                    a3 = add(a3, mul(r[2], ax));
                                    Real b1 = mul(r[12], bw);
                                    b1 = add(b1, mul(r[8], bz));
                                    b1 = add(b1, mul(r[4], by));
                                    b1 = add(b1, mul(r[0], bx));
                                    Real b2 = mul(r[13], bw);
                                    b2 = add(b2, mul(r[9], bz));
                                    b2 = add(b2, mul(r[5], by));
                                    b2 = add(b2, mul(r[1], bx));
                                    Real b3 = mul(r[14], bw);
                                    b3 = add(b3, mul(r[10], bz));
                                    b3 = add(b3, mul(r[6], by));
                                    b3 = add(b3, mul(r[2], bx));
                                    leading = Real(float(a1));
                                    ay = Real(float(a2));
                                    az = Real(float(a3));
                                    bx = Real(float(b1));
                                    by = Real(float(b2));
                                    bz = Real(float(b3));
                                    rc::stf32(S + 0x224, float(a2));
                                    rc::stf32(S + 0x228, float(a3));
                                    rc::stf32(S + 0x368, float(b3));
                                }
                                else if (s.orient <= 3)
                                {
                                    const Real cosine = Real(float(physics::sine16(cpu, s.angle + 0x4000)));
                                    const Real sine = Real(float(physics::sine16(cpu, s.angle)));
                                    if (s.orient == 1)
                                    {
                                        bx = -sine;
                                        ay = sine;
                                        az = Real(0.0f);
                                        by = cosine;
                                        bz = Real(0.0f);
                                        leading = cosine;
                                    }
                                    else if (s.orient == 2)
                                    {
                                        leading = cosine;
                                        az = -sine;
                                        ay = Real(0.0f);
                                        bx = sine;
                                        by = Real(0.0f);
                                        bz = cosine;
                                    }
                                    else
                                    {
                                        leading = k.zero;
                                        ay = cosine;
                                        az = sine;
                                        bx = Real(0.0f);
                                        by = -sine;
                                        bz = cosine;
                                    }
                                }
                            }
                            const Real p = mul(mul(s.size, s.across), k.scale);
                            const Real pxv = mul(leading, p);
                            const Real pyv = mul(p, ay);
                            const Real pzv = mul(p, az);
                            const Real q = mul(mul(s.size, s.down), k.scale);
                            const Real qxv = mul(bx, q);
                            const Real qyv = mul(q, by);
                            const Real qzv = mul(q, bz);
                            const Real d0 = sub(pxv, qxv), d1 = sub(pyv, qyv), d2 = sub(pzv, qzv);
                            const Real e0 = add(qxv, pxv), e1 = add(qyv, pyv), e2 = add(qzv, pzv);
                            const reg32 quad = S + 0x290;
                            store(quad, sub(s.px, d0));
                            store(quad + 4, sub(s.py, d1));
                            store(quad + 8, sub(s.pz, d2));
                            store(quad + 0x10, sub(s.px, e0));
                            store(quad + 0x14, sub(s.py, e1));
                            store(quad + 0x18, sub(s.pz, e2));
                            store(quad + 0x20, add(d0, s.px));
                            store(quad + 0x24, add(d1, s.py));
                            store(quad + 0x28, add(d2, s.pz));
                            store(quad + 0x30, add(e0, s.px));
                            store(quad + 0x34, add(e1, s.py));
                            store(quad + 0x38, add(e2, s.pz));
                            store(quad + 0x40, s.u0);
                            store(quad + 0x44, s.v0);
                            store(quad + 0x48, s.u1);
                            store(quad + 0x4c, s.v0);
                            store(quad + 0x50, s.u1);
                            store(quad + 0x54, s.v1);
                            store(quad + 0x58, s.u0);
                            store(quad + 0x5c, s.v1);
                            rc::st8(quad + 0x70, 0);
                            packed = (packed << 8) | ftol2(s.blue);
                            packed = (packed << 8) | ftol2(s.green);
                            packed = (packed << 8) | ftol2(s.red);
                            guestEdi = packed;
                            for (reg32 i = 0; i < 16; i += 4)
                                rc::st32(quad + 0x60 + i, packed);
                            rc::st32(S - 4, 0);
                            rc::st32(S - 8, 0);
                            rc::st32(S - 12, 0x872e90);
                            rc::st32(S - 16, batchTexture);
                            rc::st32(S - 20, quad);
                            if (!call(app, cpu, sub_5c7e80, S - 24, view, s.angle, s.tableIndex, packed))
                                return;
                        }
                        if (!more)
                            break;
                        ++count;
                        sum = add(integer(ftol2(s.depth)), sum);
                        sum = Real(float(sum));
                        particle = s.following;
                        guestEsi = s.following;
                        if (particle == head)
                            break;
                        app->allowPreemption(cpu);
                    }
                    if (pass == 0 && inlineBatch)
                    {
                        storeExtent(S, g, s);
                        storeRecord(record, k, s);
                        cpu.eax = 1;
                    }
                }
                if (pass == 0)
                {
                    rc::st32(S - 4, count);
                    rc::st32(S - 8, bits(float(sum)));
                    rc::st32(S - 12, batchTexture);
                    if (!call(app, cpu, sub_5ce120, S - 16, self, guestEbx, guestEsi, guestEdi))
                        return;
                }
                total += count;
            }
            node = next;
            if (node == self)
                break;
            app->allowPreemption(cpu);
        }
        if (pass == 0 && !call(app, cpu, sub_5b7e80, S - 4, self, self, 0, guestEdi))
            return;
        if (pass == 0)
            app->allowPreemption(cpu);
    }
    rc::st32(self + 0x54, rc::ld32(self + 0x54) + total);
    finish();
}

}

// No shadow: a second run would draw again.
void sub_48bdf0(win32::WinApplication* app, x86::CPU& cpu)
{
    const offload::Direct direct;
    body(app, cpu);
}

}}
