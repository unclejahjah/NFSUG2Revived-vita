/* guest.h -- nfsu2-web's runtime/guest.h helpers for mprace.c on the Vita port: guest calls through the free roam
 * host (roam_vita.cpp roam_win_call), from the frame hook or a patched handler, on the translated code's own stack. The
 * translated code writes no return addresses, so the ret and gap arguments have nothing to do here, nor edx (the
 * macro does not evaluate them: the trip's CPU may be the frame hook's stand-in). */
#ifndef NFSU2_VITA_MP_GUEST_H
#define NFSU2_VITA_MP_GUEST_H
#include "cpu.h"

#ifdef __cplusplus
extern "C" {
#endif
uint32_t roam_win_call(uint32_t fn, uint32_t ecx, uint32_t edx, int n, const uint32_t *args);
#ifdef __cplusplus
}
#endif

static inline int guest_ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static inline int guest_ok_bytes(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }
static inline uint32_t guest_rd32(uint32_t a){ return guest_ok(a, 3) ? MR32(a) : 0u; }
static inline void guest_read_str(uint32_t a, char *o, int n){
    int i = 0;
    for (; a && i + 1 < n && guest_ok(a + (uint32_t)i, 0) && MR8(a + (uint32_t)i); i++) o[i] = (char)MR8(a + (uint32_t)i);
    o[i] = 0;
}
static inline void guest_write_str(uint32_t a, const char *s, uint32_t cap){
    uint32_t i = 0;
    for (; s[i] && i + 1 < cap; i++) MR8(a + i) = (uint8_t)s[i];
    MR8(a + i) = 0;
}
#define GUEST_RET_MAINLOOP 0x00581475u
#define guest_call_ex(c, fn, ecx, edx, n, arg, ret, gap) ((void)(c), roam_win_call((fn), (ecx), 0u, (n), (arg)))
#define gcall(c, fn, ecx, n, arg)      guest_call_ex(c, fn, ecx, 0, n, arg, fn, 256u)
#define gcall_main(c, fn, ecx, n, arg) guest_call_ex(c, fn, ecx, 0, n, arg, GUEST_RET_MAINLOOP, 256u)
#endif
