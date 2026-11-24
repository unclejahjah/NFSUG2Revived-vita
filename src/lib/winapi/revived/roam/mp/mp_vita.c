/* mp_vita.c -- what nfsu2-web's multiplayer races from free roam (roam_game.c mp_*, mprace.c) need of the browser
 * runtime, on the Vita port.
 *
 *  - the screens: the top front-end package (autotest.c autotest_top_fng) and whether a package is active anywhere
 *    on the FEng lists (career.c career_fe_active), the same reads of the game's memory;
 *  - keys for the trip (autotest_tap): Enter, Escape and Left, pressed through the Vita's synthesized keyboard
 *    (lib/gamepad.cpp Gamepad::tapKey, roam_vita.cpp roam_vita_tap);
 *  - the career's cars (garage.c garage_find): the game's own 91-record table only (no extra garage slots here);
 *  - texts of our own for the race card's buttons (language.c language_add_text): answered by the game's text lookup
 *    0x4FF9D0 (src/ug2/menu_entry.cpp asks roam_lang_text), in guest memory;
 *  - the log: mprace.c's fprintf(stderr, ...) lines into the free roam log;
 *  - patched handlers (dispatch_patch): remembered here, run by the natives of src/ug2/mprace_hooks.cpp.
 * Not here (yet): a career win that counts (mpcredit.c: races pay prize money only, roam_game.c mp_counts), the
 * network race's car smoothing (racenet.c), the horn port's quit notices (racehorn.c). */
#define MP_NO_LOG_MACROS
#include "cpu.h"
#include "guest.h"
#include "game.h"
#include "mpcredit.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

void roam_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
long roam_win_frame_no(void);
void roam_vita_tap(unsigned dik);             /* roam_vita.cpp */
uint32_t nfsu2_guest_alloc(uint32_t bytes);   /* revived_bridge.cpp: zeroed guest memory, never freed */

/* ---- the log ------------------------------------------------------------------------------------- */
static char g_line[512];
static size_t g_len;
static void line_flush(void){
    if (!g_len) return;
    g_line[g_len] = 0;
    roam_log("%s", g_line);
    g_len = 0;
}
static void line_put(const char *s){
    for (; *s; s++){
        if (*s == '\n'){ line_flush(); continue; }
        if (g_len + 1 < sizeof g_line) g_line[g_len++] = *s;
    }
}
int mp_fprintf(FILE *f, const char *fmt, ...){
    (void)f;
    char b[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    line_put(b);
    return n;
}
int mp_fputc(int ch, FILE *f){
    (void)f;
    char b[2] = { (char)ch, 0 };
    line_put(b);
    return ch;
}

/* ---- frames and screens ------------------------------------------------------------------------------ */
long autotest_frame(void){ return roam_win_frame_no(); }

static int g_fng_off = -1;
static const char *fng_str(uint32_t p){
    if (!p || !mem_is_committed(p) || !mem_is_committed(p + 63)) return 0;
    const char *s = (const char *)(g_mem + p); int n = 0;
    while (n < 63 && s[n] >= 0x20 && s[n] < 0x7f) n++;
    if (s[n] != 0 || n < 5 || strncasecmp(s + n - 4, ".fng", 4)) return 0;
    return s;
}
const char *autotest_top_fng(void){
    if (!mem_is_committed(G_FENG)) return 0;
    uint32_t fe = MR32(G_FENG);
    if (!fe || !mem_is_committed(fe + 11)) return 0;
    uint32_t mg = MR32(fe + 8);
    if (!mg || !mem_is_committed(mg + 0xe7)) return 0;
    uint32_t pk = MR32(mg + 0xe4);                    /* GetTopPackage (0x50C8E0): FNGInfo*, its name at +0x0C */
    if (!pk || !mem_is_committed(pk + 255)) return 0;
    if (g_fng_off >= 0){ const char *s = fng_str(MR32(pk + (uint32_t)g_fng_off)); if (s) return s; }
    for (int o = 0; o < 256; o += 4){
        const char *s = fng_str(MR32(pk + (uint32_t)o));
        if (s){ g_fng_off = o; return s; }
    }
    return 0;
}

static uint32_t fe_mgr(void){
    if (!mem_is_committed(G_FENG)) return 0;
    uint32_t fe = MR32(G_FENG);
    if (!fe || !mem_is_committed(fe + 11)) return 0;
    uint32_t mg = MR32(fe + 8u);
    return mg && mem_is_committed(mg + 0xf7u) ? mg : 0;
}
static const char *fe_name(uint32_t p){
    uint32_t n = MR32(p + 0xcu);
    if (!n || !mem_is_committed(n) || !mem_is_committed(n + 47)) return 0;
    const char *q = (const char *)(g_mem + n); int k = 0;
    while (k < 47 && q[k] >= 0x20 && q[k] < 0x7f) k++;
    return (k > 4 && !q[k]) ? q : 0;
}
int career_fe_active(const char *name){
    uint32_t mg = fe_mgr();
    if (!mg) return 0;
    for (uint32_t head = 0xe4u; head <= 0xf4u; head += 0x10u){
        uint32_t p = MR32(mg + head);
        for (int k = 0; p && k < 64 && mem_is_committed(p) && mem_is_committed(p + 0x10); k++){
            const char *nm = fe_name(p);
            if (nm && !strcasecmp(nm, name)) return 1;
            p = MR32(p + 4u);
        }
    }
    return 0;
}
/* the pause menu's Save, or a save screen up (savegame.c: the port's own Save has no pending state here) */
int savegame_busy(void){ return career_fe_active("MC_Main.fng"); }

/* ---- keys ------------------------------------------------------------------------------------- */
void autotest_tap(unsigned dik, long frames){ (void)frames; roam_vita_tap(dik); }

/* ---- the career's cars --------------------------------------------------------------------------- */
#define C_TABLE   0x9bd8u     /* garage.c: the career's 91 car records' table */
#define TABLE_N   91u
#define R_HANDLE  0x08u
uint32_t garage_find(uint32_t db, uint32_t h){
    if (!h || !guest_ok(db + C_TABLE, 4u * TABLE_N)) return 0;
    for (uint32_t i = 0; i < TABLE_N; i++){
        uint32_t r = MR32(db + C_TABLE + 4u * i);
        if (r && guest_ok(r, R_HANDLE + 3u) && MR32(r + R_HANDLE) == h) return r;
    }
    return 0;
}

/* ---- texts of our own ----------------------------------------------------------------------------- */
#define OWN_MAX 16
static struct { uint32_t hash, str; char text[256]; } g_own[OWN_MAX];
static int g_nown;
uint32_t language_add_text(const char *name, const char *text){
    uint32_t h = 0xffffffffu;                   /* bStringHash (0x43DB50) */
    for (const char *s = name; *s; s++) h = h * 33u + (uint8_t)*s;
    int i = 0;
    while (i < g_nown && g_own[i].hash != h) i++;
    if (i == OWN_MAX){ roam_log("[mprace] language: no room for the text %s", name); return h; }
    if (i == g_nown) g_nown++;
    g_own[i].hash = h;
    snprintf(g_own[i].text, sizeof g_own[i].text, "%s", text);
    static uint32_t block;                     /* one guest block for all of them (allocations are 4 KB pages) */
    if (!block) block = nfsu2_guest_alloc(OWN_MAX * sizeof g_own[0].text);
    if (!g_own[i].str && block) g_own[i].str = block + (uint32_t)i * (uint32_t)sizeof g_own[i].text;
    if (g_own[i].str) guest_write_str(g_own[i].str, g_own[i].text, sizeof g_own[i].text);
    return h;
}
/* the game's text lookup 0x4FF9D0 (src/ug2/menu_entry.cpp) asks this first: a guest string of ours, else 0 */
uint32_t roam_lang_text(uint32_t hash){
    for (int i = 0; i < g_nown; i++) if (g_own[i].hash == hash) return g_own[i].str;
    return 0;
}
uint32_t language_text(uint32_t hash){ return roam_lang_text(hash); }
void language_texts_frame(CPU *c){ (void)c; }        /* (answered by the lookup itself, no table copy) */
const char *language_name(void){ return "english"; }
void language_game_letters(const char *utf8, char *out, size_t n){
    size_t k = 0;
    for (const unsigned char *s = (const unsigned char *)utf8; *s && k + 1 < n; s++) out[k++] = *s < 0x80 ? (char)*s : '?';
    if (n) out[k] = 0;
}

/* ---- patched handlers -------------------------------------------------------------------------------- */
#define PATCH_MAX 8
static struct { uint32_t addr; void (*fn)(CPU *); } g_patch[PATCH_MAX];
static int g_npatch;
void dispatch_patch(uint32_t a, void (*f)(CPU *)){
    for (int i = 0; i < g_npatch; i++) if (g_patch[i].addr == a){ g_patch[i].fn = f; return; }
    if (g_npatch < PATCH_MAX){ g_patch[g_npatch].addr = a; g_patch[g_npatch].fn = f; g_npatch++; }
}
/* the handler patched over guest function a, 0 none (src/ug2/mprace_hooks.cpp) */
void (*roam_patch_of(uint32_t a))(CPU *){
    for (int i = 0; i < g_npatch; i++) if (g_patch[i].addr == a) return g_patch[i].fn;
    return 0;
}

/* ---- not on the Vita (yet) ----------------------------------------------------------------------------- */
/* a win that counts in the career: none here (roam_game.c mp_counts says no race counts), so nothing waits */
int mpcredit_win(struct CPU *c, uint32_t race, float margin, int in_roam){ (void)c; (void)race; (void)margin; (void)in_roam; return 0; }
int mpcredit_last(MpCredit *o){ (void)o; return 0; }
int mpcredit_pending(void){ return 0; }
int mpcredit_busy(void){ return 0; }
/* the network race's cars as they came in (racenet.c): not measured here */
float racenet_jump(const char *name){ (void)name; return 0.f; }
float racenet_farthest(char name[16]){ name[0] = 0; return 0.f; }
/* the lobby card's controller keys (dinput_pad.c lobby_pad: D-pad left and right held, the page's card): not on the
 * Vita, where the D-pad is the game's; Enter's own pick is the lobby joined */
void pad_lobby_offer(int left, int right){ (void)left; (void)right; }
int pad_lobby_take(void){ return 0; }
