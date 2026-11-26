/* cpu.h -- what nfsu2-web's mprace.c (runtime/mprace.c @9e02302, copied as it is) needs of its runtime, on the Vita
 * port: guest memory at the recompiler's guest base, and a CPU of the registers a patched handler reads and writes.
 * The game's functions run as the recompiler translated them; a handler's CPU is filled from the translated code's
 * own (src/ug2/mprace_hooks.cpp) and copied back after. Guest calls go through the free roam host's roam_win_call
 * (guest.h), so a CPU is only ever passed along here, never run. */
#ifndef NFSU2_VITA_MP_CPU_H
#define NFSU2_VITA_MP_CPU_H
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "cpu_regs.h"

extern uint8_t *nfsu2_guest_base;
#define g_mem nfsu2_guest_base
typedef uint8_t  __attribute__((may_alias)) mp_u8;
typedef uint16_t __attribute__((may_alias, aligned(1))) mp_u16;
typedef uint32_t __attribute__((may_alias, aligned(1))) mp_u32;
typedef float    __attribute__((may_alias, aligned(1))) mp_f32;
#ifndef MR8
#define MR8(a)   (*(mp_u8  *)(nfsu2_guest_base + (uint32_t)(a)))
#define MR16(a)  (*(mp_u16 *)(nfsu2_guest_base + (uint32_t)(a)))
#define MR32(a)  (*(mp_u32 *)(nfsu2_guest_base + (uint32_t)(a)))
#define MRF32(a) (*(mp_f32 *)(nfsu2_guest_base + (uint32_t)(a)))
#endif
#define POP32(dst) do { (dst) = MR32(c->esp); c->esp += 4u; } while (0)
#define PUSH32(v)  do { c->esp -= 4u; MR32(c->esp) = (uint32_t)(v); } while (0)

int roam_win_committed(uint32_t a);
#define mem_is_committed(a) roam_win_committed((uint32_t)(a))

/* the game's Player index (as roam_host_win.h) */
#ifndef SPLIT_PLAYERS_INDEX
#define SPLIT_PLAYERS_INDEX 0x008900acu
#endif
#define SPLIT_PORT_TABLES   0x008655a0u

/* the trip's log lines (fprintf(stderr, ...) in mprace.c) go to the free roam log (mp_vita.c) */
int mp_fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int mp_fputc(int ch, FILE *f);
#ifndef MP_NO_LOG_MACROS
#define fprintf mp_fprintf
#define fputc   mp_fputc
#endif

#ifdef __cplusplus
}
#endif
#endif
