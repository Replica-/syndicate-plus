/* additions.c - what this version adds to the original game, as hooks on its functions (hooks.h).
 *
 * Auto saves: when the world map comes up after a mission, the game is saved in the first line of the Load/Save
 * screen, by the game's own save routine; the auto saves before it move down a line.  The first three lines hold
 * the auto saves ("AUTO dd/mm hh:mm"), newest first; only saves named so are moved or replaced, and a save of
 * the player found in those lines is first moved to a free line further down.  A save the player makes in one of
 * the auto save lines goes to the first free line from the fourth on instead (none free: not saved).
 *
 * Keys in missions: W, A, S and D scroll the view as the arrow keys do; 1 to 4 select an agent (the game's own)
 * and pressed twice within 0.4 s centre the view on it.
 *
 * A hit sprays blood out of the person hit, away from the shooter.
 *
 * Agents wear hats in the colour of their company (the original draws every hat in the first company's red).
 *
 * The selected agents' boxes (top left of a mission) have a solid background instead of the checkerboard of orange
 * and olive; the agent's number and the frame stay.
 *
 * A move order shows a green diamond on the floor where the agents go, shrinking and fading out in 0.6 s.
 *
 * All of this is drawn over the frames in true colour (game_overlay), with any colour and transparency.
 *
 * The scanner (the map at the bottom left of a mission) shows more around the leader: 6 pixels a tile instead of
 * 8, and with a scanner (the weapon) 2 instead of 4.  With a scanner it also shows doors (cyan), criminals (orange)
 * and armed people (red).
 *
 * The MINIGUN SQUAD cheat, as the company or player name (Configure Company), like the game's own cheats: every
 * weapon and mod researched, and the 18 agents of the company with every mod at version 3 and a minigun in each
 * of their 8 weapon slots.  The research is completed as the game's COOPER TEAM cheat does it.
 *
 * --cheats "COMPANY:PLAYER" names the player's company and the player when the world map first comes up and runs
 * the game's cheat check, as Configure Company does: "MINIGUN SQUAD:TO THE TOP" gives every mod and miniguns,
 * every mission and $100 000 000. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../rt/rt.h"
#include "../rt/vfs.h"
#include "hooks.h"
#include "render.h"

/* where the functions and data of each program are: English Syndicate's found by following the game (the
 * SYND_CENSUS list, hooks.c), the others' with tools/find_equiv.py */
typedef struct GameAddrs {
    int game;
    /* auto saves */
    uint32_t mission_end; /* the mission is over (results counted), before the debriefing */
    uint32_t world_map;   /* the world map screen starts */
    uint32_t save_game;   /* save(name, slot): writes <save_dir>/<slot>.GAM */
    uint32_t save_dir;    /* the save folder (a C string, "c:/synd/save") */
    /* cheats */
    uint32_t cheat_check;  /* the cheats of the company and player names are applied */
    uint32_t companies;    /* the companies (the first 4 bytes of each: its funds) */
    uint32_t company_size;
    uint32_t player;       /* (16 bits) the player's company */
    uint32_t mod_research; /* 18 research entries of MOD_RESEARCH_SIZE bytes, the mods' */
    uint32_t weapon_research; /* 20 of WEAPON_RESEARCH_SIZE, the weapons' */
    uint32_t weapon_known; /* 20 bytes: what a researched weapon's entry holds at +0x1f4 */
    uint32_t ammo;         /* (16 bits, by weapon) the ammunition of a new weapon */
    /* missions */
    uint32_t mission_tick; /* called on every tick of a mission */
    uint32_t set_view;     /* set_view(x, y): the view's position (clamped to the map) */
    uint32_t peds;         /* the people of the mission, PED_SIZE bytes each */
    uint32_t draw_scanner; /* draw_scanner(leader ped, pixels a tile: 8, or 4 with a scanner) */
    uint32_t view;         /* (16 bits x, y) the view's position (world / 128) */
    uint32_t statics;      /* the objects of the mission (doors, ...): NB_STATICS of STATIC_SIZE bytes */
    uint32_t colours;      /* (bytes, by company colour) the colours of the companies in a mission's palette */
    uint32_t data;         /* the pointer to HSPR's table, followed by the pointers to HFRA, HELE, the map and HBLK
                            * (render.c) at data_offsets */
    int ar;                /* American Revolt: its pointers are further apart */
} GameAddrs;

static const GameAddrs k_addrs[] = {
    {GAME_SYN, 0x427f80u, 0x417e00u, 0x428420u, 0x44b970u,
     0x413f10u, 0x44e49cu, 0x417u, 0x450b16u, 0x445780u, 0x447bf4u, 0x44b830u, 0x44a73au,
     0x41d380u, 0x40e300u, 0x478110u, 0x40bec0u, 0x470000u, 0x47e790u, 0x44b46au, 0x445314u, 0},
    {GAME_SYN_S, 0x427e70u, 0x417d80u, 0x428310u, 0x449168u,
     0x413e90u, 0x44bc94u, 0x417u, 0x44e30eu, 0x442f78u, 0x4453ecu, 0x449028u, 0x447f32u,
     0x41d2c0u, 0x40e2f0u, 0x478110u, 0x40beb0u, 0x470000u, 0x47e790u, 0x448c62u, 0x442b0cu, 0},
    {GAME_SYN_G, 0x427e70u, 0x417d80u, 0x428310u, 0x4491ccu,
     0x413e90u, 0x44bcf8u, 0x417u, 0x44e372u, 0x442fdcu, 0x445450u, 0x44908cu, 0x447f96u,
     0x41d2c0u, 0x40e2f0u, 0x478110u, 0x40beb0u, 0x470000u, 0x47e790u, 0x448cc6u, 0x442b70u, 0},
    {GAME_AR, 0x4295f0u, 0x418e20u, 0x429ae0u, 0x460958u,
     0x414960u, 0x463c1cu, 0x42du, 0x463bfeu, 0x455750u, 0x457bc4u, 0x460818u, 0x45f686u,
     0x41e9c0u, 0x40e270u, 0x488110u, 0x40bbe0u, 0x480000u, 0x48e790u, 0x4603e8u, 0x4552dcu, 1},
    {GAME_AR_S, 0x429570u, 0x418dd0u, 0x429a60u, 0x45e048u,
     0x414910u, 0x46130cu, 0x42du, 0x4612eeu, 0x452e40u, 0x4552b4u, 0x45df08u, 0x45cd76u,
     0x41e940u, 0x40e280u, 0x488110u, 0x40bbf0u, 0x480000u, 0x48e790u, 0x45dad8u, 0x4529ccu, 1},
    {GAME_AR_G, 0x429570u, 0x418dd0u, 0x429a60u, 0x45e0b0u,
     0x414910u, 0x461374u, 0x42du, 0x461356u, 0x452ea8u, 0x45531cu, 0x45df70u, 0x45cddeu,
     0x41e940u, 0x40e280u, 0x488110u, 0x40bbf0u, 0x480000u, 0x48e790u, 0x45db40u, 0x452a34u, 1},
};

static const GameAddrs *g_addrs; /* the running program's (NULL: the launcher, the intros) */

/* ---- calling the game's functions ---- */
#define GUEST_RET_MARK 0xFFFF0100u

/* calls the guest function at `fn` with the arguments (cdecl), leaving the registers as they were */
static void guest_call(uint32_t fn, const uint32_t *args, int nargs)
{
    Cpu saved = R;
    int i;
    for (i = nargs - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(GUEST_RET_MARK);
    rt_call_ind(fn);
    R = saved;
}

/* ==== auto saves ============================================================================================ */
#define NB_SLOTS 10
#define NB_AUTO_SLOTS 3
#define SAVE_NAME_SIZE 20 /* the name at the start of a save file */

static int g_saving; /* in a save this file asked for (not the player's) */
static int g_mission_over;

static void slot_dos_path(int slot, char *out, size_t n)
{
    char dir[128];
    uint32_t a = g_addrs->save_dir;
    size_t i;
    for (i = 0; i + 1 < sizeof dir && M[a + i]; i++) dir[i] = (char)M[a + i];
    dir[i] = '\0';
    /* the game joins them with a '/' of its own */
    snprintf(out, n, "%s%s%02d.gam", dir, i > 0 && dir[i - 1] != '/' && dir[i - 1] != '\\' ? "/" : "", slot);
}

static void slot_host_path(int slot, char *out, size_t n)
{
    char dos[160];
    slot_dos_path(slot, dos, sizeof dos);
    dos_resolve_write(dos, out, n);
}

/* 0 no save in the slot, 1 a save of the player, 2 an auto save */
static int slot_kind(int slot)
{
    char path[1024], name[SAVE_NAME_SIZE];
    VFile *f;
    size_t got;
    slot_host_path(slot, path, sizeof path);
    f = vfs_open(path, "rb");
    if (f == NULL) return 0;
    got = vfs_read(f, name, sizeof name);
    vfs_close(f);
    return got >= 5 && memcmp(name, "AUTO ", 5) == 0 ? 2 : 1;
}

static int move_slot(int from, int to)
{
    char a[1024], b[1024];
    slot_host_path(from, a, sizeof a);
    slot_host_path(to, b, sizeof b);
    remove(b);
    if (rename(a, b) == 0) return 1;
    rt_log("auto save: cannot move %s to %s", a, b);
    return 0;
}

static int first_free_player_slot(void)
{
    int s;
    for (s = NB_AUTO_SLOTS; s < NB_SLOTS; s++)
        if (slot_kind(s) == 0) return s;
    return -1;
}

static void auto_save(void)
{
    char name[SAVE_NAME_SIZE];
    uint32_t args[2], buf;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int s;
    /* saves of the player in the auto save lines move down to free lines */
    for (s = 0; s < NB_AUTO_SLOTS; s++) {
        if (slot_kind(s) == 1) {
            int to = first_free_player_slot();
            if (to >= 0) move_slot(s, to);
        }
    }
    /* the auto saves move down one line, the oldest is dropped */
    for (s = NB_AUTO_SLOTS - 2; s >= 0; s--)
        if (slot_kind(s) == 2 && slot_kind(s + 1) != 1) move_slot(s, s + 1);
    if (slot_kind(0) == 1) {
        rt_log("auto save: the first line holds a save of the player and no line is free");
        return;
    }
    snprintf(name, sizeof name, "AUTO %02d/%02d %02d:%02d", tm->tm_mday, tm->tm_mon + 1, tm->tm_hour, tm->tm_min);
    /* the name on the guest's stack, below what the call uses */
    R.esp -= 0x40u;
    buf = R.esp;
    memset(M + buf, 0, 0x40u);
    memcpy(M + buf, name, strlen(name));
    args[0] = buf;
    args[1] = 0;
    g_saving = 1;
    guest_call(g_addrs->save_game, args, 2);
    g_saving = 0;
    R.esp += 0x40u;
    rt_log("auto save: %s", name);
}

static int on_mission_end(uint32_t addr)
{
    (void)addr;
    g_mission_over = 1;
    return 0;
}

static void cheat_names(void);

static int on_world_map(uint32_t addr)
{
    (void)addr;
    cheat_names();
    if (g_mission_over) {
        g_mission_over = 0;
        auto_save();
    }
    return 0;
}

/* the player saves: not over an auto save */
static int on_save_game(uint32_t addr)
{
    uint32_t slot = RD32(R.esp + 8u);
    int to;
    (void)addr;
    if (g_saving || slot >= NB_AUTO_SLOTS) return 0;
    to = first_free_player_slot();
    if (to >= 0) {
        uint32_t args[2];
        args[0] = RD32(R.esp + 4u);
        args[1] = (uint32_t)to;
        g_saving = 1;
        guest_call(g_addrs->save_game, args, 2);
        g_saving = 0;
        rt_log("save: line %u holds an auto save, saved in line %d", slot + 1, to + 1);
    } else {
        rt_log("save: line %u holds an auto save and no line is free, not saved", slot + 1);
    }
    R.esp += 4u; /* the function's ret */
    return 1;
}

/* ==== cheats ================================================================================================ */
#define COMPANY_NAME 0x11u     /* in a company: its name and the player's (C strings) */
#define PLAYER_NAME 0x23u
#define COMPANY_AGENTS 0x11du  /* the company's agents, AGENT_SIZE bytes each */
#define NB_AGENTS 18
#define AGENT_SIZE 40u
#define AGENT_MODS 3u          /* (16 bits) bit 0: a woman; bits 1-12: the six mods, two bits each (3: version 3) */
#define AGENT_WEAPONS 8u       /* 8 x (16 bits ammunition, 16 bits weapon) */
#define NB_MODS 18             /* six mods in three versions */
#define MOD_RESEARCH_SIZE 0x1ebu
#define NB_WEAPONS 20
#define WEAPON_RESEARCH_SIZE 0x1f5u
#define RESEARCH_DONE 0x960u
#define WEAPON_MINIGUN 6u

static int name_is(uint32_t a, const char *name)
{
    size_t n = strlen(name);
    return memcmp(M + a, name, n) == 0 && M[a + n] == 0;
}

/* the research of every weapon and mod is done: as the game's COOPER TEAM cheat does it */
static void research_everything(void)
{
    const GameAddrs *g = g_addrs;
    uint32_t i, k;
    for (i = 0; i < NB_MODS; i++) {
        uint32_t e = g->mod_research + i * MOD_RESEARCH_SIZE;
        WR16(e + 8u, RESEARCH_DONE);
        for (k = 0; k < 10u * 24u; k++) WR16(e + 0xau + k * 2u, 0);
    }
    for (i = 0; i < NB_WEAPONS; i++) {
        uint32_t e = g->weapon_research + i * WEAPON_RESEARCH_SIZE;
        int32_t v;
        if (M[e] == 0xfe) continue; /* no such weapon */
        WR16(e + 0x11u, RESEARCH_DONE);
        for (k = 0; k < 10u * 24u; k++) WR16(e + 0x13u + k * 2u, 0);
        M[e + 0x1f4u] = M[g->weapon_known + i];
        v = (int32_t)RD32(e + 1u);
        if (v < 0) WR32(e + 1u, (uint32_t)-v);
    }
}

static void minigun_squad(uint32_t company)
{
    uint32_t a, k, ammo = RD16(g_addrs->ammo + WEAPON_MINIGUN * 2u);
    research_everything();
    for (a = 0; a < NB_AGENTS; a++) {
        uint32_t agent = company + COMPANY_AGENTS + a * AGENT_SIZE;
        WR16(agent + AGENT_MODS, (uint16_t)((RD16(agent + AGENT_MODS) & 1u) | 0x1ffeu));
        for (k = 0; k < 8u; k++) {
            WR16(agent + AGENT_WEAPONS + k * 4u, (uint16_t)ammo);
            WR16(agent + AGENT_WEAPONS + k * 4u + 2u, WEAPON_MINIGUN);
        }
    }
    rt_log("cheat: MINIGUN SQUAD");
}

static int on_cheat_check(uint32_t addr)
{
    uint32_t company = g_addrs->companies + (uint32_t)(int16_t)RD16(g_addrs->player) * g_addrs->company_size;
    (void)addr;
    if (name_is(company + COMPANY_NAME, "MINIGUN SQUAD") || name_is(company + PLAYER_NAME, "MINIGUN SQUAD"))
        minigun_squad(company);
    return 0;
}

/* names from --cheats, once in each run of a game program */
static int g_cheats_done;

static void cheat_names(void)
{
    const char *c = game_cheats(), *colon;
    uint32_t company;
    size_t n;
    if (g_cheats_done || c[0] == '\0') return;
    g_cheats_done = 1;
    company = g_addrs->companies + (uint32_t)(int16_t)RD16(g_addrs->player) * g_addrs->company_size;
    colon = strchr(c, ':');
    n = colon != NULL ? (size_t)(colon - c) : strlen(c);
    if (n > 16) n = 16;
    memset(M + company + COMPANY_NAME, 0, 17);
    memcpy(M + company + COMPANY_NAME, c, n);
    if (colon != NULL) {
        n = strlen(colon + 1);
        if (n > 16) n = 16;
        memset(M + company + PLAYER_NAME, 0, 17);
        memcpy(M + company + PLAYER_NAME, colon + 1, n);
    }
    guest_call(g_addrs->cheat_check, NULL, 0);
    rt_log("cheats: %s", c);
}

/* ==== keys in missions ====================================================================================== */
#define COMPANY_SQUAD 0xb5u   /* in a company: (byte) the ped of its first agent in a mission */
#define PED_SIZE 0x5cu
#define PED_X 4u              /* (16 bits) where it is */
#define PED_Y 6u
#define PED_Z 8u
#define PED_FLAGS 0x1du       /* bit 2: there (an agent of the squad still in the mission) */
#define DOUBLE_PRESS_US 400000u
#define SC_1 0x02u            /* 1 to 4: 0x02 to 0x05 */
#define SC_W 0x11u
#define SC_A 0x1eu
#define SC_S 0x1fu
#define SC_D 0x20u

static volatile uint64_t g_tick_us;   /* the last tick of a mission */
static volatile int g_centre_slot;    /* 1-4: centre the view on that agent at the next tick */
static uint64_t g_press_us[4];
static uint8_t g_down[256], g_moved[256]; /* keys held; held keys that scroll in place of an arrow key */

static int in_mission(void)
{
    return g_tick_us != 0 && host_time_us() - g_tick_us < 200000u;
}

uint32_t game_key_filter(uint32_t sc, int down)
{
    uint32_t k = sc & 0xFFu;
    int was_down;
    if (sc & 0xE000u) return sc;
    was_down = g_down[k];
    g_down[k] = (uint8_t)(down != 0);
    /* 1-4 pressed twice (not held) */
    if (k >= SC_1 && k < SC_1 + 4u && down && !was_down && in_mission()) {
        uint64_t now = host_time_us();
        int slot = (int)(k - SC_1);
        if (g_press_us[slot] != 0 && now - g_press_us[slot] < DOUBLE_PRESS_US) {
            g_centre_slot = slot + 1;
            g_press_us[slot] = 0;
        } else {
            g_press_us[slot] = now;
        }
    }
    /* W, A, S, D: the arrow keys (a key pressed in a mission stays one until it is released) */
    if (k == SC_W || k == SC_A || k == SC_S || k == SC_D) {
        if (down && !was_down) g_moved[k] = (uint8_t)in_mission();
        if (g_moved[k]) {
            if (!down) g_moved[k] = 0;
            return k == SC_W ? 0xE048u : k == SC_A ? 0xE04Bu : k == SC_S ? 0xE050u : 0xE04Du;
        }
    }
    return sc;
}

static void centre_on_agent(int slot)
{
    uint32_t company = g_addrs->companies + (uint32_t)(int16_t)RD16(g_addrs->player) * g_addrs->company_size;
    uint32_t ped = g_addrs->peds + (M[company + COMPANY_SQUAD] + (uint32_t)slot - 1u) * PED_SIZE, args[2];
    int32_t z;
    if (!(M[ped + PED_FLAGS] & 4u)) return;
    /* as the game centres the view at the start of a mission */
    z = (int16_t)RD16(ped + PED_Z);
    args[0] = (uint32_t)(int16_t)(((int16_t)RD16(ped + PED_X) - 0xa00 - z) >> 7);
    args[1] = (uint32_t)(int16_t)(((int16_t)RD16(ped + PED_Y) - 0x200 - z) >> 7);
    guest_call(g_addrs->set_view, args, 2);
}

/* ---- drawing over the mission ---- */
#define PED_TARGET 0x2eu      /* (16 bits x, y, z) where it walks to */
#define VIEW_LEFT 128         /* the map's part of a 640 x 480 frame */
#define VIEW_TOP 40
#define VIEW_BOTTOM 440
#define MARKER_US 600000u

static uint16_t g_last_target[4][2];
static int32_t g_marker[3];           /* x, y, z of the last move order */
static uint64_t g_marker_us;          /* when it was given (0: none) */

/* where a point of the world is in the frame, as the game draws it (its sprite placement) */
static int renderer_on(void);

static void world_to_frame(int32_t x, int32_t y, int32_t z, int *fx, int *fy)
{
    if (renderer_on()) {
        render_project(x, y, z, fx, fy); /* the same projection, with the view being drawn */
        return;
    }
    int32_t dx = x - (int32_t)(int16_t)RD16(g_addrs->view) * 128;
    int32_t dy = y - (int32_t)(int16_t)RD16(g_addrs->view + 2u) * 128;
    *fx = ((dx - dy) >> 3) + VIEW_LEFT;
    *fy = ((((dx + dy) / 2) - z) >> 3) + 16 + VIEW_TOP;
}

/* blends colour c (0x00RRGGBB) into a pixel of the map's part of the frame, `alpha` 0..255 */
static void plot(uint32_t *rgb, int w, int x, int y, uint32_t c, int alpha)
{
    uint32_t *p, o, a = (uint32_t)alpha;
    if (x < VIEW_LEFT || x >= w || y < VIEW_TOP || y >= VIEW_BOTTOM || alpha <= 0) return;
    p = &rgb[y * w + x];
    if (alpha >= 255) {
        *p = c;
        return;
    }
    o = *p;
    *p = ((((c >> 16) & 0xFF) * a + ((o >> 16) & 0xFF) * (255u - a)) / 255u) << 16 |
         ((((c >> 8) & 0xFF) * a + ((o >> 8) & 0xFF) * (255u - a)) / 255u) << 8 |
         (((c & 0xFF) * a + (o & 0xFF) * (255u - a)) / 255u);
}

/* a diamond lying on the floor, half width hw (half height hw / 2): filled, or only its outer `edge` */
static void diamond(uint32_t *rgb, int w, int cx, int cy, int hw, int edge, uint32_t c, int alpha)
{
    int x, y;
    for (y = -hw / 2; y <= hw / 2; y++) {
        for (x = -hw; x <= hw; x++) {
            int d = (x < 0 ? -x : x) + 2 * (y < 0 ? -y : y);
            if (d <= hw && (edge == 0 || d > hw - edge)) plot(rgb, w, cx + x, cy + y, c, alpha);
        }
    }
}

/* ---- what a scanner adds to the scanner ---- */
#define NB_PEDS 256
#define PED_LOCATION 0xau     /* 4: on the map */
#define PED_STATE 0xbu        /* 0x11: dead */
#define PED_TYPE 0x1cu        /* 1 pedestrian, 2 agent, 4 police, 8 guard, 16 criminal */
#define PED_PERSUADER 0x20u   /* (16 bits) who persuaded it (0: nobody) */
#define PED_WEAPON 0x44u      /* (16 bits) the weapon in its hands (0: none) */
#define NB_STATICS 400
#define STATIC_SIZE 30u
#define STATIC_DESC 0xau      /* 4, 6, 7: on the map */
#define STATIC_SUBTYPE 0x19u  /* 0x0c-0x0f a door, 0x26 a large door */
#define SCANNER_X 0           /* the scanner in a 640 x 480 frame */
#define SCANNER_Y 312
#define SCANNER_SIZE 128

static uint32_t g_scanner_leader; /* the ped the scanner is centred on */
static int g_scanner_scale;       /* its pixels a tile */
static int g_scanner_on;          /* an agent carries a scanner */

static void scanner_dot(uint32_t *rgb, int w, int32_t x, int32_t y, int size, uint32_t c)
{
    int32_t lx = (int16_t)RD16(g_scanner_leader + PED_X), ly = (int16_t)RD16(g_scanner_leader + PED_Y);
    int px = SCANNER_X + SCANNER_SIZE / 2 + (int)((x - lx) * g_scanner_scale / 256);
    int py = SCANNER_Y + SCANNER_SIZE / 2 + (int)((y - ly) * g_scanner_scale / 256);
    int i, j;
    for (j = 0; j < size; j++)
        for (i = 0; i < size; i++) {
            int qx = px + i - size / 2, qy = py + j - size / 2;
            if (qx >= SCANNER_X && qx < SCANNER_X + SCANNER_SIZE && qy >= SCANNER_Y && qy < SCANNER_Y + SCANNER_SIZE)
                rgb[qy * w + qx] = c;
        }
}

#define SCANNER_DOOR 0x3cdcffu
#define SCANNER_CRIMINAL 0xff961eu
#define SCANNER_ARMED 0xff3c28u

static void draw_scanner_extras(uint32_t *rgb, int w)
{
    int i;
    if (!g_scanner_on || g_scanner_leader == 0) return;
    for (i = 0; i < NB_STATICS; i++) {
        uint32_t o = g_addrs->statics + (uint32_t)i * STATIC_SIZE;
        uint8_t sub = M[o + STATIC_SUBTYPE], desc = M[o + STATIC_DESC];
        if ((desc == 4 || desc == 6 || desc == 7) && ((sub >= 0x0c && sub <= 0x0f) || sub == 0x26))
            scanner_dot(rgb, w, (int16_t)RD16(o + PED_X), (int16_t)RD16(o + PED_Y), 2, SCANNER_DOOR);
    }
    for (i = 0; i < NB_PEDS; i++) {
        uint32_t p = g_addrs->peds + (uint32_t)i * PED_SIZE;
        uint8_t type = M[p + PED_TYPE];
        if (M[p + PED_LOCATION] != 4 || M[p + PED_STATE] == 0x11 || RD16(p + PED_PERSUADER) != 0) continue;
        if (type != 1 && type != 16) continue;
        if (RD16(p + PED_WEAPON) != 0)
            scanner_dot(rgb, w, (int16_t)RD16(p + PED_X), (int16_t)RD16(p + PED_Y), 3, SCANNER_ARMED);
        else if (type == 16)
            scanner_dot(rgb, w, (int16_t)RD16(p + PED_X), (int16_t)RD16(p + PED_Y), 2, SCANNER_CRIMINAL);
    }
}

/* ---- the selected agents' boxes: solid ---- */
#define PANEL_X0 0            /* the agent selector panel in a 640 x 480 frame */
#define PANEL_Y0 40
#define PANEL_X1 128
#define PANEL_Y1 232

static int reddish(uint32_t c)
{
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
    return r > 150 && r > 2 * g && r > 2 * b;
}

/* as FreeSynd does it: a pixel of a checkerboard has no neighbour of its colour and 3 or 4 of one other colour;
 * lines drawn over it (the digits) are 2 pixels thick, so they stay */
#define SELECTED_COLOUR 0xd7460fu

static void solid_selected_boxes(const uint8_t *pix, const uint32_t *pal, uint32_t *rgb, int w)
{
    static uint8_t mask[(PANEL_Y1 - PANEL_Y0) * (PANEL_X1 - PANEL_X0)];
    static uint16_t pairs[256][256];
    int x, y, a = -1, b = -1, best = 0;
#define PX(x, y) pix[(y) * w + (x)]
#define MK(x, y) mask[((y) - PANEL_Y0) * (PANEL_X1 - PANEL_X0) + (x) - PANEL_X0]
    memset(mask, 0, sizeof mask);
    memset(pairs, 0, sizeof pairs);
    /* the checkerboard of the selection: the most common pair of colours that has a red */
    for (y = PANEL_Y0 + 1; y < PANEL_Y1 - 1; y++)
        for (x = PANEL_X0 + 1; x < PANEL_X1 - 1; x++) {
            uint8_t c = PX(x, y), n = PX(x - 1, y);
            if (n != c && PX(x + 1, y) == n && PX(x, y - 1) == n && PX(x, y + 1) == n && pairs[c][n] < 60000) pairs[c][n]++;
        }
    for (x = 0; x < 256; x++)
        for (y = 0; y < 256; y++)
            if (pairs[x][y] + pairs[y][x] > best && (reddish(pal[x]) || reddish(pal[y]))) {
                best = pairs[x][y] + pairs[y][x];
                a = x;
                b = y;
            }
    if (best < 100) return;
    for (y = PANEL_Y0 + 1; y < PANEL_Y1 - 1; y++)
        for (x = PANEL_X0 + 1; x < PANEL_X1 - 1; x++) {
            uint8_t c = PX(x, y), n[4] = {PX(x - 1, y), PX(x + 1, y), PX(x, y - 1), PX(x, y + 1)};
            int k, other = c == a ? b : a, count = 0;
            if (c != a && c != b) continue;
            for (k = 0; k < 4; k++) {
                if (n[k] == c) break;
                count += n[k] == other;
            }
            if (k == 4 && count >= 3) MK(x, y) = 1;
        }
    for (y = PANEL_Y0 + 1; y < PANEL_Y1 - 1; y++)
        for (x = PANEL_X0 + 1; x < PANEL_X1 - 1; x++) {
            uint8_t c = PX(x, y);
            int same, masked;
            if (MK(x, y) || (c != a && c != b)) continue;
            same = (PX(x - 1, y) == c) + (PX(x + 1, y) == c) + (PX(x, y - 1) == c) + (PX(x, y + 1) == c);
            masked = MK(x - 1, y) + MK(x + 1, y) + MK(x, y - 1) + MK(x, y + 1);
            if (same <= 1 && masked >= 3) MK(x, y) = 2;
        }
    for (y = PANEL_Y0; y < PANEL_Y1; y++)
        for (x = PANEL_X0; x < PANEL_X1; x++)
            if (MK(x, y)) rgb[y * w + x] = SELECTED_COLOUR;
#undef PX
#undef MK
}

/* ---- blood ---- */
#define PED_HEALTH 0x14u      /* (16 bits) */
#define PED_LAST_ENEMY 0x16u  /* (16 bits) who hit it last, from the level data's people - 2 (0: nobody) */
#define NB_DROPS 256
#define DROPS_A_HIT 18
#define DROP_SPEED 1.2        /* world units a ms (a tile is 256) */
#define DROP_GRAVITY 0.006
#define DROP_LIFE_MS 350
#define HIT_HEIGHT 96         /* the middle of a body above its feet */

typedef struct Drop {
    double x, y, z, vx, vy, vz;
    int32_t floor;
    int life, max_life; /* ms; 0: unused */
} Drop;

static Drop g_drops[NB_DROPS];
static int16_t g_health[NB_PEDS];
static uint8_t g_dead[NB_PEDS];
static uint64_t g_drops_us;
static uint32_t g_rand = 12345u;

static int rnd(int n)
{
    g_rand = g_rand * 1103515245u + 12345u;
    return (int)((g_rand >> 16) % (uint32_t)n);
}

static void spray(uint32_t ped)
{
    double x = (int16_t)RD16(ped + PED_X), y = (int16_t)RD16(ped + PED_Y), dx, dy, len;
    int32_t z = (int16_t)RD16(ped + PED_Z);
    uint32_t enemy = RD16(ped + PED_LAST_ENEMY);
    int i, k = 0;
    /* the way the shot went (any way when the shooter is not known) */
    if (enemy != 0 && enemy + PED_SIZE < NB_PEDS * PED_SIZE + 2u) {
        uint32_t s = g_addrs->peds - 2u + enemy;
        dx = x - (int16_t)RD16(s + PED_X);
        dy = y - (int16_t)RD16(s + PED_Y);
    } else {
        dx = rnd(201) - 100;
        dy = rnd(201) - 100;
    }
    len = dx * dx + dy * dy;
    if (len < 1.0) return;
    len = sqrt(len);
    dx /= len;
    dy /= len;
    for (i = 0; i < DROPS_A_HIT; i++) {
        double spread = (rnd(201) - 100) / 250.0, speed = DROP_SPEED * (0.4 + rnd(61) / 100.0);
        Drop *d;
        while (k < NB_DROPS && g_drops[k].life > 0) k++;
        if (k == NB_DROPS) return;
        d = &g_drops[k];
        d->x = x;
        d->y = y;
        d->z = z + HIT_HEIGHT;
        d->vx = (dx - dy * spread) * speed;
        d->vy = (dy + dx * spread) * speed;
        d->vz = rnd(100) / 100.0 * 0.6;
        d->floor = z;
        d->life = d->max_life = DROP_LIFE_MS - rnd(120);
    }
}

/* every tick: a person whose health went down, or who died, was hit */
static void watch_hits(int first)
{
    int i;
    for (i = 0; i < NB_PEDS; i++) {
        uint32_t p = g_addrs->peds + (uint32_t)i * PED_SIZE;
        int16_t h = (int16_t)RD16(p + PED_HEALTH);
        uint8_t dead = M[p + PED_STATE] == 0x11;
        if (!first && M[p + PED_TYPE] != 0 && M[p + PED_LOCATION] == 4 && (h < g_health[i] || (dead && !g_dead[i])))
            spray(p);
        g_health[i] = h;
        g_dead[i] = dead;
    }
}

#define BLOOD_COLOUR 0x960a0au

static void draw_drops(uint32_t *rgb, int w)
{
    uint64_t now = host_time_us();
    int elapsed = g_drops_us ? (int)((now - g_drops_us) / 1000u) : 0, i;
    if (elapsed > 100) elapsed = 100;
    g_drops_us = now;
    for (i = 0; i < NB_DROPS; i++) {
        Drop *d = &g_drops[i];
        int fx, fy;
        if (d->life <= 0) continue;
        d->life -= elapsed;
        if (d->life <= 0) continue;
        if (d->z > d->floor) {
            d->x += d->vx * elapsed;
            d->y += d->vy * elapsed;
            d->z += d->vz * elapsed;
            d->vz -= DROP_GRAVITY * elapsed;
            if (d->z < d->floor) d->z = d->floor; /* landed: it stays until it is gone */
        }
        world_to_frame((int32_t)d->x, (int32_t)d->y, (int32_t)d->z, &fx, &fy);
        {
            int alpha = 255 * d->life / d->max_life; /* it fades out */
            plot(rgb, w, fx, fy, BLOOD_COLOUR, alpha);
            plot(rgb, w, fx + 1, fy, BLOOD_COLOUR, alpha);
            plot(rgb, w, fx, fy + 1, BLOOD_COLOUR, alpha);
            plot(rgb, w, fx + 1, fy + 1, BLOOD_COLOUR, alpha);
        }
    }
}

/* ---- hats in the colours of the companies ---- */
#define HAT_COLOUR 6          /* the hats' colour in the agents' sprites (the first company's) */
#define AGENTS_A_COMPANY 8    /* company c has the people 8c to 8c + 7 */
#define COMPANY_COLOUR 0xfu

/* a company's colour in true colour: the game's (in the palette of the mission) */
static uint32_t company_rgb(uint32_t company, const uint32_t *pal)
{
    return pal[M[g_addrs->colours + M[company + COMPANY_COLOUR]]];
}

static void company_hats(const uint8_t *pix, const uint32_t *pal, uint32_t *rgb, int w)
{
    int i;
    for (i = 0; i < 8 * AGENTS_A_COMPANY; i++) {
        uint32_t p = g_addrs->peds + (uint32_t)i * PED_SIZE;
        uint32_t company = g_addrs->companies + (uint32_t)(i / AGENTS_A_COMPANY) * g_addrs->company_size;
        uint32_t colour = company_rgb(company, pal);
        int fx, fy, x, y;
        if (M[p + PED_TYPE] != 2 || M[p + PED_LOCATION] != 4 || colour == pal[HAT_COLOUR]) continue;
        world_to_frame((int16_t)RD16(p + PED_X), (int16_t)RD16(p + PED_Y), (int16_t)RD16(p + PED_Z), &fx, &fy);
        /* the head, below the number of an agent of the squad */
        for (y = fy - 31; y <= fy - 14; y++)
            for (x = fx - 8; x <= fx + 8; x++)
                if (x >= VIEW_LEFT && x < w && y >= VIEW_TOP && y < VIEW_BOTTOM && pix[y * w + x] == HAT_COLOUR)
                    rgb[y * w + x] = colour;
    }
}

/* development: SYND_RENDER_TEST=match logs how many pixels of the map the renderer draws as the game does (once
 * a second; the first time it also looks for the tiles' offset); =show draws the renderer's city instead */
static void render_test(uint32_t *rgb, const uint32_t *pal)
{
    static uint32_t mine[640 * 480];
    static uint64_t next;
    static int calibrated;
    const char *mode = getenv("SYND_RENDER_TEST");
    int x, y, same = 0;
    uint64_t now = host_time_us();
    if (strcmp(mode, "show") == 0) {
        for (y = 40; y < 440; y++)
            for (x = 128; x < 640; x++) rgb[y * 640 + x] = 0;
        render_city(rgb, 640, 480, pal, 128, 40, 640, 440, 0);
        return;
    }
    if (now < next) return;
    next = now + 1000000u;
    if (!calibrated) {
        int best = -1, bx = 0, by = 0, dx, dy;
        calibrated = 1;
        for (dy = 0; dy < 80; dy += 2)
            for (dx = 64; dx < 160; dx += 2) {
                int n = 0;
                render_tile_dx = dx;
                render_tile_dy = dy;
                memset(mine, 0, sizeof mine);
                render_city(mine, 640, 480, pal, 128, 40, 640, 440, 0);
                for (y = 40; y < 440; y += 2)
                    for (x = 128; x < 640; x += 2) n += mine[y * 640 + x] == rgb[y * 640 + x];
                if (n > best) {
                    best = n;
                    bx = dx;
                    by = dy;
                }
            }
        render_tile_dx = bx;
        render_tile_dy = by;
        rt_log("render: tiles at %+d %+d", bx, by);
    }
    memset(mine, 0, sizeof mine);
    render_city(mine, 640, 480, pal, 128, 40, 640, 440, 0);
    for (y = 40; y < 440; y++)
        for (x = 128; x < 640; x++) same += mine[y * 640 + x] == rgb[y * 640 + x];
    rt_log("render: %.1f%% of the map as the game draws it", 100.0 * same / (512.0 * 400.0));
}

/* ---- the city drawn by render.c, moving smoothly ---- */
static const uint32_t *g_overlay_pal;

static int renderer_on(void)
{
    const char *e = getenv("SYND_RENDER");
    return g_addrs != NULL && (e == NULL || strcmp(e, "off") != 0);
}

static uint32_t hat_colour(int ped)
{
    uint32_t company, c;
    if (ped >= 8 * AGENTS_A_COMPANY || M[g_addrs->peds + (uint32_t)ped * PED_SIZE + PED_TYPE] != 2) return 0;
    company = g_addrs->companies + (uint32_t)(ped / AGENTS_A_COMPANY) * g_addrs->company_size;
    c = company_rgb(company, g_overlay_pal);
    return c == g_overlay_pal[HAT_COLOUR] ? 0 : c;
}

/* the pointer table's current entry (HPOINTER: 6 bytes, pointer to the pixels, width, height): from the pointer
 * to HSPR's table, +0x10 in Syndicate (0x445324), +0x14 in American Revolt */
#define POINTER_CURRENT(ar) ((ar) ? 0x14u : 0x10u)
#define LABEL_SPRITE 1070  /* the numbers 1 to 4 of the squad: sprites 1070 + 2 x slot in yellow, + 1 in red */
#define LABEL_DX (-2)      /* the top left of their coloured pixels from the agent's feet (as the game draws them) */
#define LABEL_DY (-40)
#define LABEL_RED 14       /* their colours in the game's palette: red, yellow (the selected agents) */
#define LABEL_YELLOW 11

/* the city on the playback clock (render.c), with the pointer and the squad's numbers over it */
static void smooth_city(const uint8_t *index, const uint32_t *pal, uint32_t *rgb)
{
    uint32_t company;
    int x, y, mx, my, slot;
    g_overlay_pal = pal;
    render_hat_colour = hat_colour;
    for (y = VIEW_TOP; y < VIEW_BOTTOM; y++)
        for (x = VIEW_LEFT; x < 640; x++) rgb[y * 640 + x] = 0;
    render_city(rgb, 640, 480, pal, VIEW_LEFT, VIEW_TOP, 640, VIEW_BOTTOM, rt_display_us);
    /* the pointer: the game's (the entry of its pointer table it uses), drawn where the mouse is now, as the game
     * draws it (its top left corner 16 pixels up and left of the mouse; the game's mouse runs over its 400 lines
     * from line 40 of the frame); over the side panel and the bars the game's own picture shows it */
    mouse_get(&mx, &my, NULL);
    my += VIEW_TOP;
    {
        uint32_t entry = RD32(g_addrs->data + POINTER_CURRENT(g_addrs->ar));
        if (entry != 0)
            render_image(rgb, 640, 480, pal, RD32(entry), M[entry + 4u], M[entry + 5u], mx - 16, my - 16, VIEW_LEFT,
                         VIEW_TOP, 640, VIEW_BOTTOM);
    }
    /* the numbers over the agents of the squad: drawn again over each agent where the smooth city has it, with the
     * game's own sprites; whether the game shows a number, and in yellow (the selected agents) or red, is read from
     * its picture where it drew it */
    company = g_addrs->companies + (uint32_t)(int16_t)RD16(g_addrs->player) * g_addrs->company_size;
    for (slot = 0; slot < 4; slot++) {
        uint32_t ped = g_addrs->peds + (M[company + COMPANY_SQUAD] + (uint32_t)slot) * PED_SIZE;
        int ax, ay, bx, by, i, j, red = 0, yellow = 0;
        int32_t vx = (int32_t)(int16_t)RD16(g_addrs->view) * 128, vy = (int32_t)(int16_t)RD16(g_addrs->view + 2u) * 128;
        int32_t px = (int16_t)RD16(ped + PED_X), py = (int16_t)RD16(ped + PED_Y), pz = (int16_t)RD16(ped + PED_Z);
        if (M[ped + PED_LOCATION] != 4) continue;
        if (!render_ped_place((int)M[company + COMPANY_SQUAD] + slot, &bx, &by)) continue;
        /* where the game drew the number (its frame may be a little behind its records: a margin) */
        ax = ((px - vx - (py - vy)) >> 3) + render_obj_dx + LABEL_DX;
        ay = (((px - vx + py - vy) / 2 - pz) >> 3) + render_obj_dy + LABEL_DY;
        for (j = -3; j < 13; j++)
            for (i = -4; i < 12; i++) {
                int gx = ax + i, gy = ay + j;
                if (gx < VIEW_LEFT || gx >= 640 || gy < VIEW_TOP || gy >= VIEW_BOTTOM) continue;
                red += index[gy * 640 + gx] == LABEL_RED;
                yellow += index[gy * 640 + gx] == LABEL_YELLOW;
            }
        if (red < 4 && yellow < 4) continue; /* not shown */
        {
            int id = LABEL_SPRITE + slot * 2 + (yellow >= red ? 0 : 1), ix, iy;
            if (!render_sprite_ink(id, &ix, &iy)) continue;
            render_sprite(rgb, 640, 480, pal, id, bx + LABEL_DX - ix, by + LABEL_DY - iy, VIEW_LEFT, VIEW_TOP, 640,
                          VIEW_BOTTOM);
        }
    }
}

#define MARKER_COLOUR 0x5aff5au

void game_overlay(const uint8_t *index, const uint32_t *palette, uint32_t *rgb, int w, int h)
{
    uint64_t age;
    int fx, fy, hw, alpha;
    if (g_addrs == NULL || w != 640 || h != 480 || !in_mission()) return;
    if (getenv("SYND_RENDER_TEST") != NULL) {
        render_test(rgb, palette);
    } else if (renderer_on()) {
        smooth_city(index, palette, rgb);
    } else {
        company_hats(index, palette, rgb, w);
    }
    draw_drops(rgb, w);
    solid_selected_boxes(index, palette, rgb, w);
    draw_scanner_extras(rgb, w);
    if (g_marker_us == 0) return;
    age = host_time_us() - g_marker_us;
    if (age >= MARKER_US) {
        g_marker_us = 0;
        return;
    }
    world_to_frame(g_marker[0], g_marker[1], g_marker[2], &fx, &fy);
    hw = 6 + (int)(22 * (MARKER_US - age) / MARKER_US);
    alpha = (int)(255 * (MARKER_US - age) / MARKER_US); /* it fades out */
    diamond(rgb, w, fx, fy, hw, 4, MARKER_COLOUR, alpha);
    diamond(rgb, w, fx, fy, 4, 0, MARKER_COLOUR, alpha);
}

/* a new move order of an agent of the squad: the marker goes there */
static void watch_move_orders(void)
{
    uint32_t company = g_addrs->companies + (uint32_t)(int16_t)RD16(g_addrs->player) * g_addrs->company_size;
    int slot;
    for (slot = 0; slot < 4; slot++) {
        uint32_t ped = g_addrs->peds + (M[company + COMPANY_SQUAD] + (uint32_t)slot) * PED_SIZE;
        uint16_t tx = RD16(ped + PED_TARGET), ty = RD16(ped + PED_TARGET + 2u);
        if ((tx != g_last_target[slot][0] || ty != g_last_target[slot][1]) && (tx || ty) && (M[ped + PED_FLAGS] & 4u)) {
            g_marker[0] = (int16_t)tx;
            g_marker[1] = (int16_t)ty;
            g_marker[2] = (int16_t)RD16(ped + PED_TARGET + 4u);
            g_marker_us = host_time_us();
        }
        g_last_target[slot][0] = tx;
        g_last_target[slot][1] = ty;
    }
}

static int on_mission_tick(uint32_t addr)
{
    int slot = g_centre_slot;
    (void)addr;
    if (!in_mission()) {
        /* a new mission: the targets of the last one are not orders */
        memset(g_last_target, 0, sizeof g_last_target);
        g_marker_us = 0;
        g_tick_us = host_time_us();
        watch_move_orders();
        g_marker_us = 0;
        memset(g_drops, 0, sizeof g_drops);
        watch_hits(1);
        render_reset();
    }
    g_tick_us = host_time_us();
    if (renderer_on()) render_tick(g_tick_us);
    watch_move_orders();
    watch_hits(0);
    if (slot) {
        g_centre_slot = 0;
        centre_on_agent(slot);
    }
    return 0;
}

/* ==== the scanner =========================================================================================== */
static int on_draw_scanner(uint32_t addr)
{
    uint32_t scale = RD32(R.esp + 8u) & 0xFFFFu;
    (void)addr;
    g_scanner_on = scale == 4u;
    if (scale == 8u) scale = 6u;
    else if (scale == 4u) scale = 2u;
    WR32(R.esp + 8u, scale);
    g_scanner_leader = RD32(R.esp + 4u);
    g_scanner_scale = (int)scale;
    return 0;
}

/* ==== the hooks ============================================================================================= */
void additions_install(int game)
{
    size_t i;
    g_addrs = NULL;
    for (i = 0; i < sizeof k_addrs / sizeof k_addrs[0]; i++)
        if (k_addrs[i].game == game) g_addrs = &k_addrs[i];
    if (g_addrs == NULL) return;
    hook_add(g_addrs->mission_end, on_mission_end);
    hook_add(g_addrs->world_map, on_world_map);
    hook_add(g_addrs->save_game, on_save_game);
    hook_add(g_addrs->cheat_check, on_cheat_check);
    {
        /* what the renderer reads: the pointers in the order HSPR's table ... HBLK */
        RenderAddrs ra;
        uint32_t d = g_addrs->data;
        ra.sprite_tab = d;
        ra.frames = d + (g_addrs->ar ? 0x2cu : 0x24u);
        ra.elements = d + (g_addrs->ar ? 0x34u : 0x2cu);
        ra.map = d + (g_addrs->ar ? 0x50u : 0x48u);
        ra.tiles = d + (g_addrs->ar ? 0x54u : 0x4cu);
        ra.level = g_addrs->peds - 0x8008u;
        ra.view = g_addrs->view;
        render_set_addrs(&ra);
    }
    hook_add(g_addrs->mission_tick, on_mission_tick);
    hook_add(g_addrs->draw_scanner, on_draw_scanner);
}
