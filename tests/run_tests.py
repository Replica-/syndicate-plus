"""Automated game tests: play the games headless and check what they show, write and play.

    python tests/run_tests.py            all tests
    python tests/run_tests.py --quick    without the slow ones (the full intro)
    python tests/run_tests.py save_load  only the named tests
    python tests/run_tests.py --list

Every run uses the virtual clock (see frames.run), so a run repeats exactly and inputs land on the same frame
each time.  The frames, logs and user folder of each test go to out/tests/<test>/; a failed check names the
frame to look at.  Needs build/synd_headless(.exe) and the game files in _references/dos (the built-in-files
test also needs build/syndgame.pak, which the private build writes).

Mouse positions in inputs are 0..65535 across the screen (x;y), whatever the game's resolution.
"""
import concurrent.futures
import math
import os
import shutil
import struct
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import frames  # noqa: E402

OUT = os.path.join(frames.ROOT, 'out', 'tests')
PACK = os.path.join(frames.ROOT, 'build', 'syndgame.pak')

SYN = r'SYNDICAT\MAIN.EXE /c0 /s'
SKIP_INTRO = ['3000:esc', '7000:esc']                    # the intro, then the title screen
LINE4 = 29800                                            # line 4 of the Load/Save screen (lines are 12 pixels apart)

# Syndicate, from the main menu to a mission with agent 3 selected (about 27 s in)
TO_MISSION = SKIP_INTRO + [
    '10000:move@29500;28800', '11000:click@29500;28800',   # main menu: begin mission -> world map
    '14000:click@8500;58500',                               # world map: brief
    '17000:click@8500;58700',                               # brief: accept -> team selection
    '20000:click@8500;58700',                               # team selection: accept -> the mission
    '26000:click@6550;18000',                               # select agent 3
]


class Failure(Exception):
    pass


def check(cond, text):
    if not cond:
        raise Failure(text)


TESTS = []


def test(slow=False):
    def wrap(fn):
        TESTS.append((fn.__name__, fn, slow))
        return fn
    return wrap


class Ctx:
    """one test's folder: runs go to subfolders of it, each with its own frames and log"""

    def __init__(self, name):
        self.dir = os.path.join(OUT, name)
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        self.user = os.path.join(self.dir, 'user')

    def run(self, name, seconds, start=None, inputs=(), every=1000, user=None, extra=(), codes=(0,)):
        user = user or self.user
        rc, fr, log = frames.run(start=start, seconds=seconds, inputs=list(inputs),
                                 dump_dir=os.path.join(self.dir, name), dump_every=every, user_dir=user,
                                 extra=extra, log=os.path.join(self.dir, name + '.log'))
        check(rc in codes, '%s: exit code %d (see %s.log)' % (name, rc, name))
        return Run(name, fr, log)


class Run:
    def __init__(self, name, fr, log):
        self.name, self.frames, self.log = name, fr, log

    def ran(self, program):
        return ('run D:\\' + program) in self.log

    def at(self, ms):
        """the first frame dumped at or after `ms`"""
        for t, p in self.frames:
            if t >= ms:
                f = frames.read_bmp(p)
                f.path = p
                return f
        raise Failure('%s: no frame at %d ms (the run has %d frames)' % (self.name, ms, len(self.frames)))

    def last(self):
        return self.at(self.frames[-1][0]) if self.frames else self.at(0)


def colour_mean(f, x0=0.0, y0=0.0, x1=1.0, y1=1.0):
    a, b, c, d = int(x0 * f.w), int(x1 * f.w), int(y0 * f.h), int(y1 * f.h)
    tot = [0, 0, 0]
    for y in range(c, d):
        row = f.pix[y * f.w + a:y * f.w + b]
        for p in row:
            r, g, bl = (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF
            tot[0] += r
            tot[1] += g
            tot[2] += bl
    n = max(1, (b - a) * (d - c))
    return [t / n for t in tot]


def is_green_screen(f):
    """the menus of Syndicate are green on black"""
    r, g, b = colour_mean(f)
    return g > 20 and g > 1.6 * r and g > 1.6 * b


def is_world_map(f):
    """the world map: countries in orange, blue, white and so on over the green screen"""
    n = 0
    for p in f.pix:
        r, g, b = (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF
        if r > g + 30 or b > g + 30:
            n += 1
    return n > 0.08 * len(f.pix)


def same_pixels(f1, f2):
    return f1.w == f2.w and f1.h == f2.h and f1.pix == f2.pix


def name(f):
    return os.path.relpath(getattr(f, 'path', '?'), OUT)


# ---- the launcher ----

@test()
def launcher(ctx):
    """SYND.EXE shows the two games"""
    r = ctx.run('first', 6)
    check(r.ran('SYND.EXE'), 'SYND.EXE did not start')
    f = r.last()
    check((f.w, f.h) == (320, 200), 'launcher frame is %dx%d' % (f.w, f.h))
    check(f.colour_count() > 64 and f.mean_brightness() > 15, 'launcher looks empty: ' + name(f))


@test()
def launcher_syndicate(ctx):
    """Enter in the launcher starts Syndicate and its intro"""
    r = ctx.run('first', 8, inputs=['4000:enter'])
    check(r.ran(r'SYNDICAT\MAIN.EXE'), 'SYNDICAT\\MAIN.EXE did not start')
    check(r.ran(r'SYNDICAT\INTRO.EXE'), 'the intro did not start')


@test()
def launcher_revolt(ctx):
    """Right, Enter in the launcher starts American Revolt and its intro"""
    r = ctx.run('first', 8, inputs=['2500:right', '4000:enter'])
    check(r.ran(r'DATADISK\MAIN.EXE'), 'DATADISK\\MAIN.EXE did not start')
    check(r.ran(r'DATADISK\INTRO.EXE'), 'the intro did not start')


def launcher_language(ctx, prefix, lang):
    """the installer writes the program name for the language to SYND\\SYND.INF; the launcher follows it"""
    os.makedirs(os.path.join(ctx.user, 'C', 'SYND'))
    with open(os.path.join(ctx.user, 'C', 'SYND', 'SYND.INF'), 'w') as f:
        f.write(prefix + 'main /c0 /iirq7 /idma1 /iio220')
    r = ctx.run('run', 8, inputs=['2500:right', '4000:enter'])
    check(r.ran('DATADISK\\%sMAIN.EXE' % prefix.upper()), 'American Revolt (%s) did not start' % lang)
    check(r.ran('DATADISK\\%sINTRO.EXE' % prefix.upper()), 'the %s intro did not start' % lang)


@test()
def launcher_german(ctx):
    """with "gmain" in SYND.INF the launcher starts the German programs"""
    launcher_language(ctx, 'g', 'German')


@test()
def launcher_spanish(ctx):
    """with "smain" in SYND.INF the launcher starts the Spanish programs"""
    launcher_language(ctx, 's', 'Spanish')


# ---- the main menus, in every language ----

def main_menus(ctx, folder):
    titles = {}
    for prefix in ('', 'G', 'S'):
        r = ctx.run('menu' + (prefix or 'E'), 14, start='%s\\%sMAIN.EXE /c0 /s' % (folder, prefix),
                    inputs=SKIP_INTRO)
        check(r.ran('%s\\%sINTRO.EXE' % (folder, prefix)), '%s\\%sINTRO.EXE did not start' % (folder, prefix))
        f = r.last()
        check(is_green_screen(f), 'no main menu: ' + name(f))
        titles[prefix] = f.region_signature(0.2, 0.05, 0.8, 0.17)
    check(len(set(titles.values())) == 3, 'the menu titles are not different in the three languages')


@test()
def menus_syndicate(ctx):
    """Syndicate in English, German and Spanish reaches its main menu, each with its own title"""
    main_menus(ctx, 'SYNDICAT')


@test()
def menus_revolt(ctx):
    """American Revolt in English, German and Spanish reaches its main menu"""
    main_menus(ctx, 'DATADISK')


# ---- playing ----

@test()
def mission(ctx):
    """into a mission; an agent walks where it is sent; Esc withdraws to the debriefing; then the world map"""
    walk = TO_MISSION + ['27000:click@36000;42000', '31000:esc', '35000:enter']
    r = ctx.run('first', 40, start=SYN, inputs=walk)
    f0, f1 = r.at(27000), r.at(30000)
    check((f0.w, f0.h) == (640, 480), 'the mission is not on screen at 27 s: ' + name(f0))
    # the agent walks across the middle of the view
    check(f0.region_signature(0.45, 0.4, 0.75, 0.75) != f1.region_signature(0.45, 0.4, 0.75, 0.75),
          'nothing moved in the mission between %s and %s' % (name(f0), name(f1)))
    d = r.at(33500)
    check(is_green_screen(d), 'no debriefing after Esc: ' + name(d))
    m = r.last()
    check(is_world_map(m) and not is_world_map(d), 'no world map after the debriefing: ' + name(m))


@test()
def combat(ctx):
    """agent 3 draws the pistol and fires: the debriefing counts the shots ("0%" instead of "no bullets fired")"""
    calm = ctx.run('calm', 37, start=SYN, inputs=TO_MISSION + ['31000:esc'])
    fire = ctx.run('fire', 37, start=SYN, inputs=TO_MISSION + [
        '28000:click@1640;35500',                               # the pistol
        '29000:move@58368;42600', '29500:rclick@58368;42600', '30500:rclick@58368;42600',
        '31500:rclick@58368;42600', '34000:esc'])
    a, b = calm.last(), fire.last()
    check(is_green_screen(a) and is_green_screen(b), 'no debriefing: %s %s' % (name(a), name(b)))
    acc = (0.5, 0.635, 0.95, 0.685)
    check(a.region_signature(*acc) != b.region_signature(*acc),
          'the hit accuracy line is the same with and without shooting: %s %s' % (name(a), name(b)))


@test()
def mission_keys(ctx):
    """in a mission D scrolls the view as the right arrow key does, and 2 pressed twice brings it back to agent 2"""
    r = ctx.run('keys', 34, start=SYN, inputs=TO_MISSION + ['28000:d:down', '30000:d:up', '31000:2', '31200:2'])
    start, moved, back = r.at(27000), r.at(30500), r.at(33000)

    def alike(f, g):
        """the share of the pixels of the map view that are the same (the agents move a little)"""
        same = total = 0
        for y in range(int(f.h * 0.1), int(f.h * 0.9)):
            for x in range(int(f.w * 0.25), int(f.w * 0.95)):
                total += 1
                same += f.pix[y * f.w + x] == g.pix[y * g.w + x]
        return same / total
    check(alike(start, moved) < 0.5, 'D did not scroll: ' + name(moved))
    check(alike(start, back) > 0.9, '2 2 did not come back: ' + name(back))


@test()
def wide_zoom(ctx):
    """a 16:9 mission (854 wide) zoomed out with the wheel: the city fills the width, and a click on it beyond the
    game's own map window sends the agent there (the move marker appears under the pointer)"""
    w, cx, cy = 854, 560, 240
    fx, fy = cx * 65536 // w, (cy - 40) * 65536 // 400
    r = ctx.run('wide', 29, start=SYN, every=100, extra=['--wide', str(w)],
                inputs=TO_MISSION + ['27000:wheel@-2', '27500:move@%d;%d' % (fx, fy), '28000:click@%d;%d' % (fx, fy)])
    f = r.at(27900)
    check(f.w == w, 'the mission is not %d wide: %s' % (w, name(f)))
    check(colour_mean(f, 0.8, 0.2, 1.0, 0.8) != colour_mean(f, 0.8, 0.0, 1.0, 0.05), 'no city at the right: ' + name(f))
    marker = []
    for t in range(28100, 28900, 100):
        g = r.at(t)
        # its green (it fades out over what is under it)
        marker += [(i % g.w, i // g.w) for i, p in enumerate(g.pix)
                   if (p >> 8) & 0xFF > 180 and (p >> 8) & 0xFF > ((p >> 16) & 0xFF) + 70 and (p >> 8) & 0xFF > (p & 0xFF) + 70
                   and i % g.w >= 128]
    check(marker, 'no move marker after the click')
    mx = sum(p[0] for p in marker) / len(marker)
    my = sum(p[1] for p in marker) / len(marker)
    check(abs(mx - cx) < 8 and abs(my - cy) < 8, 'the marker is at %d, %d, not under the click at %d, %d' % (mx, my, cx, cy))


@test()
def gpu_night(ctx):
    """(Windows) the picture as Direct3D makes it, offscreen: at night the city is darker than by day but its lamps
    still light it, and a screenshot is the picture as the window shows it"""
    if os.name != 'nt':
        return
    shot = os.path.join(ctx.dir, 'shot.png')
    day = ctx.run('day', 27.5, start=SYN, extra=['--gpu', '1280x720', '--time', 'day'], inputs=TO_MISSION)
    night = ctx.run('night', 27.5, start=SYN, extra=['--gpu', '1280x720', '--time', 'night', '--screenshot', '27000:' + shot],
                    inputs=TO_MISSION)
    d, n = day.at(27000), night.at(27000)
    check((d.w, d.h) == (1280, 720), 'the GPU picture is not 1280 x 720: ' + name(d))

    def light(f, x0, y0, x1, y1):
        return sum(colour_mean(f, x0, y0, x1, y1)) / 3
    city = (0.3, 0.2, 0.95, 0.9)
    check(light(n, *city) < light(d, *city) * 0.6, 'night is not darker than day: %s %s' % (name(d), name(n)))
    # the lit pads (top left of the city) are much brighter than the dark ground
    check(light(n, 0.25, 0.1, 0.45, 0.3) > light(n, 0.6, 0.5, 0.9, 0.8) * 2, 'no lamps lit at night: ' + name(n))
    check(os.path.exists(shot), 'no screenshot ' + shot)


@test()
def company_colour(ctx):
    """a company colour chosen in the window's menu (headless --colour): the squad's hats are drawn in it"""
    r = ctx.run('colour', 27, start=SYN, every=250, extra=['--colour', '00E5FF'], inputs=TO_MISSION + ['26200:wheel@3'])
    f = r.at(26700)
    cyan = sum(1 for p in f.pix if p & 0xFFFFFF == 0x00E5FF)
    check(cyan >= 4, 'no hats in the chosen colour: ' + name(f))


@test()
def squad_route(ctx):
    """an agent sent across the trench of the first mission, away from its bridge, is walked over the bridge (a
    route) and gets there"""
    import struct
    dump = os.path.join(ctx.dir, 'mem.bin')
    w, cx, cy = 854, 330, 90
    fx, fy = cx * 65536 // w, (cy - 40) * 65536 // 400
    ctx.run('route', 60, start=SYN, extra=['--wide', str(w), '--mem-dump', '59500:' + dump],
            inputs=TO_MISSION + ['26300:wheel@-3', '27000:move@%d;%d' % (fx, fy), '27600:click@%d;%d' % (fx, fy)])
    check(os.path.exists(dump), 'no memory dump')
    mem = open(dump, 'rb').read()
    ped = 0x478110 + 3 * 0x5c - 0x400000   # the fourth agent of the squad (selected by the clicks of TO_MISSION)
    pos, target = struct.unpack_from('<hh', mem, ped + 4), struct.unpack_from('<hh', mem, ped + 0x2e)
    check(target != (0, 0) and abs(pos[0] - target[0]) < 64 and abs(pos[1] - target[1]) < 64,
          'the agent is at %s, not at %s' % (pos, target))


@test()
def save_load(ctx):
    """save a game named ABC in line 4 (1-3 hold the auto saves), then load it in a new run: the funds stored in the
    save show up"""
    save = ctx.run('first', 19, start=SYN, inputs=SKIP_INTRO + [
        '10000:f3', '12000:click@19600;%d' % LINE4,              # load/save, line 4
        '13000:del', '13300:del', '13600:del', '13900:del', '14200:del',   # "EMPTY"
        '14500:a', '14800:b', '15100:c', '15500:enter',
        '16500:click@19600;58700'])                             # SAVE
    path = os.path.join(ctx.user, 'Syndicate', '03.GAM')                   # the game's C:\SYND\SAVE
    check(os.path.exists(path), 'no save file Syndicate/03.GAM')
    data = bytearray(open(path, 'rb').read())
    check(data[:4] == b'ABC\0', 'the save is named %r' % bytes(data[:8]))
    check(struct.unpack_from('<I', data, 0x14)[0] == 30000, 'the funds in the save are not 30000')
    struct.pack_into('<I', data, 0x14, 12345)
    with open(path, 'wb') as f:
        f.write(data)
    team = ['click@29500;28800', 'click@8500;58500', 'click@8500;58700']   # map, brief, accept
    fresh = ctx.run('fresh', 24, start=SYN, user=os.path.join(ctx.dir, 'fresh_user'),
                    inputs=SKIP_INTRO + ['%d:%s' % (t, e) for t, e in zip((10000, 14000, 17000), team)])
    loaded = ctx.run('loaded', 30, start=SYN, inputs=SKIP_INTRO + [
        '10000:f3', '12000:click@19600;%d' % LINE4, '13000:click@8500;58700'] +   # line 4, LOAD
        ['%d:%s' % (t, e) for t, e in zip((16000, 20000, 23000), team)])
    a, b = fresh.last(), loaded.last()
    funds = (0.76, 0.19, 0.97, 0.26)
    check(a.region_signature(0.2, 0.0, 0.8, 0.12) == b.region_signature(0.2, 0.0, 0.8, 0.12),
          'not the team selection in both runs: %s %s' % (name(a), name(b)))
    check(a.region_signature(*funds) != b.region_signature(*funds),
          'the loaded funds look the same as a new game\'s: %s %s' % (name(a), name(b)))


@test()
def auto_save(ctx):
    """after a mission the world map saves the game in line 1 ("AUTO ..."); a save of the player found there moves
    to line 4, and a save the player makes in line 1 goes to line 4 too"""
    ctx.run('player', 19, start=SYN, inputs=SKIP_INTRO + [
        '10000:f3', '12000:click@19600;18000', '13000:x', '15500:enter', '16500:click@19600;58700'])  # line 1
    folder = os.path.join(ctx.user, 'Syndicate')
    check(sorted(os.listdir(folder)) == ['03.GAM'], 'the save in line 1 is not in line 4: %s' % os.listdir(folder))
    os.rename(os.path.join(folder, '03.GAM'), os.path.join(folder, '00.GAM'))   # as saved before auto saves
    ctx.run('mission', 40, start=SYN, inputs=TO_MISSION + ['27000:click@36000;42000', '31000:esc', '35000:enter'])
    check(sorted(os.listdir(folder)) == ['00.GAM', '03.GAM'], 'the saves are %s' % os.listdir(folder))
    with open(os.path.join(folder, '00.GAM'), 'rb') as f:
        auto = f.read()
    with open(os.path.join(folder, '03.GAM'), 'rb') as f:
        moved = f.read()
    check(auto[:5] == b'AUTO ' and len(auto) == 28247, 'line 1 is not an auto save: %r' % auto[:20])
    check(moved[:1] == b'X', 'line 4 is not the save of the player: %r' % moved[:20])


@test()
def minigun_squad(ctx):
    """MINIGUN SQUAD as the company name (here in a save, cheats apply when a game is loaded): all 18 agents get
    every mod at version 3 and 8 miniguns, and the funds stay"""
    ctx.run('save', 19, start=SYN, inputs=SKIP_INTRO + [
        '10000:f3', '12000:click@19600;%d' % LINE4, '13000:x', '15500:enter', '16500:click@19600;58700'])
    path = os.path.join(ctx.user, 'Syndicate', '03.GAM')
    data = bytearray(open(path, 'rb').read())
    data[20 + 0x11:20 + 0x11 + 14] = b'MINIGUN SQUAD\0'                # the company name
    with open(path, 'wb') as f:
        f.write(data)
    ctx.run('cheat', 24, start=SYN, inputs=SKIP_INTRO + [
        '10000:f3', '12000:click@19600;%d' % LINE4, '13000:click@8500;58700',                     # load line 4
        '16000:f3', '18000:click@19600;%d' % (LINE4 + 3930), '19000:z', '20500:enter', '21500:click@19600;58700'])
    with open(os.path.join(ctx.user, 'Syndicate', '04.GAM'), 'rb') as f:
        comp = f.read()[20:]
    for a in range(18):
        o = 0x11d + a * 40
        mods = struct.unpack_from('<H', comp, o + 3)[0]
        weapons = [struct.unpack_from('<H', comp, o + 10 + 4 * k)[0] for k in range(8)]
        check(mods & 0x1ffe == 0x1ffe, 'agent %d has the mods %04x' % (a, mods))
        check(weapons == [6] * 8, 'agent %d has the weapons %s' % (a, weapons))
    check(struct.unpack_from('<I', comp, 0)[0] == 30000, 'the funds changed')


@test()
def save_revolt(ctx):
    """American Revolt saves into a folder of its own, apart from Syndicate's (both name their saves 00.GAM...);
    a save in line 1 goes to line 4, as line 1 holds an auto save"""
    ctx.run('first', 19, start=r'DATADISK\MAIN.EXE /c0 /s', inputs=SKIP_INTRO + [
        '10000:f3', '12000:click@19600;18000', '13000:a', '15500:enter', '16500:click@19600;58700'])
    check(os.path.exists(os.path.join(ctx.user, 'American Revolt', '03.GAM')), 'no save file American Revolt/03.GAM')
    check(not os.path.exists(os.path.join(ctx.user, 'Syndicate')), 'American Revolt wrote into Syndicate/')


# ---- sound, built-in files, repeatability ----

@test()
def audio(ctx):
    """the intro music is not silent"""
    wav = os.path.join(ctx.dir, 'intro.wav')
    ctx.run('first', 12, start=SYN, extra=['--wav', wav])
    with open(wav, 'rb') as f:
        d = f.read()
    check(len(d) > 44 + 4 * 44100 * 5, 'the WAV holds %d bytes' % len(d))
    n = (len(d) - 44) // 2
    s = struct.unpack_from('<%dh' % n, d, 44)
    tail = s[4 * 44100 * 2:]                                    # after the first 4 seconds (stereo)
    rms = math.sqrt(sum(v * v for v in tail[::7]) / max(1, len(tail[::7])))
    check(rms > 300, 'the intro is (nearly) silent: RMS %.0f' % rms)


@test()
def builtin_files(ctx):
    """the game files read from the pack (as Syndicate64.exe reads its built-in ones) give the same game"""
    if not os.path.exists(PACK):
        print('    (no %s: skipped)' % os.path.relpath(PACK, frames.ROOT))
        return
    inputs = TO_MISSION + ['27000:click@36000;42000']
    a = ctx.run('folder', 30, start=SYN, inputs=inputs, user=os.path.join(ctx.dir, 'u1'))
    b = ctx.run('pack', 30, start=SYN, inputs=inputs, user=os.path.join(ctx.dir, 'u2'), extra=['--builtin', PACK])
    check('game files: built in' in b.log, 'the pack was not used')
    fa, fb = a.last(), b.last()
    check(same_pixels(fa, fb), 'the pack run differs from the folder run: %s %s' % (name(fa), name(fb)))


@test()
def repeatable(ctx):
    """two runs with the same input end on the same picture"""
    inputs = TO_MISSION + ['27000:click@36000;42000']
    a = ctx.run('a', 30, start=SYN, inputs=inputs, user=os.path.join(ctx.dir, 'u1'))
    b = ctx.run('b', 30, start=SYN, inputs=inputs, user=os.path.join(ctx.dir, 'u2'))
    check(same_pixels(a.last(), b.last()), 'the two runs differ: %s %s' % (name(a.last()), name(b.last())))


@test(slow=True)
def intro_length(ctx):
    """the Syndicate intro runs its full length, as on a real PC (about 2 minutes 6 seconds)"""
    r = ctx.run('first', 170, start=r'SYNDICAT\INTRO.EXE', every=5000, codes=(255,))   # the intro ends with 255
    tail = [l for l in r.log.splitlines() if l.startswith('exit ')]
    check(tail, 'no exit line in the log')
    ms = int(tail[-1].split()[3])
    check(120000 <= ms <= 140000, 'the intro took %.1f s' % (ms / 1000))


def main():
    args = sys.argv[1:]
    if '--list' in args:
        for n, fn, slow in TESTS:
            print('%-20s %s%s' % (n, (fn.__doc__ or '').strip().splitlines()[0] if fn.__doc__ else '',
                                  ' (slow)' if slow else ''))
        return 0
    quick = '--quick' in args
    names = [a for a in args if not a.startswith('--')]
    todo = [(n, fn) for n, fn, slow in TESTS if (n in names if names else not (quick and slow))]
    unknown = set(names) - {n for n, _, _ in TESTS}
    if unknown:
        print('unknown tests:', ', '.join(sorted(unknown)))
        return 2
    if not os.path.exists(frames.HEADLESS):
        print('%s is missing: build first' % os.path.relpath(frames.HEADLESS, frames.ROOT))
        return 2

    def one(item):
        n, fn = item
        t = time.time()
        try:
            fn(Ctx(n))
            return n, None, time.time() - t
        except Failure as e:
            return n, str(e), time.time() - t
        except Exception:
            return n, traceback.format_exc(), time.time() - t

    t0 = time.time()
    failed = 0
    workers = max(1, min(len(todo), (os.cpu_count() or 4) // 2))
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        for n, err, secs in pool.map(one, todo):
            print('%-20s %s  (%.0f s)' % (n, 'ok' if err is None else 'FAILED', secs))
            if err:
                failed += 1
                print('    ' + err.replace('\n', '\n    '))
    print('%d of %d passed in %.0f s; frames and logs in %s' % (len(todo) - failed, len(todo), time.time() - t0,
                                                               os.path.relpath(OUT, frames.ROOT)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
