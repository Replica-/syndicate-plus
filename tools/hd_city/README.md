# tools/hd_city: the HD texture pack (Mission 1's tiles, cyberpunk look)

The game loads the pack from `hd` beside the saves (`src/game/render.c`, README: *HD textures*).  These scripts make
it from your own copy of the game; nothing of the game, and no pack, is in this repository.  Work files go in a
folder of your choice (`HD` below; `out/hd_work` is ignored by git).

Needs [Fooocus](https://github.com/lllyasviel/Fooocus) 2.5 (SDXL, its own `python_embeded`) and
[DeepBump](https://github.com/HugoTini/DeepBump) (its own venv); the steps that say "Fooocus" run with Fooocus's
Python, "DeepBump" with DeepBump's, the others with any Python 3 with Pillow and NumPy.

1. The tiles: `synd_headless ... --gpu 1280x720 --dump-tiles tiles.bin` with input that goes into Mission 1 (as
   `tests/run_tests.py`'s `TO_MISSION`).  The city palettes: `unrnc.c` (built with `src/game/rnc.c`, e.g.
   `cl unrnc.c ..\..\src\game\rnc.c /I..\..\src\game`) unpacks `SYNDICAT/DATA/HPAL02.DAT` (Mission 1's) and
   `HPAL01.DAT` (the pack's look) to `pal/`.
2. `export.py tiles.bin pal/HPAL02.bin pal/HPAL01.bin HD mission1_tiles.txt` - each tile Mission 1 uses, in HPAL01's
   colours: `HD/base/NNN.png`, and contact sheets.
3. `redraw2.py HD` (Fooocus) - every tile redrawn img2img with its description (`desc.py`), in a cyberpunk look:
   `HD/out/NNN.png` (`redraw.py`: the first version, its colours locked to the palette's).
4. `faithful.py HD` (Fooocus) - roads and pavements as the game draws them (ESRGAN on the tile among copies of
   itself, a fine grain over it), their markings in the palette's colours (`roadgrey.py`, which greyed them, is no
   longer run); `paving.py HD` (after step 7) - the pavements' tops as paving stones on the ground's grid, in the game's
   colours, with their own relief.
5. `materials.py HD` (Fooocus; the dirt as dirt and fine gravel, made smaller and repeated), `procedural.py HD`,
   `grade.py HD` (the dirt in the game's olive soil and slate stones) - the ground materials, seamless 1024 x 1024
   (`HD/mat/graded_KK.png`); `matnormals.py HD` (DeepBump) their normals; then `water.py HD` - the water's
   ripples (material 11, drawn by the game's shader: black, reflecting the sky and the sun).
6. `mark.py HD` - each ground tile's top marked with its material (alpha 240 + K), the trench's tops as water; roads
   and pavements keep their own tops; the tower bases' pads and the painted shadows and light patches drawn as plain pavement.
7. `normals.py HD` (DeepBump) - each tile's normals, seen among copies of itself (no seam at its edges).
7-. `roadmatch.py HD` (after the normals) - the roads' tops to one colour, their normals' mean lean taken away.
7a. `pavemat.py HD` (Fooocus) - the pavement's slabs redrawn as a seamless material (6 versions), `pavement.py HD`
   the chosen one (4) with lighter grooves as material 7, then its normals (`matnormals.py`); `mark.py` lays it on the
   pavement tiles (`paving.py` is the earlier, drawn version).
7b. `grass.py HD` (after steps 6 and 7) - the grass tiles (210): the grass material on top, HD tufts rising from it,
   dark soil sides.
7c. `sprites.py HD SPRITES --denoise D --tag T id...` (Fooocus) - HD sprites (the trees: 1571, 1572 at 0.5, 1589 at
   0.65), from the sprites the game draws (`SYND_DUMP_SPRITES=SPRITES`, a mission run headless with `--gpu`); the
   picks copied to `HD/sprites/`; `spritecolour.py HD SPRITES id...` gives them the game's colours (the trees' yellow).
7d. `upscale.py HD id...` (Fooocus's ESRGAN) - tiles no redraw covered, 4x as the game draws them (45: pavement with
   floor lights, its top the pavement material, its lights kept; 177: the brick wall, its rows running on from tile
   to tile, where the redraw made each tile a box of its own).
   The other cities: every other tile of the set (133: `export.py` without IDS) upscaled the same way; `mark.py`
   lays the pavement material on their paving's colours (PAVE_ONLY), `grass.py` does their grass blocks (their own
   sides kept).
7e2. `fire.py HD SPRITES` - the burning object's frames (blazes 1756-1759, tongues and smoke 992-1002) drawn in their own outlines.
7e3. `explode.py HD SPRITES` (after `upscale_sprites.py`) - fireballs (1552, 1562-1565) as billowing fire and smoke;
   burnt stumps and wrecks (1575-1578, 1592-1599, 1743-1744) with glowing embers.
7e. `effects.py HD SPRITES` - the bullet impacts (sprites 729-733) drawn in HD: a white-hot core, sparks, embers.
7f. `upscale_sprites.py HD SPRITES id...` (Fooocus's ESRGAN) - sprites 4x as the game draws them: the doors and
   their opening frames (1522-1547), and every other sprite from 450 up (the people, 1-449, keep the game's: their
   colours are swapped as they are drawn).
7g. `crater.py HD` (after `upscale.py HD 209` and its normals) - tile 209 as a blast crater in the pavement, drawn
   from a height field: pit, broken rim, rubble, soot.
8. `install.py HD/out SAVE/hd` - the pack where the game looks for it, the buildings' orange muted on the way, with `tile_lights.ini` (the pack's tile lights:
   the pads' light off).

`tops.py` is an earlier step (one repeating square per tile top) that the materials replaced.  Steps 4, 6 and 7
change `HD/out` in place: run them after the redraw, in that order.
