/* so_vita.h -- what nfsu2-web's showoff.c and stance_vita.c need of its runtime on the Vita port (built with
 * -DSHOWOFF_VITA, the mp/ shims for cpu.h and guest.h): the game's addresses (nfsu2-web runtime/game.h, v1.2 NA), the
 * front end's top package, and the host's answers for what the browser's input and heap gave */
#ifndef NFSU2_VITA_SO_VITA_H
#define NFSU2_VITA_SO_VITA_H
#include <stdint.h>
#include "mp/cpu.h"
#include "mp/guest.h"

#define G_FLOW          0x008654a4u   /* TheGameFlowManager's state: 3 the front end, 4/5 loading, 6 in the world */
#define G_GAMEMODE      0x00850078u   /* the front end's game mode: 1 career, 2 the main menu, 4 a split race, 8 Online, 0x200 LAN */
#define G_RACETYPE      0x0089e7b0u   /* the race's type: 0 free roam, 7 an outrun */
#define G_RACE          0x00890118u   /* the race status: +0 its state (4 racing, free roam too, 6 paused) */
#define G_CAREER_RACE   0x00863470u   /* the career race being run, 0 free roam */
#define G_FENG          0x008384c4u   /* cFEng: +8 the package manager (+0xE4 the top package, +0xC its name) */
#define G_TXT_ON        0x0083a9c0u   /* byte: the game's text entry takes the typing ... */
#define G_TXT_REC       0x0083a9c4u   /* ... for this record (0: none) */
#define G_CARTYPES      0x008a1cccu   /* CarTypeInfo[]: 0x890 bytes, +0xD0 name hash */
#define CAR_FWD_SPEED   0x42cu        /* car: float, its speed, m/s forward */
#define FN_NEW          0x00575620u   /* cdecl (size): operator new */
#define PL_CAR          4u            /* Player: its car */

/* the top package of the front end's manager (no co-op on the Vita: its own) */
static inline uint32_t coop_fe_top(uint32_t mg){ return mg ? MR32(mg + 0xe4u) : 0; }
static inline uint32_t coop_p2_car(void){ return 0; }

/* the frame number (mp_vita.c) */
long autotest_frame(void);
/* the show-off's presses since the last frame (controls.cpp: Select + L), from roam_vita.cpp */
int roam_host_showoff_presses(void);
#endif
