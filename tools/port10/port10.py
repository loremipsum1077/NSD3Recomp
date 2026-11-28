#!/usr/bin/env python3
"""Ports the PvP arenas of the Diablo III 1.0 PC release to 2.7.7, as loose files for the
Loose Files mod (D3_MOD_LOOSE_FILES, src/mods/loose_files.cpp).

1.0 shipped arenas that later versions dropped. Their scenes, models, collision, marker
sets, textures, materials, lights, particles and weather are still on the 1.0 discs; this
converts them from 1.0's 32-bit layouts to 2.7.7's 64-bit ones with the two versions' own
type descriptor tables (textures in texture.py), and builds a 2.7.7 world that places the
scenes and keeps the arena's environment (world.py). Nothing of the game is in this
repository: every input comes from your own copies of the two versions, and the outputs
stay on your computer.

    python3 tools/port10/port10.py setup [--disc build/D3C1.0] [--game game/2.7.7] [--d3 BUILD/d3]
    python3 tools/port10/port10.py list
    python3 tools/port10/port10.py port PvP_Neph_Arena [more arenas] [--out FOLDER]
    python3 tools/port10/port10.py selftest

setup     reads what the conversion needs into build/port10/:
            the 1.0 type tables, from the 1.0 Mac client on the disc (its start-up code
              is run under the Unicorn emulator: pip install unicorn);
            the 2.7.7 type tables, from a build with the Loose Files mod (it is started
              with D3_DUMP_TYPES and quits once it has written them);
            CoreTOC.dat (2.7.7's file list) and a few 2.7.7 files used as templates,
              from game/2.7.7/romfs/CPKs.
list      the arenas on the disc, with their scenes.
port      converts arenas into the loose folder (default save/2.7.7/loose) and adds their
          files to its index.txt. Start one with D3_LOAD_WORLD=<the world's name>.
selftest  converts a marker set that both versions ship (trDun_ButcherLair_Jump) and
          compares it with 2.7.7's own copy.

--disc is the folder holding the 1.0 discs' contents (Tome-1, Tome-2, Tome-3, each with
"Diablo III/Data_D3/PC/MPQs/<archive>.MPQ/<group>/<file>" extracted).
"""
import argparse
import glob
import json
import os
import pathlib
import struct
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check  # noqa: E402
import convert  # noqa: E402
import cpk  # noqa: E402
import texture  # noqa: E402
import world  # noqa: E402

ROOT = HERE.parent.parent
WORK = ROOT / "build" / "port10"

# The groups this ports: 1.0 folder name -> (2.7.7 group number, loose folder, extension,
# root type).
GROUPS = {
    "Scene": (33, "Scene", ".scn", "Scene"),
    "Appearance": (9, "Appearance", ".app", "Appearance"),
    "PhysMesh": (61, "PhysMesh", ".phm", "PhysMesh"),
    "MarkerSet": (24, "MarkerSet", ".mrk", "MarkerSet"),
    "Worlds": (48, "Worlds", ".wrl", "Worlds"),
    "Textures": (44, "Textures", ".tex", "Textures"),  # texture.py, not convert.py
    "Material": (57, "Material", ".mat", "Material#136"),
    "Light": (23, "Light", ".lit", "Light"),
    "Particle": (27, "Particle", ".prt", "Particle"),
    "Weather": (47, "Weather", ".wth", "Weather"),
}
# 2.7.7 files whose headers (version, asset version, layout hash) the converted files get,
# and the world every ported world is built from (the PvP dueling arena's).
SAMPLES = {
    "Scene": "Scene\\PvP_Duel_Small_Multi_E01_N01.scn",
    "Appearance": "Appearance\\PvP_Duel_Small_Multi_E01_N01.app",
    "PhysMesh": "PhysMesh\\PvP_Duel_Small_Multi_E01_N01.phm",
    "MarkerSet": "MarkerSet\\PvP_Duel_Small_Multi_E01_N01.mrk",
    "Worlds": "Worlds\\PvP_Duel_Small.wrl",
    "Textures": "Textures\\PvP_Duel_Small_Multi_W01_S01_worldMask.tex",
    "Material": "Material\\trOut_nephalemTiles_ground_mstr.mat",
    "Light": "Light\\a2dun_Aqd_torch.lit",
    "Particle": "Particle\\a2dun_aqd_TorchFire.prt",
    "Weather": "Weather\\a2dun_Zolt_Weather1.wth",
}
# The 1.0 arena worlds and the scenes each is made of: a name, or a prefix ending in _ for
# tiled arenas (their tiles' names give the layout, world.tile_position).
ARENAS = {
    "PVP_Butcher_Arena_01": ["PvP_Butcher_Arena"],
    "PvP_Cald_Swr_Ver2": ["PvP_Cald_Swr_Ver2"],
    "PvP_caOut_Arena_01": ["PvP_caOut_Arena_01"],
    "PvP_CraterST_Arena": ["PvP_Crater_ST_Arena_"],
    "PvP_Crypt_Arena_01": ["PvP_Crypt_Arena_01"],
    "PvP_Crypt_Arena_02": ["PvP_Crypt_Arena_02"],
    "PvP_Garden_Arena": ["PvP_Garden_Arena"],
    "PvP_Keep_Arena": ["PvP_Keep_Arena"],
    "PvP_Map2": ["PvP_Map2_"],
    "PvP_Maze_Arena_01": ["PvP_Maze_Arena_01_"],
    "PvP_Neph_Arena": ["PvP_Neph_Arena_"],
    "PvP_Octagon_Arena_01": ["PvP_Octogon_Arena_01_"],
    "PVP_Pillar_01": ["PVP_Pillar_01"],
    "PvP_Stairs_Arena_01": ["PVP_Stairs_Arena_01_"],
    "PvP_Zolt_Arena": ["PvP_Zolt_Arena"],
    "PvP_Zolt_Ruins_Arena": ["PvP_Zolt_Ruins_Arena"],
}
NAV_SQUARE = 2.5  # a navmesh square's width in world units
# 1.0 actors the arenas place that 2.7.7 no longer has, and the 2.7.7 actor put in their
# place: 1.0 SNO -> (1.0 name, 2.7.7 Actor name).
STANDINS = {
    4921: ("pvpHealthSpawner", "PVP_HealingWell"),  # 2.7.7's dueling arenas heal with this
}


def say(*a):
    print(*a, flush=True)


# ---------------------------------------------------------------------------------------
# Inputs

class Disc:
    """The 1.0 files of the ported groups, by SNO number and by name."""

    def __init__(self, folder):
        self.folder = pathlib.Path(folder)
        self.by_id, self.by_name = {}, {}
        mpqs = sorted(glob.glob(str(self.folder / "Tome-*" / "Diablo III" / "Data_D3" / "PC" / "MPQs" / "*.MPQ")))
        if not mpqs:
            sys.exit(f"No 1.0 data under {self.folder} (expected Tome-*/Diablo III/Data_D3/PC/MPQs/*.MPQ).")
        for mpq in mpqs:
            for group, (_, _, ext, _) in GROUPS.items():
                for path in sorted(glob.glob(os.path.join(mpq, group, "*" + ext))):
                    with open(path, "rb") as f:
                        head = f.read(0x14)
                    if len(head) < 0x14 or head[:4] != b"\xef\xbe\xad\xde":
                        continue
                    sno = struct.unpack_from("<i", head, 0x10)[0]
                    name = os.path.basename(path)[:-len(ext)]
                    entry = (group, sno, name, path)
                    self.by_id.setdefault(sno, entry)
                    self.by_name.setdefault((group, name.lower()), entry)

    def describe(self, snos):
        """{sno: "Group/name"} for 1.0 files of any group (reads every file's header once)."""
        want, found = set(snos), {}
        for path in glob.glob(str(self.folder / "Tome-*" / "Diablo III" / "Data_D3" / "PC" / "MPQs" / "*.MPQ" / "*" / "*")):
            try:
                with open(path, "rb") as f:
                    head = f.read(0x14)
            except OSError:
                continue
            if len(head) == 0x14 and head[:4] == b"\xef\xbe\xad\xde":
                sno = struct.unpack_from("<i", head, 0x10)[0]
                if sno in want and sno not in found:
                    found[sno] = "%s/%s" % (os.path.basename(os.path.dirname(path)), os.path.splitext(os.path.basename(path))[0])
        return found

    def client_binary(self):
        found = glob.glob(str(self.folder / "Tome-*" / "Diablo III" / "Diablo III.app" / "Contents" / "MacOS" / "Diablo III"))
        return found[0] if found else None


class Game:
    """2.7.7's file list and template files, from build/port10/."""

    def __init__(self):
        rows = json.loads((WORK / "toc.json").read_text())
        self.ids = {}
        self.names = {}
        for g, sno, name in rows:
            self.ids.setdefault(sno, (g, name))
            self.names[(g, name.lower())] = sno
        self.max_id = max(r[1] for r in rows)

    def sample(self, group):
        return (WORK / "samples" / (group + ".bin")).read_bytes()


def need_setup():
    missing = [n for n in ("types10.json", "types277.json", "toc.json") if not (WORK / n).exists()]
    missing += ["samples/%s.bin" % g for g in SAMPLES if not (WORK / "samples" / (g + ".bin")).exists()]
    if missing:
        sys.exit("Run setup first (missing in build/port10: %s)." % ", ".join(missing))


# ---------------------------------------------------------------------------------------
# setup

def setup(args):
    WORK.mkdir(parents=True, exist_ok=True)
    disc = Disc(args.disc)
    say(f"1.0 disc: {len(disc.by_id)} files of the ported groups")

    if not (WORK / "types10.json").exists():
        binary = disc.client_binary()
        if not binary:
            sys.exit("The 1.0 Mac client (Diablo III.app) is not on the disc folder.")
        say("1.0 type tables: running the 1.0 client's start-up code under Unicorn (a few minutes)")
        try:
            import unicorn  # noqa: F401
        except ImportError:
            sys.exit("This step needs the Unicorn emulator: python3 -m pip install unicorn")
        subprocess.run([sys.executable, str(HERE / "emu.py"), binary, str(WORK / "mem.pkl")], check=True)
        subprocess.run([sys.executable, str(HERE / "types10.py"), str(WORK / "mem.pkl"), binary], check=True)
        (WORK / "mem.pkl").unlink()

    if not (WORK / "types277.json").exists():
        if not args.d3:
            sys.exit("2.7.7 type tables: pass --d3 <a build's d3>, built with -DD3_MOD_LOOSE_FILES=1.")
        say("2.7.7 type tables: starting the game with D3_DUMP_TYPES")
        env = dict(os.environ, D3_DUMP_TYPES=str(WORK / "types277.json"))
        env.pop("D3_LOAD_WORLD", None)
        subprocess.run([args.d3], env=env, timeout=300, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not (WORK / "types277.json").exists():
            sys.exit("The game did not write its type tables: was it built with -DD3_MOD_LOOSE_FILES=1?")

    cpks = sorted(glob.glob(str(pathlib.Path(args.game) / "romfs" / "CPKs" / "*.cpk")))
    if not cpks:
        sys.exit(f"No CPK archives under {args.game}/romfs/CPKs.")
    archives = [cpk.CPK(p) for p in cpks]

    def find(name):
        for a in archives:
            data = a.find(name)
            if data is not None:
                return data
        sys.exit(f"{name} is not in the 2.7.7 archives.")

    rows = cpk.read_toc(find("CoreTOC.dat"))
    (WORK / "toc.json").write_text(json.dumps(rows))
    (WORK / "samples").mkdir(exist_ok=True)
    for group, name in SAMPLES.items():
        (WORK / "samples" / (group + ".bin")).write_bytes(find(name))
    say(f"2.7.7: {len(rows)} files listed, {len(SAMPLES)} templates")
    say("Ready. These files are made from your copies of the game: keep them out of git (build/ is ignored).")


# ---------------------------------------------------------------------------------------
# list

def arena_scenes(disc, world_name):
    scenes = []
    for (group, lname), entry in sorted(disc.by_name.items()):
        if group != "Scene":
            continue
        for p in ARENAS[world_name]:
            if lname == p.lower() or (p.endswith("_") and lname.startswith(p.lower())):
                scenes.append(entry)
    return scenes


def list_arenas(args):
    disc = Disc(args.disc)
    for name in ARENAS:
        w = disc.by_name.get(("Worlds", name.lower()))
        scenes = arena_scenes(disc, name)
        state = "" if w and scenes else "  (not on this disc)"
        say(f"{name:24} {len(scenes):3} scene{'s' if len(scenes) != 1 else ' '}{state}")


# ---------------------------------------------------------------------------------------
# port

class Port:
    """One conversion run: which 1.0 files are needed, their 2.7.7 numbers, the results."""

    def __init__(self, disc, game, out):
        self.disc, self.game, self.out = disc, game, pathlib.Path(out)
        self.new_ids = {}      # 1.0 sno -> 2.7.7 sno
        self.queue = []        # 1.0 entries waiting to be converted
        self.done = {}         # 1.0 sno -> (group number, 2.7.7 sno, name)
        self.missing = {}      # sno -> count, named by a ported file but in neither version
        self.skipped = {}      # 1.0 sno -> name, files that could not be converted
        self.next_free = max(game.max_id, max(disc.by_id)) + 1

    def number_for(self, entry):
        """The 2.7.7 number a 1.0 file gets: its own, unless 2.7.7 uses that number."""
        group, sno, name, _ = entry
        if sno not in self.new_ids:
            if sno in self.game.ids:
                self.new_ids[sno] = self.next_free
                self.next_free += 1
            else:
                self.new_ids[sno] = sno
            self.queue.append(entry)
        return self.new_ids[sno]

    def on_sno(self, sno):
        """A SNO number a converted file names -> the number it names in 2.7.7."""
        if sno in (0, -1):
            return sno
        entry = self.disc.by_id.get(sno)
        if entry is None:
            if sno in STANDINS:
                return self.game.names[(1, STANDINS[sno][1].lower())]
            if sno > 0 and sno not in self.game.ids:
                self.missing[sno] = self.missing.get(sno, 0) + 1
            return sno
        group, _, name, _ = entry
        num = GROUPS[group][0]
        same = self.game.names.get((num, name.lower()))
        if same is not None:
            return same  # 2.7.7 still ships this file: use its copy
        return self.number_for(entry)

    def has(self, group, sno):
        g = self.game.ids.get(sno)
        return (g is not None and g[0] == group) or sno in self.new_ids.values()

    def write(self, group, sno, name, data):
        num, folder, ext, root = GROUPS[group]
        problems = check.check(root, data) if group != "Textures" else []
        for p in problems[:5]:
            say(f"    check: {p}")
        path = self.out / folder / f"{sno}{ext}"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        self.done[sno] = (num, sno, name)

    def drain(self):
        while self.queue:
            group, sno, name, path = self.queue.pop(0)
            new = self.new_ids[sno]
            src = open(path, "rb").read()
            if group == "Textures":
                data, log = texture.convert_texture(src, self.game.sample(group), new)
            else:
                data, log = convert.convert_file(GROUPS[group][3], src, self.game.sample(group),
                                                 self.on_sno, self.has, sno=new)
            renumber = f" (as {new}: 2.7.7 uses {sno})" if new != sno else ""
            say(f"  {group}/{name}{renumber}")
            for line in log:
                if not line.startswith("tag map copied"):
                    say("    " + line)
            if data is None:
                self.skipped[sno] = name
                continue
            self.write(group, new, name, data)

    def arena(self, world_name):
        disc = self.disc
        w = disc.by_name.get(("Worlds", world_name.lower()))
        scenes = arena_scenes(disc, world_name)
        if not w or not scenes:
            sys.exit(f"{world_name}: not on the disc.")
        say(f"{w[2]}: {len(scenes)} scene{'s' if len(scenes) != 1 else ''}")
        placed = []
        for entry in scenes:
            src = open(entry[3], "rb").read()
            nav = next(f["offset"] for f in convert.T10["Scene"]["fields"] if f["type"] == "NavMeshDef")
            size = struct.unpack_from("<i", src, 0x10 + nav)[0] * NAV_SQUARE
            if len(scenes) == 1:
                pos = (0.0, 0.0)
            else:
                pos = world.tile_position(entry[2], size)
                if pos is None:
                    sys.exit(f"{entry[2]}: cannot tell where this tile goes.")
            placed.append((self.number_for(entry), pos))
            # The minimap draws each scene with the texture named mmap_<scene name>
            # (SceneDefinitionGetMinimapName), found by name, so nothing names it by number.
            mmap = disc.by_name.get(("Textures", "mmap_" + entry[2].lower()))
            if mmap and not self.game.names.get((44, mmap[2].lower())):
                self.number_for(mmap)
        self.drain()
        # The world: 2.7.7's dueling arena with these scenes, under the 1.0 world's number.
        xs = [p[0] for _, p in placed]
        ys = [p[1] for _, p in placed]
        dx, dy = -min(xs), -min(ys)
        sno_world = self.number_for(w)
        self.queue.remove(w)
        def port_env(sno):
            entry = self.disc.by_id.get(sno)
            return self.number_for(entry) if entry and entry[0] != "Worlds" else None
        env, log = world.environment(open(w[3], "rb").read(), self.game.sample("Worlds"), self.game.ids, port_env)
        for line in log:
            say("    " + line)
        self.drain()
        data = world.build(self.game.sample("Worlds"), sno_world, [(s, x + dx, y + dy) for s, (x, y) in placed], env=env)
        self.write("Worlds", sno_world, w[2], data)
        say(f"  Worlds/{w[2]} ({sno_world}): {len(placed)} scenes placed")
        return w[2]

    def write_index(self):
        path = self.out / "index.txt"
        lines, keys = [], set()
        for num, sno, name in self.done.values():
            lines.append(f"{num} {sno} {name}")
            keys.add((num, sno))
        if path.exists():
            for line in path.read_text().splitlines():
                parts = line.split(None, 2)
                if line.startswith("#") or len(parts) < 3:
                    continue
                if (int(parts[0]), int(parts[1])) not in keys:
                    lines.append(line)
        head = "# Files the Loose Files mod adds to the game's list: <group> <SNO> <name>.\n"
        path.write_text(head + "\n".join(sorted(lines, key=lambda l: (int(l.split()[0]), int(l.split()[1])))) + "\n")


def port(args):
    need_setup()
    convert.load(str(WORK))
    disc, game = Disc(args.disc), Game()
    names = {n.lower(): n for n in ARENAS}
    p = Port(disc, game, args.out)
    started = []
    for a in args.arenas:
        if a.lower() not in names:
            sys.exit(f"{a}: not an arena this knows (see list).")
        started.append(p.arena(names[a.lower()]))
    p.write_index()
    if p.skipped:
        say("Not converted (what names them goes without): " + ", ".join(sorted(p.skipped.values())))
    if p.missing:
        # Markers naming them were dropped; a model's missing texture leaves it undrawn.
        say("Named by the 1.0 files but not in 2.7.7 (markers naming them were dropped):")
        names = disc.describe(p.missing)
        for sno in sorted(p.missing):
            say(f"  {sno:7} {names.get(sno, '(not on the disc either)')}")
    say(f"{len(p.done)} files in {p.out}.")
    for w in started:
        say(f"Start it with D3_LOAD_WORLD={w} in a build with -DD3_MOD_LOOSE_FILES=1.")


# ---------------------------------------------------------------------------------------
# selftest

def markers(data):
    off, size = struct.unpack_from("<2I", data, 0x10 + 0xc)
    result = {}
    for i in range(size // 0x58):
        m = 0x10 + off + i * 0x58
        h = struct.unpack_from("<I", data, m)[0]
        to, ts = struct.unpack_from("<2I", data, m + 0x30)
        tags = data[0x10 + to:0x10 + to + ts] if ts else b""
        result[h] = (data[m + 0x8:m + 0x30], struct.unpack_from("<i", data, m + 0x40)[0], tags)
    return result


def selftest(args):
    need_setup()
    convert.load(str(WORK))
    disc, game = Disc(args.disc), Game()
    name = "trDun_ButcherLair_Jump"
    entry = disc.by_name.get(("MarkerSet", name.lower()))
    if not entry:
        sys.exit(f"{name} is not on the disc.")
    cpks = sorted(glob.glob(str(pathlib.Path(args.game) / "romfs" / "CPKs" / "*.cpk")))
    theirs = None
    for path in cpks:
        theirs = cpk.CPK(path).find(f"MarkerSet\\{name}.mrk")
        if theirs is not None:
            break
    if theirs is None:
        sys.exit(f"{name} is not in the 2.7.7 archives.")
    has = lambda g, s: game.ids.get(s, (None,))[0] == g  # noqa: E731
    ours, _ = convert.convert_file("MarkerSet", open(entry[3], "rb").read(), game.sample("MarkerSet"), None, has)
    a, b = markers(ours), markers(theirs)
    shared = set(a) & set(b)
    differ = [h for h in shared if a[h] != b[h]]
    say(f"{name}: {len(a)} markers converted, {len(b)} in 2.7.7, {len(shared)} shared, {len(differ)} differ")
    for h in differ[:10]:
        say(f"  {h:#x}: ours {a[h][0].hex()} {a[h][1]}, 2.7.7's {b[h][0].hex()} {b[h][1]}")
    if differ or not shared:
        sys.exit(1)
    say("ok")


def main():
    global WORK
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--disc", default=str(ROOT / "build" / "D3C1.0"), help="the 1.0 disc folder")
    ap.add_argument("--game", default=str(ROOT / "game" / "2.7.7"), help="the 2.7.7 game folder (with romfs/)")
    ap.add_argument("--work", default=str(WORK), help="where setup keeps what it reads (default build/port10)")
    sub = ap.add_subparsers(dest="command", required=True)
    s = sub.add_parser("setup")
    s.add_argument("--d3", help="a build's d3 with the Loose Files mod, for the 2.7.7 type tables")
    sub.add_parser("list")
    s = sub.add_parser("port")
    s.add_argument("arenas", nargs="+")
    s.add_argument("--out", default=str(ROOT / "save" / "2.7.7" / "loose"), help="the loose folder")
    sub.add_parser("selftest")
    args = ap.parse_args()
    WORK = pathlib.Path(args.work)
    {"setup": setup, "list": list_arenas, "port": port, "selftest": selftest}[args.command](args)


if __name__ == "__main__":
    main()
