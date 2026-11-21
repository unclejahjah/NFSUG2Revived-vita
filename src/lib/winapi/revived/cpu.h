/* cpu.h -- what nfsu2-web's Winsock HLE (winsock.c, net_lan.c) needs of its runtime, on the Vita port:
 * the calling guest thread's stack pointer and thread block, and guest memory. ws2_32.cpp fills a CPU for each
 * call (revived_bridge.cpp). */
#ifndef NFSU2_VITA_REVIVED_CPU_H
#define NFSU2_VITA_REVIVED_CPU_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPU {
    uint32_t esp;       /* at the import's entry: the return address, the arguments above it */
    uint32_t fs_base;   /* the thread block (TEB) */
    uint32_t eax, eip;
} CPU;

extern uint8_t *nfsu2_guest_base;
#define g_mem nfsu2_guest_base

typedef uint8_t  __attribute__((may_alias)) gm_u8;
typedef uint16_t __attribute__((may_alias, aligned(1))) gm_u16;
typedef uint32_t __attribute__((may_alias, aligned(1))) gm_u32;
#define MR8(a)  (*(gm_u8  *)(g_mem + (uint32_t)(a)))
#define MR16(a) (*(gm_u16 *)(g_mem + (uint32_t)(a)))
#define MR32(a) (*(gm_u32 *)(g_mem + (uint32_t)(a)))

int mem_is_committed(uint32_t address);
void nfsu2_log(const char *line);            /* one line in the port's log */
uint32_t nfsu2_guest_alloc(uint32_t bytes);   /* zeroed guest memory, never freed */
static inline int nfsu2_test_run(void) { return 0; }

/* the guest lock: released while a call waits */
void guest_wait_begin(void);
void guest_wait_end(void);

/* memory.atomic.wait32 / notify of the browser build */
int  vlan_atomic_wait32(int *address, int expected, long long timeout_ns);
int  vlan_atomic_notify(int *address, unsigned count);

#ifdef __cplusplus
}
#endif
#endif
