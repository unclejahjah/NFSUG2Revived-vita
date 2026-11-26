/* stance_vita.c -- a car's stance on the Vita port (after nfsu2-web's runtime/stance.c, MIT, its drawing part): the
 * wheels' camber, front and rear, how far they stand out, and the tires' width, as the car is drawn; the car drives as
 * in the original game.  Yours are set per car, as in the browser: in the Car Shop's page (Customize, Body Shop, Stance
 * after Rims: src/ug2/menu_entry.cpp) for the car you drive (the career's, else Quick Race's), kept per profile in
 * scripts/NFSU2Stance.ini, and shown on that car out in the world while the .ini's Stance (Vita Settings, Mods) is on;
 * a car with none of its own is drawn as the game draws it.  A friend's come with their car's record in free roam
 * (roam_core.c RC_EXT_STANCE), and nfsu2.xyz's players' per car stances show on their cars here too.  While the Body
 * Shop or the page is open the front end's car is drawn with its stance (stance_fe_preview).
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

static int g_mine_on = 0; static long g_mine_read = -1000; static int g_mine_stale = 1;
static int g_fe_preview = 0;
static StanceV g_friend[STANCE_FRIENDS]; static uint32_t g_friend_car[STANCE_FRIENDS];

/* the .ini's Stance changed (Vita Settings, the Car Shop's page): read again at the next car drawn */
void stance_reload(void){ g_mine_stale = 1; }
/* the front end's car drawn with the player's stance (on: the Body Shop or its Stance page is open) */
void stance_fe_preview(int on){
    if (on != g_fe_preview) fprintf(stderr, "[stance] f=%ld the front end's car: %s\n", autotest_frame(), on ? "its stance" : "the game's");
    g_fe_preview = on;
}

static int16_t clamp16(int v, int lo, int hi){ return (int16_t)(v < lo ? lo : v > hi ? hi : v); }
int stance_ok(const StanceV *v){
    return v->fc >= STANCE_CAMBER_MIN && v->fc <= STANCE_CAMBER_MAX && v->rc >= STANCE_CAMBER_MIN && v->rc <= STANCE_CAMBER_MAX
        && v->fo >= STANCE_OFFSET_MIN && v->fo <= STANCE_OFFSET_MAX && v->ro >= STANCE_OFFSET_MIN && v->ro <= STANCE_OFFSET_MAX
        && v->tw >= STANCE_TIRE_MIN && v->tw <= STANCE_TIRE_MAX;
}

/* ---- the player's cars (as the browser's stance.c, player 1 only) ------------------------------------------------ */
#define G_CAREER_CAR    0x00863480u   /* the career car's handle */
#define G_QR_CAR        0x0083aab8u   /* the car's handle in Quick Race, LAN, Online */
#define G_PROFILE_NAME  0x0083a9e0u   /* player 1's profile: +0 its name (32 bytes), +0x20 1: one is loaded */
#define G_FECARS        0x0083ad90u   /* player 1's front-end cars: +0x9BD8 the 91 records' table */
#define FC_TABLE        0x9bd8u
#define FC_TABLE_N      91u
#define R_HANDLE        0x08u         /* a car record: its handle */
#define R_TYPE          0x20u         /* ... its car type */
#define G_FE_RIDE       0x008392c0u   /* the front end's ride of the car */
#define RIDE_COILOVERS  0x363u        /* ride: the two suspension parts the game's camber comes from (0x59A420) */
#define RIDE_PLATES     0x364u
#define GAME_CAMBER     7.0f          /* [0x7A6754] */
#define STANCE_FILE     "ux0:data/NeedForSpeedUnderground2/scripts/NFSU2Stance.ini"

typedef struct { uint32_t handle, type; StanceV v; } Entry;
static struct { Entry e[STANCE_CARS]; int n; char name[33]; int loaded; } g_cars;   /* the profile's of name */
static struct { int open; uint32_t handle, type; StanceV v; float game_fc; } M;    /* the page's car, while open */

static uint32_t driven(void){ return MR32(G_GAMEMODE) == 1u ? MR32(G_CAREER_CAR) : MR32(G_QR_CAR); }
static uint32_t record(uint32_t h){
    if (!h) return 0;
    for (uint32_t i = 0; i < FC_TABLE_N; i++){
        uint32_t r = MR32(G_FECARS + FC_TABLE + 4u * i);
        if (r && guest_ok(r, R_TYPE + 4u) && MR32(r + R_HANDLE) == h) return r;
    }
    return 0;
}
/* player 1's profile now: its name, whether or not +0x20 says one is loaded (with that flag's "none" test15's stances
 * were likely kept in memory only, never written); "Default" with no name at all */
static const char *profile(void){
    static char nm[33];
    int n = 0;
    for (; n < 32; n++){
        char ch = (char)MR8(G_PROFILE_NAME + (uint32_t)n);
        if (!ch) break;
        nm[n] = ch >= ' ' && ch <= '~' && ch != '[' && ch != ']' ? ch : '_';
    }
    nm[n] = 0;
    if (!n) snprintf(nm, sizeof nm, "Default");
    return nm;
}

/* the file: a [profile] section each, a line a car: <handle> = <car type> <front camber> <rear> <front offset> <rear>
 * <tires' width> (hex, then stance.h's units) */
static void load_profile(const char *name){
    g_cars.n = 0; g_cars.loaded = 1;
    snprintf(g_cars.name, sizeof g_cars.name, "%s", name);
    FILE *f = fopen(STANCE_FILE, "r");
    if (!f) return;
    char line[256]; int in = 0;
    while (fgets(line, sizeof line, f)){
        char *l = line; while (*l == ' ' || *l == '\t') l++;
        if (*l == '['){
            char *e = strchr(l, ']'); if (e) *e = 0;
            in = name[0] && !strcasecmp(l + 1, name);
            continue;
        }
        unsigned h, t; int a[5];
        if (!in || *l == ';' || sscanf(l, "%x = %x %d %d %d %d %d", &h, &t, &a[0], &a[1], &a[2], &a[3], &a[4]) != 7) continue;
        if (!h || g_cars.n == STANCE_CARS) continue;
        Entry *e = &g_cars.e[g_cars.n];
        e->handle = h; e->type = t;
        e->v.fc = (int16_t)a[0]; e->v.rc = (int16_t)a[1]; e->v.fo = (int16_t)a[2]; e->v.ro = (int16_t)a[3]; e->v.tw = (uint16_t)a[4];
        if (stance_ok(&e->v)) g_cars.n++;
    }
    fclose(f);
    fprintf(stderr, "[stance] profile %s: %d car%s with a stance of their own\n", name[0] ? name : "(none)", g_cars.n, g_cars.n == 1 ? "" : "s");
}
/* the file again with this profile's section as it is now (the others' kept as they were) */
static void save_profile(void){
    char *keep = 0; size_t kn = 0, kcap = 0;
    FILE *f = fopen(STANCE_FILE, "r");
    if (f){
        char line[256]; int in = 0;
        while (fgets(line, sizeof line, f)){
            char *l = line; while (*l == ' ' || *l == '\t') l++;
            if (*l == '['){
                char nm[64]; snprintf(nm, sizeof nm, "%s", l + 1);
                char *e = strchr(nm, ']'); if (e) *e = 0;
                in = !strcasecmp(nm, g_cars.name);
            }
            if (in) continue;
            size_t n = strlen(line);
            if (kn + n + 1 > kcap){ kcap = (kn + n + 1) * 2; char *b = realloc(keep, kcap); if (!b) break; keep = b; }
            memcpy(keep + kn, line, n); kn += n; keep[kn] = 0;
        }
        fclose(f);
    }
    f = fopen(STANCE_FILE, "w");
    if (!f){ free(keep); fprintf(stderr, "[stance] %s could not be written\n", STANCE_FILE); return; }
    if (!kn) fputs("; NFSU2Stance.ini: each car's stance (Customize, Body Shop, Stance), per profile:\n"
                   "; car handle = car type, front camber, rear camber (tenths of a degree), front offset, rear offset (mm),\n"
                   "; tires' width (per cent). Vita Settings > Mods > Stance turns them on or off.\n", f);
    else fwrite(keep, 1, kn, f);
    if (g_cars.n){
        if (kn && keep[kn - 1] != '\n') fputc('\n', f);
        fprintf(f, "[%s]\n", g_cars.name);
        for (int i = 0; i < g_cars.n; i++){
            const Entry *e = &g_cars.e[i];
            fprintf(f, "%08x = %08x %d %d %d %d %u\n", e->handle, e->type, e->v.fc, e->v.rc, e->v.fo, e->v.ro, e->v.tw);
        }
    }
    int bad = ferror(f);
    bad = fclose(f) != 0 || bad;
    free(keep);
    fprintf(stderr, "[stance] %s: profile %s, %d car%s%s\n", STANCE_FILE, g_cars.name, g_cars.n, g_cars.n == 1 ? "" : "s",
            bad ? " (the write FAILED)" : " written");
}
/* the cars of the profile now */
static void cars_now(void){
    const char *p = profile();
    if (!g_cars.loaded || strcasecmp(g_cars.name, p)) load_profile(p);
}
static Entry *entry(uint32_t h){
    if (!h) return 0;
    cars_now();
    for (int i = 0; i < g_cars.n; i++) if (g_cars.e[i].handle == h) return &g_cars.e[i];
    return 0;
}

/* the player's own: the page's while it is open, else the driven car's (its car type as set: not a car traded since) */
static const StanceV *mine(void){
    long f = autotest_frame();
    if (g_mine_stale || f - g_mine_read >= 90 || f < g_mine_read){
        g_mine_read = f; g_mine_stale = 0;
        char on[8] = "";
        roam_host_ini_value("Stance", on, (int)sizeof on);
        /* (no value: the .ini not there, or being written over by Vita Settings at that moment: as it was) */
        int now = on[0] ? on[0] == '1' : g_mine_on;
        if (now != g_mine_on) fprintf(stderr, "[stance] f=%ld your cars' stances: %s\n", f, now ? "on" : "off");
        g_mine_on = now;
    }
    if (M.open) return &M.v;
    if (!g_mine_on) return 0;
    uint32_t h = driven(), r = record(h);
    Entry *e = entry(h);
    return e && r && MR32(r + R_TYPE) == e->type ? &e->v : 0;
}

/* ---- the Car Shop's page (menu_entry.cpp, options_menu.cpp's Car rows) ------------------------------------------- */
static float game_camber(uint32_t ride){
    if (!guest_ok(ride, RIDE_PLATES + 1u)) return 0;
    return GAME_CAMBER * (0.5f * (MR8(ride + RIDE_COILOVERS) != 0) + 0.5f * (MR8(ride + RIDE_PLATES) != 0));
}
/* the page opens on the car driven: its stance, else the game's look (its front camber from its suspension) */
void stance_menu_open(void){
    M.handle = driven();
    uint32_t r = record(M.handle);
    M.type = r ? MR32(r + R_TYPE) : 0;
    M.game_fc = game_camber(G_FE_RIDE);
    Entry *e = entry(M.handle);
    if (e && e->type == M.type) M.v = e->v;
    else { memset(&M.v, 0, sizeof M.v); M.v.fc = (int16_t)lroundf(M.game_fc * 10); M.v.tw = 100; }
    M.open = 1;
    fprintf(stderr, "[stance] the car %08x (type %08x): camber %d/%d, offset %d/%d mm, tires %u %% (%s)\n", M.handle, M.type,
            M.v.fc, M.v.rc, M.v.fo, M.v.ro, M.v.tw, e && e->type == M.type ? "its own" : "the game's");
}
/* ... and closes: the car keeps the values, or none of its own when they are the game's look */
void stance_menu_close(void){
    if (!M.open) return;
    M.open = 0;
    if (!M.handle || !record(M.handle)){ fprintf(stderr, "[stance] the page's car has no record: not kept\n"); return; }
    StanceV stock = { (int16_t)lroundf(M.game_fc * 10), 0, 0, 0, 100 };
    int same = !memcmp(&M.v, &stock, sizeof stock);
    Entry *e = entry(M.handle);
    if (same){
        if (e){ int i = (int)(e - g_cars.e); memmove(e, e + 1, sizeof *e * (size_t)(g_cars.n - 1 - i)); g_cars.n--; }
    } else {
        if (!e && g_cars.n < STANCE_CARS) e = &g_cars.e[g_cars.n++];
        if (e){ e->handle = M.handle; e->type = M.type; e->v = M.v; }
    }
    save_profile();
    fprintf(stderr, "[stance] the car %08x: %s\n", M.handle, same ? "the game's stance" : "a stance of its own");
}
/* a row's value as text (the .ini's units: degrees, mm, per cent), and a new one */
static int16_t *field(const char *key, int *tenths){
    *tenths = 0;
    if (!strcmp(key, "FrontCamber")){ *tenths = 1; return &M.v.fc; }
    if (!strcmp(key, "RearCamber")){ *tenths = 1; return &M.v.rc; }
    if (!strcmp(key, "FrontOffset")) return &M.v.fo;
    if (!strcmp(key, "RearOffset")) return &M.v.ro;
    if (!strcmp(key, "TireWidth")) return (int16_t *)&M.v.tw;
    return 0;
}
int stance_menu_get(const char *key, char *out, int n){
    int t; int16_t *v = field(key, &t);
    if (!v || n <= 0) return 0;
    if (t) snprintf(out, (size_t)n, "%g", *v * 0.1);
    else snprintf(out, (size_t)n, "%d", key[0] == 'T' ? (int)*(uint16_t *)v : (int)*v);
    return 1;
}
void stance_menu_set(const char *key, const char *value){
    int t; int16_t *v = field(key, &t);
    if (!v || !value) return;
    float x = strtof(value, 0);
    if (t) *v = clamp16((int)lroundf(x * 10.f), STANCE_CAMBER_MIN, STANCE_CAMBER_MAX);
    else if (key[0] == 'T') *(uint16_t *)v = (uint16_t)clamp16((int)lroundf(x), STANCE_TIRE_MIN, STANCE_TIRE_MAX);
    else *v = clamp16((int)lroundf(x), STANCE_OFFSET_MIN, STANCE_OFFSET_MAX);
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

/* the stance the car of render info ri is drawn with, 0: the game's (out in the world; in the front end only the
 * Car Shop's car, while stance_fe_preview has it) */
static const StanceV *of_drawn(uint32_t ri){
    uint32_t car = guest_rd32(ri + RI_CAR);
    if (MR32(G_FLOW) != 6u) return g_fe_preview ? mine() : 0;   /* (the browser's page: every car the front end draws) */
    if (!car) return 0;
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
