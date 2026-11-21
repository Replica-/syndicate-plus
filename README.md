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

The original launcher comes first: pick *Syndicate* or *American Revolt* with the arrow keys and Enter (the
launcher has no mouse pointer, as on DOS).  Without game files built in, the program looks for the game next to
itself, in `_references/dos` of a source checkout and in the folder chosen last time; otherwise it asks.

Saved games go to `Save/Syndicate` and `Save/American Revolt` next to the program (on macOS: next to
`Syndicate64.app`, never inside it), the settings to `Syndicate64.ini` beside it.  Where that folder cannot be
written to, both go to `%LOCALAPPDATA%\Syndicate64` (Windows) or `~/Library/Application Support/Syndicate64`
(macOS).

| | Windows | macOS |
|---|---|---|
| Full screen (on the display the window is on) | Alt+Enter, or the window menu | Ctrl+Cmd+F, the green button, or View > Full Screen |
| CRT scanlines (off at first, remembered) | Alt+S, or the window menu | Cmd+Shift+S, or View > CRT Scanlines |
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

### In missions

W, A, S and D scroll the view as the arrow keys do (only in missions: in the menus they are letters). 1 to 4 select
an agent, as in the original; pressed twice within 0.4 s they also bring the view to that agent. A move order shows
a green diamond on the floor where the agents go, shrinking and fading out in 0.6 s. The boxes of the selected
agents have a solid background instead of the checkerboard of orange and olive. A hit sprays blood out of the
person hit, away from the shooter. Agents wear hats in the colour of their company, yours and the rival syndicates'
(the original draws every hat in the red of the first colour).

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
in `Screenshots`.  The headless program records with `--record DIR`.

## Development (Windows)

`dev\build.cmd [RelWithDebInfo|Release|Debug]` builds with Ninja into `build-dev\<config>`, without the game
files built in: the programs find them in `_references\dos`.  `_references\dos` can be a junction to an
installed copy (`mklink /J _references\dos <game folder>`).  VS Code has build tasks, a task for the tests and
launch configurations (the launcher, Syndicate or American Revolt directly, and a headless run into a mission
recorded as a video); `.vscode/`.  The tests run on that build with
`SYND_HEADLESS=build-dev\RelWithDebInfo\synd_headless.exe`.

Started directly, the games need the sound card's arguments the launcher gives them, as in `SYND\SYND.INF`:
`--start "SYNDICAT\MAIN.EXE /c0 /iirq7 /idma1 /iio220"` (the tests' `/s` plays without sound).

### True colour and the GPU

The game is shown in true colour on the GPU (Direct3D 11 on Windows): the core turns each frame of the original
into 32-bit colour, this version draws its additions over it with any colour and transparency, and the window
scales it with sharp bilinear filtering (every pixel of the game stays a sharp square, of the same size, at any
window size), with vsync.  GDI is only used when Direct3D cannot start.

### Smooth motion

The original engine draws the city about 10 to 18 times a second (in DOSBox too).  In missions this version draws
the city itself (`src/game/render.c`, from the game's own data in memory) at every frame shown, 70 a second, with
the people, vehicles, objects and the view placed on a steady clock that runs through the game's updates at
their average rate, about one and a half updates behind (the game's updates come unevenly, a few at once), so
they move smoothly and evenly; the pointer and the agents' numbers are drawn with the game's own sprites (the
pointer where the mouse is now).  At most 60
frames a second, each made just before the display's refresh that shows it.  The emulated CPU runs
at the speed of GOG's DOSBox (`[game] cycles=8000` in `Syndicate64.ini`).  The frames a second shown are at the
top right (window menu: *Show frames a second*).  Both games, every language; `SYND_RENDER=off` turns it off.

## Tests

    python3 tests/run_tests.py           # about 20 s to 1.5 minutes; --quick leaves out the full-length intro
    SYND_ARCH=x86_64 python3 tests/run_tests.py    # macOS: the Intel half of the universal build (Rosetta)

The tests play the games headless on an emulated clock (runs repeat exactly) and check pictures, files and
sound: the launcher in several installations, the main menus of both games in every language, a mission, shooting,
saving and loading, the intro music and length, the built-in files, and that two runs end on the same frame.
They need the game files in `_references/dos`.

## Layout

| Path | What it holds |
|------|---------------|
| `tools/` | `find_equiv.py` (a function of one program in the others, for the hooks), `le.py` (LE/LX loader), `discover.py` (code discovery), `recomp.py` (x86 to C translator), `gen_all.py` (writes `gen/`), `pack_game.py` (the built-in game files), icon and notarization helpers |
| `src/rt/` | the portable runtime: CPU state, DOS/DPMI/BIOS, VGA, timer, keyboard, mouse, Sound Blaster and OPL2 |
| `src/game/` | which recompiled DOS programs make up the game and how it starts; the hooks on the game's functions (`hooks.c`) and what they add (`additions.c`) |
| `src/video/` | the CRT scanline filter, the video recorder and screenshots |
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
