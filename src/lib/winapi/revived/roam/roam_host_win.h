/* roam_host_win.h -- the Vita port's host for runtime/roam_game.c, built as for the PC's NFSU2FreeRoam.asi
 * (-DROAM_HOST_WIN, nfsu2-web's win/roam_asi/roam_host_win.h): the same features as the PC plugin (the horn, crash
 * damage, plates, drift mode and the port's other extras left out), with the guest's memory at the recompiler's
 * guest base and guest calls through the runtime's dynamic call (roam_vita.cpp). */
#ifndef ROAM_HOST_WIN_H
#define ROAM_HOST_WIN_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct CPU CPU;                  /* not used here (always 0) */

/* ---- guest memory: at the guest base -------------------------------------------- */
extern uint8_t *nfsu2_guest_base;
typedef uint8_t  __attribute__((may_alias)) rh_u8;
typedef uint16_t __attribute__((may_alias, aligned(1))) rh_u16;
typedef uint32_t __attribute__((may_alias, aligned(1))) rh_u32;
typedef float    __attribute__((may_alias, aligned(1))) rh_f32;
#define MR8(a)   (*(rh_u8  *)(nfsu2_guest_base + (uint32_t)(a)))
#define MR16(a)  (*(rh_u16 *)(nfsu2_guest_base + (uint32_t)(a)))
#define MR32(a)  (*(rh_u32 *)(nfsu2_guest_base + (uint32_t)(a)))
#define MRF32(a) (*(rh_f32 *)(nfsu2_guest_base + (uint32_t)(a)))
#define GPTR(a)  (nfsu2_guest_base + (uint32_t)(a))
int      roam_win_committed(uint32_t a);          /* inside the guest's address space */
#define rh_committed(a) roam_win_committed(a)

/* the game's Player index (runtime/splitscreen_mem.h, the native builds' address) */
#define SPLIT_PLAYERS_INDEX 0x008900acu

/* ---- guest calls --------------------------------------------------------------- */
uint32_t roam_win_call(uint32_t fn, uint32_t ecx, uint32_t edx, int n, const uint32_t *args);
#define rh_call(c, fn, ecx, edx, n, args) ((void)(c), roam_win_call((fn), (ecx), (edx), (n), (args)))
uint32_t roam_win_esp(void);                      /* the game thread's esp in the frame hook */

/* ---- options, log, clocks ------------------------------------------------------- */
const char *roam_opt(const char *name);           /* environment, then the .ini */
void        roam_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
long        roam_win_frame_no(void);
uint64_t    roam_win_ns(void);
#define rlog(...)        roam_log(__VA_ARGS__)
#define rh_opt(k)        roam_opt(k)
#define rh_frame()       roam_win_frame_no()
#define rh_heap_mb()     0u
#define rh_hud_marker(x, y) ((void)(x), (void)(y))   /* the HUD as the game shows it */
#define RH_FE_NODES      512             /* the game's own level sorter (0x536520's 0x1000 bytes): nothing patched here */
#define rh_ns()          roam_win_ns()
/* no horn here: the companion neither sounds one nor plays friends', nor
 * their nitrous and hydraulics (carsfx.c) */
#define rh_horn_held(stick) ((void)(stick), 0)
#define rh_horn_mine()   (-1)
#define rh_horn_name(k)  ((void)(k), "another")
#define rh_horn_begin(pos, fwd, vel)   ((void)(pos), (void)(fwd), (void)(vel))
#define rh_horn_sound(who, name, kind, p, v) ((void)(who), (void)(name), (void)(kind), (void)(p), (void)(v))
#define rh_horn_end()    ((void)0)
#define rh_carsfx_begin(pos, fwd, vel)    ((void)(pos), (void)(fwd), (void)(vel))
#define rh_carsfx_car(who, name, p, v, st) ((void)(who), (void)(name), (void)(p), (void)(v), (void)(st))
#define rh_carsfx_end()  ((void)0)
/* no show-off here: the companion opens no parts and shows friends' cars closed (showoff.c needs the port's
 * overrides of the cars' drawing) */
#define rh_showoff_mine() ((uint8_t)0)
#define rh_showoff_car(c, k, car, open) ((void)(c), (void)(k), (void)(car), (void)(open))
/* no light key here either: the companion's lights are the game's, and friends' cars show theirs */
#define rh_lights_mine() 0
#define rh_lights_car(k, car, st) ((void)(k), (void)(car), (void)(st))
/* no crash damage here: the companion sends none, asks for none (no RC_HF_DENTS) and shows friends' cars undamaged */
#define rh_damage_on()   0
#define rh_damage_rev()  0u
#define rh_damage_mine(e, f, d, m) ((void)(e), (void)(f), (void)(d), (void)(m), 0)
#define rh_damage_car(c, k, car, peer, e, f, n, d) \
    ((void)(c), (void)(k), (void)(car), (void)(peer), (void)(e), (void)(f), (void)(n), (void)(d))
/* no license plates of our own here: the companion sends none, and friends' cars wear the game's (plates.c needs the
 * port's overrides of the cars' drawing) */
#define rh_plate_mine()  ""
#define rh_plate_car(k, car, text) ((void)(k), (void)(car), (void)(text))
#define rh_plates_forget() ((void)0)
#define rh_stance_mine(v) ((void)(v), 0)
#define rh_stance_car(k, car, has, v) ((void)(k), (void)(car), (void)(has), (void)(v))
#define rh_stance_forget() ((void)0)
/* the game's "No Profile" in English only (the port also knows the other
 * languages' names from the game's text, language.c) */
#define rh_no_profile(name) (!strcasecmp((name), "No Profile"))
/* no drift mode here (drift.c): the ride's dyno setup is always its own */
#define rh_drift_tuning(out) ((void)(out), 0)
#define ROAM_HOST_DRIFT  0               /* ...so no drift battles */
#define rh_drift_want()  0
/* no trips to the race tracks' maps here (travel.c): always the city */
#define rh_travel_room() 0u
#define rh_drift_begin() ((void)0)
#define rh_drift_go(c)   ((void)(c))
#define rh_drift_score(b, p) (*(b) = 0, *(p) = 0)
#define rh_drift_x()     0u
#define rh_drift_end()   ((void)0)

/* the pose log (test aid, PoseLog in the .ini) */
typedef struct rh_file rh_file;
rh_file *rh_fopen(const char *path);
int      rh_fprintf(rh_file *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void     rh_fflush(rh_file *f);

#endif
