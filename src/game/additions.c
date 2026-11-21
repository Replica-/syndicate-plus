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
 * The MINIGUN SQUAD cheat, as the company or player name (Configure Company), like the game's own cheats: every
 * weapon and mod researched, and the 18 agents of the company with every mod at version 3 and a minigun in each
 * of their 8 weapon slots.  The research is completed as the game's COOPER TEAM cheat does it. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../rt/rt.h"
#include "../rt/vfs.h"
#include "hooks.h"

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
} GameAddrs;

static const GameAddrs k_addrs[] = {
    {GAME_SYN, 0x427f80u, 0x417e00u, 0x428420u, 0x44b970u,
     0x413f10u, 0x44e49cu, 0x417u, 0x450b16u, 0x445780u, 0x447bf4u, 0x44b830u, 0x44a73au,
     0x41d380u, 0x40e300u, 0x478110u},
    {GAME_SYN_S, 0x427e70u, 0x417d80u, 0x428310u, 0x449168u,
     0x413e90u, 0x44bc94u, 0x417u, 0x44e30eu, 0x442f78u, 0x4453ecu, 0x449028u, 0x447f32u,
     0x41d2c0u, 0x40e2f0u, 0x478110u},
    {GAME_SYN_G, 0x427e70u, 0x417d80u, 0x428310u, 0x4491ccu,
     0x413e90u, 0x44bcf8u, 0x417u, 0x44e372u, 0x442fdcu, 0x445450u, 0x44908cu, 0x447f96u,
     0x41d2c0u, 0x40e2f0u, 0x478110u},
    {GAME_AR, 0x4295f0u, 0x418e20u, 0x429ae0u, 0x460958u,
     0x414960u, 0x463c1cu, 0x42du, 0x463bfeu, 0x455750u, 0x457bc4u, 0x460818u, 0x45f686u,
     0x41e9c0u, 0x40e270u, 0x488110u},
    {GAME_AR_S, 0x429570u, 0x418dd0u, 0x429a60u, 0x45e048u,
     0x414910u, 0x46130cu, 0x42du, 0x4612eeu, 0x452e40u, 0x4552b4u, 0x45df08u, 0x45cd76u,
     0x41e940u, 0x40e280u, 0x488110u},
    {GAME_AR_G, 0x429570u, 0x418dd0u, 0x429a60u, 0x45e0b0u,
     0x414910u, 0x461374u, 0x42du, 0x461356u, 0x452ea8u, 0x45531cu, 0x45df70u, 0x45cddeu,
     0x41e940u, 0x40e280u, 0x488110u},
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

static int on_world_map(uint32_t addr)
{
    (void)addr;
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

static int on_mission_tick(uint32_t addr)
{
    int slot = g_centre_slot;
    (void)addr;
    g_tick_us = host_time_us();
    if (slot) {
        g_centre_slot = 0;
        centre_on_agent(slot);
    }
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
    hook_add(g_addrs->mission_tick, on_mission_tick);
}
