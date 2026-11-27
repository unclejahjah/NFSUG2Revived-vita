/* stance_vita.c -- a car's stance on the Vita port (after nfsu2-web's runtime/stance.c, MIT, its drawing part): the
 * wheels' camber, front and rear, how far they stand out, and the tires' width, as the car is drawn; the car drives as
 * in the original game.  Yours are set per car, as in the browser: in the Car Shop's page (Customize, Body Shop, Stance
 * after Rims: src/ug2/menu_entry.cpp) for the car you drive (the career's, else Quick Race's), kept as the browser keeps
 * them, in the profile's MODPARTS.DAT beside it (so they go with the cloud saves both ways), and shown on that car out in the world while the .ini's Stance (Vita Settings, Mods) is on;
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
#define R_PARTS         0x18u         /* ... its parts: +8 its car type, +0xC + 4 x slot each of the 170 slots' key */
#define N_SLOTS         0xaau
/* MODPARTS.DAT beside the profile (the browser's modpacks.c, the same file): 'N2MP', version 1, count, its write
 * counter, then count entries of 8 words, the last its kind: 3 the head {'MPHD', the name's hash, the counter, the
 * payload's cars' fingerprint (2 words), 0, 0, 3}, 4 a car's stance {handle, car type, front | rear camber << 16,
 * front | rear offset << 16, tires' width, the car's fingerprint (2 words), 4}, 0 a mod part (kept as it is) */
#define MP_FILE         "MODPARTS.DAT"
#define MP_MAGIC        0x504d324eu   /* "N2MP" */
#define HEAD_MAGIC      0x4448504du   /* "MPHD" */
#define K_HEAD          3u
#define MP_MAX          1024
int roam_host_profile_file(const char *profile, const char *file, char *out, int n);   /* roam_vita.cpp */

/* ok: its car checked (its parts as when it was saved, as the browser checks a car's stance at a profile's load) */
typedef struct { uint32_t handle, type; StanceV v; uint64_t fp; int ok; } Entry;
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

/* the browser's car fingerprint (modpacks.c car_fp): its car type and its 170 parts' keys */
static uint64_t car_fp(uint32_t r){
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t o = 8u; o < 0xcu + 4u * N_SLOTS; o++){ h ^= MR8(r + R_PARTS + o); h *= 0x100000001b3ull; }
    return h;
}
/* the head's name: bStringHash of the name in capitals (modpacks.c name_hash) */
static uint32_t name_hash(const char *s){
    uint32_t h = 0xffffffffu;
    for (; *s; s++) h = h * 33u + (uint8_t)((*s >= 'a' && *s <= 'z') ? *s - 32 : *s);
    return h;
}
static int mp_path(const char *name, char *out, int n){ return name[0] && roam_host_profile_file(name, MP_FILE, out, n); }

/* a MODPARTS file: its entries but the head (w), its write counter and its head; 1 read, 0 none, -1 not one we read */
static uint32_t g_w[MP_MAX][8];
static int mp_read(const char *path, int *n, uint32_t *count, uint32_t head[8]){
    *n = 0; *count = 0; memset(head, 0, 8 * sizeof *head);
    FILE *in = fopen(path, "rb");
    if (!in) return 0;
    uint32_t hd[4] = { 0 };
    int ok = fread(hd, sizeof hd, 1, in) == 1 && hd[0] == MP_MAGIC && hd[1] == 1u && hd[2] <= MP_MAX + 1u, got = 0;
    for (uint32_t i = 0; ok && i < hd[2]; i++){
        uint32_t w[8];
        ok = fread(w, sizeof w, 1, in) == 1 && (w[7] || w[2] < N_SLOTS);
        if (ok && w[7] == K_HEAD && w[0] == HEAD_MAGIC && !got){ got = 1; memcpy(head, w, sizeof w); continue; }
        if (ok && *n == MP_MAX) ok = 0;
        if (ok) memcpy(g_w[(*n)++], w, sizeof w);
    }
    ok = ok && fgetc(in) == EOF;
    fclose(in);
    if (!ok){ *n = 0; return -1; }
    *count = hd[3] > head[2] ? hd[3] : head[2];
    return 1;
}
/* the profile's cars' stances from its MODPARTS.DAT (none without) */
static void load_profile(const char *name){
    g_cars.n = 0; g_cars.loaded = 1;
    snprintf(g_cars.name, sizeof g_cars.name, "%s", name);
    char path[512]; uint32_t count, head[8]; int n = 0;
    if (!mp_path(name, path, (int)sizeof path)){ fprintf(stderr, "[stance] profile %s: no save folder\n", name); return; }
    int r = mp_read(path, &n, &count, head);
    for (int i = 0; i < n && g_cars.n < STANCE_CARS; i++){
        const uint32_t *w = g_w[i];
        if (w[7] != STANCE_KIND || !w[0] || w[4] > 0xffffu) continue;
        Entry *e = &g_cars.e[g_cars.n];
        memset(e, 0, sizeof *e);
        e->handle = w[0]; e->type = w[1];
        e->v.fc = (int16_t)(w[2] & 0xffffu); e->v.rc = (int16_t)(w[2] >> 16);
        e->v.fo = (int16_t)(w[3] & 0xffffu); e->v.ro = (int16_t)(w[3] >> 16); e->v.tw = (uint16_t)w[4];
        e->fp = (uint64_t)w[5] | (uint64_t)w[6] << 32;
        if (stance_ok(&e->v)) g_cars.n++;
    }
    fprintf(stderr, "[stance] profile %s: %s %s, %d car%s with a stance of its own\n", name, path,
            r > 0 ? "read" : r < 0 ? "NOT one we read (left as it is)" : "not there", g_cars.n, g_cars.n == 1 ? "" : "s");
}
/* the car record of entry e if it is that car: of its car type, and checked once against the fingerprint it was saved
 * with (its parts changed since: another car of that handle, or one changed elsewhere; its stance waits as it is) */
static uint32_t entry_car(Entry *e){
    uint32_t r = record(e->handle);
    if (!r || MR32(r + R_TYPE) != e->type || !guest_ok(r, R_PARTS + 0xcu + 4u * N_SLOTS)) return 0;
    if (!e->ok){
        if (car_fp(r) != e->fp) return 0;
        e->ok = 1;
    }
    return r;
}
/* the profile's MODPARTS.DAT again: its other entries as they were (mod parts, a later page's kinds), each stance of a
 * car of ours as it is now (its fingerprint now), those of cars not here as they were */
static void save_profile(const char *why){
    if (!g_cars.loaded) return;
    char path[512], part[530]; uint32_t count = 0, head[8]; int n = 0;
    if (!mp_path(g_cars.name, path, (int)sizeof path)){ fprintf(stderr, "[stance] profile %s: no save folder, not written\n", g_cars.name); return; }
    int r = mp_read(path, &n, &count, head);
    if (r < 0){ fprintf(stderr, "[stance] %s is not one we read: not written over\n", path); return; }
    static uint32_t out[MP_MAX + 1][8];
    int m = 0, ours = 0;
    for (int i = 0; i < n; i++){
        const uint32_t *w = g_w[i];
        if (w[7] == STANCE_KIND){
            int mine_now = 0;
            for (int k = 0; k < g_cars.n; k++) if (g_cars.e[k].handle == w[0]) mine_now = 1;
            if (mine_now || record(w[0])) continue;      /* ours now, or a car here with none (set back to the game's) */
        }
        memcpy(out[m++], w, sizeof out[0]);
    }
    for (int k = 0; k < g_cars.n && m < MP_MAX; k++){
        Entry *e = &g_cars.e[k];
        uint32_t c = entry_car(e);
        if (c) e->fp = car_fp(c);
        uint32_t *w = out[m++];
        w[0] = e->handle; w[1] = e->type;
        w[2] = (uint16_t)e->v.fc | (uint32_t)(uint16_t)e->v.rc << 16;
        w[3] = (uint16_t)e->v.fo | (uint32_t)(uint16_t)e->v.ro << 16;
        w[4] = e->v.tw; w[5] = (uint32_t)e->fp; w[6] = (uint32_t)(e->fp >> 32); w[7] = STANCE_KIND;
        ours++;
    }
    if (!m && r == 0) return;                              /* (nothing to keep and no file: none made) */
    count++;
    if (head[0] != HEAD_MAGIC){ memset(head, 0, 8 * sizeof *head); head[0] = HEAD_MAGIC; head[7] = K_HEAD; }
    head[1] = name_hash(g_cars.name); head[2] = count;
    uint32_t hd[4] = { MP_MAGIC, 1u, (uint32_t)m + 1u, count };
    snprintf(part, sizeof part, "%s.part", path);
    FILE *f = fopen(part, "wb");
    int ok = f && fwrite(hd, sizeof hd, 1, f) == 1 && fwrite(head, 8 * sizeof *head, 1, f) == 1;
    for (int i = 0; ok && i < m; i++) ok = fwrite(out[i], sizeof out[0], 1, f) == 1;
    if (f) ok = fclose(f) == 0 && ok;
    /* (the Vita renames over no file: the old one goes first) */
    if (ok && rename(part, path) != 0){ remove(path); ok = rename(part, path) == 0; }
    if (!ok) remove(part);
    fprintf(stderr, "[stance] %s (%s): %d car%s' stance%s, %d other entr%s: %s\n", path, why, ours, ours == 1 ? "" : "s",
            ours == 1 ? "" : "s", m - ours, m - ours == 1 ? "y" : "ies", ok ? "written" : "the write FAILED");
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
/* the game has written a save file (menu_entry.cpp): the cars' fingerprints as they are now in MODPARTS.DAT, as the
 * browser writes it with each save of the profile */
void stance_saved(void){
    if (g_cars.loaded && g_cars.n) save_profile("the game saved");
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
    uint32_t h = driven();
    Entry *e = entry(h);
    const StanceV *v = e && entry_car(e) ? &e->v : 0;
    static uint32_t said_h = 1; static int said_v = -1;
    if (h != said_h || (v != 0) != said_v){
        said_h = h; said_v = v != 0;
        fprintf(stderr, "[stance] f=%ld the car driven %08x: %s\n", f, h, v ? "its stance" : e ? "its stance waits (not that car as saved)" :
                record(h) ? "none of its own" : "no record of it");
    }
    return v;
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
    int own = e && entry_car(e);
    if (own) M.v = e->v;
    else { memset(&M.v, 0, sizeof M.v); M.v.fc = (int16_t)lroundf(M.game_fc * 10); M.v.tw = 100; }
    M.open = 1;
    fprintf(stderr, "[stance] profile %s, the car %08x (type %08x): camber %d/%d, offset %d/%d mm, tires %u %% (%s)\n",
            g_cars.name, M.handle, M.type, M.v.fc, M.v.rc, M.v.fo, M.v.ro, M.v.tw, own ? "its own" : "the game's");
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
        uint32_t r = record(M.handle);
        if (e){ e->handle = M.handle; e->type = M.type; e->v = M.v; e->fp = car_fp(r); e->ok = 1; }
    }
    save_profile("the Stance page closed");
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
