/* additions.c - what this version adds to the original game, as hooks on its functions (hooks.h).
 *
 * Auto saves: when the world map comes up after a mission, the game is saved in the first line of the Load/Save
 * screen, by the game's own save routine; the auto saves before it move down a line.  The first three lines hold
 * the auto saves ("AUTO dd/mm hh:mm"), newest first; only saves named so are moved or replaced, and a save of
 * the player found in those lines is first moved to a free line further down.  A save the player makes in one of
 * the auto save lines goes to the first free line from the fourth on instead (none free: not saved).
 *
 * Keys in missions: the arrow keys and W, A, S, D (and the pointer at an edge of the screen) scroll the view at
 * once, at every frame shown: the view is this program's, and the game's camera follows it at its ticks (the
 * clicks on the city are moved by the difference, so they go where they are seen); 1 to 4 select an agent (the
 * game's own) and pressed twice within 0.4 s centre the view on it.  A double click on the city sets the selected
 * agents' speed drug (adrenaline) to the top.
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
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../rt/rt.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "../third_party/stb_image.h"
#include "../rt/vfs.h"
#include "game.h"
#include "hooks.h"
#include "menu_backdrop.h"
#include "menu_sides.h"
#include "path.h"
#include "rnc.h"
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
    /* the pace of a mission */
    uint32_t frame_wait;   /* the mission loop's wait for the next tick (the timer counted `speed` interrupts) */
    uint32_t speed;        /* (32 bits) the interrupts a tick waits for (7; the game's keys change it, 3..12) */
    uint32_t wait_count;   /* (8 bits) the interrupts counted since the last tick */
    uint32_t label_frames; /* (16 bits x 4) the frames of the squad's numbers, as the game draws them */
    uint32_t draw_logo;    /* draw_logo(logo, colour, x, y, w, h, logos): a company's logo */
    uint32_t logo_ret;     /* where Configure Company draws its own (the return address) */
    uint32_t logo_local;   /* there: the logo it edits, from the stack pointer */
    uint32_t colour_local; /* and the colour */
    int ar;                /* American Revolt: its pointers are further apart */
} GameAddrs;

static const GameAddrs k_addrs[] = {
    {GAME_SYN, 0x427f80u, 0x417e00u, 0x428420u, 0x44b970u,
     0x413f10u, 0x44e49cu, 0x417u, 0x450b16u, 0x445780u, 0x447bf4u, 0x44b830u, 0x44a73au,
     0x41d380u, 0x40e300u, 0x478110u, 0x40bec0u, 0x470000u, 0x47e790u, 0x44b46au, 0x445314u,
     0x408810u, 0x445304u, 0x450b50u, 0x443ef8u, 0x4086f0u, 0x4077e7u, 0xF4u, 0xECu, 0},
    {GAME_SYN_S, 0x427e70u, 0x417d80u, 0x428310u, 0x449168u,
     0x413e90u, 0x44bc94u, 0x417u, 0x44e30eu, 0x442f78u, 0x4453ecu, 0x449028u, 0x447f32u,
     0x41d2c0u, 0x40e2f0u, 0x478110u, 0x40beb0u, 0x470000u, 0x47e790u, 0x448c62u, 0x442b0cu,
     0x408810u, 0x442afcu, 0x44e348u, 0x441e10u, 0x4086f0u, 0x4077e7u, 0xF4u, 0xECu, 0},
    {GAME_SYN_G, 0x427e70u, 0x417d80u, 0x428310u, 0x4491ccu,
     0x413e90u, 0x44bcf8u, 0x417u, 0x44e372u, 0x442fdcu, 0x445450u, 0x44908cu, 0x447f96u,
     0x41d2c0u, 0x40e2f0u, 0x478110u, 0x40beb0u, 0x470000u, 0x47e790u, 0x448cc6u, 0x442b70u,
     0x408810u, 0x442b60u, 0x44e3acu, 0x441e74u, 0x4086f0u, 0x4077e7u, 0xF4u, 0xECu, 0},
    {GAME_AR, 0x4295f0u, 0x418e20u, 0x429ae0u, 0x460958u,
     0x414960u, 0x463c1cu, 0x42du, 0x463bfeu, 0x455750u, 0x457bc4u, 0x460818u, 0x45f686u,
     0x41e9c0u, 0x40e270u, 0x488110u, 0x40bbe0u, 0x480000u, 0x48e790u, 0x4603e8u, 0x4552dcu,
     0x408700u, 0x4552ccu, 0x4662b1u, 0x453e5cu, 0x4085e0u, 0x4076e9u, 0xECu, 0xF0u, 1},
    {GAME_AR_S, 0x429570u, 0x418dd0u, 0x429a60u, 0x45e048u,
     0x414910u, 0x46130cu, 0x42du, 0x4612eeu, 0x452e40u, 0x4552b4u, 0x45df08u, 0x45cd76u,
     0x41e940u, 0x40e280u, 0x488110u, 0x40bbf0u, 0x480000u, 0x48e790u, 0x45dad8u, 0x4529ccu,
     0x408700u, 0x4529bcu, 0x4639a1u, 0x451c9cu, 0x4085e0u, 0x4076e9u, 0xECu, 0xF0u, 1},
    {GAME_AR_G, 0x429570u, 0x418dd0u, 0x429a60u, 0x45e0b0u,
     0x414910u, 0x461374u, 0x42du, 0x461356u, 0x452ea8u, 0x45531cu, 0x45df70u, 0x45cddeu,
     0x41e940u, 0x40e280u, 0x488110u, 0x40bbf0u, 0x480000u, 0x48e790u, 0x45db40u, 0x452a34u,
     0x408700u, 0x452a24u, 0x463a09u, 0x451d04u, 0x4085e0u, 0x4076e9u, 0xECu, 0xF0u, 1},
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
#define AGENT_BOX_X 32        /* the middle of agent 1's box in the side panel (the game's coordinates), 2 right of */
#define AGENT_BOX_Y 24        /* it, 3 under it */
#define AGENT_BOX_DX 64
#define AGENT_BOX_DY 98
#define SC_W 0x11u
#define SC_A 0x1eu
#define SC_S 0x1fu
#define SC_D 0x20u
#define SC_B 0x30u                /* held in a mission: see inside the buildings */
static volatile int g_see_inside;
static int g_click_sx, g_click_sy; /* where on the picture the last click was (B held: the order goes to the floor seen
                                    * there) */
/* the light editor (Ctrl+L in a mission): its keys, then what they do to the tile under the pointer */
#define SC_CTRL 0x1du
#define SC_L 0x26u
#define SC_G 0x22u
#define SC_C 0x2eu
#define SC_LBRACKET 0x1au
#define SC_RBRACKET 0x1bu
static volatile int g_editor;
static volatile int g_edit_key;

static volatile uint64_t g_tick_us;   /* the last tick of a mission */
static volatile int g_pan[4];         /* scroll keys held: left, right, up, down */
static volatile int g_centre_slot;    /* 1-4: centre the view on that agent at the next tick */
static uint64_t g_press_us[4];
static uint8_t g_down[256], g_moved[256]; /* keys held; held keys that scroll in place of an arrow key */

static volatile int g_menu; /* the mission's menu (Escape) is open: the mission waits */
static volatile int g_pick_active, g_pick_click; /* the choice of game is on screen (game_pick), a click on it */
static volatile uint32_t g_pick_key;

static int in_mission(void)
{
    return g_tick_us != 0 && (g_menu || host_time_us() - g_tick_us < 200000u);
}

static int renderer_on(void);

int game_canvas_width(int w, int h)
{
    if (h == 200) {
        /* a menu: as wide as the window shows it, the sides at most as wide as the photo's (menu_sides.h) */
        int mw = host_menu_width();
        if (g_addrs == NULL || w != 320) return w;
        return mw < 320 ? 320 : mw > 320 + 2 * MENU_SIDE_W ? 320 + 2 * MENU_SIDE_W : mw;
    }
    if (g_addrs == NULL || w != 640 || h != 480 || !in_mission() || !renderer_on() || getenv("SYND_RENDER_TEST") != NULL)
        return w;
    return host_canvas_width();
}

/* the menu's keys (a scancode, 0xE000 for the arrows), read on the game thread */
static volatile uint32_t g_menu_key;
static volatile int g_pass_esc; /* Abort mission: the game gets this Escape */
#define SC_ESC 0x01u

uint32_t game_key_filter(uint32_t sc, int down)
{
    uint32_t k = sc & 0xFFu;
    int was_down;
    if (g_pick_active) { /* the choice of game has the keys */
        if (down) g_pick_key = sc;
        return 0;
    }
    if (k == SC_ESC && !(sc & 0xE000u)) {
        static uint8_t esc_down;
        int was = esc_down;
        esc_down = (uint8_t)(down != 0);
        if (g_pass_esc) {
            if (!down) g_pass_esc = 0;
            return sc;
        }
        if (!in_mission() || g_addrs == NULL || !renderer_on()) return sc;
        if (down && !was) g_menu_key = g_menu ? SC_ESC : 0xFFFFu; /* open, or close (on the game thread) */
        return 0;
    }
    if (g_menu) {
        if (down) g_menu_key = sc;
        return 0;
    }
    if (sc & 0xE000u) {
        /* the arrows (extended keys): this program's view too */
        static uint8_t moved[256];
        int dir = k == 0x4B ? 0 : k == 0x4D ? 1 : k == 0x48 ? 2 : k == 0x50 ? 3 : -1;
        if (dir < 0) return sc;
        if (down && !moved[k]) moved[k] = (uint8_t)in_mission();
        if (!moved[k]) return sc;
        if (!down) moved[k] = 0;
        g_pan[dir] = down;
        return 0;
    }
    was_down = g_down[k];
    g_down[k] = (uint8_t)(down != 0);
    /* 1-4: the agent selected at once, as a click on its box in the side panel does (the game itself looks at
     * these keys only every few ticks, and misses a short press); pressed twice (not held), the view centred */
    if (k >= SC_1 && k < SC_1 + 4u && in_mission()) {
        uint64_t now = host_time_us();
        int slot = (int)(k - SC_1);
        if (!down || was_down) return 0;
        rt_mouse_click_at(AGENT_BOX_X + (slot & 1) * AGENT_BOX_DX, AGENT_BOX_Y + (slot >> 1) * AGENT_BOX_DY);
        if (g_press_us[slot] != 0 && now - g_press_us[slot] < DOUBLE_PRESS_US) {
            g_centre_slot = slot + 1;
            g_press_us[slot] = 0;
        } else {
            g_press_us[slot] = now;
        }
        return 0;
    }
    /* Ctrl+L in a mission: the light editor on or off; while it is on its keys are its own */
    if (k == SC_L && down && !was_down && g_down[SC_CTRL] && in_mission()) {
        g_editor = !g_editor;
        return 0;
    }
    if (g_editor && in_mission() && (k == SC_G || k == SC_L || k == SC_C || k == SC_LBRACKET || k == SC_RBRACKET)) {
        if (down && !was_down) g_edit_key = (int)k;
        return 0;
    }
    /* B held in a mission: the buildings are seen inside (the game does not get it) */
    if (k == SC_B) {
        static uint8_t held;
        if (down && !was_down) held = (uint8_t)in_mission();
        if (held) {
            if (!down) held = 0;
            g_see_inside = down && held;
            return 0;
        }
    }
    /* W, A, S, D and the arrows scroll this program's view (a key pressed in a mission does until it is released);
     * the game does not get them */
    if (k == SC_W || k == SC_A || k == SC_S || k == SC_D) {
        if (down && !was_down) g_moved[k] = (uint8_t)in_mission();
        if (g_moved[k]) {
            if (!down) g_moved[k] = 0;
            g_pan[k == SC_A ? 0 : k == SC_D ? 1 : k == SC_W ? 2 : 3] = down;
            return 0;
        }
    }
    return sc;
}

/* ---- the view: this program's in missions, the game's camera following it ---- */
#define PAN_PX_S 480.0      /* scrolling speed, pixels of the city a second */
#define EDGE_PX 2           /* the pointer this close to an edge of the screen scrolls */
#define FOLLOW_UNITS 3      /* the game moved its camera this far itself (camera units): the view goes there */

static double g_view_x, g_view_y;     /* the view (world units) */
static int g_view_on;                 /* set for this mission */
static int32_t g_cam_set_x, g_cam_set_y; /* the camera this program gave the game last (camera units) */
static double g_glide_x, g_glide_y;   /* where the view glides to (the game moved its camera itself) */
static int g_gliding;
static uint64_t g_view_us;

static void game_camera(int32_t *x, int32_t *y)
{
    *x = (int16_t)RD16(g_addrs->view);
    *y = (int16_t)RD16(g_addrs->view + 2u);
}

/* ---- the picture in missions: as wide as the window shows it (host_canvas_width), the city zoomed with the wheel
 * about the middle of the map's part.  The game still sees its own 640 x 480 picture: its camera is kept where the
 * middle of its map window (x 128..640) is the middle of this picture, nudged at each tick so that the city under
 * the pointer is in that window, and the mouse it reads is moved to that point (game_mouse_filter). ---- */
#define MAP_LEFT 128
#define GAME_TOP 40         /* the game's map window: from line 40 of its frame, 400 lines (its mouse's) */
#define VIEW_TOP 0          /* this picture's city: the whole height right of the side panel (as FreeSynd's) */
#define VIEW_BOTTOM 480
#define MAP_CY 240.0
#define GAME_MAP_CX 384.0   /* the middle of the game's map window */
#define ZOOM_MIN 0.5
#define ZOOM_MAX 1.5
#define ZOOM_STEP 1.15      /* a notch of the wheel */
#define MOUSE_MARGIN 8      /* the game's mouse kept this far inside its map window */
#define NUDGE_MARGIN 24     /* the camera puts the city under the pointer at least this far inside it */
static int g_cw = 640;      /* the width of the picture */
static double g_zoom = 1.0, g_zoom_target = 1.0;

static double map_cx(void)
{
    return (MAP_LEFT + g_cw) / 2.0;
}

/* the world units from the view to the camera that puts the middle of this picture in the middle of the game's */
static void centre_shift(double *dx, double *dy)
{
    double e = map_cx() - GAME_MAP_CX; /* screen right: world (+4, -4) a pixel */
    *dx = 4.0 * e;
    *dy = -4.0 * e;
}

/* the mouse on this picture */
static void canvas_mouse(int *sx, int *sy)
{
    int fx, fy;
    mouse_fraction(&fx, &fy);
    *sx = (int)((long long)fx * g_cw / 65536);
    *sy = (int)((long long)fy * 480 / 65536);
}

/* a point of this picture on the city as it would be unzoomed */
static void unzoom(double sx, double sy, double *lx, double *ly)
{
    *lx = map_cx() + (sx - map_cx()) / g_zoom;
    *ly = MAP_CY + (sy - MAP_CY) / g_zoom;
}

/* where the game's mouse (its map window: x 128..639, y 0..399) has the point lx, ly of the unzoomed picture,
 * with its camera at world wx, wy */
static void game_point(double lx, double ly, double wx, double wy, double *gx, double *gy)
{
    double dx = g_view_x - wx, dy = g_view_y - wy;
    *gx = lx + (dx - dy) / 8.0;
    *gy = ly - GAME_TOP + (dx + dy) / 16.0;
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void game_wheel(int notches)
{
    if (g_addrs == NULL || !in_mission() || !g_view_on) return;
    g_zoom_target = clampd(g_zoom_target * pow(ZOOM_STEP, notches), ZOOM_MIN, ZOOM_MAX);
}

/* every frame: the keys held and the pointer at the edges move the view */
static void move_view(void)
{
    uint64_t now = host_time_us();
    double dt = g_view_us != 0 && now > g_view_us ? (double)(now - g_view_us) / 1e6 : 0.0, sx = 0, sy = 0, shx, shy;
    int mx, my;
    if (dt > 0.1) dt = 0.1;
    g_view_us = now;
    if (!g_view_on) return;
    /* the zoom eases to where the wheel set it */
    if (g_zoom != g_zoom_target) {
        g_zoom += (g_zoom_target - g_zoom) * (dt * 14.0 > 1.0 ? 1.0 : dt * 14.0);
        if (fabs(g_zoom - g_zoom_target) < 0.002) g_zoom = g_zoom_target;
    }
    render_set_zoom(g_zoom, (float)map_cx(), (float)MAP_CY);
    canvas_mouse(&mx, &my);
    sx = (g_pan[1] || mx >= g_cw - 1 - EDGE_PX ? 1 : 0) - (g_pan[0] || mx <= EDGE_PX ? 1 : 0);
    /* the window shows the game's 400 lines (the picture's top and bottom GAME_TOP lines are off it) */
    sy = (g_pan[3] || my >= VIEW_BOTTOM - GAME_TOP - 1 - EDGE_PX ? 1 : 0) -
         (g_pan[2] || my <= VIEW_TOP + GAME_TOP + EDGE_PX ? 1 : 0);
    if (sx != 0 || sy != 0) {
        /* screen right is world (+1, -1) at 1/4 px a unit, screen down (+1, +1) at 1/8 (the same speed on the
         * screen at any zoom) */
        double px = sx * PAN_PX_S * dt / g_zoom, py = sy * PAN_PX_S * dt / g_zoom;
        double nx = g_view_x + px * 2.0 + py * 4.0, ny = g_view_y - px * 2.0 + py * 4.0;
        /* the middle of the view on the map, as far as its edges (past the game's own limits for its camera: it
         * stays at the nearest it can be) */
        int tw, th, tz;
        centre_shift(&shx, &shy);
        if (render_map_size(&tw, &th, &tz)) {
            nx = clampd(nx + shx, 0.0, tw * 256.0) - shx;
            ny = clampd(ny + shy, 0.0, th * 256.0) - shy;
        }
        g_view_x = nx;
        g_view_y = ny;
        g_gliding = 0;
    } else if (g_gliding) {
        double k = dt * 12.0 > 1.0 ? 1.0 : dt * 12.0;
        g_view_x += (g_glide_x - g_view_x) * k;
        g_view_y += (g_glide_y - g_view_y) * k;
        if (fabs(g_glide_x - g_view_x) < 2.0 && fabs(g_glide_y - g_view_y) < 2.0) {
            g_view_x = g_glide_x; /* there: exactly where the game's camera has it */
            g_view_y = g_glide_y;
            g_gliding = 0;
        }
    }
    render_set_view_f(g_view_x, g_view_y);
}

/* at a tick: the game's camera follows the view (the nearest it can be); a camera the game moved itself (the start
 * of a mission, centring on an agent) takes the view there */
static void sync_camera(void)
{
    int32_t cx, cy;
    uint32_t args[2];
    double shx, shy, wx, wy;
    int mx, my;
    centre_shift(&shx, &shy);
    game_camera(&cx, &cy);
    if (!g_view_on) {
        g_view_x = cx * 128.0 - shx;
        g_view_y = cy * 128.0 - shy;
        g_cam_set_x = cx;
        g_cam_set_y = cy;
        g_view_on = 1;
        g_gliding = 0;
        render_set_view(1, (int32_t)g_view_x, (int32_t)g_view_y);
        return;
    }
    if (abs(cx - g_cam_set_x) >= FOLLOW_UNITS || abs(cy - g_cam_set_y) >= FOLLOW_UNITS) {
        g_glide_x = cx * 128.0 - shx;
        g_glide_y = cy * 128.0 - shy;
        g_gliding = 1;
        g_cam_set_x = cx;
        g_cam_set_y = cy;
        return;
    }
    /* the camera with the middle of the game's map window in the middle of this picture, moved so the city under
     * the pointer is inside that window (where the game can be given the mouse) */
    wx = g_view_x + shx;
    wy = g_view_y + shy;
    canvas_mouse(&mx, &my);
    if (mx >= MAP_LEFT && my >= VIEW_TOP && my < VIEW_BOTTOM) {
        double lx, ly, gx, gy, ex, ey;
        unzoom(mx, my, &lx, &ly);
        game_point(lx, ly, wx, wy, &gx, &gy);
        ex = gx - clampd(gx, MAP_LEFT + NUDGE_MARGIN, 639 - NUDGE_MARGIN);
        ey = gy - clampd(gy, NUDGE_MARGIN, 399 - NUDGE_MARGIN);
        wx += 4.0 * ex + 8.0 * ey;
        wy += 8.0 * ey - 4.0 * ex;
    }
    args[0] = (uint32_t)(int32_t)floor((wx + 64.0) / 128.0);
    args[1] = (uint32_t)(int32_t)floor((wy + 64.0) / 128.0);
    if ((int32_t)args[0] != cx || (int32_t)args[1] != cy) guest_call(g_addrs->set_view, args, 2);
    game_camera(&cx, &cy);
    g_cam_set_x = cx;
    g_cam_set_y = cy;
}

void game_mouse_filter(int *x, int *y)
{
    int32_t cx, cy;
    int fx, fy, sx, sy;
    double lx, ly, gx, gy;
    if (!g_view_on || g_addrs == NULL || !in_mission() || *y < 0 || *y >= 400) return;
    mouse_fraction(&fx, &fy);
    sx = (int)((long long)fx * g_cw / 65536);
    sy = (int)((long long)fy * 480 / 65536);
    if (sx < MAP_LEFT) {
        /* the side panel: where it is on this picture */
        *x = sx;
        *y = sy - GAME_TOP < 0 ? 0 : sy - GAME_TOP > 399 ? 399 : sy - GAME_TOP;
        return;
    }
    /* the point of the city under the pointer, where the game's camera has it */
    unzoom(sx, sy, &lx, &ly);
    game_camera(&cx, &cy);
    game_point(lx, ly, cx * 128.0, cy * 128.0, &gx, &gy);
    *x = (int)floor(clampd(gx, MAP_LEFT + MOUSE_MARGIN, 639 - MOUSE_MARGIN));
    *y = (int)floor(clampd(gy, MOUSE_MARGIN, 399 - MOUSE_MARGIN));
}

/* the drugs (IPA) of a person: adrenaline (speed), perception, intelligence, 4 bytes each; the third the level set
 * by its bar (0-255, 128 neutral) */
#define PED_ADRENALINE_SET 0x49u
#define DOUBLE_CLICK_US 400000u
static volatile int g_boost; /* a double click on the city: the selected agents' speed drug to the top */
static volatile int g_selected; /* the squad's agents selected (bit 0: agent 1), as the panel shows them */

static void boost_selected(void)
{
    uint32_t company = g_addrs->companies + (uint32_t)(int16_t)RD16(g_addrs->player) * g_addrs->company_size;
    int slot, sel = g_selected;
    for (slot = 0; slot < 4; slot++) {
        uint32_t ped = g_addrs->peds + (M[company + COMPANY_SQUAD] + (uint32_t)slot) * PED_SIZE;
        if ((sel >> slot) & 1) M[ped + PED_ADRENALINE_SET] = 255;
    }
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
    if (g_view_on) {
        /* this program's view glides there */
        int32_t cx, cy;
        double shx, shy;
        game_camera(&cx, &cy);
        centre_shift(&shx, &shy);
        g_glide_x = cx * 128.0 - shx;
        g_glide_y = cy * 128.0 - shy;
        g_gliding = 1;
        g_cam_set_x = cx;
        g_cam_set_y = cy;
    }
}

/* ---- drawing over the mission ---- */
#define PED_TARGET 0x2eu      /* (16 bits x, y, z) where it walks to */
#define VIEW_LEFT 128         /* the map's part of the frame: right of the side panel */
#define MARKER_US 600000u

static uint16_t g_last_target[4][2];
static int32_t g_marker[3];           /* x, y, z of the last move order */
static uint64_t g_marker_us;          /* when it was given (0: none) */

/* where a point of the world is in the frame, as the game draws it (its sprite placement) */
static void world_to_frame(int32_t x, int32_t y, int32_t z, int *fx, int *fy)
{
    if (renderer_on()) {
        render_project(x, y, z, fx, fy); /* the same projection, with the view being drawn */
        return;
    }
    int32_t dx = x - (int32_t)(int16_t)RD16(g_addrs->view) * 128;
    int32_t dy = y - (int32_t)(int16_t)RD16(g_addrs->view + 2u) * 128;
    *fx = ((dx - dy) >> 3) + VIEW_LEFT;
    *fy = ((((dx + dy) / 2) - z) >> 3) + 16 + GAME_TOP;
}

/* blends colour c (0x00RRGGBB) into a pixel of the map's part of the frame, `alpha` 0..255; the pixel may be
 * see-through (its top byte: the city drawn by the GPU under the frame shows through), and stays as see-through
 * as the colour lets it */
static void plot(uint32_t *rgb, int w, int x, int y, uint32_t c, int alpha)
{
    uint32_t *p, o, a = (uint32_t)alpha, ob, op, na, k;
    if (x < VIEW_LEFT || x >= w || y < VIEW_TOP || y >= VIEW_BOTTOM || alpha <= 0) return;
    p = &rgb[y * w + x];
    if (alpha >= 255) {
        *p = c;
        return;
    }
    o = *p;
    op = 255u - (o >> 24); /* the opacity of what is there */
    ob = op * (255u - a) / 255u;
    na = a + ob;           /* the opacity after */
    if (na == 0) return;
    for (k = 0; k < 24; k += 8) {
        uint32_t v = (((c >> k) & 0xFF) * a + ((o >> k) & 0xFF) * ob) / na;
        *p = (*p & ~(0xFFu << k)) | (v << k);
    }
    *p = (*p & 0x00FFFFFFu) | ((255u - na) << 24);
}

/* a pixel anywhere in the frame (opaque), and blended */
static void plot_any(uint32_t *rgb, int w, int x, int y, uint32_t c)
{
    if (x >= 0 && x < w && y >= 0 && y < 480) rgb[y * w + x] = c;
}

static void plot_any_blend(uint32_t *rgb, int w, int x, int y, uint32_t c, int alpha)
{
    uint32_t o, a = (uint32_t)alpha, k, r = 0, op;
    if (x < 0 || x >= w || y < 0 || y >= 480) return;
    o = rgb[y * w + x];
    op = 255u - (o >> 24);
    /* over a see-through pixel (the city under it) the colour shows as opaque as the panel */
    for (k = 0; k < 24; k += 8) r |= (((c >> k) & 0xFF) * a + ((o >> k) & 0xFF) * (255u - a) * op / 255u) / 255u << k;
    rgb[y * w + x] = r | (uint32_t)(255u - (a + (255u - a) * op / 255u)) << 24;
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
#define PED_LOCATION 0xau     /* 4: on the map; the game adds 8 while the person fights or is hit (and keeps it
                               * on a dead agent): PED_ON_MAP */
#define PED_ON_MAP(p) ((M[(p) + PED_LOCATION] & ~8u) == 4u)
#define PED_STATE 0xbu        /* 0x11: dead */
#define PED_TYPE 0x1cu        /* 1 pedestrian, 2 agent, 4 police, 8 guard, 16 criminal; the game adds 0x40 once the
                               * person has killed: PED_KIND */
#define PED_KIND(p) (M[(p) + PED_TYPE] & 0x1fu)
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
        uint8_t type = (uint8_t)PED_KIND(p);
        if (!PED_ON_MAP(p) || M[p + PED_STATE] == 0x11 || RD16(p + PED_PERSUADER) != 0) continue;
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
static const int k_agent_box[4][4] = {{4, 44, 60, 84}, {68, 44, 124, 84}, {4, 142, 60, 182}, {68, 142, 124, 182}};

static void solid_selected_boxes(const uint8_t *pix, const uint32_t *pal, uint32_t *rgb, int w)
{
    static uint8_t mask[(PANEL_Y1 - PANEL_Y0) * (PANEL_X1 - PANEL_X0)];
    static uint16_t pairs[256][256];
    int x, y, a = -1, b = -1, best = 0;
#define PX(x, y) pix[(y) * 640 + (x)] /* the game's picture: 640 wide */
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
    if (best < 100) {
        g_selected = 0;
        return;
    }
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
    {
        /* which boxes are selected: most of their checkerboard */
        int k, sel = 0;
        for (k = 0; k < 4; k++) {
            int n = 0;
            for (y = k_agent_box[k][1]; y < k_agent_box[k][3]; y++)
                for (x = k_agent_box[k][0]; x < k_agent_box[k][2]; x++) n += MK(x, y) != 0;
            if (n > 200) sel |= 1 << k;
        }
        g_selected = sel;
    }
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
        if (!first && M[p + PED_TYPE] != 0 && PED_ON_MAP(p) && (h < g_health[i] || (dead && !g_dead[i])))
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
        if (PED_KIND(p) != 2 || !PED_ON_MAP(p) || colour == pal[HAT_COLOUR]) continue;
        world_to_frame((int16_t)RD16(p + PED_X), (int16_t)RD16(p + PED_Y), (int16_t)RD16(p + PED_Z), &fx, &fy);
        /* the head, below the number of an agent of the squad */
        for (y = fy - 31; y <= fy - 14; y++)
            for (x = fx - 8; x <= fx + 8; x++)
                if (x >= VIEW_LEFT && x < w && y >= GAME_TOP && y < GAME_TOP + 400 && pix[y * w + x] == HAT_COLOUR)
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

/* the player's company in a colour of the player's choice (0: the game's own) */
static volatile uint32_t g_player_colour;

/* ---- Configure Company: every colour and logo in two grids under the game's panel, one click each ----
 * The game's 8 company colours (as its preview draws them), then 18 more: one of these is shown in true colour
 * (the agents' hats), the game keeping the nearest of its own (the world map, its menus). */
#define NB_GAME_COLOURS 8
#define NB_PICK_COLOURS 26
#define NB_LOGOS 40
static const uint32_t k_pick_colours[NB_PICK_COLOURS] = {
    0xAA2400, 0x4D5979, 0xFF3C08, 0xFFA65D, 0xE3DB28, 0xDFFFFF, 0x967918, 0x008EBA, /* the game's */
    0xDC143C, 0xFF8C00, 0xFFD700, 0x9ACD32, 0x2ECC71, 0x00C896, 0x00E5FF, 0x5DADE2,
    0x2F4FD8, 0x5B2C9F, 0x9B59B6, 0xFF00C8, 0xFF8FB8, 0xF4F4F4, 0x8A9AA8, 0x303030,
    0x8B5A2B, 0x008080, /* brown, teal */
};
#define PLAYER_COLOUR_FILE "company_colour.txt"
static uint32_t g_config_us;      /* Configure Company was drawn then */
static uint32_t g_config_logos;   /* the game's logos (32 x 32, 254 the colour) */
static int g_config_logo, g_config_colour = -1; /* the game's, as it edits them */
static volatile int g_pick_logo = -1, g_pick_colour = -1;
static int g_player_pick;         /* the colour chosen (0..23) */

/* --colour (headless): the hats in `rgb`, and the menus too when it is one of the colours to choose from */
void game_set_player_colour(uint32_t rgb)
{
    int k;
    g_player_colour = rgb;
    for (k = 0; k < NB_PICK_COLOURS; k++)
        if (k_pick_colours[k] == (rgb & 0xFFFFFFu)) g_player_pick = k;
}

static void player_colour_save(void)
{
    char path[1200];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", rt_user_dir, PLAYER_COLOUR_FILE);
    f = fopen(path, "w");
    if (f == NULL) return;
    fprintf(f, "%d\n", g_player_pick);
    fclose(f);
}

static void player_colour_load(void)
{
    char path[1200];
    FILE *f;
    int k = -1;
    snprintf(path, sizeof path, "%s/%s", rt_user_dir, PLAYER_COLOUR_FILE);
    f = fopen(path, "r");
    if (f == NULL) return;
    if (fscanf(f, "%d", &k) == 1 && k >= NB_GAME_COLOURS && k < NB_PICK_COLOURS) {
        g_player_pick = k;
        g_player_colour = 0xFF000000u | k_pick_colours[k];
    }
    fclose(f);
}

/* the game's own colour nearest to colour k */
static int nearest_game_colour(int k)
{
    int i, best = 0;
    long bd = -1;
    for (i = 0; i < NB_GAME_COLOURS; i++) {
        long d = 0;
        int c;
        for (c = 0; c < 24; c += 8) {
            long e = (long)((k_pick_colours[k] >> c) & 0xFF) - (long)((k_pick_colours[i] >> c) & 0xFF);
            d += e * e;
        }
        if (bd < 0 || d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* the game draws a logo: on Configure Company, what it edits is read, and changed to a choice made in the grids */
static int on_draw_logo(uint32_t addr)
{
    uint32_t at, cat;
    (void)addr;
    if (RD32(R.esp) != g_addrs->logo_ret) return 0;
    at = R.esp + g_addrs->logo_local;
    cat = R.esp + g_addrs->colour_local;
    if (g_pick_logo >= 0) {
        M[at] = (uint8_t)g_pick_logo;
        WR32(R.esp + 4u, (uint32_t)g_pick_logo);
        g_pick_logo = -1;
    }
    if (g_pick_colour >= 0) {
        M[cat] = (uint8_t)nearest_game_colour(g_pick_colour);
        WR32(R.esp + 8u, M[cat]);
        g_player_pick = g_pick_colour;
        g_player_colour = g_pick_colour >= NB_GAME_COLOURS ? 0xFF000000u | k_pick_colours[g_pick_colour] : 0;
        player_colour_save();
        g_pick_colour = -1;
    }
    g_config_logo = M[at];
    g_config_colour = M[cat];
    g_config_logos = RD32(R.esp + 28u);
    g_config_us = (uint32_t)(host_time_us() / 1000u);
    return 0;
}

/* where the grids are on the 320 x 200 screen: each centred under the game's panel (x 140 to 306) */
#define PICK_BRIGHT 0x61FF24u
#define PANEL_MID 223
#define PICK_ROW (NB_PICK_COLOURS / 2) /* the colours in 2 rows */
#define PICK_X (PANEL_MID - (PICK_ROW * (SWATCH + 1) - 1) / 2)
#define PICK_Y 80
#define SWATCH 10
#define LOGO_CELL 15   /* a logo 14 x 14 and a gap: the 4 rows end above the MENU button (172) */
#define LOGO_SIZE 14
#define LOGOS_X (PANEL_MID - (10 * LOGO_CELL - 1) / 2)
#define LOGOS_Y (PICK_Y + 2 * (SWATCH + 1) + 3)

/* ---- the menus as wide as the window: the game's 320 x 200 picture in the middle of a wider one; on the green menus
 * (the desk photo and the frame round it, in all of them: its bright line, palette entry FRAME_INDEX, at columns 3
 * and 317, rows 2 and 197) the sides are the desk's dark green background, the desk's picture fading into it at the
 * game's edges, the frame round all of it.  While the game plays the desk's animation (the desk in colour, the
 * green wiping down over it, the frame drawn) each side is as the game's row beside it is: black beside the
 * coloured desk.  menu_sides.h: the desk's palette entries ---- */
#define FRAME_INDEX 252
static int g_menu_w = 320, g_menu_off; /* the menu's picture, and where the game's is in it */
static volatile int g_desk; /* the game plays MSCRENUP.DAT: the desk in colour, then the green taking it over */

/* the menus' animations (Syndicate's DATA folder): MSCRENUP.DAT the desk's, any other another screen's */
void game_file_opened(const char *path)
{
    static const char *const k_screens[] = {"MBRIEF", "MBRIEOUT", "MCONFUP", "MCONFOUT", "MDEBRIEF", "MDEOUT", "MMAP",
                                            "MMAPOUT", "MOPTION", "MOPTOUT", "MRESRCH", "MRESOUT", "MSELECT", "MSELOUT",
                                            "MLOSA", "MLOSAOUT", "MMULTI", "MMULTOUT", "MTITLE", "MENDWIN", "MENDLOSE",
                                            "MGAMEWIN", "MLOSEGAM"};
    const char *name = path;
    char base[16];
    size_t k, n;
    for (k = 0; path[k]; k++)
        if (path[k] == '/' || path[k] == '\\' || path[k] == ':') name = path + k + 1;
    for (n = 0; name[n] && name[n] != '.' && n < sizeof base - 1; n++) base[n] = (char)toupper((unsigned char)name[n]);
    base[n] = 0;
    if (strcmp(base, "MSCRENUP") == 0) g_desk = 1;
    for (k = 0; k < sizeof k_screens / sizeof k_screens[0]; k++)
        if (strcmp(base, k_screens[k]) == 0) g_desk = 0;
}

/* how bright the palette has the desk's entries (`ref`, their colours 0..63), each channel (1: as bright as those
 * are; less while the game fades a menu) */
static void palette_bright(const uint32_t *pal, const unsigned char (*ref)[4], int n, float bright[3])
{
    int c, k;
    for (c = 0; c < 3; c++) {
        uint32_t now = 0, was = 0;
        for (k = 0; k < n; k++) {
            uint32_t v = ref[k][1 + c];
            now += (pal[ref[k][0]] >> (16 - 8 * c)) & 255u;
            was += v << 2 | v >> 4;
        }
        bright[c] = was == 0 || now * 100u >= was * 98u ? 1.0f : (float)now / (float)was;
    }
}

/* the outpainted sides (menu_backdrop.h, JPEG) to the platform, which draws them at the display's resolution: 1
 * when it does */
static int side_images(void)
{
    uint32_t *px[4];
    int k, w = 0, h = 0, ok = 1, on;
    for (k = 0; k < 4; k++) {
        int n, i;
        unsigned char *d = stbi_load_from_memory(k_menu_backdrop[k], (int)k_menu_backdrop_size[k], &w, &h, &n, 4);
        px[k] = d != NULL ? (uint32_t *)malloc(sizeof(uint32_t) * (size_t)w * (size_t)h) : NULL;
        if (px[k] == NULL)
            ok = 0;
        else
            for (i = 0; i < w * h; i++)
                px[k][i] = (uint32_t)d[i * 4] << 16 | (uint32_t)d[i * 4 + 1] << 8 | d[i * 4 + 2];
        stbi_image_free(d);
    }
    on = ok && host_menu_side_images((const uint32_t *const *)px, w, h);
    for (k = 0; k < 4; k++) free(px[k]);
    return on;
}

/* the desk in colour between x0 and x1 of a row (its entries more than the green one's) */
static int colour_part(const uint8_t *row, int x0, int x1)
{
    int x, colour = 0, green = 0;
    for (x = x0; x < x1; x++) {
        uint8_t i = row[x];
        if (k_menu_colour_set[i >> 3] & (1u << (i & 7))) colour++;
        else green++;
    }
    return colour > green;
}

/* the mouse on the game's 320 x 200 picture (left of it, right of it: past its edges) */
static void menu_mouse(int *x, int *y)
{
    int fx, fy;
    mouse_fraction(&fx, &fy);
    *x = (int)((long long)fx * g_menu_w / 65536) - g_menu_off;
    *y = (int)((long long)fy * 200 / 65536);
}

/* a green menu: the frame's bright line where it is in all of them */
static int menu_framed(const uint8_t *index)
{
    int y, x, n = 0;
    for (y = 4; y < 196; y++) n += index[y * 320 + 3] == FRAME_INDEX && index[y * 320 + 317] == FRAME_INDEX;
    for (x = 4; x < 316; x++) n += index[2 * 320 + x] == FRAME_INDEX && index[197 * 320 + x] == FRAME_INDEX;
    return n >= (192 + 312) * 9 / 10;
}

#define VIGNETTE 18 /* the game's columns over which the desk's picture fades into the sides (within the
                     * outpainted sides' overlap under it) */
#define SEE_THROUGH 0xFF000000u /* a pixel of the frame the platform's drawing under it shows through */

/* the green desk's darkest palette entry (its background) */
static int darkest_photo(void)
{
    int k, best = 0, bl = 1 << 30;
    for (k = 0; k < MENU_PHOTO_N; k++) {
        int l = k_menu_photo[k][1] * 3 + k_menu_photo[k][2] * 6 + k_menu_photo[k][3];
        if (l < bl) bl = l, best = k;
    }
    return k_menu_photo[best][0];
}

/* `a` of `b` (0..256) and the rest of `c` */
static uint32_t blend(uint32_t c, uint32_t b, int a)
{
    uint32_t out = 0;
    int k;
    for (k = 0; k < 24; k += 8)
        out |= (uint32_t)((((b >> k) & 255u) * (uint32_t)a + ((c >> k) & 255u) * (uint32_t)(256 - a)) >> 8) << k;
    return out;
}

/* `game`, the game's picture (320 x 200, what this version draws on it included), into the menu's `rgb` (w wide) */
static void wide_menu(const uint8_t *index, const uint32_t *pal, const uint32_t *game, uint32_t *rgb, int w)
{
    static uint8_t green_set[32];
    static int hardware = -1; /* the platform draws the outpainted sides */
    int off = (w - 320) / 2, x, y, framed = menu_framed(index), desk = g_desk;
    uint32_t dark = pal[darkest_photo()] & 0xFFFFFFu;
    HostMenuSides sides;
    if (green_set[k_menu_photo[0][0] >> 3] == 0)
        for (x = 0; x < MENU_PHOTO_N; x++) green_set[k_menu_photo[x][0] >> 3] |= (uint8_t)(1u << (k_menu_photo[x][0] & 7));
    if (hardware < 0) hardware = side_images();
    memset(&sides, 0, sizeof sides);
    sides.on = hardware && (framed || desk);
    sides.x0[0] = off - MENU_SIDE_W;
    sides.x1[0] = off + MENU_BACKDROP_OVERLAP;
    sides.x0[1] = off + 320 - MENU_BACKDROP_OVERLAP;
    sides.x1[1] = off + 320 + MENU_SIDE_W;
    palette_bright(pal, k_menu_colour, MENU_COLOUR_N, sides.bright[0]);
    palette_bright(pal, k_menu_photo, MENU_PHOTO_N, sides.bright[1]);
    for (y = 0; y < 200; y++) {
        uint32_t *row = rgb + y * w;
        const uint32_t *g = game + y * 320;
        const uint8_t *ix = index + y * 320;
        int line = (y >= 1 && y <= 3) || (y >= 196 && y <= 198); /* a row of the frame's top or bottom */
        /* each side as the game's picture is beside it (the green wipes over the coloured desk) */
        int left_colour = desk && colour_part(ix, 6, 22), right_colour = desk && colour_part(ix, 298, 314);
        /* the sides: the outpainted desk under them (see-through), else the desk's background colour */
        uint32_t lbg = sides.on ? SEE_THROUGH : left_colour ? 0 : dark;
        uint32_t rbg = sides.on ? SEE_THROUGH : right_colour ? 0 : dark;
        const uint8_t *lset = left_colour ? k_menu_colour_set : green_set;
        const uint8_t *rset = right_colour ? k_menu_colour_set : green_set;
        if (!left_colour) sides.green_rows[0][y >> 5] |= 1u << (y & 31);
        if (!right_colour) sides.green_rows[1][y >> 5] |= 1u << (y & 31);
        for (x = 0; x < w; x++) row[x] = 0;
        memcpy(row + off, g, sizeof *g * 320);
        if (!framed && !desk) continue;
        /* the game's frame at its sides too (in green its dark lines are there before the bright one is drawn), but
         * for the top and bottom lines going on */
        for (x = 0; x < off; x++) {
            row[x] = lbg;
            row[off + 320 + x] = rbg;
        }
        for (x = 0; x < 6; x++) {
            row[off + x] = framed && line ? g[8] : lbg;
            row[off + 314 + x] = framed && line ? g[311] : rbg;
        }
        /* the desk's own picture fading into the sides (its pixels only, not what is drawn over it) */
        for (x = 0; x < VIGNETTE; x++) {
            int a = 256 * (VIGNETTE - x) / (VIGNETTE + 1), l = 6 + x, r = 313 - x;
            uint32_t see = (uint32_t)(a * 255 / 256) << 24;
            if (lset[ix[l] >> 3] & (1u << (ix[l] & 7)))
                row[off + l] = sides.on ? (row[off + l] & 0xFFFFFFu) | see : blend(row[off + l], lbg, a);
            if (rset[ix[r] >> 3] & (1u << (ix[r] & 7)))
                row[off + r] = sides.on ? (row[off + r] & 0xFFFFFFu) | see : blend(row[off + r], rbg, a);
        }
        if (!framed) continue; /* the desk before its frame is drawn */
        if (line)
            for (x = 2; x < off + 6; x++) {
                row[x] = g[8];
                row[w - 1 - x] = g[311];
            }
        /* and its sides at the picture's */
        if (y >= 1 && y <= 198)
            for (x = 2; x < 5; x++) {
                row[x] = g[x];
                row[w - 320 + 314 + x] = g[314 + x];
            }
    }
    host_menu_sides(&sides);
}

/* the pointer on a wide menu: the game draws it into its picture, which ends at the game's edges, so with the mouse
 * on a side it stays at the edge, cut by it.  The picture without it is kept (away from it): what differs near the
 * game's pointer is the pointer, its shape learnt while it is well inside the picture; past an edge the picture is
 * put back under the game's and the shape drawn where the mouse is */
#define POINTER_MAX 1200
typedef struct PointerPx {
    int16_t x, y;
    uint32_t c;
} PointerPx;
static PointerPx g_pointer[POINTER_MAX]; /* its shape, from the mouse's point */
static int g_pointer_n, g_pointer_x, g_pointer_y; /* and where it is drawn (the game's columns: past them on a side) */

/* 1 when the pointer is to be drawn on a side (pointer_draw); `out`: the game's picture (index) without its pointer
 * then */
static int pointer_past_edge(const uint8_t *index, const uint32_t *pal, uint32_t *game, uint8_t *out)
{
    static uint8_t ref[320 * 200];
    static PointerPx seen[POINTER_MAX];
    int mx, my, gx, gy, x, y, n = 0;
    menu_mouse(&mx, &my);
    gx = mx < 0 ? 0 : mx > 319 ? 319 : mx;
    gy = my < 0 ? 0 : my > 199 ? 199 : my;
    memcpy(out, index, 320 * 200);
    for (y = 0; y < 200; y++)
        for (x = 0; x < 320; x++) {
            int near = x >= gx - 24 && x < gx + 24 && y >= gy - 24 && y < gy + 28;
            int i = y * 320 + x;
            if (!near) {
                ref[i] = index[i];
            } else if (index[i] != ref[i] && n < POINTER_MAX) {
                seen[n].x = (int16_t)(x - gx);
                seen[n].y = (int16_t)(y - gy);
                seen[n].c = pal[index[i]] & 0xFFFFFFu;
                n++;
                if (mx != gx) { /* the game's, at the edge, away */
                    game[i] = pal[ref[i]] & 0xFFFFFFu;
                    out[i] = ref[i];
                }
            }
        }
    if (n > 0 && gx >= 24 && gx < 296 && gy >= 24 && gy < 172) {
        memcpy(g_pointer, seen, sizeof seen[0] * (size_t)n);
        g_pointer_n = n;
    }
    g_pointer_x = mx;
    g_pointer_y = gy;
    return mx != gx && g_pointer_n > 0;
}

static void pointer_draw(uint32_t *rgb, int w, int on)
{
    int k;
    for (k = 0; on && k < g_pointer_n; k++) {
        int x = g_menu_off + g_pointer_x + g_pointer[k].x, y = g_pointer_y + g_pointer[k].y;
        if (x >= 0 && x < w && y >= 0 && y < 200) rgb[y * w + x] = g_pointer[k].c;
    }
}

static int config_shown(void)
{
    return g_config_logos != 0 && (uint32_t)(host_time_us() / 1000u) - g_config_us < 300u;
}

/* a click on Configure Company: in a grid, the choice */
static void config_click(void)
{
    int x, y;
    menu_mouse(&x, &y);
    if (y >= PICK_Y && y < PICK_Y + 2 * (SWATCH + 1) && x >= PICK_X) {
        int k = (y - PICK_Y) / (SWATCH + 1) * PICK_ROW + (x - PICK_X) / (SWATCH + 1);
        if ((x - PICK_X) / (SWATCH + 1) < PICK_ROW && k < NB_PICK_COLOURS) g_pick_colour = k;
    } else if (y >= LOGOS_Y && y < LOGOS_Y + 4 * LOGO_CELL && x >= LOGOS_X) {
        int k = (y - LOGOS_Y) / LOGO_CELL * 10 + (x - LOGOS_X) / LOGO_CELL;
        if ((x - LOGOS_X) / LOGO_CELL < 10 && k < NB_LOGOS) g_pick_logo = k;
    }
}

static void frame_box(uint32_t *rgb, int w, int h, int x0, int y0, int x1, int y1, uint32_t c)
{
    int x, y;
    for (x = x0; x < x1; x++)
        for (y = y0; y < y1; y += (y == y0 ? y1 - y0 - 1 : 1)) {
            if (x >= 0 && x < w && y >= 0 && y < h) rgb[y * w + x] = c;
            if (y1 - y0 <= 1) break;
        }
    for (y = y0; y < y1; y++) {
        if (x0 >= 0 && x0 < w && y >= 0 && y < h) rgb[y * w + x0] = c;
        if (x1 - 1 >= 0 && x1 - 1 < w && y >= 0 && y < h) rgb[y * w + x1 - 1] = c;
    }
}

/* the player's company's colour as the game has it (its palette entry) */
static int player_colour_index(void)
{
    uint32_t company = g_addrs->companies + (uint32_t)(int16_t)RD16(g_addrs->player) * g_addrs->company_size;
    return M[g_addrs->colours + (uint32_t)(M[company + COMPANY_COLOUR] & 7u)];
}

/* the menus in true colour: what the game draws in the player's company colour (its logo on the world map and the
 * team screens, its countries, the logos of Configure Company: the palette entry of the game's colour nearest the
 * one chosen) is in the colour chosen.  Only where that entry holds that colour: the videos and other pictures
 * have their own colours there */
static void menu_true_colour(const uint8_t *index, const uint32_t *pal, uint32_t *rgb, int w, int h)
{
    int i, game, idx;
    uint32_t c, company;
    if (g_player_pick < NB_GAME_COLOURS) return;
    company = g_addrs->companies + (uint32_t)(int16_t)RD16(g_addrs->player) * g_addrs->company_size;
    game = config_shown() && g_config_colour >= 0 ? g_config_colour : (int)(M[company + COMPANY_COLOUR] & 7u);
    idx = M[g_addrs->colours + (uint32_t)game];
    if ((pal[idx] & 0xFFFFFFu) != k_pick_colours[game]) return;
    c = k_pick_colours[g_player_pick];
    for (i = 0; i < w * h; i++)
        if (index[i] == idx) rgb[i] = c;
}

/* the pointer over the grids: the game draws it into its picture, under them; the picture as it is without the
 * pointer is kept (away from it), so what differs near it is the pointer, drawn again on top */
static void config_pointer(uint32_t *rgb, int w, int h, const uint8_t *index, const uint32_t *pal)
{
    static uint8_t ref[320 * 200];
    static uint32_t ref_us;
    int mx, my, x, y;
    uint32_t now = (uint32_t)(host_time_us() / 1000u);
    menu_mouse(&mx, &my);
    if (now - ref_us > 500u) memcpy(ref, index, sizeof ref); /* the screen came again */
    ref_us = now;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int near = x >= mx - 24 && x < mx + 24 && y >= my - 24 && y < my + 28;
            if (!near)
                ref[y * w + x] = index[y * w + x];
            else if (index[y * w + x] != ref[y * w + x])
                rgb[y * w + x] = pal[index[y * w + x]];
        }
}

/* the grids: the colours, then the logos in the colour chosen (each half size), the choices outlined */
static void config_grids(uint32_t *rgb, int w, int h, const uint8_t *index, const uint32_t *pal)
{
    int k, x, y, colour = g_player_pick >= NB_GAME_COLOURS ? g_player_pick : g_config_colour;
    uint32_t c;
    if (colour < 0 || colour >= NB_PICK_COLOURS) colour = 0;
    c = k_pick_colours[colour];
    for (k = 0; k < NB_PICK_COLOURS; k++) {
        int x0 = PICK_X + (k % PICK_ROW) * (SWATCH + 1), y0 = PICK_Y + (k / PICK_ROW) * (SWATCH + 1);
        for (y = y0; y < y0 + SWATCH; y++)
            for (x = x0; x < x0 + SWATCH; x++) rgb[y * w + x] = k_pick_colours[k];
        frame_box(rgb, w, h, x0, y0, x0 + SWATCH, y0 + SWATCH, k == colour ? PICK_BRIGHT : 0x082800u);
    }
    for (k = 0; k < NB_LOGOS; k++) {
        int x0 = LOGOS_X + (k % 10) * LOGO_CELL, y0 = LOGOS_Y + (k / 10) * LOGO_CELL;
        const uint8_t *l = M + g_config_logos + (uint32_t)k * 1024u;
        for (y = 0; y < LOGO_SIZE; y++)
            for (x = 0; x < LOGO_SIZE; x++) {
                uint8_t v = l[((y * 32 + 16) / LOGO_SIZE) * 32 + (x * 32 + 16) / LOGO_SIZE];
                rgb[(y0 + y) * w + x0 + x] = v == 254 ? c : 0;
            }
        if (k == g_config_logo) frame_box(rgb, w, h, x0 - 1, y0 - 1, x0 + LOGO_SIZE + 1, y0 + LOGO_SIZE + 1, PICK_BRIGHT);
    }
    config_pointer(rgb, w, h, index, pal);
}

/* the company an agent of the mission belongs to: the one whose agents (from the first, COMPANY_SQUAD) it is among */
static int agent_company(int ped)
{
    int c;
    for (c = 0; c < 8; c++) {
        int first = M[g_addrs->companies + (uint32_t)c * g_addrs->company_size + COMPANY_SQUAD];
        if (ped >= first && ped < first + AGENTS_A_COMPANY) return c;
    }
    return ped / AGENTS_A_COMPANY < 8 ? ped / AGENTS_A_COMPANY : -1;
}

static uint32_t hat_colour(int ped)
{
    uint32_t company, c;
    int owner;
    if (ped < 0 || ped >= NB_PEDS || PED_KIND(g_addrs->peds + (uint32_t)ped * PED_SIZE) != 2) return 0;
    owner = agent_company(ped);
    if (owner < 0) return 0;
    if (g_player_colour != 0 && owner == (int16_t)RD16(g_addrs->player)) return g_player_colour & 0xFFFFFFu;
    company = g_addrs->companies + (uint32_t)owner * g_addrs->company_size;
    c = company_rgb(company, g_overlay_pal);
    return c == g_overlay_pal[HAT_COLOUR] ? 0 : c;
}

/* the pointer table's current entry (HPOINTER: 6 bytes, pointer to the pixels, width, height): from the pointer
 * to HSPR's table, +0x10 in Syndicate (0x445324), +0x14 in American Revolt */
#define POINTER_CURRENT(ar) ((ar) ? 0x14u : 0x10u)
/* the numbers over the squad's agents: the game's own routine (0x40dcb0 in Syndicate) draws, for each agent of the
 * squad shown (+0x1d bit 2 set, +0xb bit 0 clear), the frame it keeps for it in label_frames (the selected agent's
 * goes on to the next frame now and then: its number blinks), 16 pixels above where the agent stands; the frame
 * of an object is drawn render_obj_dy (56) below that point */
#define PED_SHOWN 0x1du
#define PED_SHOWN_BIT 4u
#define PED_GONE 0xbu
#define LABEL_DY (-16 + GAME_TOP - 56)

static int g_cut_level = -1;
#define FLOOR_OUTLINE 0x5AFF5Au

/* B held (as FreeSynd's): the tiles of the level the highest agent of the squad is in (z / 128) and above are drawn
 * almost see-through, and where one can walk at that level is outlined in green */
#define SEE_INSIDE_LEVELS 0

static void draw_pointer(uint32_t *rgb, const uint32_t *pal)
{
    int mx, my;
    uint32_t entry = RD32(g_addrs->data + POINTER_CURRENT(g_addrs->ar));
    canvas_mouse(&mx, &my);
    if (entry != 0)
        render_image(rgb, g_cw, 480, pal, RD32(entry), M[entry + 4u], M[entry + 5u], mx - 16, my - 16, VIEW_LEFT, VIEW_TOP,
                     g_cw, VIEW_BOTTOM);
}

/* the city on the playback clock (render.c), with the pointer and the squad's numbers over it */
static void smooth_city(const uint8_t *index, const uint32_t *pal, uint32_t *rgb)
{
    uint32_t company;
    int x, y, slot, gpu;
    g_overlay_pal = pal;
    render_hat_colour = hat_colour;
    move_view();
    /* B held: the buildings from the squad's level up see-through */
    {
        int level = -1;
        if (g_see_inside) {
            uint32_t co = g_addrs->companies + (uint32_t)(int16_t)RD16(g_addrs->player) * g_addrs->company_size;
            for (slot = 0; slot < 4; slot++) {
                uint32_t ped = g_addrs->peds + (M[co + COMPANY_SQUAD] + (uint32_t)slot) * PED_SIZE;
                int l = (int16_t)RD16(ped + PED_Z) / 128 + SEE_INSIDE_LEVELS;
                /* not a dead agent's body (the game keeps it on the map, its health below 1) */
                if (PED_ON_MAP(ped) && (int16_t)RD16(ped + PED_HEALTH) > 0 && l > level) level = l;
            }
            if (level < 0) level = 0;
        }
        render_set_cut(level);
        g_cut_level = level;
    }
    /* on the GPU the map's part of the frame is see-through, the city drawn under it at the display's resolution */
    gpu = host_city_gpu();
    for (y = VIEW_TOP; y < VIEW_BOTTOM; y++)
        for (x = VIEW_LEFT; x < g_cw; x++) rgb[y * g_cw + x] = gpu ? 0xFF000000u : 0;
    render_city(gpu ? NULL : rgb, g_cw, 480, pal, VIEW_LEFT, VIEW_TOP, g_cw, VIEW_BOTTOM, rt_display_us);
    /* B held: where the floor the squad walks on ends */
    if (g_cut_level >= 0)
        render_floor_outline(rgb, g_cw, 480, g_cut_level, FLOOR_OUTLINE, VIEW_LEFT, VIEW_TOP, g_cw, VIEW_BOTTOM, path_walkable);
    /* the pointer: the game's (the entry of its pointer table it uses), drawn where the mouse is now, as the game
     * draws it (its top left corner 16 pixels up and left of the mouse; the game's mouse runs over its 400 lines
     * from line 40 of the frame); over the side panel and the bars the game's own picture shows it */
    draw_pointer(rgb, pal);
    /* the numbers over the agents of the squad: the frames the game shows, over each agent where the smooth city
     * has it */
    company = g_addrs->companies + (uint32_t)(int16_t)RD16(g_addrs->player) * g_addrs->company_size;
    for (slot = 0; slot < 4; slot++) {
        uint32_t ped = g_addrs->peds + (M[company + COMPANY_SQUAD] + (uint32_t)slot) * PED_SIZE;
        int frame = RD16(g_addrs->label_frames + 2u * (uint32_t)slot);
        if (!(M[ped + PED_SHOWN] & PED_SHOWN_BIT) || (M[ped + PED_GONE] & 1u) || frame == 0) continue;
        render_ped_frame(rgb, g_cw, 480, pal, frame, (int)M[company + COMPANY_SQUAD] + slot, LABEL_DY, VIEW_LEFT, VIEW_TOP,
                         g_cw, VIEW_BOTTOM);
    }
}

/* ---- the light editor's panel: a small font of its own (3 x 5 pixels, drawn twice the size) ---- */
static const char *const k_font_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:-+[]./,_()";
static const uint16_t k_font[] = {
    /* A-Z: 15 bits, 5 rows of 3 from the top, bit 0 the top left */
    0x5BEA, 0x7AEB, 0x624E, 0x3B6B, 0x73CF, 0x13CF, 0x6B4E, 0x5BED, 0x7497, 0x2B24, 0x5AED, 0x7249, 0x5B7D, 0x5B6B,
    0x2B6A, 0x13EB, 0x4F6A, 0x5AEB, 0x388E, 0x2497, 0x7B6D, 0x2B6D, 0x5F6D, 0x5AAD, 0x24AD, 0x72A7,
    /* 0-9 */
    0x7B6F, 0x749A, 0x73E7, 0x79E7, 0x49ED, 0x79CF, 0x7BCF, 0x4927, 0x7BEF, 0x79EF,
    /* : - + [ ] . / , _ ( ) */
    0x0410, 0x01C0, 0x05D0, 0x6496, 0x3493, 0x2000, 0x12A4, 0x1400, 0x7000, 0x4494, 0x1491,
};

static void draw_text(uint32_t *rgb, int w, int x, int y, const char *t, uint32_t c)
{
    for (; *t; t++, x += 8) {
        const char *k = strchr(k_font_chars, *t >= 'a' && *t <= 'z' ? *t - 32 : *t);
        int gx, gy, bits;
        if (*t == ' ' || k == NULL) continue;
        bits = k_font[k - k_font_chars];
        for (gy = 0; gy < 5; gy++)
            for (gx = 0; gx < 3; gx++)
                if (bits & (1 << (gy * 3 + gx))) {
                    int px = x + gx * 2, py = y + gy * 2, a, b;
                    for (b = 0; b < 2; b++)
                        for (a = 0; a < 2; a++)
                            if (px + a >= 0 && px + a < w && py + b >= 0 && py + b < 480) rgb[(py + b) * w + px + a] = c;
                }
    }
}

static void fill_box(uint32_t *rgb, int w, int x0, int y0, int x1, int y1, uint32_t c, int alpha)
{
    int x, y;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) plot(rgb, w, x, y, c, alpha);
}

static void outline_box(uint32_t *rgb, int w, int x0, int y0, int x1, int y1, uint32_t c)
{
    int x, y;
    for (x = x0; x < x1; x++) {
        plot(rgb, w, x, y0, c, 255);
        plot(rgb, w, x, y1 - 1, c, 255);
    }
    for (y = y0; y < y1; y++) {
        plot(rgb, w, x0, y, c, 255);
        plot(rgb, w, x1 - 1, y, c, 255);
    }
}

#define EDITOR_TEXT 0xFFE070u
static const uint32_t k_edit_colours[] = {0, 0xFFFFE6B4u, 0xFFFFFFFFu, 0xFFFF4030u, 0xFFFFA030u, 0xFF50FF60u, 0xFF50E0FFu,
                                          0xFF6080FFu};
static const char *const k_edit_colour_names[] = {"AUTO", "WARM", "WHITE", "RED", "AMBER", "GREEN", "CYAN", "BLUE"};

/* the tile under the pointer: outlined, its settings shown, the keys pressed applied to it */
static void light_editor(uint32_t *rgb, int w)
{
    static const char *const glow[3] = {"NONE", "BRIGHT", "FULL"}, *const light[3] = {"AUTO", "NONE", "ON"};
    int mx, my, id = -1, rx = 0, ry = 0, rw = 0, rh = 0, key = g_edit_key, k, c = 0;
    char line[96];
    RenderTileSetting set;
    g_edit_key = 0;
    canvas_mouse(&mx, &my);
    if (mx >= VIEW_LEFT && my >= VIEW_TOP && my < VIEW_BOTTOM) render_tile_at(mx, my, &id, &rx, &ry, &rw, &rh);
    fill_box(rgb, w, VIEW_LEFT + 4, VIEW_TOP + 4, VIEW_LEFT + 4 + 8 * 62, VIEW_TOP + 4 + 62, 0, 190);
    draw_text(rgb, w, VIEW_LEFT + 10, VIEW_TOP + 10, "LIGHT EDITOR (CTRL+L CLOSES). DUSK OR NIGHT SHOWS IT.", EDITOR_TEXT);
    if (id < 0) {
        draw_text(rgb, w, VIEW_LEFT + 10, VIEW_TOP + 26, "POINT AT A TILE OF THE CITY.", 0xFFFFFF);
        return;
    }
    render_tile_setting(id, &set);
    if (key != 0) {
        if (key == SC_G) set.glow = (set.glow + 1) % 3;
        if (key == SC_L) set.light = (set.light + 1) % 3;
        if (key == SC_LBRACKET) set.radius = set.radius >= 10 ? set.radius - 10 : 0;
        if (key == SC_RBRACKET) set.radius = set.radius + 10 > 250 ? 250 : set.radius + 10;
        if (key == SC_C) {
            for (k = 0; k < (int)(sizeof k_edit_colours / sizeof k_edit_colours[0]); k++)
                if (k_edit_colours[k] == set.colour) c = k;
            set.colour = k_edit_colours[(c + 1) % (sizeof k_edit_colours / sizeof k_edit_colours[0])];
        }
        set.set = 1;
        render_set_tile_setting(id, &set);
    }
    for (k = 0, c = -1; k < (int)(sizeof k_edit_colours / sizeof k_edit_colours[0]); k++)
        if (k_edit_colours[k] == set.colour) c = k;
    outline_box(rgb, w, rx, ry, rx + rw, ry + rh, EDITOR_TEXT);
    snprintf(line, sizeof line, "TILE %d.  G GLOW: %s.  L LIGHT: %s.", id, glow[set.glow % 3], light[set.light % 3]);
    draw_text(rgb, w, VIEW_LEFT + 10, VIEW_TOP + 26, line, 0xFFFFFF);
    if (set.radius)
        snprintf(line, sizeof line, "[ ] RADIUS: %d.  C COLOUR: %s.", set.radius, c >= 0 ? k_edit_colour_names[c] : "SET");
    else
        snprintf(line, sizeof line, "[ ] RADIUS: AUTO.  C COLOUR: %s.", c >= 0 ? k_edit_colour_names[c] : "SET");
    draw_text(rgb, w, VIEW_LEFT + 10, VIEW_TOP + 42, line, 0xFFFFFF);
}

/* ---- the mission's menu (Escape): the game's briefing look, its menu font (MFNT-0: each character a few rows
 * of runs: a count of pixels that follow, a negative one of see-through ones, 0 the end of a row), twice its size,
 * its greens ---- */
#define MENU_W 500
#define MENU_ROW 30
#define MENU_SCALE 2
#define MENU_BG 0x082800u
#define MENU_BAR 0x144904u
#define MENU_BAR_SEL 0x1E6A0Au
#define MENU_FRAME 0x1C9200u
#define MENU_TEXT 0x1C9200u
#define MENU_BRIGHT 0x61FF24u
#define MENU_SEL_TEXT 0xC8FFB4u
#define MENU_SHADE 0x041800u
#define NB_GLYPHS 205
#define MAX_OPTIONS 16

typedef struct Glyph {
    uint8_t w, h;
    uint8_t px[16][16]; /* 0 see-through, 1 the edge, 2 the letter */
} Glyph;
static Glyph g_font[NB_GLYPHS];
static int g_font_loaded;

static void load_menu_font(void)
{
    char path[1200];
    static uint8_t dat[16384], tab[2048];
    size_t nd, nt;
    VFile *f;
    int i;
    g_font_loaded = 1;
    snprintf(path, sizeof path, "%s/%s/DATA/MFNT-0.DAT", rt_game_dir, g_addrs->ar ? "DATADISK" : "SYNDICAT");
    f = vfs_open(path, "rb");
    if (f == NULL) return;
    nd = vfs_read(f, dat, sizeof dat);
    vfs_close(f);
    snprintf(path, sizeof path, "%s/%s/DATA/MFNT-0.TAB", rt_game_dir, g_addrs->ar ? "DATADISK" : "SYNDICAT");
    f = vfs_open(path, "rb");
    if (f == NULL) return;
    nt = vfs_read(f, tab, sizeof tab);
    vfs_close(f);
    for (i = 0; i < NB_GLYPHS && (size_t)(i * 6 + 6) <= nt; i++) {
        uint32_t at = (uint32_t)tab[i * 6] | (uint32_t)tab[i * 6 + 1] << 8 | (uint32_t)tab[i * 6 + 2] << 16 |
                      (uint32_t)tab[i * 6 + 3] << 24;
        Glyph *g = &g_font[i];
        int y;
        g->w = tab[i * 6 + 4] > 16 ? 16 : tab[i * 6 + 4];
        g->h = tab[i * 6 + 5] > 16 ? 16 : tab[i * 6 + 5];
        for (y = 0; y < g->h; y++) {
            int x = 0;
            for (;;) {
                int n;
                if (at >= nd) return;
                n = (int8_t)dat[at++];
                if (n == 0) break;
                if (n < 0) {
                    x -= n;
                    continue;
                }
                while (n-- > 0 && at < nd) {
                    uint8_t c = dat[at++];
                    if (x < 16) g->px[y][x] = (uint8_t)(c >= 0xFD ? 2 : c != 0 ? 1 : 0);
                    x++;
                }
            }
        }
    }
}

static int menu_text_width(const char *t)
{
    int w = 0;
    for (; *t; t++) {
        int i = (uint8_t)*t - 32;
        w += (i > 0 && i < NB_GLYPHS && g_font[i].w > 0 ? g_font[i].w : 4) * MENU_SCALE;
    }
    return w;
}

static void menu_text_scaled(uint32_t *rgb, int w, int x, int y, const char *t, uint32_t body, int scale);

static void menu_text(uint32_t *rgb, int w, int x, int y, const char *t, uint32_t body)
{
    menu_text_scaled(rgb, w, x, y, t, body, MENU_SCALE);
}

static int text_width_scaled(const char *t, int scale)
{
    return menu_text_width(t) / MENU_SCALE * scale;
}

static void menu_text_scaled(uint32_t *rgb, int w, int x, int y, const char *t, uint32_t body, int scale)
{
    for (; *t; t++) {
        int i = (uint8_t)(*t >= 'a' && *t <= 'z' ? *t - 32 : *t) - 32, gx, gy, a, b;
        const Glyph *g;
        if (i <= 0 || i >= NB_GLYPHS || g_font[i].w == 0) {
            x += 4 * scale;
            continue;
        }
        g = &g_font[i];
        for (gy = 0; gy < g->h; gy++)
            for (gx = 0; gx < g->w; gx++) {
                uint8_t v = g->px[gy][gx];
                if (v == 0) continue;
                for (b = 0; b < scale; b++)
                    for (a = 0; a < scale; a++)
                        plot_any(rgb, w, x + gx * scale + a, y + gy * scale + b, v == 2 ? body : MENU_SHADE);
            }
        x += g->w * scale;
    }
}

static int g_menu_sel;
static int g_menu_mx, g_menu_my; /* where the pointer was: it chooses an item when it moves */
static volatile int g_menu_click, g_menu_buttons, g_menu_hold;
static int g_abort;

int game_mouse_buttons(int buttons)
{
    int pressed = (buttons & 1) && !(g_menu_buttons & 1);
    g_menu_buttons = buttons;
    if (pressed && g_addrs != NULL && g_view_on) canvas_mouse(&g_click_sx, &g_click_sy);
    if (pressed && g_addrs != NULL && !g_menu && in_mission()) {
        /* a double click on the city: the selected agents' speed drug to the top */
        static uint64_t last_us;
        static int last_x, last_y;
        uint64_t now = host_time_us();
        int x, y;
        canvas_mouse(&x, &y);
        if (x >= MAP_LEFT && last_us != 0 && now - last_us < DOUBLE_CLICK_US && abs(x - last_x) < 8 &&
            abs(y - last_y) < 8) {
            g_boost = 1;
            last_us = 0;
        } else {
            last_us = x >= MAP_LEFT ? now : 0;
            last_x = x;
            last_y = y;
        }
    }
    if (pressed && g_addrs != NULL && config_shown()) config_click();
    if (g_pick_active) {
        if (pressed) g_pick_click = 1;
        return 1;
    }
    if (g_menu) {
        if (pressed) g_menu_click = 1;
        return 1;
    }
    if (g_menu_hold) {
        if (buttons == 0) g_menu_hold = 0;
        return 1;
    }
    return 0;
}

static void menu_close(void)
{
    g_menu = 0;
    g_menu_hold = g_menu_buttons != 0; /* the click that closed it is not the game's */
}

/* ---- Syndicate or American Revolt, chosen before the main menu (game.c): the CD launcher's two box covers
 * (SELECT.DAT of the CD's DATA folder: 320 x 200, its palette after it, 0..63) on the green menu ---- */
#define COVER_W 65
#define COVER_H 90
#define COVER_Y 63          /* in SELECT.DAT, at x 86 and 160 */
#define PICK_COVER_Y 46     /* on the screen, either side of its middle */
#define PICK_COVER_GAP 30
static uint32_t g_covers[2][COVER_H][COVER_W];
static int g_covers_loaded, g_pick_sel = -1, g_pick_mx = -1, g_pick_my = -1;

static void load_covers(void)
{
    static const int k_x[2] = {86, 160};
    static uint8_t d[64768];
    char path[1200];
    VFile *f;
    int k, x, y;
    g_covers_loaded = 1;
    snprintf(path, sizeof path, "%s/DATA/SELECT.DAT", rt_game_dir);
    f = vfs_open(path, "rb");
    if (f == NULL) return;
    if (vfs_read(f, d, sizeof d) == sizeof d)
        for (k = 0; k < 2; k++)
            for (y = 0; y < COVER_H; y++)
                for (x = 0; x < COVER_W; x++) {
                    const uint8_t *c = d + 64000 + 3 * d[(COVER_Y + y) * 320 + k_x[k] + x];
                    g_covers[k][y][x] = (uint32_t)(c[0] << 2 | c[0] >> 4) << 16 |
                                        (uint32_t)(c[1] << 2 | c[1] >> 4) << 8 | (uint32_t)(c[2] << 2 | c[2] >> 4);
                }
    vfs_close(f);
}

static int cover_x(int k)
{
    return 160 - COVER_W - PICK_COVER_GAP / 2 + k * (COVER_W + PICK_COVER_GAP);
}

/* the choice on the game's picture (320 x 200), its keys and clicks acted on: the game run (it goes on to its main
 * menu) or the other one (game.c ends this one) */
static void game_pick(uint32_t *pic)
{
    static const char *const names[2] = {"SYNDICATE", "AMERICAN REVOLT"};
    int k, x, y, mx, my, act = 0;
    uint32_t key = g_pick_key;
    g_pick_key = 0;
    if (!g_covers_loaded) load_covers();
    if (!g_font_loaded) load_menu_font();
    if (g_pick_sel < 0) g_pick_sel = g_addrs->ar != 0;
    if ((key & 0xFFu) == 0x4B || (key & 0xFFu) == 0x4D) g_pick_sel = !g_pick_sel;
    if ((key & 0xFFu) == 0x1C) act = 1;
    menu_mouse(&mx, &my);
    for (k = 0; k < 2; k++) {
        int over = mx >= cover_x(k) && mx < cover_x(k) + COVER_W && my >= PICK_COVER_Y && my < PICK_COVER_Y + COVER_H;
        if (over && (mx != g_pick_mx || my != g_pick_my)) g_pick_sel = k;
        if (over && g_pick_click) g_pick_sel = k, act = 1;
    }
    g_pick_mx = mx;
    g_pick_my = my;
    g_pick_click = 0;
    if (act) {
        g_pick_active = 0;
        g_menu_hold = g_menu_buttons != 0; /* the click is not the main menu's */
        game_choice_made(g_pick_sel);      /* the other game: does not return */
        return;
    }
    for (y = 6; y < 194; y++)
        for (x = 6; x < 314; x++) plot_any_blend(pic, 320, x, y, MENU_BG, 235);
    menu_text_scaled(pic, 320, 160 - text_width_scaled("SELECT GAME", 2) / 2, 14, "SELECT GAME", MENU_BRIGHT, 2);
    for (k = 0; k < 2; k++) {
        int x0 = cover_x(k), i;
        for (y = 0; y < COVER_H; y++)
            for (x = 0; x < COVER_W; x++) pic[(PICK_COVER_Y + y) * 320 + x0 + x] = g_covers[k][y][x];
        for (i = 1; i <= 2; i++)
            frame_box(pic, 320, 200, x0 - i, PICK_COVER_Y - i, x0 + COVER_W + i, PICK_COVER_Y + COVER_H + i,
                      k == g_pick_sel ? PICK_BRIGHT : MENU_FRAME);
        menu_text_scaled(pic, 320, x0 + COVER_W / 2 - text_width_scaled(names[k], 1) / 2, PICK_COVER_Y + COVER_H + 8,
                         names[k], k == g_pick_sel ? MENU_BRIGHT : MENU_TEXT, 1);
    }
    menu_text_scaled(pic, 320, 160 - text_width_scaled("CLICK ONE OR PRESS ENTER", 1) / 2, 176,
                     "CLICK ONE OR PRESS ENTER", MENU_TEXT, 1);
}

/* drawn over the mission, its keys and clicks acted on (game thread) */
static void mission_menu(uint32_t *rgb, int w)
{
    HostOption opts[MAX_OPTIONS];
    int nopt = host_options(opts, MAX_OPTIONS), n = nopt + 2, i, mx, my, key = (int)g_menu_key, act = 0, dir = 1;
    int left, top, right, bottom, cx = (VIEW_LEFT + w) / 2;
    g_menu_key = 0;
    if (!g_font_loaded) load_menu_font();
    left = cx - MENU_W / 2;
    right = cx + MENU_W / 2;
    top = 240 - (n * MENU_ROW + 80) / 2;
    if (top < VIEW_TOP + 4) top = VIEW_TOP + 4;
    bottom = top + n * MENU_ROW + 80;
    /* the keys: up, down, left, right, Enter, Escape (resume) */
    if (key == 0x48 + 0xE000 || key == 0x48) g_menu_sel = (g_menu_sel + n - 1) % n;
    if (key == 0x50 + 0xE000 || key == 0x50) g_menu_sel = (g_menu_sel + 1) % n;
    if (key == 0x4B + 0xE000 || key == 0x4B) act = 1, dir = -1;
    if (key == 0x4D + 0xE000 || key == 0x4D || key == 0x1C || key == 0x1C + 0xE000) act = 1;
    if (key == SC_ESC) {
        menu_close();
        return;
    }
    /* the pointer: over an item it is the one chosen, a click acts on it */
    canvas_mouse(&mx, &my);
    if (mx != g_menu_mx || my != g_menu_my || g_menu_click) {
        int row = (my - (top + 60)) / MENU_ROW;
        if (mx >= left + 16 && mx < right - 16 && my >= top + 60 && row >= 0 && row < n) {
            g_menu_sel = row;
            if (g_menu_click) act = 1;
        }
        g_menu_mx = mx;
        g_menu_my = my;
    }
    g_menu_click = 0;
    if (act) {
        if (g_menu_sel == 0) {
            menu_close();
            return;
        }
        if (g_menu_sel == n - 1) {
            g_abort = 1; /* the game gets an Escape at its next tick */
            menu_close();
            return;
        }
        host_option_step(g_menu_sel - 1, dir);
        nopt = host_options(opts, MAX_OPTIONS);
    }
    /* the panel: dark green, a green frame, the title, a bar for each item */
    for (i = top; i < bottom; i++) {
        int x;
        for (x = left; x < right; x++) plot_any_blend(rgb, w, x, i, MENU_BG, 235);
    }
    for (i = 0; i < 2; i++) {
        int x, y;
        for (x = left; x < right; x++) {
            plot_any(rgb, w, x, top + i, MENU_FRAME);
            plot_any(rgb, w, x, bottom - 1 - i, MENU_FRAME);
        }
        for (y = top; y < bottom; y++) {
            plot_any(rgb, w, left + i, y, MENU_FRAME);
            plot_any(rgb, w, right - 1 - i, y, MENU_FRAME);
        }
    }
    menu_text(rgb, w, cx - menu_text_width("MISSION PAUSED") / 2, top + 18, "MISSION PAUSED", MENU_BRIGHT);
    for (i = 0; i < n; i++) {
        int y = top + 60 + i * MENU_ROW, x, yy, sel = i == g_menu_sel;
        const char *name = i == 0 ? "RESUME" : i == n - 1 ? "ABORT MISSION" : opts[i - 1].name;
        for (yy = y; yy < y + MENU_ROW - 6; yy++)
            for (x = left + 16; x < right - 16; x++) plot_any(rgb, w, x, yy, sel ? MENU_BAR_SEL : MENU_BAR);
        for (yy = y + 7; yy < y + 17; yy++)
            for (x = left + 26; x < left + 36; x++) plot_any(rgb, w, x, yy, MENU_BRIGHT);
        menu_text(rgb, w, left + 48, y + 5, name, sel ? MENU_SEL_TEXT : MENU_TEXT);
        if (i > 0 && i < n - 1) {
            const char *v = opts[i - 1].value;
            menu_text(rgb, w, right - 28 - menu_text_width(v), y + 5, v, sel ? MENU_SEL_TEXT : MENU_BRIGHT);
        }
    }
}

#define MARKER_COLOUR 0x5aff5au


/* the side panel of a mission in true colour: the agents in their boxes wear the colour chosen. The game draws their
 * hats in its own company colour, and the boxes' backgrounds in it too: only the small spots of it (a hat) are
 * the colour chosen, the large ones (a background) stay as the game draws them */
#define PANEL_TOP 40
#define PANEL_BOTTOM 200
#define HAT_SPOT 24 /* a spot of the colour this big at most is a hat */
#define HAT_SHINE_INDEX 14 /* the hats' highlight (the game's bright red) */

/* a hat's colour where the light catches it */
static uint32_t shine_colour(uint32_t c)
{
    uint32_t r = 0;
    int k;
    for (k = 0; k < 24; k += 8) {
        int v = (int)((c >> k) & 255) * 3 / 2 + 20;
        r |= (uint32_t)(v > 255 ? 255 : v) << k;
    }
    return r;
}
static void panel_true_colour(const uint8_t *index, uint32_t *rgb, int w)
{
    static uint8_t seen[(PANEL_BOTTOM - PANEL_TOP) * 128];
    static int stack[(PANEL_BOTTOM - PANEL_TOP) * 128], spot[(PANEL_BOTTOM - PANEL_TOP) * 128];
    int x, y, idx;
    uint32_t c;
    if (g_player_pick < NB_GAME_COLOURS) return;
    idx = player_colour_index();
    c = k_pick_colours[g_player_pick];
    memset(seen, 0, sizeof seen);
    for (y = PANEL_TOP; y < PANEL_BOTTOM; y++)
        for (x = 0; x < VIEW_LEFT; x++) {
            int i = (y - PANEL_TOP) * 128 + x, n = 0, sp = 0, k;
            if (seen[i] || index[y * 640 + x] != idx) continue;
            /* the spot of the colour this pixel is in */
            seen[i] = 1;
            stack[sp++] = i;
            while (sp > 0) {
                int j = stack[--sp], jx = j % 128, jy = j / 128, d;
                spot[n++] = j;
                for (d = 0; d < 4; d++) {
                    int nx = jx + (d == 0) - (d == 1), ny = jy + (d == 2) - (d == 3), ni = ny * 128 + nx;
                    if (nx < 0 || nx >= VIEW_LEFT || ny < 0 || ny >= PANEL_BOTTOM - PANEL_TOP || seen[ni]) continue;
                    if (index[(ny + PANEL_TOP) * 640 + nx] != idx) continue;
                    seen[ni] = 1;
                    stack[sp++] = ni;
                }
            }
            if (n <= HAT_SPOT)
                for (k = 0; k < n; k++) {
                    int sx = spot[k] % 128, sy = spot[k] / 128 + PANEL_TOP, d;
                    rgb[sy * w + sx] = c;
                    /* the hat's highlight beside it (the game's bright red): the colour brighter */
                    for (d = 0; d < 4; d++) {
                        int nx = sx + (d == 0) - (d == 1), ny = sy + (d == 2) - (d == 3);
                        if (nx >= 0 && nx < VIEW_LEFT && ny >= PANEL_TOP && ny < PANEL_BOTTOM &&
                            index[ny * 640 + nx] == HAT_SHINE_INDEX)
                            rgb[ny * w + nx] = shine_colour(c);
                    }
                }
        }
}

int game_overlay(const uint8_t *index, const uint32_t *palette, uint32_t *rgb, int w, int h)
{
    uint64_t age;
    int fx, fy, hw, alpha;
    render_gpu_changed = 0;
    {
        static const HostMenuSides none; /* the wide menu's sides only under it (wide_menu) */
        host_menu_sides(&none);
    }
    if (g_addrs != NULL && w >= 320 && h == 200) {
        static uint32_t game[320 * 200];
        int y;
        uint32_t *pic = w == 320 ? rgb : game;
        if (w > 320)
            for (y = 0; y < 200; y++) memcpy(game + y * 320, rgb + y * w, sizeof *game * 320);
        menu_true_colour(index, palette, pic, 320, h);
        if (config_shown()) config_grids(pic, 320, h, index, palette);
        if (!g_pick_active && game_choice_offered() && g_desk && menu_framed(index)) g_pick_active = 1;
        if (g_pick_active) game_pick(pic);
        g_menu_w = w;
        g_menu_off = (w - 320) / 2;
        rt_mouse_window(g_menu_off * 65536 / w, (g_menu_off + 320) * 65536 / w);
        if (w > 320) {
            static uint8_t bare[320 * 200];
            int on = pointer_past_edge(index, palette, game, bare);
            wide_menu(bare, palette, game, rgb, w);
            pointer_draw(rgb, w, on);
        }
        return 0;
    }
    rt_mouse_window(0, 65536);
    if (g_addrs != NULL && !in_mission()) menu_true_colour(index, palette, rgb, w, h);
    if (g_addrs == NULL || w < 640 || h != 480 || !in_mission()) return 0;
    g_cw = w;
    panel_true_colour(index, rgb, w);
    if (getenv("SYND_RENDER_TEST") != NULL) {
        render_test(rgb, palette);
    } else if (renderer_on()) {
        smooth_city(index, palette, rgb);
    } else {
        company_hats(index, palette, rgb, w);
    }
    draw_drops(rgb, w);
    if (g_editor && renderer_on()) light_editor(rgb, w);
    if (g_menu_key == 0xFFFFu) { /* Escape: the menu opens */
        g_menu_key = 0;
        g_menu = 1;
        g_menu_sel = 0;
        canvas_mouse(&g_menu_mx, &g_menu_my);
    }
    solid_selected_boxes(index, palette, rgb, w);
    draw_scanner_extras(rgb, w);
    if (g_marker_us == 0 || host_time_us() - g_marker_us >= MARKER_US) {
        g_marker_us = 0;
        if (g_menu) {
        mission_menu(rgb, w);
        draw_pointer(rgb, palette); /* over the menu */
    }
        return render_gpu_changed;
    }
    age = host_time_us() - g_marker_us;
    world_to_frame(g_marker[0], g_marker[1], g_marker[2], &fx, &fy);
    hw = 6 + (int)(22 * (MARKER_US - age) / MARKER_US);
    alpha = (int)(255 * (MARKER_US - age) / MARKER_US); /* it fades out */
    diamond(rgb, w, fx, fy, hw, 4, MARKER_COLOUR, alpha);
    diamond(rgb, w, fx, fy, 4, 0, MARKER_COLOUR, alpha);
    if (g_menu) {
        mission_menu(rgb, w);
        draw_pointer(rgb, palette); /* over the menu */
    }
    return render_gpu_changed;
}

/* a new move order of an agent of the squad: the marker goes there */
/* ---- routes: a move order the game's straight walk would not do well (another level, a wall between, or close to
 * one on the way) is walked as a route found over the map's floors (path.c).  The agent is steered along it: at each
 * tick sent to a point ROUTE_AHEAD further on along the route than it is (round corners in a curve, never back),
 * but not past stairs before it reaches them ---- */
#define ROUTE_MAX 64
#define ROUTE_AHEAD 384  /* world units ahead along the route (a tile is 256) */
#define ROUTE_NEAR 96    /* this near a point to be reached, the agent goes on past it */
#define ROUTE_STUCK 30   /* ticks without getting on along the route: a new route from there ... */
#define ROUTE_RETRIES 3  /* ... this many times, then the player's order as the game had it */
static struct {
    PathPoint pt[ROUTE_MAX];
    int n, i;       /* the points, the one being walked to */
    int32_t sx, sy; /* where it started */
    int32_t best;   /* the least of the route left so far */
    int stuck;
    int retries;    /* the new routes it was given on the way */
} g_route[4];
static int g_types_loaded;
static volatile int g_routes_on = 1; /* game_set_routes; SYND_ROUTES=off: the game's straight walks only */

void game_set_routes(int on)
{
    g_routes_on = on != 0;
}

/* a file of the running game's DATA folder, unpacked when it is packed (rnc.c): its bytes (malloc'd, *size of
 * them), or NULL */
static uint8_t *load_data(const char *name, size_t *size)
{
    char path[1200];
    uint8_t *raw = NULL, *out;
    size_t n = 0, cap = 0, got;
    VFile *f;
    snprintf(path, sizeof path, "%s/%s/DATA/%s", rt_game_dir, g_addrs->ar ? "DATADISK" : "SYNDICAT", name);
    f = vfs_open(path, "rb");
    if (f == NULL) return NULL;
    for (;;) {
        uint8_t *r;
        if (n == cap) {
            cap = cap ? cap * 2 : 65536;
            r = (uint8_t *)realloc(raw, cap);
            if (r == NULL) break;
            raw = r;
        }
        got = vfs_read(f, raw + n, cap - n);
        if (got == 0) break;
        n += got;
    }
    vfs_close(f);
    if (raw == NULL) return NULL;
    if (rnc_size(raw, n) == 0) {
        *size = n;
        return raw;
    }
    out = (uint8_t *)malloc(rnc_size(raw, n));
    *size = out != NULL ? rnc_unpack(raw, n, out, rnc_size(raw, n)) : 0;
    free(raw);
    if (*size == 0) {
        free(out);
        return NULL;
    }
    return out;
}

/* the tile types: COL01.DAT of the running game */
static void load_tile_types(void)
{
    size_t n;
    uint8_t *types = load_data("COL01.DAT", &n);
    g_types_loaded = 1;
    if (types != NULL && n >= 256) {
        path_set_types(types);
        render_set_tile_types(types);
    }
    free(types);
}

/* a new order of the player's for the agent in `slot`, to tx, ty, tz: walked as a route if it needs one */
static double dist(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    return sqrt((double)(bx - ax) * (bx - ax) + (double)(by - ay) * (by - ay));
}

static void send_to(int slot, uint32_t ped, int32_t x, int32_t y, int32_t z)
{
    WR16(ped + PED_TARGET, (uint16_t)x);
    WR16(ped + PED_TARGET + 2u, (uint16_t)y);
    WR16(ped + PED_TARGET + 4u, (uint16_t)z);
    g_last_target[slot][0] = (uint16_t)x;
    g_last_target[slot][1] = (uint16_t)y;
}

static void route_order(int slot, uint32_t ped, int32_t tx, int32_t ty, int32_t tz)
{
    g_route[slot].n = g_route[slot].i = 0;
    if (!g_types_loaded) load_tile_types();
    g_route[slot].sx = (int16_t)RD16(ped + PED_X);
    g_route[slot].sy = (int16_t)RD16(ped + PED_Y);
    g_route[slot].n = path_route(g_route[slot].sx, g_route[slot].sy, (int16_t)RD16(ped + PED_Z), tx, ty, tz,
                                 g_route[slot].pt, ROUTE_MAX);
    g_route[slot].best = 0x7FFFFFFF;
    g_route[slot].stuck = 0;
    if (g_route[slot].n > 0) rt_log("route: agent %d, %d points", slot + 1, g_route[slot].n);
}

/* at each tick: each agent on a route sent on along it */
static void follow_routes(uint32_t company)
{
    int slot;
    for (slot = 0; slot < 4; slot++) {
        uint32_t ped = g_addrs->peds + (M[company + COMPANY_SQUAD] + (uint32_t)slot) * PED_SIZE;
        int32_t x = (int16_t)RD16(ped + PED_X), y = (int16_t)RD16(ped + PED_Y), ax, ay, left;
        int n = g_route[slot].n, k;
        const PathPoint *pt = g_route[slot].pt;
        double along, d;
        if (n == 0) continue;
        if (!PED_ON_MAP(ped) || RD16(ped + PED_TARGET) != g_last_target[slot][0] ||
            RD16(ped + PED_TARGET + 2u) != g_last_target[slot][1]) {
            g_route[slot].n = 0; /* another order, or gone */
            continue;
        }
        /* on past the points it is beyond: near them, or (not to be reached) past them along the route */
        for (;;) {
            k = g_route[slot].i;
            if (k >= n - 1) break;
            ax = k == 0 ? g_route[slot].sx : pt[k - 1].x;
            ay = k == 0 ? g_route[slot].sy : pt[k - 1].y;
            d = dist(ax, ay, pt[k].x, pt[k].y);
            if (dist(x, y, pt[k].x, pt[k].y) < ROUTE_NEAR ||
                (!pt[k].hard && d > 0 && ((x - ax) * (double)(pt[k].x - ax) + (y - ay) * (double)(pt[k].y - ay)) / d >= d))
                g_route[slot].i++;
            else
                break;
        }
        k = g_route[slot].i;
        /* the route left: to the point being walked to, then from point to point */
        along = dist(x, y, pt[k].x, pt[k].y);
        for (left = (int32_t)along; k + 1 < n; k++) left += (int32_t)dist(pt[k].x, pt[k].y, pt[k + 1].x, pt[k + 1].y);
        if (left < g_route[slot].best - 8) {
            g_route[slot].best = left;
            g_route[slot].stuck = 0;
        } else if (++g_route[slot].stuck > ROUTE_STUCK) {
            PathPoint goal = pt[n - 1];
            int retries = g_route[slot].retries;
            if (retries < ROUTE_RETRIES) {
                route_order(slot, ped, goal.x, goal.y, goal.z);
                g_route[slot].retries = retries + 1;
                if (g_route[slot].n > 0) continue;
            }
            /* the player's order as the game had it */
            g_route[slot].n = 0;
            send_to(slot, ped, goal.x, goal.y, goal.z);
            continue;
        }
        /* ROUTE_AHEAD on from where it is along the route, not past a point to be reached */
        k = g_route[slot].i;
        {
            int32_t px = x, py = y;
            double ahead = ROUTE_AHEAD;
            for (;;) {
                double seg = dist(px, py, pt[k].x, pt[k].y);
                if (seg >= ahead || pt[k].hard || k == n - 1) {
                    double f = seg > ahead ? ahead / seg : 1.0;
                    send_to(slot, ped, px + (int32_t)((pt[k].x - px) * f), py + (int32_t)((pt[k].y - py) * f), pt[k].z);
                    break;
                }
                ahead -= seg;
                px = pt[k].x;
                py = pt[k].y;
                k++;
            }
        }
        if (g_route[slot].i == n - 1 && dist(x, y, pt[n - 1].x, pt[n - 1].y) < ROUTE_AHEAD) {
            /* the end in sight: the game walks it there */
            send_to(slot, ped, pt[n - 1].x, pt[n - 1].y, pt[n - 1].z);
            g_route[slot].n = 0;
        }
    }
}

static void watch_move_orders(void)
{
    uint32_t company = g_addrs->companies + (uint32_t)(int16_t)RD16(g_addrs->player) * g_addrs->company_size;
    int slot;
    follow_routes(company);
    for (slot = 0; slot < 4; slot++) {
        uint32_t ped = g_addrs->peds + (M[company + COMPANY_SQUAD] + (uint32_t)slot) * PED_SIZE;
        uint16_t tx = RD16(ped + PED_TARGET), ty = RD16(ped + PED_TARGET + 2u);
        if ((tx != g_last_target[slot][0] || ty != g_last_target[slot][1]) && (tx || ty) && (M[ped + PED_FLAGS] & 4u)) {
            g_marker[0] = (int16_t)tx;
            g_marker[1] = (int16_t)ty;
            g_marker[2] = (int16_t)RD16(ped + PED_TARGET + 4u);
            g_marker_us = host_time_us();
            /* B held: the floor of the squad's level seen where the player clicked, through the buildings (as
             * FreeSynd's), when one can walk there */
            if (g_see_inside && g_cut_level >= 1) {
                int32_t fx, fy;
                render_unproject(g_click_sx, g_click_sy, g_cut_level * 128, &fx, &fy);
                if (path_walkable(fx >> 8, fy >> 8, g_cut_level)) {
                    g_marker[0] = (int16_t)fx;
                    g_marker[1] = (int16_t)fy;
                    g_marker[2] = (int16_t)(g_cut_level * 128);
                    WR16(ped + PED_TARGET, (uint16_t)fx);
                    WR16(ped + PED_TARGET + 2u, (uint16_t)fy);
                    WR16(ped + PED_TARGET + 4u, (uint16_t)(g_cut_level * 128));
                }
            }
            if (g_routes_on && g_marker_us != 0) {
                g_route[slot].retries = 0;
                route_order(slot, ped, g_marker[0], g_marker[1], g_marker[2]);
            }
            tx = RD16(ped + PED_TARGET);
            ty = RD16(ped + PED_TARGET + 2u);
        }
        g_last_target[slot][0] = tx;
        g_last_target[slot][1] = ty;
    }
}

/* ---- a steady pace: the mission's ticks on this program's clock ----
 * The game waits in its mission loop (frame_wait) for its timer to count `speed` (7) interrupts since the last
 * tick, but does not wait at all when none came yet: on a fast CPU a tick with no wait follows most ticks that
 * waited, so the ticks come in pairs (~14 ms apart, then ~85 ms).  Syndicate's timer runs at ~72 a second (10 ticks
 * a second as designed, ~20 with the pairs), American Revolt's at 120 (17 a second as designed).  Here each tick
 * starts on a steady clock instead, `speed` PACE_HZ-ths of a second apart, and the game's own wait is then passed
 * at once.  The ticks do the same; only when they come changes.  At 120 (17 a second) an agent without mods walks
 * 15 units a tick, 256 a second, as in FreeSynd; the user found that too fast and chose 84 (12 a second;
 * Syndicate as designed would be 73, 10.4 a second).  SYND_PACE=off: the game's own pace. */
#define PACE_HZ 84u
static uint64_t g_pace_next; /* when the next tick starts */
static uint64_t g_pace_slot; /* when the last one started (0: not paced) */
static uint64_t g_pace_period;

static int on_frame_wait(uint32_t addr)
{
    /* the menu open: the mission waits (the frames go on) */
    while (g_menu && !host_should_quit()) {
        host_sleep_us(2000);
        rt_poll();
    }
    /* Abort mission: the game's Escape, held a few ticks (the game looks at the keys held at its ticks) */
    if (g_abort == 1) {
        g_pass_esc = 1;
        rt_key_event(SC_ESC, 1);
    }
    if (g_abort > 0 && ++g_abort > 4) {
        g_abort = 0;
        rt_key_event(SC_ESC, 0);
    }
    uint64_t now = host_time_us(), period;
    int32_t n = (int32_t)RD32(g_addrs->speed);
    const char *e = getenv("SYND_PACE");
    (void)addr;
    g_pace_slot = 0;
    if (!in_mission() || (e != NULL && strcmp(e, "off") == 0)) return 0;
    if (n < 1) n = 1;
    if (n > 12) n = 12;
    period = 1000000u * (uint64_t)n / PACE_HZ;
    /* late (a pause, a slow tick): start again from now, no catching up */
    if (g_pace_next == 0 || now > g_pace_next + period) g_pace_next = now;
    while (now < g_pace_next && !host_should_quit()) {
        uint64_t left = g_pace_next - now;
        if (left > 1500u) host_sleep_us(left > 2500u ? 2000u : (uint32_t)(left - 500u));
        rt_poll(); /* interrupts, frames and sound go on */
        now = host_time_us();
    }
    g_pace_slot = g_pace_next;
    g_pace_period = period;
    g_pace_next += period;
    WR8(g_addrs->wait_count, 0xFFu); /* the game's wait is over */
    return 0;
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
        memset(g_route, 0, sizeof g_route);
        load_tile_types(); /* (they may differ from game to game) */
        if (getenv("SYND_ROUTES") != NULL && strcmp(getenv("SYND_ROUTES"), "off") == 0) g_routes_on = 0;
        g_view_on = 0;
        render_set_view(0, 0, 0);
    }
    g_tick_us = host_time_us();
    if (renderer_on()) {
        sync_camera();
        /* paced: the tick is the one that started on the last slot (the snapshot then has its exact time; it is
         * taken later, after the game's drawing, which takes more or less time) */
        render_tick(g_pace_slot != 0 && g_tick_us - g_pace_slot < 2 * g_pace_period ? g_pace_slot : g_tick_us);
    }
    watch_move_orders();
    watch_hits(0);
    if (g_boost) {
        g_boost = 0;
        boost_selected();
    }
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
    hook_add(g_addrs->draw_logo, on_draw_logo);
    player_colour_load();
    hook_add(g_addrs->frame_wait, on_frame_wait);
    hook_add(g_addrs->draw_scanner, on_draw_scanner);
}
