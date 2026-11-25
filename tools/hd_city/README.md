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
8. `install.py HD/out SAVE/hd` - the pack where the game looks for it, with `tile_lights.ini` (the pack's tile lights:
   the pads' light off).

`tops.py` is an earlier step (one repeating square per tile top) that the materials replaced.  Steps 4, 6 and 7
change `HD/out` in place: run them after the redraw, in that order.
