# Resume: porting the freesynd-enhanced features to syndicate-plus

Where the work stands, for the next session. Everything is committed locally on `main`; nothing is pushed
(the remote is Onur BUYUKCAGLAR's repo; never push).

## Ground rules from the user

- Never change the AI, mission triggers or gameplay (combat stays as the original). Close-range friendly fire was dropped.
- Licence: syndicate-plus stays **MIT**. Never copy or port FreeSynd (GPL v3) code; carry over ideas, behaviour and
  file-format knowledge only, and write fresh code.
- The game is played only in **true colour on the GPU** (no 256-colour limits for anything new).
- Test headless, without sound (when the window must be checked: mute both volumes in `Syndicate64.ini`, then
  delete that ini afterwards).
- Group 1 (FreeSynd bug fixes the original never had) is skipped.

## Build, run, test

- `_references/dos` is a junction to `D:\development\syndicate\game_data` (GOG install). `gen/` is generated:
  `python tools/gen_all.py` (needs `pip install capstone`), about 40 s.
- `dev\build.cmd` builds into `build-dev\RelWithDebInfo` (Ninja; a change to `src/rt/recomp.h` or `gen/` rebuilds
  everything, about 8 min).
- Tests: `SYND_HEADLESS=build-dev\RelWithDebInfo\synd_headless.exe python tests/run_tests.py --quick` (17 tests).
- Start the games with the launcher's sound arguments: `--start "SYNDICAT\MAIN.EXE /c0 /iirq7 /idma1 /iio220"`
  (`DATADISK\...` for American Revolt). `/s` means no sound.
- VS Code: `.vscode/launch.json` has the launcher, both games, both games with `--cheats "MINIGUN SQUAD:TO THE TOP"`
  (all missions, $100M, v3 mods, miniguns) and a headless recorded mission.

## Done (groups 2 and 3, and the start of 4)

Music from files (`src/rt/music.c`, follows the AIL/32 driver's sequence calls), volumes, video recording (F12,
ffmpeg), screenshots (F11), auto saves (lines 1-3), MINIGUN SQUAD, `--cheats`, WASD scrolling, double-press 1-4 to
centre, scanner range and extra marks, move-order marker, solid selected-agent box, hats in company colours, blood
sprays, true-colour frames (`host_present` takes 0x00RRGGBB; `game_overlay` in `src/game/additions.c` draws over
them), Direct3D 11 presentation on Windows (`src/platform/win32/win32_gpu.c`).

How additions hook the game: `RT_ENTER` in every recompiled function checks a bitmap (`src/game/hooks.c`);
`additions_install()` in `src/game/additions.c` adds the hooks of each program from one address table per program
(English/Spanish/German Syndicate and American Revolt). `tools/find_equiv.py` finds a function and its data addresses
in the other programs. Dev aids: `SYND_CENSUS=FILE`, `SYND_SKIP=addr,...`, headless `--mem-dump MS:FILE`.

## In progress when the session ended: frame pacing and FPS

The user saw the game "frame skipping, as if double". Measured: the recompiled game runs on an unlimited CPU, so in
each ~100 ms it does 2-3 updates and 2 frames within a few ms (the first frame is never seen), then waits.
Fix: the emulated CPU gets the speed of GOG's DOSBox (`cycles=8000`):

- `tools/recomp.py` emits `RT_CYC(n)` (instructions of each run) and `gen/` was regenerated; `rt_cycles` in
  `src/rt/recomp.h`; `cpu_speed_limit()` in `rt_poll` (`src/rt/core.c`); `rt_set_cpu_speed()` in `host.h`.
- The window sets 8000 (`[game] cycles` in `Syndicate64.ini`); headless is unlimited unless `--cycles N`.
- Measured after: frames evenly spaced, about 55 ms apart (was pairs 5 ms apart then ~90 ms gaps).
- FPS counter at the top right (`rt_set_show_fps`, the game's different frames a second), on in the window
  (system menu "Show frames a second", `[video] fps`), headless `--fps`.

To finish: run the tests, check the window (FPS readable, motion smooth), compare the feel with DOSBox and tune the
cycles if needed, update README.md (pacing and FPS), commit.

## Smoothness (the user's top complaint)

The user finds the game not smooth, much less than FreeSynd ("very low frame rate"). Measured (headless, real
clock, mission frames that differ from the one before): the original engine draws the map at about 10-18 frames a
second whatever the CPU speed (unlimited: ~18 drawn but in pairs 5 ms apart, so ~10 seen; 8000: ~16, mostly even;
16000: ~10, even; 30000: ~18 with pairs). The measurement is noisy (a still scene counts few frames). No CPU speed
reaches FreeSynd's smoothness: FreeSynd draws at the display's rate with its own renderer. So the first goal of
the new renderer is smoothness: draw at the display's rate (60+), placing people, vehicles and the view between
the game's updates (interpolate their positions from the level data between two ticks of the mission).

## Done since: the smooth city (renderer v1)

`src/game/render.c` draws the mission's city from the game's data in memory: 99.7% the same pixels as the game
(`SYND_RENDER_TEST=match` measures it, `=show` shows it). In missions it now replaces the game's map in each frame
(`smooth_city` in `src/game/additions.c`): positions and the view interpolated between the last two mission ticks
(`render_tick` at each tick of `on_mission_tick`), one tick behind; the game's pointer (a box of +-96 px around
the mouse; the game's mouse y + 40) and the squad's numbers are copied from its frame where it differs from the
city as it is now; the hats are coloured in the renderer. Measured: ~46 different frames a second while agents
walk, gaps 22 ms on average. All six programs (99.7% match in Syndicate, 99.6% in American Revolt; vehicles are
on the map with +0xa = 4 or 0x0c); `SYND_RENDER=off` turns it off.

The core makes a frame for each refresh of the display (`rt_set_frame_rate`, the window reads the refresh rate).

To do next: hear back from the user on how it feels in the window; the animation frames still change
only at the game's ticks (walk cycles); then widescreen and zoom (the renderer can draw any rectangle at any
view), lighting, see-inside-buildings.

## Next: group 4, the new renderer (fresh MIT code)

Plan: (1) read the game's decompressed data from memory: in English Syndicate the pointers are at `0x44535c` (map,
MAPxx.DAT), `0x445360` (tiles, HBLK01.DAT), `0x445330` (sprites, HSPR-0.DAT); level data at `0x470108` (people
`0x478110`, 0x5c each; statics `0x47e790`, 30 bytes each; layout as FreeSynd's `leveldata.h`); camera words at
`0x470000`; world-to-frame: `dx = x - camx*128, dy = y - camy*128, fx = (dx - dy)/8 + 128,
fy = ((dx + dy)/2 - z)/8 + 56`. (2) A reference renderer checked pixel for pixel against the game's frame.
(3) Widescreen and zoom on the GPU, with clicks mapped back into the game. (4) Lighting, time of day,
see-inside-buildings (hold B), traffic-light glow, light editor.

Also open: the 16 extra company colours and a colour/logo picker (now possible in true colour); the blood spray's
hit detection was not checked on a real hit; the macOS front end has no menu items for the new features and was not
built here.
