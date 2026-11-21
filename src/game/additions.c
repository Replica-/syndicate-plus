/* additions.c - what this version adds to the original game, as hooks on its functions (hooks.h).
 *
 * Auto saves: when the world map comes up after a mission, the game is saved in the first line of the Load/Save
 * screen, by the game's own save routine; the auto saves before it move down a line.  The first three lines hold
 * the auto saves ("AUTO dd/mm hh:mm"), newest first; only saves named so are moved or replaced, and a save of
 * the player found in those lines is first moved to a free line further down.  A save the player makes in one of
 * the auto save lines goes to the first free line from the fourth on instead (none free: not saved). */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../rt/rt.h"
#include "../rt/vfs.h"
#include "hooks.h"

#define NB_SLOTS 10
#define NB_AUTO_SLOTS 3
#define SAVE_NAME_SIZE 20 /* the name at the start of a save file */

/* where the functions and data of each program are */
typedef struct GameAddrs {
    int game;
    uint32_t mission_end;   /* the mission is over (results counted), before the debriefing */
    uint32_t world_map;     /* the world map screen starts */
    uint32_t save_game;     /* save(name, slot): writes C:\SYND\SAVE\<slot>.GAM */
    uint32_t save_dir;      /* the save folder (a C string, "c:/synd/save") */
} GameAddrs;

/* English Syndicate's, found in the others with tools/find_equiv.py */
#define GAME_ADDRS(X)                                             X(GAME_SYN, 0x427f80u, 0x417e00u, 0x428420u, 0x44b970u)       X(GAME_SYN_S, 0x427e70u, 0x417d80u, 0x428310u, 0x449168u)     X(GAME_SYN_G, 0x427e70u, 0x417d80u, 0x428310u, 0x4491ccu)     X(GAME_AR, 0x4295f0u, 0x418e20u, 0x429ae0u, 0x460958u)        X(GAME_AR_S, 0x429570u, 0x418dd0u, 0x429a60u, 0x45e048u)      X(GAME_AR_G, 0x429570u, 0x418dd0u, 0x429a60u, 0x45e0b0u)

#define ADDRS_ENTRY(g, end, map, save, dir) {g, end, map, save, dir},
static const GameAddrs k_addrs[] = {GAME_ADDRS(ADDRS_ENTRY)};

static const GameAddrs *addrs(void)
{
    size_t i;
    for (i = 0; i < sizeof k_addrs / sizeof k_addrs[0]; i++)
        if (k_addrs[i].game == game_current()) return &k_addrs[i];
    return NULL;
}

static int g_saving; /* in a save this file asked for (not the player's) */

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

/* ---- the save files ---- */
static void slot_dos_path(int slot, char *out, size_t n)
{
    char dir[128];
    uint32_t a = addrs()->save_dir;
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
    const GameAddrs *g = addrs();
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
    guest_call(g->save_game, args, 2);
    g_saving = 0;
    R.esp += 0x40u;
    rt_log("auto save: %s", name);
}

/* ---- the hooks ---- */
static int g_mission_over;

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
        guest_call(addrs()->save_game, args, 2);
        g_saving = 0;
        rt_log("save: line %u holds an auto save, saved in line %d", slot + 1, to + 1);
    } else {
        rt_log("save: line %u holds an auto save and no line is free, not saved", slot + 1);
    }
    R.esp += 4u; /* the function's ret */
    return 1;
}

#define AUTO_SAVE_HOOKS(g, end, map, save, dir) {g, end, on_mission_end}, {g, map, on_world_map}, {g, save, on_save_game},
const HookDef g_hooks[] = {
    GAME_ADDRS(AUTO_SAVE_HOOKS)
    {GAME_NONE, 0, NULL},
};
