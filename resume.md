# Resume: porting the freesynd-enhanced features to syndicate-plus

Where the work stands, for the next session. Everything is committed locally on `main`; nothing is pushed
(the remote is Onur BUYUKCAGLAR's repo; never push). See `git log` for the last commit.

## Ground rules from the user

- Never change the AI, mission triggers or gameplay (combat stays as the original). Close-range friendly fire was
  dropped.
- Licence: syndicate-plus stays **MIT**. Never copy or port FreeSynd (GPL v3) code; carry over ideas, behaviour and
  file-format knowledge only, and write fresh code.
- The game is played only in **true colour on the GPU** (no 256-colour limits for anything new).
- **Test headless only, muted. Never open the game window, capture the desktop or post input to a window**
  (it steals focus). Window-only features: build, test headless, then ask the user to look.
- Commit to `main` (no branches). Group 1 (FreeSynd bug fixes the original never had) is skipped.

## Build, run, test

- `_references/dos` is a junction to `D:\development\syndicate\game_data` (GOG install). `gen/` is generated:
  `python tools/gen_all.py` (needs `pip install capstone`), about 40 s.
- `dev\build.cmd` builds into `build-dev\RelWithDebInfo` (Ninja; a change to `src/rt/recomp.h` or `gen/` rebuilds
  everything, about 8 min).
- Tests (18): `SYND_HEADLESS=D:/development/syndicate-plus/build-dev/RelWithDebInfo/synd_headless.exe python
  tests/run_tests.py` (the path must be absolute). `tests/frames.py` `run(..., virtual_step=0)` runs on the real
  clock; `run_tests.TO_MISSION` is the input script into a mission (~27 s).
- Games start with `--start "SYNDICAT\MAIN.EXE /c0 /iirq7 /idma1 /iio220"` (`DATADISK\...` for American Revolt).
- VS Code: `.vscode/launch.json` has the launcher, both games, both with `--cheats "MINIGUN SQUAD:TO THE TOP"`
  (all missions, $100M, v3 mods, miniguns) and a headless recorded mission.
- Measuring timing headless: temporarily add an `rt_log` under a `getenv("SYND_..._TRACE")` check, run with
  `virtual_step=0`, parse the log, then remove it (scratchpad scripts for this were `ticks.py`, `playsim.py`).

## How additions hook the game

`RT_ENTER` in every recompiled function checks a bitmap (`src/game/hooks.c`); `additions_install()` in
`src/game/additions.c` adds the hooks of each program from one address table per program (English/Spanish/German
Syndicate and American Revolt). `tools/find_equiv.py` finds a function and its data addresses in the other
programs. `guest_call(fn, args, n)` calls game functions. Dev aids: `SYND_CENSUS=FILE`, `SYND_SKIP=addr,...`,
headless `--mem-dump MS:FILE`, `SYND_RENDER_TEST=match|show`, `SYND_RENDER=off`.

## Done

Groups 2 and 3: music from files (`src/rt/music.c`), volumes, video recording (F12, ffmpeg), screenshots (F11),
auto saves (lines 1-3), MINIGUN SQUAD, `--cheats`, WASD scrolling, double-press 1-4 to centre, scanner extras,
move-order marker, solid selected-agent box, hats in company colours, blood sprays, true-colour frames
(`host_present` takes 0x00RRGGBB; `game_overlay` draws over them), Direct3D 11 presentation, FPS counter (top right,
counts frames that differ from the one before; a dip = the picture stalled).

Group 4 so far, the renderer (`src/game/render.c`, fresh code, 99.7% the same pixels as the game):

- **Smooth city:** in missions the city is drawn from the game's data in memory at the display's rate, things
  placed between the game's ticks on a playback clock (snapshots of position, frame and on-map byte at each tick).
- **Pacing:** frames at most 60 a second, each made 5 ms before the refresh that shows it (`next_frame_time` in
  `src/rt/core.c`, DWM timing); on displays over 60 Hz frames keep to every k-th refresh (it was ~90 a second at
  120 Hz). Window CPU speed default 60000 cycles (8000 gave only 10 updates a second).
- **Free view:** the view is ours (`move_view` every frame; `sync_camera` gives the game the nearest camera at each
  tick; `game_mouse_filter` maps clicks).
- **GPU city (`3e28467`):** with Direct3D the tiles and sprites go to the platform as a list of images
  (`host_city_*` in `host.h`), packed in an R8 palette-index atlas (`win32_gpu.c`) and drawn at their exact
  sub-pixel places at the window's resolution with sharp bilinear filtering; the frame is see-through over the map
  (top byte of its pixels: 255 = city shows). Pointer, labels, marker and blood stay in the frame (`plot` blends
  over see-through pixels). Videos, screenshots (taken from the next CPU-made frame) and the CRT filter use the CPU
  city. Headless and macOS have stubs (`host_city_gpu` returns 0). Checked in the window: correct, scrolls.
- **Steadier motion (`baa31b6`):** the game's ticks come in pairs (one VGA retrace apart) then ~85 ms of nothing,
  and the rate wanders 19-23 a second. Playback is now 3.5 ticks behind (~120 ms), rate over the last 80 ticks,
  continuous target, filtered speed correction (gain 0.03 per tick off, at most 10%). Measured: 91% of frames
  within 10% of the median speed; still ~16 stalled frames in 10 s (the game's own hiccups).
- **Squad numbers:** found in the game's picture by their own digit sprite (1070 + 2 x slot yellow, +1 red) within
  12 px, holding 0.4 s when not found. The selected agents' numbers blink yellow/red: that is the game's own blink
  (irregular because the game draws ~10 times a second).

## Done: steady simulation (our clock drives the mission's ticks)

The mission loop is `0x4001e0` (Syndicate); each pass is one tick, and its wait is in `frame_wait` (`0x408810`,
American Revolt `0x408700`): it waits until the game's timer interrupt (handler `0x417a90`) has counted `speed`
(`0x445304`, 7; the game's keys change it 3..12) interrupts in `wait_count` (`0x450b50`), but skips the wait when
the count is still 0, so on a fast CPU the ticks came in pairs. Timers: Syndicate ~72 Hz (10 ticks/s as designed,
~20 with the pairs), American Revolt 120 Hz (17/s). `on_frame_wait` (additions.c) now starts each tick on a steady
clock, `speed` PACE_HZ-ths of a second apart (PACE_HZ 84: 12 ticks/s, the user's choice; FreeSynd's 256 units/s would be
120, which the user found too fast; Syndicate's designed rate is 10.4/s, PACE_HZ 73), waiting with `rt_poll()` so frames and sound go on, then sets
the count so the game's own wait passes. Snapshots are stamped with the slot time; the playback runs 2 ticks behind.
Measured: 100% of frames within 10% of the same speed, 0 stalls (was 66%, 7). `SYND_PACE=off` gives the game's own
pace. Addresses for all six programs are in the GameAddrs table (frame_wait, speed, wait_count).

## Done: widescreen and zoom

In missions the frame is as wide as the window shows a 480-line picture (`host_canvas_width`, 640..1280; headless
`--wide W`): the game's 640 x 480 at the left, the city filling the rest (`game_canvas_width` in additions.c,
`present` in core.c). The wheel zooms the city 0.5x..1.5x about the middle of the map part (`game_wheel`,
`render_set_zoom`; the GPU scales its quads, the CPU path draws nearest-pixel). The game's camera is kept with its
map window's middle at this picture's middle (`centre_shift`) and nudged at each tick so the city under the
pointer is inside that window; `game_mouse_filter` gives the game that point (`unzoom`, `game_point`). The camera
stays within the mission's limits (view + 0x1be32: the playable area, not the map's size), so a click beyond them
(seen when wide or zoomed out at an edge) goes to the nearest point the game takes. The squad numbers come from
the game's state (its routine 0x40dcb0: frames at label_frames, shown when +0x1d bit 2 and not +0xb bit 0), drawn
at the zoomed place at their own size. Recordings letterbox wide frames. Test: `wide_zoom` (19 tests).

## Done: lighting, see inside, light editor, screenshots, GPU tests

- Headless `--gpu WxH` renders through Direct3D offscreen (`gpu_init(NULL)`, hardware else WARP) and dumps that
  picture; `--time day|dusk|night`, `--screenshot MS:FILE`. Test `gpu_night`.
- Time of day (window menu, `[video] time`): at dusk/night the city goes into a scene texture + an emissive one
  (MRT), a light map (ambient + additive soft ellipses), composited under the frame. Lights: lamps found in tiles
  (a small spot of very bright pixels in the mission palette), tiles' settings (defaults = the fork's
  tile_lights.ini data, overridden by `tile_lights.ini` in the user folder), shot flashes (the effects list), car
  headlights (direction from motion).
- Hold B: tiles above the squad's floor (highest agent z/128 - 1) not drawn (`render_set_cut`).
- Light editor: Ctrl+L in a mission; G/L/[/]/C on the tile under the pointer; saved at once.
- Screenshots in GPU mode capture the presented picture (`gpu_capture`); PNGs keep their shape.

## Still to do after that

Traffic lights glowing (find their tiles with the light editor), a walkable outline when B is held, sun shadows by
day; the 16 extra company
colours with a colour/logo picker; instant click feedback (optional); improved path finding (the user asked for it
to be on the list; it changes how agents move, so say what it would change and ask before doing it). Loose ends: blood spray on a real hit and the
corpse fix on a real death are unchecked; the macOS front end has no menu items for the new features and was not
built here; README.md needs a note on the GPU city and the playback clock.
