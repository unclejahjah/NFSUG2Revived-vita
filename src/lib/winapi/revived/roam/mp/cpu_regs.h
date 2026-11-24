/* cpu_regs.h -- the registers of a patched game handler on the Vita port (mp/cpu.h; src/ug2/mprace_hooks.cpp fills
 * them from the translated code's CPU and copies them back) */
#ifndef NFSU2_VITA_MP_CPU_REGS_H
#define NFSU2_VITA_MP_CPU_REGS_H
#include <stdint.h>

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t eip;
} CPU;

#endif
