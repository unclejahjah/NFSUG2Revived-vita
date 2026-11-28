// DriftManager::BuildLeaderBoard (0x56D850) as SPEED2.EXE's instructions, with Extra Options' fmul at 0x56DB51.

#include "../mods_runtime.h"
#include <native/common.h>

namespace ug2
{

void sub_4022c0(win32::WinApplication* app, x86::CPU& cpu);
void sub_4fe970(win32::WinApplication* app, x86::CPU& cpu);
void sub_4feaf0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5ac6e0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5b5330(win32::WinApplication* app, x86::CPU& cpu);
void sub_5ed900(win32::WinApplication* app, x86::CPU& cpu);
void sub_5f00f0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5f0220(win32::WinApplication* app, x86::CPU& cpu);
void sub_610270(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

typedef x86::reg32 reg32;
typedef x86::sreg32 sreg32;

const reg32 List = 0x864630, ListTail = 0x864634, Entries = 0x864640;
const reg32 RaceParameters = 0x89e7a0, DriverCount = 0x89e7c0, OtherCount = 0x89e7c8, ExtraCount = 0x89e7c4;
const reg32 Tracks = 0x89cf48, Scores = 0x7fa5c0, ScoresEnd = 0x7fa6f8;

#define CALL(f) do { cpu.esp -= 4; f(app, cpu); if (cpu.terminate) return; } while (0)

}

// DriftManager::BuildLeaderBoard with the sprint drift score factor.
void exoptsDriftBoard(win32::WinApplication* app, x86::CPU& cpu)
{
    using rc::ld32;
    using rc::st32;
    using rc::st8;
    reg32& eax = cpu.eax;
    reg32& ecx = cpu.ecx;
    reg32& edx = cpu.edx;
    reg32& ebx = cpu.ebx;
    reg32& esp = cpu.esp;
    reg32& ebp = cpu.ebp;
    reg32& esi = cpu.esi;
    reg32& edi = cpu.edi;
    auto push = [&](reg32 v) { esp -= 4; st32(esp, v); };
    auto pop = [&]() { const reg32 v = ld32(esp); esp += 4; return v; };
    auto movsd = [&]() {
        const reg32 step = cpu.flags.df ? 0u - 4u : 4u;
        for (reg32 n = ecx; n != 0; --n)
        {
            st32(cpu.ees + edi, ld32(esi));
            esi += step;
            edi += step;
        }
        ecx = 0;
    };
    auto al = [&]() { return x86::reg8(eax); };
    auto sx8 = [](x86::reg8 v) { return reg32(sreg32(int8_t(v))); };

    push(ebp);
    ebp = esp;
    esp &= 0xfffffff0u;
    push(0xffffffffu);
    push(0x778756);
    eax = ld32(cpu.efs);
    push(eax);
    st32(cpu.efs, esp);
    esp -= 0x958;
    eax = ld32(List);
    push(ebx);
    push(esi);
    push(edi);
    while (eax != List)
    {
        ecx = ld32(eax);
        eax = ld32(eax + 4);
        st32(eax, ecx);
        st32(ecx + 4, eax);
        eax = ld32(List);
    }
    eax = ld32(OtherCount);
    ecx = ld32(DriverCount);
    ebx = 0;
    eax += ecx;
    st32(esp + 0x28, ebx);
    st32(esp + 0x24, 0x3e7);
    st32(esp + 0x18, ebx);
    st32(esp + 0x20, ebx);
    if (sreg32(eax) > 0)
    {
        st32(esp + 0x1c, Entries);
        do
        {
            edx = ld32(esp + 0x20);
            push(edx);
            ecx = RaceParameters;
            CALL(sub_5ed900);
            esi = eax;
            eax = ld32(esi + 4);
            ecx = (ecx & 0xffffff00u) | rc::ld8(esi + 0x900);
            edi = 0;
            st8(esp + 0x17, 0);
            const bool one = eax == 1;
            const x86::reg8 cl = x86::reg8(ecx);
            if (one && cl == 0)
            {
                eax = esi + 0x10;
                ecx = Tracks;
                st32(esp + 0x28, eax);
                st8(esp + 0x17, 1);
                CALL(sub_5f00f0);
                if (al() != 0)
                    edi = ld32(Tracks + 4) + 0x12;
                else
                    edi = esi + 0x90c;
            }
            else if (cl != 0 && eax != 2)
            {
                ecx = sx8(rc::ld8(esi + 1));
                push(ecx);
                ecx = Tracks;
                CALL(sub_5f0220);
                edi = eax + 0x12;
            }
            ebx = esi + 0x20;
            ecx = ebx;
            CALL(sub_5b5330);
            edx = sx8(rc::ld8(esi + 1));
            ecx = ld32(esi + 0x10);
            st32(esp + 0x58, edx);
            edx = (edx & 0xffffff00u) | rc::ld8(esp + 0x17);
            eax = sx8(al());
            st8(esp + 0x64, x86::reg8(edx));
            edx = ld32(esp + 0x18);
            st32(esp + 0x6c, ecx);
            st32(esp + 0x68, edi);
            edi = ld32(esp + 0x1c);
            st32(esp + 0x5c, eax);
            ecx = 8;
            esi = esp + 0x50;
            st32(esp + 0x60, 0);
            movsd();
            ++edx;
            ecx = ebx;
            st32(esp + 0x18, edx);
            CALL(sub_5b5330);
            ecx = ld32(esp + 0x24);
            eax = sx8(al());
            if (sreg32(eax) < sreg32(ecx))
            {
                ecx = ebx;
                CALL(sub_5b5330);
                ecx = sx8(al());
                st32(esp + 0x24, ecx);
            }
            eax = ld32(esp + 0x1c);
            ecx = ld32(ListTail);
            st32(ecx, eax);
            edx = ld32(OtherCount);
            st32(eax + 4, ecx);
            ecx = ld32(esp + 0x20);
            st32(ListTail, eax);
            st32(eax, List);
            eax += 0x20;
            st32(esp + 0x1c, eax);
            eax = ld32(DriverCount);
            ++ecx;
            edx += eax;
            st32(esp + 0x20, ecx);
        } while (sreg32(ecx) < sreg32(edx));
        ebx = ld32(esp + 0x18);
    }
    eax = ld32(RaceParameters);
    bool scores = sreg32(eax) >= 0x104e && sreg32(eax) <= 0x1053;
    if (scores)
    {
        ecx = Tracks;
        CALL(sub_5f00f0);
        scores = al() == 0;
    }
    if (scores)
    {
        eax = ld32(0x8900bc);
        edi = eax == 1 ? 3 : 0;
        ecx = esp + 0x50;
        push(4);
        push(ecx);
        st8(esp + 0x1f, 0);
        CALL(sub_4fe970);
        esp += 8;
        if (sreg32(edi) <= 0)
            goto done;
        esi = 0x64;
        edx = esp + 0x50;
        ebx <<= 5;
        st32(esp + 0x18, esi);
        st32(esp + 0x1c, edx);
        ebx += Entries;
        st32(esp + 0x20, edi);
        for (bool first = true;; first = false)
        {
            if (!first)
                esi = ld32(esp + 0x18);
            ecx = esp + 0x80;
            CALL(sub_5ac6e0);
            push(0);
            push(0);
            push(0);
            push(0xffffffffu);
            ecx = esp + 0x80;
            st32(esp + 0x97c, 0);
            CALL(sub_610270);
            edi = ld32(esp + 0x28);
            eax = ld32(esp + 0x1c);
            ecx = ld32(eax);
            push(0xbf800000u);
            push(0);
            push(edi);
            push(1);
            push(0);
            edx = esp + 0x84;
            push(esi);
            push(edx);
            st32(esp + 0x988, 1);
            st32(esp + 0x48, ecx);
            CALL(sub_4feaf0);
            eax = (eax & 0xffffff00u) | rc::ld8(esp + 0x33);
            esp += 0x1c;
            if (al() != 0)
                st8(esp + 0x17, 1);
            else
            {
                eax = ld32(esp + 0x70);
                st8(esp + 0x17, eax == ld32(edi) ? 1 : 0);
            }
            x86::Float score = x86::Float(rc::ldf32(0x7d7d3c));
            edx = ld32(RaceParameters);
            ecx = 0;
            eax = Scores;
            bool found = false;
            do
            {
                found = ld32(eax) == edx;
                if (found)
                    break;
                eax += 0x34;
                ++ecx;
            } while (sreg32(eax) < sreg32(ScoresEnd));
            if (found)
            {
                eax = ld32(esp + 0x24);
                ecx = reg32(int64_t(sreg32(ecx)) * 0xd);
                edi = ld32(0x89e7f4);
                edx = eax + eax * 2;
                edi += ecx;
                edx += edi;
                score = x86::Float(rc::ldf32(edx * 4 + 0x7fa5c4));
            }
            else
                eax = ld32(esp + 0x24);
            ecx = esi - 0x63;
            if (ecx == 2)
                score *= x86::Float(rc::ldf32(0x79da48));
            else if (ecx == 3)
                score *= x86::Float(rc::ldf32(0x79da4c));
            ecx = ld32(esp + 0x2c);
            // ExtraOptions 5.1.0.1340 [Drift] MaximumMultiplier: DriftMultiplierCodeCave fmul dword ptr [0x925100]
            score *= x86::Float(rc::ldf32(modrt::DriftFactor));
            rc::stf32(esp + 0x40, float(score));
            edx = ld32(esp + 0x70);
            st32(esp + 0x48, ecx);
            st32(esp + 0x38, esi);
            st32(esp + 0x3c, eax);
            eax = ld32(ListTail);
            st32(esp + 0x4c, edx);
            ecx = 8;
            esi = esp + 0x30;
            edi = ebx;
            st8(esp + 0x44, 0);
            movsd();
            st32(eax, ebx);
            ecx = esp + 0x3f4;
            st32(ListTail, ebx);
            st32(ebx + 4, eax);
            st32(ebx, List);
            st32(esp + 0x96c, 0xffffffffu);
            CALL(sub_4022c0);
            edx = ld32(esp + 0x18);
            ecx = ld32(esp + 0x1c);
            eax = ld32(esp + 0x20);
            ebx += 0x20;
            ++edx;
            ecx += 4;
            --eax;
            st32(esp + 0x18, edx);
            st32(esp + 0x1c, ecx);
            st32(esp + 0x20, eax);
            if (eax == 0)
                break;
        }
        ecx = ld32(esp + 0x964);
        st32(cpu.efs, ecx);
        edi = pop();
        esi = pop();
        ebx = pop();
        esp = ebp;
        ebp = pop();
        esp += 4;
        return;
    }
    eax = ld32(ExtraCount);
    ebx = 0;
    if (sreg32(eax) > 0)
    {
        do
        {
            ecx = ld32(DriverCount);
            eax = ld32(OtherCount);
            esi = eax + ecx + ebx;
            push(esi);
            ecx = RaceParameters;
            CALL(sub_5ed900);
            edi = eax;
            ecx = edi + 0x20;
            CALL(sub_5b5330);
            edx = sx8(rc::ld8(edi + 1));
            eax = sx8(al());
            esi <<= 5;
            ecx = edi + 0x90c;
            st32(esp + 0x3c, eax);
            eax = esi + Entries;
            st32(esp + 0x38, edx);
            edx = ld32(edi + 0x10);
            st32(esp + 0x48, ecx);
            edi = eax;
            ecx = 8;
            esi = esp + 0x30;
            st32(esp + 0x40, 0);
            st8(esp + 0x44, 1);
            st32(esp + 0x4c, edx);
            movsd();
            ecx = ld32(ListTail);
            st32(ecx, eax);
            st32(ListTail, eax);
            st32(eax + 4, ecx);
            st32(eax, List);
            eax = ld32(ExtraCount);
            ++ebx;
        } while (sreg32(ebx) < sreg32(eax));
    }
done:
    ecx = ld32(esp + 0x964);
    edi = pop();
    esi = pop();
    st32(cpu.efs, ecx);
    ebx = pop();
    esp = ebp;
    ebp = pop();
    esp += 4;
}

}

}
