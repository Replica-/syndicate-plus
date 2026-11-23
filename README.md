# synd: Syndicate Plus 64bit

A static recompilation of the 1993 DOS game *Syndicate Plus* (Bullfrog's *Syndicate* with the *American Revolt*
data disk) into a native 64-bit program for **Windows (x64)** and **macOS** (one universal app for Apple silicon
and Intel Macs, macOS 11 or later).

The original 32-bit DOS/4GW programs are translated to C function by function (`tools/recomp.py`); a small
portable runtime stands in for DOS, DPMI, the BIOS, the VGA card, the timer, the keyboard, the mouse, the Sound
Blaster and the AdLib (OPL2) chip.  The result is the original game, unchanged, running at its original pace,
with its own sound and music, in a window or full screen.

| The game selection | The intro | Syndicate's main menu |
|---|---|---|
| ![The launcher: Syndicate or American Revolt](screenshots/1-game-selection.png) | ![The intro](screenshots/2-intro.png) | ![The main menu](screenshots/3-main-menu.png) |

**You need your own copy of the original game.**  Nothing of the original game is in this repository: not its
programs or data, and not the C code generated from its programs.  The program reads the original files at run
time and checks that they are the ones it was built from.

## Download

[Releases](https://github.com/Buyukcaglar/synd/releases) has the Windows program (`Syndicate64.exe`, 64-bit
Windows 10 or later) and the macOS app (signed and notarized).  On first start it asks for the folder of your
Syndicate Plus installation (the one with `SYND.EXE`), then remembers it.

## The game files

The program needs the Syndicate Plus files as installed by the original (DOS) game, for example from the CD
or a digital release: the folder with `SYND.EXE`, `DOS4GW.EXE`, `DATA`, `SYNDICAT` (Syndicate), `DATADISK`
(American Revolt) and `SYND`.  English, French, Italian, Spanish and German installations all work.  The files
are never changed: everything the game writes goes to a `Save` folder next to the program.

## Building

Both platforms build the same way: first the C code is generated from your copy of the game, then CMake builds
the program.

1. Put the game folder (the one with `SYND.EXE`) at `_references/dos` in this repository.
2. Generate the code (Python 3 and [Capstone](https://www.capstone-engine.org/)):

       pip install capstone
       python3 tools/gen_all.py         # writes gen/ (about 38 MB of C)

3. Build:

   - **Windows**: Visual Studio 2022 or later with the C++ workload, then `build.cmd`.  The result is
     `build/Syndicate64.exe` (and `synd_headless.exe`, the test program).
   - **macOS**: the Xcode command line tools, CMake and optionally Ninja (`brew install cmake ninja`), then
     `./build.sh`.  The result is `build/Syndicate64.app` (universal) and `build/synd_headless`.

When `_references/dos` is present the build puts the game files *inside* the program (CMake option
`SYN_EMBED_GAME`), so your own copy is one self-contained file to keep or carry around.  Such a build contains
the game: keep it to yourself.  Configured with `-DSYN_EMBED_GAME=OFF` (on macOS: `./build.sh
-DSYN_EMBED_GAME=OFF`) CMake builds a program that asks for the game folder instead.

On macOS the app is signed ad hoc by default, which is enough on the Mac that built it.  To give it to others,
sign it with a Developer ID and notarize it:

    ./build.sh -DSYN_EMBED_GAME=OFF -DSYN_CODESIGN_IDENTITY="Developer ID Application: <name> (<team id>)"
    tools/notarize_mac.sh build/Syndicate64.app <notarytool keychain profile>

## Playing

A *Select game* screen in the menus' green comes first and shows the two box covers: click one, or the arrow keys
and Enter.  Nothing of either game runs until the choice is made; the one chosen then starts with its introduction,
with the language and sound card setup the original launcher made (`SYND\SYND.INF`; the launcher only comes up when
there is none).  The choice is kept in `game.txt` beside the saves and is offered first next time.  Without game files built in, the program looks for the game next to
itself, in `_references/dos` of a source checkout and in the folder chosen last time; otherwise it asks.

Saved games go to `Save/Syndicate` and `Save/American Revolt` next to the program (on macOS: next to
`Syndicate64.app`, never inside it), the settings to `Syndicate64.ini` beside it.  Where that folder cannot be
written to, both go to `%LOCALAPPDATA%\Syndicate64` (Windows) or `~/Library/Application Support/Syndicate64`
(macOS).

| | Windows | macOS |
|---|---|---|
| Full screen (on the display the window is on) | Alt+Enter, or the window menu | Ctrl+Cmd+F, the green button, or View > Full Screen |
| Music and sound volumes (remembered) | the window menu | |
| Record a video (again to stop) | F12, or the window menu | |
| Screenshot | F11, or the window menu | |
| About | the window menu (Alt+Space) | the app menu |
| The game's Alt key | Alt | Option |
| Right button | right click | right click or Ctrl+click |

In full screen the pointer stays on the picture while the game has the focus (free in menus, dialogs and when
you switch to another program).  The game pauses while a dialog is open or the window is minimized.  The
window, the menus and the About box follow the system's dark or light mode.  On a Mac laptop the function keys
need Fn unless they are set to act as standard function keys.

### Music from files

The songs can play from files instead of the FM music: `assassinate` (the city), `danger` (rival agents near
yours), `intro`, `mission_completed`, `mission_failed` and `game_completed`, as `.mp3`, `.ogg`, `.flac` or
`.wav`, in a `music` folder inside the game folder (or the folder given by `--music-dir`, or by `dir` in the
`[music]` section of `Syndicate64.ini`).  The game still decides when each song plays: when it starts a song of
its own, the file plays instead (`src/rt/music.c` follows the calls into the AIL/32 music driver).  A song the
game pauses and picks up again, like the city song around a fight, goes on from where it stopped, with a short
fade in.  Songs without a file play as before.

### Auto saves

When the world map comes up after a mission, the game saves itself in line 1 of the Load/Save screen, named
`AUTO <day>/<month> <hour>:<minute>`; the auto saves before it move down to lines 2 and 3, the oldest is dropped.
They load like any save.  Lines 4 to 10 are the player's: a save made in one of the first three lines goes to the
first empty line from 4 on, and a save from before (in lines 1 to 3) moves to an empty line when the first auto
save is made.  This works in both games and every language (`src/game/additions.c`).

When the main menu first comes up, the newest auto save is loaded, as Load would.  Under its items the main menu
shows the company: its logo in its colour, its name and the player's, its funds, how many of the 50 countries it
holds and how complete that is (in percent), and the auto save loaded.  On the Load/Save screen each save shows
its logo in its colour and its completion on its line, and the chosen line's company, funds and countries show
between the buttons.  The colour chosen in Configure Company (the game's saves only hold the nearest of its own 8)
is in each save (this version adds a part of its own at the end of a save file, which the game does not read:
`S64X`, its version, then the colour) and is the player's again when the save is loaded.  A save left named as an
empty line ("EMPTY") or with no name is named `SAVE <day>/<month> <hour>:<minute>`.

### In missions

In missions the view scrolls at once and smoothly, at every frame shown, with the arrow keys, W, A, S and D (only
in missions: in the menus they are letters) or the pointer at an edge of the screen; the game's camera follows it
at its own pace, and clicks go where they are seen. 1 to 4 select
an agent as a click on its box does (the original looks at these keys only every few ticks and misses a short
press); pressed twice within 0.4 s they also bring the view to that agent.  A double click on the city sets the
selected agents' speed drug (adrenaline) to the top. A move order shows
a green diamond on the floor where the agents go, shrinking and fading out in 0.6 s. The boxes of the selected
agents have a solid background instead of the checkerboard of orange and olive. A hit sprays blood out of the
person hit, away from the shooter. Agents wear hats in the colour of their company, yours and the rival syndicates'
(the original draws every hat in the red of the first colour).

With Direct3D, a mission fills a wide window: the city goes on to its right edge (the side panel stays at the
left), and the game's 400 lines of it fill the window's height, the side panel's too, as in FreeSynd (the empty
lines above and below them are off the window).  The mouse wheel zooms the city out to half its size or in to one and a half.  The view pans as far as
the map's edges (the middle of the window on the map), past the mission area the game's own camera keeps to: a
click out there goes to the nearest point the game takes.  Holding B shows inside the buildings: what stands on the floor of the squad's
highest agent (walls, roofs) is not drawn.

The menus fill a wide window too (up to 21:9): the game's 320 x 200 picture stays in the middle, the green menus'
frame goes round all of it, and the sides are the desk outpainted with Fooocus from the coloured desk the main menu
first shows (`tools/menu_outpaint.py`), drawn at the window's own resolution, not stretched from the game's (in
colour, and in green as the game makes its green desk: `tools/menu_sides.py`, `src/game/menu_backdrop.h`).  The
game's desk fades into them at its edges (its own pixels only: what is drawn over it stays); while the main menu's
animation wipes the green over the coloured desk each side follows it, and they dim with the game's fades.  Where
the platform cannot draw them (headless without `--gpu`) the sides are the desk's dark green.  The title screen, the
videos and the other screens' opening animations keep black sides.

The window menu's *Time of day* lights the city at dusk or at night (the side panel stays as it is): the city is
darker, with the time's colour, and lit by its lamps (found in the tiles), the cars' headlights and the flashes of
the shots.  Ctrl+L in a mission opens the light editor: the tile under the pointer is outlined, G changes what of
it glows in the dark, L whether it lights what is around it, [ and ] the size of its light, C its colour; the
settings are saved in `tile_lights.ini` beside the saves.

Traffic lights and other small coloured lights in the objects glow at dusk and
night.  Held B also outlines in cyan the edges of the floor the squad walks on.

Configure Company shows every colour and logo in grids under its panel, one click each: the game's 8 colours and
18 more (one of these goes on your agents' hats in true colour; the game keeps its nearest own colour for the world
map and its menus), and the 40 logos in the colour chosen.  Escape in a mission opens a menu in the game's
briefing look while the mission waits: Resume, time of day, full screen, frames a second,
the volumes, squad routes, and Abort mission (the game's Escape).  *Squad finds routes* (on by default): a move order the game's straight walk
would not do well (another level, a gap or a wall between, or passing close to one) is walked as a route over the
map's floors and stairs: A* on the tiles (with the game's own tile types), a step beside a wall costing more so the
route keeps to open ground, then pulled into long straight stretches that keep a quarter of a tile clear of the
walls either side.  The agent is steered along it, sent at each tick to a point 1.5 tiles further on along the route
(round corners in a curve, never back past where it was), but onto stairs only by way of their ends; an agent that
stops getting on gets a new route from where it is, three times, then the order as the game had it.  Only the
squad's orders: the other people and the enemies' AI are untouched.

A car the squad drives goes the same way (with squad routes on): an order to an agent in it drives it over the
roads to the lane nearest the point clicked, the side of the road clicked, facing that lane's traffic.  The route
keeps to the lanes' ways (traffic keeps right; never against a lane, never backing up) and turns only where the
road does, at junctions and round the loops at the roads' ends (a lane that cannot be reached so: the nearest
that can).  The car runs on its lanes' lines as on rails, round the corners where they meet, slowing into them and
to a stop at the end; a green marker shows where it will stop until it gets there, and the numbers of the agents
in it are drawn over it.  The game's own driving
(which keeps to the lane a car is in, driving on away from a point behind it) stays for every other car, and for
the squad's when no lane is near the point clicked.

### The scanner

The scanner (the map at the bottom left of a mission) shows more of the city around the leader: 6 pixels a tile
instead of 8, and 2 instead of 4 when an agent of the squad carries a scanner.  With a scanner it also shows the
doors (cyan), the criminals (orange) and the people holding a weapon (red).

### MINIGUN SQUAD

Besides the game's own cheats (the company or player name in Configure Company), `MINIGUN SQUAD`: every weapon
and mod is researched, and the 18 agents get every mod at version 3 and a minigun in each of their 8 weapon slots.
The funds and the missions stay as they are; it goes with `TO THE TOP` as the player name for both.
`--cheats "MINIGUN SQUAD:TO THE TOP"` gives the two names when the world map first comes up (the VS Code launch
configurations *all missions, miniguns, full mods*, for both games).

### Videos and screenshots

F12 records the game with its sound as an MP4 (1280x960, sharp pixels) in a `Videos` folder beside
`Syndicate64.ini`; it needs [ffmpeg](https://ffmpeg.org) in the PATH (`winget install ffmpeg`).  While it records,
the title bar says so; F12 again stops, and the video is finished in the background.  F11 saves a PNG screenshot
in `Screenshots`: with Direct3D, the picture as the window shows it (its size, lit at dusk or night).  The headless program records with `--record DIR`.

## Development (Windows)

`dev\build.cmd [RelWithDebInfo|Release|Debug]` builds with Ninja into `build-dev\<config>`, without the game
files built in: the programs find them in `_references\dos`.  `_references\dos` can be a junction to an
installed copy (`mklink /J _references\dos <game folder>`).  VS Code has build tasks, a task for the tests and
launch configurations (the launcher, Syndicate or American Revolt directly, straight into Scandinavia to try the
squad's routes, and a headless run into a mission recorded as a video); `.vscode/`.  Both programs take scripted
input, `--input "ms:event,..."` (keys, clicks at fractions of the picture, on a wide menu of the game's 320 x 200 in
it: `src/rt/script.c`), which is how a launch goes through the menus.  The tests run on that build with
`SYND_HEADLESS=build-dev\RelWithDebInfo\synd_headless.exe`.

Started directly, the games need the sound card's arguments the launcher gives them, as in `SYND\SYND.INF`:
`--start "SYNDICAT\MAIN.EXE /c0 /iirq7 /idma1 /iio220"` (the tests' `/s` plays without sound).

### True colour and the GPU

The game is shown in true colour on the GPU (Direct3D 11 on Windows): the core turns each frame of the original
into 32-bit colour, this version draws its additions over it with any colour and transparency, and the window
scales it with sharp bilinear filtering (every pixel of the game stays a sharp square, of the same size, at any
window size), with vsync.  There is no other way to draw it: without a usable graphics card Direct3D uses its
software rasterizer (WARP), and if Direct3D 11 cannot start at all the game says so and stops.

### Smooth motion

The original engine draws the city about 10 to 18 times a second (in DOSBox too), and on a fast computer makes its
updates in pairs (two at once, then nothing for 85 ms: its wait for the next update is skipped when it finds no
timer tick yet).  In missions this version starts each of the game's updates on a steady clock instead, 12 a
second (`PACE_HZ` in `src/game/additions.c`; the game's own updates are unchanged, only when they come), and draws
the city itself (`src/game/render.c`, from the game's own data in memory) at every frame shown, the people,
vehicles, objects and the view placed between the updates, two updates behind: every frame moves by the same
amount.  The pointer and the agents' numbers are drawn with the game's own sprites and frames (the pointer where
the mouse is now).  At most 60 frames a second, each made just before the display's refresh that shows it (every
second refresh at 120 Hz).  The frames a second shown are at the top right (window menu: *Show frames a second*).
Both games, every language; `SYND_RENDER=off` turns the renderer off and `SYND_PACE=off` the steady clock.

With Direct3D the city is not drawn into the frame but as a list of images on the GPU (`win32_gpu.c`): each tile
and sprite at its exact place at the window's resolution, sharp, under the frame (whose map part is see-through);
at dusk and night through a light map.  The headless program does the same offscreen with `--gpu WxH` (and
`--time`, `--wide`, `--screenshot`), so the tests check it.

## Tests

    python3 tests/run_tests.py           # about 20 s to 1.5 minutes; --quick leaves out the full-length intro
    SYND_ARCH=x86_64 python3 tests/run_tests.py    # macOS: the Intel half of the universal build (Rosetta)

The tests play the games headless on an emulated clock (runs repeat exactly) and check pictures, files and
sound: the launcher in several installations, the main menus of both games in every language, a mission, shooting,
saving and loading, the intro music and length, the built-in files, that two runs end on the same frame, a wide
zoomed mission's clicks, the GPU's picture at night, a company colour and a route over a bridge.
They need the game files in `_references/dos`.

## Layout

| Path | What it holds |
|------|---------------|
| `tools/` | `find_equiv.py` (a function of one program in the others, for the hooks), `le.py` (LE/LX loader), `discover.py` (code discovery), `recomp.py` (x86 to C translator), `gen_all.py` (writes `gen/`), `pack_game.py` (the built-in game files), icon and notarization helpers |
| `src/rt/` | the portable runtime: CPU state, DOS/DPMI/BIOS, VGA, timer, keyboard, mouse, Sound Blaster and OPL2 |
| `src/game/` | which recompiled DOS programs make up the game and how it starts; the hooks on the game's functions (`hooks.c`) and what they add (`additions.c`); the city drawn (`render.c`), the squad's routes (`path.c`) and its cars' (`drive.c`), the game's packed data files read (`rnc.c`) |
| `src/video/` | the video recorder and screenshots |
| `src/platform/` | the front ends: `win32/`, `macos/`, `posix/` (shared POSIX services), `headless/` (tests) |
| `src/third_party/` | dr_mp3, dr_flac, dr_wav, stb_vorbis and stb_image_write (public domain / MIT-0) |
| `dev/`, `.vscode/` | the development build and the VS Code setup (Windows) |
| `res/` | icons, the Windows resources, the macOS `Info.plist` |
| `tests/` | the automated tests |

`src/rt/host.h` is the whole boundary between the portable core and a platform.

## Licence

The code of this port is under the MIT licence (`LICENSE`).  *Syndicate*, *Syndicate: American Revolt* and
*Syndicate Plus*, their programs, sound drivers, data and box art belong to their owners; they are not part of
this software.  This project is not affiliated with or endorsed by them.

64bit version by Onur BUYUKCAGLAR, with Anthropic Claude AI assistance.
