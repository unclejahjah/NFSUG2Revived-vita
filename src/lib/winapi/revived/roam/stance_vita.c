/* stance_vita.c -- a car's stance on the Vita port (after nfsu2-web's runtime/stance.c, MIT, its drawing part): the
 * wheels' camber, front and rear, how far they stand out, and the tires' width, as the car is drawn; the car drives as
 * in the original game.  Yours are the .ini's (Vita Settings, Free roam: Stance and its five rows) for whichever car
 * you drive out in the world; a friend's come with their car's record in free roam (roam_core.c RC_EXT_STANCE), and
 * nfsu2.xyz's players' per car stances show on their cars here too.  No Body Shop page: the browser's is a patch of
 * the game's own Options screen, which the Vita's translation does not take.
 *
 * The game (v1.2 NA), in the car's drawing 0x620AE0 (thiscall render info, 12 arguments, ret 0x30):
 *   - Front camber: the angle is int(7.0 [0x7A6754] * f * 65536) / 360, f = [0x7A6750] when that is not 0 (a tweak,
 *     0 in the game), else from the car's suspension parts.  Each front wheel's matrix of arguments 5 and 7 (4 x 0x40
 *     each) is turned by it about the car's forward axis.  Nothing tilts the rear wheels.
 *   - The render info: +0x400 each wheel's sideways place (2 floats, the left's below 0), +0x420 each wheel's tire
 *     width (m; the tire's model scaled to it).
 * Here the three constants are ours for the call, the rear wheels' matrices are turned the same way before it, and
 * the places and widths are ours: each put back after (showoff.c's 0x620AE0 calls stance_draw_begin/_end). */
#include "so_vita.h"
#include "stance.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *roam_opt(const char *name);
int roam_host_ini_value(const char *key, char *out, int n);   /* roam_vita.cpp: the .ini's [FreeRoam] key, read now */

#define G_CAMBER_F     0x007a6750u   /* float: the camber's factor where not 0 (the game: 0, the parts decide) */
#define G_CAMBER_DEG   0x007a6754u   /* float: the camber at factor 1, degrees (7.0) */
#define G_CAMBER_K     0x007a6758u   /* float: 0.03, times [0x7A6750] (a shift of the front wheels with the tweak) */
#define CD_ARG_WHEELS  0x14u         /* the drawing's arguments 5 and 7 (esp at its entry: the return address) */
#define CD_ARG_WHEELS2 0x1cu
#define WHEEL_MX       0x40u
#define RI_CAR         0u            /* render info: its car */
#define RI_WHEEL_Y     0x400u        /* ... each wheel's sideways place, two floats (the car's left: below 0) */
#define RI_TIRE_W      0x420u        /* ... each wheel's tire width, m */

static StanceV g_mine; static int g_mine_on = 0; static long g_mine_read = -1000;
static StanceV g_friend[STANCE_FRIENDS]; static uint32_t g_friend_car[STANCE_FRIENDS];

static int16_t clamp16(int v, int lo, int hi){ return (int16_t)(v < lo ? lo : v > hi ? hi : v); }
static float ini_num(const char *key, float def){
    char b[32];
    if (!roam_host_ini_value(key, b, (int)sizeof b) || !b[0]) return def;
    char *e = 0; float v = strtof(b, &e);
    return e != b && v == v ? v : def;
}
/* the player's own, from the .ini (read again every 3 s or so: Vita Settings may have changed it) */
static const StanceV *mine(void){
    long f = autotest_frame();
    if (f - g_mine_read >= 90 || f < g_mine_read){
        g_mine_read = f;
        char on[8] = "";
        roam_host_ini_value("Stance", on, (int)sizeof on);
        StanceV v;
        v.fc = clamp16((int)lroundf(ini_num("FrontCamber", 0.f) * 10.f), STANCE_CAMBER_MIN, STANCE_CAMBER_MAX);
        v.rc = clamp16((int)lroundf(ini_num("RearCamber", 0.f) * 10.f), STANCE_CAMBER_MIN, STANCE_CAMBER_MAX);
        v.fo = clamp16((int)lroundf(ini_num("FrontOffset", 0.f)), STANCE_OFFSET_MIN, STANCE_OFFSET_MAX);
        v.ro = clamp16((int)lroundf(ini_num("RearOffset", 0.f)), STANCE_OFFSET_MIN, STANCE_OFFSET_MAX);
        v.tw = (uint16_t)clamp16((int)lroundf(ini_num("TireWidth", 100.f)), STANCE_TIRE_MIN, STANCE_TIRE_MAX);
        int now = on[0] == '1';
        if (now != g_mine_on || memcmp(&v, &g_mine, sizeof v))
            fprintf(stderr, "[stance] f=%ld my car: %s (camber %d/%d, offset %d/%d mm, tires %u %%)\n", f, now ? "on" : "off",
                    v.fc, v.rc, v.fo, v.ro, v.tw);
        g_mine = v; g_mine_on = now;
    }
    return g_mine_on ? &g_mine : 0;
}

/* ---- friends' (roam_game.c, RC_EXT_STANCE) ---------------------------------------------------------------------- */
int stance_wire_mine(int16_t v[5]){
    const StanceV *s = mine();
    if (!s) return 0;
    v[0] = s->fc; v[1] = s->rc; v[2] = s->fo; v[3] = s->ro; v[4] = (int16_t)s->tw;
    return 1;
}
void stance_wire_car(int k, uint32_t car, int has, const int16_t v[5]){
    static StanceV said[STANCE_FRIENDS]; static uint8_t said_on[STANCE_FRIENDS];
    if (k < 0 || k >= STANCE_FRIENDS) return;
    g_friend_car[k] = car && has ? car : 0;
    if (!g_friend_car[k]) return;
    StanceV *f = &g_friend[k];
    f->fc = clamp16(v[0], STANCE_CAMBER_MIN, STANCE_CAMBER_MAX); f->rc = clamp16(v[1], STANCE_CAMBER_MIN, STANCE_CAMBER_MAX);
    f->fo = clamp16(v[2], STANCE_OFFSET_MIN, STANCE_OFFSET_MAX); f->ro = clamp16(v[3], STANCE_OFFSET_MIN, STANCE_OFFSET_MAX);
    f->tw = (uint16_t)clamp16(v[4], STANCE_TIRE_MIN, STANCE_TIRE_MAX);
    if (said_on[k] && !memcmp(&said[k], f, sizeof *f)) return;
    said[k] = *f; said_on[k] = 1;
    fprintf(stderr, "[stance] f=%ld slot %d's car: camber %d/%d, offset %d/%d mm, tires %u %%\n", autotest_frame(), k,
            f->fc, f->rc, f->fo, f->ro, f->tw);
}
void stance_forget(void){ memset(g_friend_car, 0, sizeof g_friend_car); }

/* ---- the drawing ------------------------------------------------------------------------------------------------- */
static struct {
    int      on;
    uint32_t keep_c[3];
    uint32_t mx[4];                  /* the rear wheels' matrices turned: argument 5's and 7's, wheels 2 and 3 */
    float    keep_mx[4][12];
    uint32_t ri;
    float    keep_y[8], keep_w[4];
} D;

/* the stance the car of render info ri is drawn with, 0: the game's (out in the world only) */
static const StanceV *of_drawn(uint32_t ri){
    uint32_t car = guest_rd32(ri + RI_CAR);
    if (MR32(G_FLOW) != 6u || !car) return 0;
    if (car == guest_rd32(guest_rd32(SPLIT_PLAYERS_INDEX) + PL_CAR)) return mine();
    for (int k = 0; k < STANCE_FRIENDS; k++) if (g_friend_car[k] == car) return &g_friend[k];
    return 0;
}

/* m (row-major 4x4, rows 0..2 its rotation) = m x Rx(a), as 0x5BACF0 turns the front wheels */
static void turn(uint32_t m, float a, float keep[12]){
    float r[12];
    for (int k = 0; k < 12; k++) keep[k] = r[k] = MRF32(m + 4u * (uint32_t)k);
    float cs = cosf(a), sn = sinf(a);
    for (int row = 0; row < 3; row++){
        float y = r[row * 4 + 1], z = r[row * 4 + 2];
        MRF32(m + 4u * (uint32_t)(row * 4 + 1)) = y * cs - z * sn;
        MRF32(m + 4u * (uint32_t)(row * 4 + 2)) = y * sn + z * cs;
    }
}

void stance_draw_begin(CPU *c){
    D.on = 0;
    uint32_t ri = c->ecx;
    if (!guest_ok(ri, RI_TIRE_W + 0x10u)) return;
    const StanceV *s = of_drawn(ri);
    if (!s) return;
    D.on = 1; D.ri = ri;
    D.keep_c[0] = MR32(G_CAMBER_F); D.keep_c[1] = MR32(G_CAMBER_DEG); D.keep_c[2] = MR32(G_CAMBER_K);
    MRF32(G_CAMBER_F) = 1.0f; MRF32(G_CAMBER_DEG) = s->fc * 0.1f; MRF32(G_CAMBER_K) = 0.0f;
    for (int k = 0; k < 8; k++){
        float y = D.keep_y[k] = MRF32(ri + RI_WHEEL_Y + 4u * (uint32_t)k), o = (k < 4 ? s->fo : s->ro) * 0.001f;
        MRF32(ri + RI_WHEEL_Y + 4u * (uint32_t)k) = y + (y < 0 ? -o : o);
    }
    for (int k = 0; k < 4; k++){ D.keep_w[k] = MRF32(ri + RI_TIRE_W + 4u * (uint32_t)k); MRF32(ri + RI_TIRE_W + 4u * (uint32_t)k) = D.keep_w[k] * s->tw * 0.01f; }
    float a = s->rc * 0.1f * 3.14159265f / 180.0f;
    uint32_t w[2] = { guest_rd32(c->esp + CD_ARG_WHEELS), guest_rd32(c->esp + CD_ARG_WHEELS2) };
    for (int set = 0; set < 2; set++){
        int ok = a != 0 && guest_ok_bytes(w[set], 4u * WHEEL_MX);
        for (int i = 2; i < 4; i++){
            uint32_t m = ok ? w[set] + WHEEL_MX * (uint32_t)i : 0;
            D.mx[set * 2 + (i - 2)] = m;
            if (m) turn(m, i == 2 ? -a : a, D.keep_mx[set * 2 + (i - 2)]);   /* wheel 2 on the right, as wheel 1 */
        }
    }
}
void stance_draw_end(void){
    if (!D.on) return;
    D.on = 0;
    MR32(G_CAMBER_F) = D.keep_c[0]; MR32(G_CAMBER_DEG) = D.keep_c[1]; MR32(G_CAMBER_K) = D.keep_c[2];
    for (int k = 0; k < 8; k++) MRF32(D.ri + RI_WHEEL_Y + 4u * (uint32_t)k) = D.keep_y[k];
    for (int k = 0; k < 4; k++) MRF32(D.ri + RI_TIRE_W + 4u * (uint32_t)k) = D.keep_w[k];
    for (int k = 0; k < 4; k++){
        if (!D.mx[k]) continue;
        for (int j = 0; j < 12; j++) MRF32(D.mx[k] + 4u * (uint32_t)j) = D.keep_mx[k][j];
    }
}
