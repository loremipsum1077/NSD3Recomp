#!/usr/bin/env python3
"""A small local page for choosing mods, building, and starting the game.

    python3 tools/mods.py            (or double-click Mods.command)

It serves one page on this computer only (127.0.0.1) and opens it in the browser.
The mods are listed in config/mods.json; each is a CMake option of this project
(see cmake/DecompSources.cmake). Ticking a box and pressing Build runs the same two
commands you would type: `cmake -B <build directory> -D<option>=<value> ...` (or
`cmake --preset macos ...` for a build directory not configured yet) and
`cmake --build <build directory>`. The page stops this program when it is closed.

The page builds one of two game versions:

  2.7.7  this checkout (the dev branch), in build/macos, or build/macos-277 when
         build/macos was configured for something else.
  2.6.2  the 2.6.2 branch of this repository and of the decomp. The first build sets
         them up as git worktrees under build/2.6.2/ and generates the decomp's game
         data there from your 2.6.2 dump in game/; later builds reuse them. It offers
         the mods that branch has, which are fewer.

--version picks the one selected at start (2.7.7 by default).
"""
import argparse
import json
import os
import pathlib
import re
import secrets
import shutil
import subprocess
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent.parent
OLD = ROOT / "build" / "2.6.2"          # the 2.6.2 worktrees
OLD_SOURCE = OLD / "NSD3Recomp"
OLD_DECOMP = OLD / "NSDiablo3Decomp"
OLD_GAME = ROOT / "game"                 # the 2.6.2 dump (2.7.7's is game/2.7.7)
VERSIONS = {"2.7.7": "Diablo III 2.7.7", "2.6.2": "Diablo III 2.6.2 (older, fewer mods)"}
TOKEN = secrets.token_urlsafe(16)

lock = threading.Lock()
state = {"log": [], "building": False, "result": "", "last_seen": time.time(), "seen": False, "version": "2.7.7"}
game = None


def cache_text(build_dir):
    cache = build_dir / "CMakeCache.txt"
    return cache.read_text(errors="replace") if cache.exists() else ""


def cache_value(build_dir, name):
    found = re.search(rf"^{re.escape(name)}:[A-Z]+=(.*)$", cache_text(build_dir), re.M)
    return found.group(1) if found else ""


def is_277(build_dir):
    """A build directory configured from the dev branch: it reads 2.7.7's symbols."""
    return cache_value(build_dir, "D3_SYMBOLS_CSV").rstrip("/").endswith("2.7.7")


def build_dir_for(version):
    if version == "2.6.2":
        return OLD_SOURCE / "build" / "macos"
    candidates = [ROOT / "build" / "macos", ROOT / "build" / "macos-277"]
    for d in candidates:
        if is_277(d):
            return d
    # Nothing configured for 2.7.7 yet: the preset's directory, unless it already holds
    # a build of something else (an older checkout's 2.6.2 build, say).
    return candidates[0] if not cache_text(candidates[0]) else candidates[1]


def current_version():
    with lock:
        return state["version"]


def current_build():
    return build_dir_for(current_version())


def mods(version=None):
    version = version or current_version()
    if version == "2.6.2":
        if (OLD_SOURCE / "config" / "mods.json").exists():
            return json.loads((OLD_SOURCE / "config" / "mods.json").read_text())
        # Not set up yet: the branch's own list.
        for ref in ("2.6.2", "origin/2.6.2"):
            try:
                return json.loads(subprocess.run(["git", "show", f"{ref}:config/mods.json"], cwd=ROOT,
                                                 capture_output=True, text=True, check=True).stdout)
            except (subprocess.CalledProcessError, ValueError, OSError):
                pass
        return []
    return json.loads((ROOT / "config" / "mods.json").read_text())


def versions():
    result = []
    for vid, label in VERSIONS.items():
        d = build_dir_for(vid)
        if vid == "2.6.2" and not OLD_SOURCE.exists():
            note = "The first build sets up the 2.6.2 branches in build/2.6.2."
        else:
            note = f"Builds in {d.relative_to(ROOT)}."
        result.append({"id": vid, "label": label, "note": note})
    return result


def game_path():
    return current_build() / "d3"


def save_dir():
    found = cache_value(current_build(), "D3_SAVE_DIR")
    return pathlib.Path(found) if found else ROOT / "save"


def cheats_dir():
    return save_dir() / "cheats"


def cheats_status():
    try:
        return json.loads((cheats_dir() / "status.json").read_text())
    except (OSError, ValueError):
        return None


# What the page may ask the game to do (src/mods/cheats.cpp reads the same lines).
CHEAT = re.compile(r"^(level ([1-9]|[1-6][0-9]|70)|paragon [1-9][0-9]{0,3}|item [A-Za-z0-9_\-]{1,127}( ([1-9]|[1-4][0-9]|50))?|gold [1-9][0-9]{0,10}|cube|rifts ([1-9]|[1-9][0-9]|1[0-4][0-9]|150)|cache [1-5]( ([1-9]|1[0-9]|20))?|spawn [A-Za-z0-9_\-]{1,127}( (normal|champion|rare|boss))?)$")


# Game rules: the game type's own switches (GameParamFlags) and two server switches, which
# src/mods/cheats.cpp reads from rules.txt in the cheats folder while the game runs. Each is
# (name, title, what it does, on in a normal game). Left out: saving heroes, forcing new
# heroes and how actors reach the client (they lose or break saves), and the PvP-only ones.
RULES = [
    ("REVEAL_MINIMAP", "Reveal the minimap", "The whole minimap is drawn, explored or not.", False),
    ("SHOW_MONSTER_HP_BARS", "Monster health bars", "A health bar over every monster, as in the PvP arenas.", False),
    ("IGNORE_DURABILITY_LOSS", "No durability loss", "Gear never wears down.", False),
    ("GRANT_GOLD_UPON_KILL", "Gold for every kill", "Each kill also pays gold straight to you, as in the old team deathmatch.", False),
    ("RESTOCK_SHOPS_UPON_TALK", "Vendors restock on talk", "A vendor's stock is rolled again every time you talk to them.", False),
    ("KEEP_ALL_SCENES_ACTIVE", "Keep the whole map running", "Every loaded area keeps simulating, not only the ones near you. Heavier; takes effect as areas load.", False),
    ("RESURRECTION_DELAY", "Respawn timer", "Dying starts the PvP respawn countdown instead of the usual checkpoint rules.", False),
    ("NO_EXPERIENCE", "No experience", "Kills and quests give no experience.", False),
    ("LIMIT_WEAPON_EFFECTS", "Fewer weapon effects", "Weapons draw fewer of their glow and trail effects.", False),
    ("NO_AREA_DAMAGE", "No Area Damage", "The Area Damage stat does nothing (a server switch).", False),
    ("NO_DOT_AREA_DAMAGE", "No Area Damage from damage over time", "Damage over time no longer sets off Area Damage (a server switch).", False),
    ("ALLOW_TOWN_PORTALS", "Town portals", "Off: Town Portal cannot be used.", True),
    ("QUEST_UI", "Quest journal", "Off: the quest journal cannot be opened.", True),
    ("CREATE_HEADSTONES_UPON_PLAYER_DEATH", "Headstones", "A headstone marks where a hero died.", True),
    ("APPLY_RESURRECTION_BUFFS", "Resurrection buffs", "Coming back to life gives the usual short protective buffs.", True),
    ("RESET_LIFE_STATS_UPON_LEVEL_UP", "Full health on level up", "Levelling up refills life and resource.", True),
    ("ADJUST_EXP_WITH_RELATIVE_LEVEL", "Experience by level difference", "Off: monsters far below or above your level give full experience.", True),
]
RULE_NAMES = {r[0] for r in RULES}


def read_rules():
    found = {}
    try:
        for line in (cheats_dir() / "rules.txt").read_text().splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] in RULE_NAMES and parts[1] in ("on", "off"):
                found[parts[0]] = parts[1]
    except OSError:
        pass
    return found


def current_values():
    """What the build directory is configured with now."""
    build_dir = current_build()
    return {m["id"]: cache_value(build_dir, m["id"]) for m in mods()}


def say(line):
    with lock:
        state["log"].append(line.rstrip("\n"))


def run(command, cwd=ROOT):
    """Run a command, copying its output to the page; True when it succeeded."""
    say("$ " + " ".join(str(c) for c in command))
    p = subprocess.Popen([str(c) for c in command], cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, errors="replace")
    for line in p.stdout:
        say(line)
    if p.wait() != 0:
        say(f"Failed (exit {p.returncode}).")
        return False
    return True


def find_decomp():
    """The dev decomp checkout, found the way cmake/Decomp.cmake finds it."""
    for c in (cache_value(build_dir_for("2.7.7"), "D3_DECOMP_DIR"), os.environ.get("D3_DECOMP_DIR", ""),
              ROOT / "extern" / "NSDiablo3Decomp", ROOT.parent / "NSDiablo3Decomp"):
        if c and (pathlib.Path(c) / ".git").exists():
            return pathlib.Path(c)
    return None


def set_up_262():
    """Make build/2.6.2 hold the 2.6.2 branches of both repositories, with the decomp's
    game data generated from the 2.6.2 dump. Each step is skipped once done."""
    if not (OLD_GAME / "exefs" / "main").exists():
        say(f"2.6.2 needs your 2.6.2 dump in {OLD_GAME.relative_to(ROOT)}/ (exefs/ and romfs/); see the README.")
        return False
    OLD.mkdir(parents=True, exist_ok=True)
    if not OLD_SOURCE.exists():
        say("Setting up 2.6.2: this repository's 2.6.2 branch.")
        if not run(["git", "worktree", "add", OLD_SOURCE, "2.6.2"]):
            return False
    if not OLD_DECOMP.exists():
        decomp = find_decomp()
        if decomp is None:
            say("Could not find the NSDiablo3Decomp checkout (next to this one, or D3_DECOMP_DIR).")
            return False
        say(f"Setting up 2.6.2: the decomp's 2.6.2 branch, from {decomp}.")
        if not run(["git", "-C", decomp, "worktree", "add", OLD_DECOMP, "2.6.2"]):
            return False
    if not (OLD_DECOMP / "src" / "X" / "Common" / "generated-game-data").is_dir():
        say("Setting up 2.6.2: generating the decomp's game data from your 2.6.2 dump.")
        command = [sys.executable, "tools/bootstrap_game_data.py", OLD_GAME]
        decomp = find_decomp()
        protoc = decomp / "build" / "game-tools" / "protoc-2.6.0" if decomp else None
        command += ["--protoc", protoc] if protoc and protoc.exists() else ["--build-protoc"]
        objdump = pathlib.Path("/opt/homebrew/opt/llvm/bin/llvm-objdump")
        if not shutil.which("llvm-objdump") and objdump.exists():
            command += ["--objdump", objdump]
        if not run(command, cwd=OLD_DECOMP):
            return False
    return True


def build(version, values):
    try:
        build_dir = build_dir_for(version)
        if version == "2.6.2":
            if not set_up_262():
                with lock:
                    state["result"] = "failed"
                return
            # The 2.6.2 branch would look for the game, saves and shaders inside the
            # worktree; point it at this checkout's.
            configure = ["cmake", "-S", OLD_SOURCE, "-B", build_dir, "-G", "Ninja",
                         "-DCMAKE_BUILD_TYPE=RelWithDebInfo", f"-DD3_DECOMP_DIR={OLD_DECOMP}",
                         f"-DD3_GAME_DIR={OLD_GAME}", f"-DD3_SAVE_DIR={ROOT / 'save'}",
                         f"-DD3_SHADER_CACHE={ROOT / 'cache' / 'shaders'}"]
        elif cache_text(build_dir):
            # An existing build directory keeps its own settings (decomp, game files).
            configure = ["cmake", "-B", build_dir]
        elif build_dir.name == "macos":
            configure = ["cmake", "--preset", "macos"]
        else:
            configure = ["cmake", "-B", build_dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo"]
        args = [f"-D{k}={v}" for k, v in values.items()]
        for command in (configure + args, ["cmake", "--build", build_dir]):
            if not run(command):
                with lock:
                    state["result"] = "failed"
                return
        say("Build finished.")
        shaders = ROOT / "cache" / "shaders"
        if version == "2.6.2" and not any(shaders.glob("*.metal")):
            say("2.6.2 draws nothing until its shaders are translated: start the game for half a minute, "
                f"quit, then run `python3 tools/shader_translate.py --cache {shaders}` in {OLD_SOURCE}.")
        with lock:
            state["result"] = "ok"
    except FileNotFoundError as e:
        say(f"Could not run {e.filename}: is it installed?")
        with lock:
            state["result"] = "failed"
    finally:
        with lock:
            state["building"] = False


def game_running():
    return game is not None and game.poll() is None


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Mods</title>
<style>
  :root { --bg:#f6f4ef; --card:#fffdf8; --raise:#f0ece3; --ink:#1f1b16; --dim:#6b6257; --line:#ddd6c8; --accent:#9a3412; --accent-soft:#9a341214; --onaccent:#fff; --ok:#166534; --bad:#b91c1c; --log:#17140f; --logink:#e9e2d3; --leg:#c2410c; --set:#15803d; }
  @media (prefers-color-scheme: dark) { :root { --bg:#14110d; --card:#1f1b15; --raise:#29241c; --ink:#efe8da; --dim:#a79c8b; --line:#3a3329; --accent:#f59e5b; --accent-soft:#f59e5b1c; --onaccent:#1a1208; --ok:#86efac; --bad:#fca5a5; --log:#0b0907; --logink:#d9d1c0; --leg:#fb923c; --set:#4ade80; } }
  * { box-sizing:border-box; }
  [hidden] { display:none !important; }
  body { margin:0; background:var(--bg); color:var(--ink); font:15px/1.5 -apple-system, "Helvetica Neue", sans-serif; }
  .wrap { max-width:1120px; margin:0 auto; padding:24px 20px 48px; }
  header { display:flex; align-items:flex-end; justify-content:space-between; gap:16px; flex-wrap:wrap; margin-bottom:20px; }
  h1 { font:600 30px/1.1 Georgia, serif; margin:0 0 4px; }
  .sub { color:var(--dim); margin:0; }
  nav { display:flex; gap:4px; background:var(--raise); padding:4px; border-radius:10px; }
  nav button { border:0; background:none; padding:7px 16px; border-radius:7px; font-weight:600; color:var(--dim); }
  nav button[aria-selected=true] { background:var(--card); color:var(--ink); box-shadow:0 1px 2px #0002; }
  .layout { display:grid; grid-template-columns:minmax(0,1fr) 300px; gap:24px; align-items:start; }
  @media (max-width:880px) { .layout { grid-template-columns:1fr; } aside { position:static !important; order:-1; } }
  h2 { font:600 13px/1 -apple-system, sans-serif; text-transform:uppercase; letter-spacing:.08em; color:var(--dim); margin:28px 0 10px; }
  section > h2:first-child, #mods > h2:first-child { margin-top:0; }
  .grid { display:grid; grid-template-columns:repeat(auto-fill, minmax(320px, 1fr)); gap:12px; }
  .mod { background:var(--card); border:1px solid var(--line); border-radius:12px; padding:14px 16px; transition:border-color .15s; }
  .mod.on { border-color:color-mix(in srgb, var(--accent) 55%, var(--line)); box-shadow:inset 3px 0 0 var(--accent); }
  .mod.wide { grid-column:1 / -1; }
  .head { display:flex; align-items:center; gap:12px; min-height:32px; }
  .head label { font-weight:600; flex:1; cursor:pointer; }
  .desc { margin:6px 0 0 50px; color:var(--dim); font-size:13.5px; }
  .more { border:0; background:none; padding:0; color:var(--accent); font-size:13px; font-weight:600; cursor:pointer; }
  .sub-opt { margin:12px 0 0 50px; padding-top:12px; border-top:1px dashed var(--line); }
  .sub-opt .head label { font-weight:500; }
  .sub-opt .desc { margin-left:50px; }
  .ctl select, .ctl input[type=text] { font:inherit; padding:6px 8px; border:1px solid var(--line); border-radius:7px; background:var(--bg); color:var(--ink); }
  .ctl { flex:none; }
  .pick { margin:6px 0 0 50px; }
  .pick .ctl select { width:100%; max-width:420px; }
  .ctl input[type=text] { width:72px; text-align:right; }
  .head label { min-width:90px; }
  input.switch { appearance:none; -webkit-appearance:none; flex:none; width:38px; height:22px; border-radius:11px; background:var(--line); position:relative; cursor:pointer; margin:0; transition:background .15s; }
  input.switch::before { content:""; position:absolute; top:2px; left:2px; width:18px; height:18px; border-radius:50%; background:#fff; box-shadow:0 1px 2px #0004; transition:transform .15s; }
  input.switch:checked { background:var(--accent); }
  input.switch:checked::before { transform:translateX(16px); }
  input.switch:focus-visible { outline:2px solid var(--accent); outline-offset:2px; }
  .chips { display:flex; flex-wrap:wrap; gap:6px; margin:10px 0 0 50px; }
  .chip { display:inline-flex; align-items:center; gap:6px; padding:4px 11px; border:1px solid var(--line); border-radius:999px; font-size:13px; cursor:pointer; user-select:none; background:var(--bg); }
  .chip input { margin:0; accent-color:var(--accent); }
  .chip:has(input:checked:not(:disabled)) { border-color:var(--accent); background:var(--accent-soft); }
  .chip:has(input:disabled) { cursor:default; }
  .mod :disabled, .chips:has(:disabled) { opacity:.5; }
  input.switch:disabled { opacity:.5; }
  aside { position:sticky; top:16px; }
  .panel { background:var(--card); border:1px solid var(--line); border-radius:12px; padding:16px; }
  .panel + .panel { margin-top:12px; }
  .panel label.cap { display:block; font-size:12px; font-weight:600; color:var(--dim); text-transform:uppercase; letter-spacing:.06em; margin-bottom:6px; }
  .panel select { width:100%; font:inherit; font-size:13px; padding:7px 8px; border:1px solid var(--line); border-radius:7px; background:var(--bg); color:var(--ink); }
  .hint { color:var(--dim); font-size:12.5px; margin:6px 0 0; }
  #onlist { list-style:none; padding:0; margin:8px 0 0; font-size:13.5px; max-height:220px; overflow:auto; }
  #onlist li { padding:3px 0; display:flex; justify-content:space-between; gap:8px; }
  #onlist li span { color:var(--dim); text-align:right; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; max-width:55%; }
  .count { font:600 22px/1 Georgia, serif; }
  button { font:inherit; font-weight:600; padding:9px 16px; border-radius:8px; border:1px solid var(--line); background:var(--card); color:var(--ink); cursor:pointer; }
  button:hover:not(:disabled) { border-color:var(--dim); }
  button.primary { background:var(--accent); border-color:var(--accent); color:var(--onaccent); }
  button:disabled { opacity:.5; cursor:default; }
  .buttons { display:grid; grid-template-columns:1fr 1fr; gap:8px; margin-top:4px; }
  #status { display:block; margin-top:10px; font-size:13.5px; color:var(--dim); }
  #status.ok { color:var(--ok); } #status.bad { color:var(--bad); }
  #pending { color:var(--accent); font-size:13px; font-weight:600; margin-top:8px; }
  details summary { cursor:pointer; font-size:13px; font-weight:600; color:var(--dim); margin-top:12px; }
  pre { background:var(--log); color:var(--logink); border-radius:8px; padding:10px 12px; overflow:auto; font:11.5px/1.45 ui-monospace, Menlo, monospace; white-space:pre-wrap; word-break:break-word; margin:8px 0 0; }
  #log { height:260px; }
  .note { color:var(--dim); font-size:13px; margin-top:14px; }
  .tools { display:grid; grid-template-columns:repeat(auto-fill, minmax(250px, 1fr)); gap:12px; }
  .tool h3 { margin:0 0 10px; font-size:14px; }
  .tool .row { display:flex; gap:8px; align-items:center; flex-wrap:wrap; }
  .tool .row + .row { margin-top:8px; }
  .tool button { padding:7px 12px; }
  #cheats input[type=text], #cheats input[type=search], #cheats select { font:inherit; padding:7px 10px; border:1px solid var(--line); border-radius:7px; background:var(--bg); color:var(--ink); }
  .acts button { min-width:40px; padding:7px 0; }
  .finder { display:flex; gap:8px; flex-wrap:wrap; align-items:center; margin-bottom:10px; }
  .finder input[type=search] { flex:1; min-width:200px; }
  .finder label { font-size:13.5px; color:var(--dim); display:flex; gap:6px; align-items:center; }
  .tablewrap { max-height:440px; overflow:auto; border:1px solid var(--line); border-radius:10px; background:var(--card); }
  table { border-collapse:collapse; width:100%; }
  th, td { text-align:left; padding:7px 12px; border-bottom:1px solid var(--line); font-size:14px; }
  th { position:sticky; top:0; background:var(--raise); color:var(--dim); font-weight:600; font-size:12px; text-transform:uppercase; letter-spacing:.05em; z-index:1; }
  tr:last-child td { border-bottom:0; }
  #items td.n { color:var(--dim); font:12px ui-monospace, Menlo, monospace; }
  #items .legendary { color:var(--leg); } #items .set { color:var(--set); }
  #items td:last-child { text-align:right; width:1%; }
  #items button { padding:3px 12px; font-size:13px; }
  #cheatstate { padding:10px 14px; border-radius:10px; background:var(--raise); margin:0 0 16px; font-size:14px; }
  .controls { max-width:560px; border:1px solid var(--line); border-radius:10px; overflow:hidden; background:var(--card); }
  .controls th { position:static; }
  kbd { font:12.5px ui-monospace, Menlo, monospace; background:var(--bg); border:1px solid var(--line); border-bottom-width:2px; border-radius:5px; padding:1px 6px; }
</style></head><body><div class="wrap">
<header>
  <div><h1>Mods</h1><p class="sub">Switch on what you want, then build. With everything off the game builds exactly as it shipped.</p></div>
  <nav role="tablist">
    <button role="tab" data-tab="mods" aria-selected="true">Mods</button>
    <button role="tab" data-tab="cheats" id="cheatstab" hidden>Cheats</button>
    <button role="tab" data-tab="controls">Controls</button>
  </nav>
</header>
<div class="layout">
<div>
<section data-panel="mods"><form id="mods" onsubmit="return false"></form></section>

<section data-panel="cheats" id="cheats" hidden>
  <p id="cheatstate">Start the game and load a hero.</p>
  <div class="tools">
    <div class="panel tool"><h3>Hero</h3>
      <div class="row"><button data-cheat="level 70">Level 70</button></div>
      <div class="row"><input type="text" inputmode="numeric" id="paragon" value="100" aria-label="Paragon levels to add" style="width:80px"><button id="addparagon">Add paragon</button></div>
      <div class="row"><input type="text" inputmode="numeric" id="gold" value="10000000" aria-label="Gold to add" style="width:110px"><button id="addgold">Add gold</button></div>
    </div>
    <div class="panel tool"><h3>Account</h3>
      <div class="row"><button data-cheat="cube">Unlock Kanai's Cube</button></div>
      <div class="row"><button data-cheat="rifts 70">Greater rifts to 70</button></div>
      <p class="hint">Clearing greater rifts also opens Challenge Rifts.</p>
    </div>
    <div class="panel tool"><h3>Horadric Caches</h3>
      <div class="row"><input type="text" inputmode="numeric" id="caches" value="1" aria-label="Number of caches" style="width:56px"><span class="hint" style="margin:0">caches, from act</span></div>
      <div class="row acts"><button data-act="1">I</button><button data-act="2">II</button><button data-act="3">III</button><button data-act="4">IV</button><button data-act="5">V</button></div>
    </div>
    <div class="panel tool"><h3>Spawn a monster</h3>
      <div class="row"><input type="text" id="spawnname" value="X1_LR_Boss_MorluSpellcaster_Ice" aria-label="Monster's actor name" style="width:220px"></div>
      <div class="row"><select id="spawnrarity" aria-label="Rarity"><option value="normal">Normal</option><option value="champion">Champion</option><option value="rare">Rare</option><option value="boss" selected>Boss</option></select><button id="spawn">Spawn</button></div>
      <p class="hint">By its actor name, beside your hero. The default is Rime, the ice rift guardian of the BlizzCon 2017 rift.</p>
    </div>
  </div>
  <h2>Game rules</h2>
  <p class="hint">Switches each kind of game (the campaign, the PvP arenas, Challenge Rifts) is made with. They change while you play and are kept for later games; "Normal" leaves the game's own setting.</p>
  <div class="tablewrap" style="max-height:none"><table id="rules"></table></div>
  <h2>Items</h2>
  <div class="finder">
    <input type="search" id="itemsearch" placeholder="Find an item: name or type" aria-label="Find an item">
    <select id="rarity" aria-label="Rarity"><option value="">All items</option><option value="legendary">Legendary</option><option value="set">Set</option><option value="legset" selected>Legendary and set</option></select>
    <label><input type="checkbox" id="current" checked> Hide old versions</label>
  </div>
  <div class="tablewrap"><table id="items"></table></div>
  <p class="note" id="itemnote"></p>
  <h2>What the game did</h2>
  <pre id="cheatlog" aria-label="What the game did" style="height:140px"></pre>
</section>

<section data-panel="controls" hidden>
<p class="sub" style="margin-bottom:12px">The game shows Switch button names on screen. On the keyboard they are these keys; a connected controller also works.</p>
<div class="controls"><table>
  <tr><th>Switch</th><th>Keyboard</th></tr>
  <tr><td>Left stick (move)</td><td><kbd>W</kbd> <kbd>A</kbd> <kbd>S</kbd> <kbd>D</kbd></td></tr>
  <tr><td>Right stick</td><td><kbd>I</kbd> <kbd>J</kbd> <kbd>K</kbd> <kbd>L</kbd></td></tr>
  <tr><td>Directional buttons</td><td>Arrow keys</td></tr>
  <tr><td>A (accept)</td><td><kbd>Space</kbd> or <kbd>Return</kbd></td></tr>
  <tr><td>B (back)</td><td><kbd>Esc</kbd></td></tr>
  <tr><td>X</td><td><kbd>E</kbd></td></tr>
  <tr><td>Y</td><td><kbd>Q</kbd></td></tr>
  <tr><td>L</td><td><kbd>1</kbd></td></tr>
  <tr><td>R</td><td><kbd>2</kbd></td></tr>
  <tr><td>ZL</td><td><kbd>3</kbd></td></tr>
  <tr><td>ZR</td><td><kbd>4</kbd></td></tr>
  <tr><td>+ (plus)</td><td><kbd>Tab</kbd></td></tr>
  <tr><td>&minus; (minus)</td><td><kbd>Backspace</kbd></td></tr>
  <tr><td>Cheats mod: level 70 / +100 paragon / +10 million gold</td><td><kbd>F1</kbd> <kbd>F2</kbd> <kbd>F3</kbd></td></tr>
</table></div>
</section>
<p class="note">This page runs on your computer only. Closing it stops the helper program; a game you started keeps running.</p>
</div>

<aside>
  <div class="panel">
    <div id="versionbox" hidden style="margin-bottom:14px">
      <label class="cap" for="version">Game version</label>
      <select id="version"></select>
      <p class="hint" id="versionnote"></p>
    </div>
    <label class="cap">Switched on</label>
    <div class="count" id="count">0 mods</div>
    <ul id="onlist"></ul>
    <div id="pending" hidden>Changes not built yet.</div>
  </div>
  <div class="panel">
    <div class="buttons">
      <button class="primary" id="build">Build</button>
      <button id="play">Start game</button>
    </div>
    <span id="status"></span>
    <details id="logbox"><summary>Build output</summary><pre id="log" aria-label="Build output"></pre></details>
  </div>
</aside>
</div>
</div><script>
const T = new URLSearchParams(location.search).get("t");
const $ = id => document.getElementById(id);
const esc = t => String(t).replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
let mods = [], built = {}, shown = 0, drawn = false, drawnBuild = null, wasBuilding = false;

// Tabs.
document.querySelector("nav").addEventListener("click", e => {
  const b = e.target.closest("button[data-tab]");
  if (b) showTab(b.dataset.tab);
});
function showTab(name) {
  document.querySelectorAll("nav button").forEach(b => b.setAttribute("aria-selected", b.dataset.tab === name));
  document.querySelectorAll("[data-panel]").forEach(p => { p.hidden = p.dataset.panel !== name; });
}

// One option: its switch, its value control and its description (first sentence shown,
// the rest behind "More").
const labelOf = (m, c) => m.labels && m.choices.indexOf(c) >= 0 ? m.labels[m.choices.indexOf(c)] : c;
function control(m, v) {
  if (m.type === "toggle") return "";
  if (m.type === "multi") return "";
  if (m.type === "choice") {
    const all = m.choices.includes(v) ? m.choices : m.choices.concat([v]);
    return `<span class="ctl"><select data-v="${m.id}" aria-label="${esc(m.title)}">` +
      all.map(c => `<option value="${esc(c)}"${c === v ? " selected" : ""}>${esc(labelOf(m, c))}</option>`).join("") + `</select></span>`;
  }
  return `<span class="ctl"><input type="text" inputmode="decimal" data-v="${m.id}" value="${esc(v)}" aria-label="${esc(m.title)} value"></span>`;
}
function describe(m) {
  const d = m.description, cut = d.search(/[.!?]\s/);
  if (cut < 0 || cut > d.length - 40) return `<p class="desc">${esc(d)}</p>`;
  return `<p class="desc">${esc(d.slice(0, cut + 1))} <button type="button" class="more" data-more>More</button>` +
    `<span hidden> ${esc(d.slice(cut + 2))}</span></p>`;
}
function option(m, value) {
  const on = value !== "", v = on ? value : m.default;
  let chips = "";
  if (m.type === "multi") {
    const picked = v.split(",");
    chips = `<div class="chips" data-v="${m.id}">` + m.choices.map(c =>
      `<label class="chip"><input type="checkbox" value="${esc(c)}"${picked.includes(c) ? " checked" : ""}> ${esc(labelOf(m, c))}</label>`).join("") + `</div>`;
  }
  // A dropdown goes on its own line under the title; a number fits beside it.
  const ctl = control(m, v), below = m.type === "choice";
  return `<div class="head"><input type="checkbox" class="switch" id="c_${m.id}" data-c="${m.id}"${on ? " checked" : ""}>` +
    `<label for="c_${m.id}">${esc(m.title)}</label>${below ? "" : ctl}</div>${below ? `<div class="pick">${ctl}</div>` : ""}${describe(m)}${chips}`;
}
function drawMods(values) {
  const groups = [];
  mods.filter(m => !m.parent && !m.follows).forEach(m => {
    let g = groups.find(g => g.name === (m.group || "Mods"));
    if (!g) groups.push(g = {name: m.group || "Mods", mods: []});
    g.mods.push(m);
  });
  $("mods").innerHTML = groups.map(g => `<h2>${esc(g.name)}</h2><div class="grid">` + g.mods.map(m => {
    const kids = mods.filter(k => k.parent === m.id);
    const wide = m.type === "multi" || kids.length;
    return `<div class="mod${wide ? " wide" : ""}" data-mod="${m.id}">${option(m, values[m.id] || "")}` +
      kids.map(k => `<div class="sub-opt">${option(k, values[k.id] || "")}</div>`).join("") + `</div>`;
  }).join("") + `</div>`).join("");
}
$("mods").addEventListener("click", e => {
  const b = e.target.closest("[data-more]");
  if (!b) return;
  const rest = b.nextElementSibling;
  rest.hidden = !rest.hidden;
  b.textContent = rest.hidden ? "More" : "Less";
});

// A field's value: a multi field's ticked choices, comma-separated.
const fieldValue = v => v.classList.contains("chips")
  ? [...v.querySelectorAll("input:checked")].map(i => i.value).join(",") : v.value.trim();
function formValues() {
  const values = {};
  document.querySelectorAll("[data-c]").forEach(c => {
    const v = document.querySelector(`[data-v="${c.dataset.c}"]`);
    values[c.dataset.c] = c.checked ? (v ? fieldValue(v) : "1") : "";
  });
  // An option with "follows" is not shown: it is its default while that mod is on.
  mods.filter(m => m.follows).forEach(m => { values[m.id] = values[m.follows] ? m.default : ""; });
  return values;
}
function sync() {
  document.querySelectorAll("[data-c]").forEach(c => {
    const v = document.querySelector(`[data-v="${c.dataset.c}"]`);
    if (v && v.classList.contains("chips")) v.querySelectorAll("input").forEach(i => { i.disabled = !c.checked; });
    else if (v) v.disabled = !c.checked;
  });
  document.querySelectorAll("[data-mod]").forEach(card => {
    card.classList.toggle("on", !!card.querySelector("input.switch:checked"));
  });
  const values = formValues();
  const on = mods.filter(m => values[m.id] && !m.follows);
  $("count").textContent = on.length === 1 ? "1 mod" : `${on.length} mods`;
  $("onlist").innerHTML = on.map(m => {
    const v = values[m.id];
    const shownValue = m.type === "toggle" ? "" : m.type === "multi" ? `${v.split(",").length} of ${m.choices.length}` : labelOf(m, v);
    return `<li>${esc(m.title)}<span title="${esc(shownValue)}">${esc(shownValue)}</span></li>`;
  }).join("") || `<li style="color:var(--dim)">None: the game as it shipped.</li>`;
  $("pending").hidden = mods.every(m => (values[m.id] || "") === (built[m.id] || ""));
}
$("mods").addEventListener("change", sync);
$("mods").addEventListener("input", sync);

// Cheats.
let items = null, cheatsShown = false;
async function cheat(command) {
  const r = await (await fetch(`/cheat?t=${T}`, {method: "POST", body: JSON.stringify({command})})).json();
  $("itemnote").textContent = r.error ? r.error : `Sent: ${command}. The game does it within a second.`;
}
async function loadItems() {
  try { items = await (await fetch(`/items?t=${T}`)).json(); } catch (e) { items = []; }
  // A legendary or set item whose name also belongs to one that drops is an old copy.
  const dropping = new Set(items.filter(i => i.drops).map(i => i.display));
  items.forEach(i => { i.old = i.rarity && !i.drops && dropping.has(i.display); });
  drawItems();
}
const LIMIT = 200;
function drawItems() {
  if (!items) return;
  if (!items.length) { $("items").innerHTML = ""; $("items").parentElement.hidden = true; $("itemnote").textContent = "The item list appears once the game has started with this mod."; return; }
  $("items").parentElement.hidden = false;
  const words = $("itemsearch").value.toLowerCase().split(/\s+/).filter(Boolean);
  const r = $("rarity").value, current = $("current").checked;
  const hits = items.filter(i => i.display &&
    (!r || (r === "legset" ? i.rarity : i.rarity === r)) && (!current || !i.old) &&
    words.every(w => (i.display + " " + i.name + " " + i.type).toLowerCase().includes(w)));
  $("items").innerHTML = `<tr><th>Item</th><th>Type</th><th>Game name</th><th></th></tr>` + hits.slice(0, LIMIT).map(i =>
    `<tr><td class="${i.rarity}">${esc(i.display)}</td><td>${esc(i.type)}</td><td class="n">${esc(i.name)}</td>` +
    `<td><button data-item="${esc(i.name)}">Give</button></td></tr>`).join("");
  $("itemnote").textContent = hits.length > LIMIT ? `Showing ${LIMIT} of ${hits.length}. Type more to narrow it down.` : `${hits.length} items.`;
}
function drawCheats(s) {
  $("cheatstab").hidden = !s.cheats;
  if (!s.cheats) { if (!$("cheats").hidden) showTab("mods"); return; }
  if (!cheatsShown) { cheatsShown = true; loadItems(); }
  const st = s.cheat_status, fresh = st && Date.now() / 1000 - st.updated < 10;
  $("cheatstate").textContent = !s.running && !fresh ? "Start the game and load a hero. Requests made now wait until you are in a game." :
    fresh && st.in_game ? `In game: level ${st.level}, paragon ${st.paragon}, gold ${(st.gold || 0).toLocaleString()}.` : "Waiting for you to enter a game; requests wait until then.";
  if (st && st.log) $("cheatlog").textContent = st.log.join("\n");
  if (items && !items.length && fresh) loadItems();
  drawRules(s);
}
let rulesDrawn = false;
function drawRules(s) {
  if (!s.rules) return;
  if (!rulesDrawn) {
    rulesDrawn = true;
    $("rules").innerHTML = "<tr><th>Rule</th><th>What it does</th><th>Setting</th></tr>" + s.rules.map(r =>
      `<tr><td>${esc(r.title)}</td><td>${esc(r.text)}</td><td><select data-rule="${esc(r.name)}" aria-label="${esc(r.title)}">` +
      `<option value="default">Normal (${r.normal ? "on" : "off"})</option><option value="on">On</option><option value="off">Off</option></select></td></tr>`).join("");
  }
  document.querySelectorAll("#rules select").forEach(sel => {
    if (sel !== document.activeElement) sel.value = s.rule_states[sel.dataset.rule] || "default";
  });
}
$("rules").addEventListener("change", async e => {
  const sel = e.target.closest("select[data-rule]");
  if (!sel) return;
  const r = await (await fetch(`/rules?t=${T}`, {method: "POST", body: JSON.stringify({name: sel.dataset.rule, state: sel.value})})).json();
  if (r.error) $("cheatstate").textContent = r.error;
});
$("cheats").addEventListener("click", e => {
  const b = e.target.closest("button");
  if (!b) return;
  if (b.dataset.cheat) cheat(b.dataset.cheat);
  else if (b.id === "addparagon") cheat(`paragon ${parseInt($("paragon").value, 10) || 0}`);
  else if (b.id === "addgold") cheat(`gold ${parseInt($("gold").value, 10) || 0}`);
  else if (b.dataset.item) cheat(`item ${b.dataset.item}`);
  else if (b.id === "spawn") cheat(`spawn ${$("spawnname").value.trim()} ${$("spawnrarity").value}`);
  else if (b.dataset.act) cheat(`cache ${b.dataset.act} ${Math.min(20, Math.max(1, parseInt($("caches").value, 10) || 1))}`);
});
["itemsearch", "rarity", "current"].forEach(id => $(id).addEventListener("input", drawItems));

// Build and play.
async function poll() {
  let s;
  try { s = await (await fetch(`/state?t=${T}&from=${shown}`)).json(); }
  catch (e) { $("status").textContent = "The helper program has stopped. Run Mods.command again."; $("status").className = "bad"; return; }
  built = s.values;
  if (!drawn || s.build !== drawnBuild) {
    mods = s.mods;
    drawMods(s.values);
    if (drawn) { items = null; cheatsShown = false; }  // another build has its own save folder
    drawn = true; drawnBuild = s.build;
    $("version").innerHTML = s.builds.map(b => `<option value="${esc(b.id)}"${b.id === s.build ? " selected" : ""}>${esc(b.label)}</option>`).join("");
    $("versionbox").hidden = s.builds.length < 2;
  }
  const chosen = s.builds.find(b => b.id === s.build);
  $("versionnote").textContent = chosen ? chosen.note : "";
  sync();
  $("version").disabled = s.building || s.running;
  drawCheats(s);
  if (s.lines.length) {
    const log = $("log");
    log.textContent += s.lines.join("\n") + "\n"; shown += s.lines.length; log.scrollTop = log.scrollHeight;
  }
  if (s.building && !wasBuilding) $("logbox").open = true;
  if (!s.building && wasBuilding && s.result === "failed") $("logbox").open = true;
  wasBuilding = s.building;
  $("build").disabled = s.building;
  $("play").disabled = s.building || !s.built || s.running;
  $("play").textContent = s.running ? "Running" : "Start game";
  const st = $("status");
  if (s.building) { st.textContent = "Building…"; st.className = ""; }
  else if (s.result === "ok") { st.textContent = "Built. Ready to play."; st.className = "ok"; }
  else if (s.result === "failed") { st.textContent = "Build failed. See the build output."; st.className = "bad"; }
  else if (!s.built) { st.textContent = "Not built yet."; st.className = ""; }
  else { st.textContent = s.running ? "The game is running." : "Built."; st.className = ""; }
  setTimeout(poll, s.building ? 400 : 1000);
}
$("build").addEventListener("click", async e => {
  e.preventDefault();
  const r = await (await fetch(`/build?t=${T}`, {method: "POST", body: JSON.stringify(formValues())})).json();
  if (r.error) { $("status").textContent = r.error; $("status").className = "bad"; }
});
$("version").addEventListener("change", async e => {
  const r = await (await fetch(`/select?t=${T}`, {method: "POST", body: JSON.stringify({build: e.target.value})})).json();
  if (r.error) { $("status").textContent = r.error; $("status").className = "bad"; }
});
$("play").addEventListener("click", async e => {
  e.preventDefault();
  const r = await (await fetch(`/play?t=${T}`, {method: "POST"})).json();
  if (r.error) { $("status").textContent = r.error; $("status").className = "bad"; }
});
poll();
</script></body></html>
"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, body, kind="application/json", code=200):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def allowed(self):
        # Only this computer's browser, on the address printed at start-up.
        host = self.headers.get("Host", "")
        return re.search(r"[?&]t=" + re.escape(TOKEN) + r"(&|$)", self.path) and host.split(":")[0] in ("127.0.0.1", "localhost")

    def do_GET(self):
        if not self.allowed():
            return self.send({"error": "not allowed"}, code=403)
        if self.path.startswith("/state"):
            start = int((re.search(r"from=(\d+)", self.path) or [0, 0])[1])
            with lock:
                state["last_seen"], state["seen"] = time.time(), True
                body = {"lines": state["log"][start:], "building": state["building"], "result": state["result"]}
            values = current_values()
            body.update(mods=mods(), values=values, built=game_path().exists(), running=game_running(),
                        builds=versions(), build=current_version(),
                        cheats=bool(values.get("D3_MOD_CHEATS")), cheat_status=cheats_status(),
                        rules=[{"name": n, "title": t, "text": d, "normal": on} for n, t, d, on in RULES], rule_states=read_rules())
            return self.send(body)
        if self.path.startswith("/items"):
            try:
                return self.send((cheats_dir() / "items.json").read_bytes())
            except OSError:
                return self.send([])
        return self.send(PAGE.encode(), "text/html; charset=utf-8")

    def do_POST(self):
        global game
        if not self.allowed():
            return self.send({"error": "not allowed"}, code=403)
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        if self.path.startswith("/build"):
            try:
                asked = json.loads(body or b"{}")
            except ValueError:
                return self.send({"error": "bad request"}, code=400)
            values = {}
            for m in mods():
                v = str(asked.get(m["id"], "")).strip()
                if v and not re.match(m["pattern"], v):
                    return self.send({"error": f"{m['title']}: '{v}' is not a valid value."})
                values[m["id"]] = v
            if game_running():
                return self.send({"error": "Close the game before building."})
            with lock:
                if state["building"]:
                    return self.send({"error": "A build is already running."})
                state["building"], state["result"] = True, ""
            threading.Thread(target=build, args=(current_version(), values), daemon=True).start()
            return self.send({"ok": True})
        if self.path.startswith("/cheat"):
            try:
                command = str(json.loads(body or b"{}").get("command", "")).strip()
            except ValueError:
                command = ""
            if not CHEAT.match(command):
                return self.send({"error": "That is not a cheat the game understands."}, code=400)
            folder = cheats_dir()
            folder.mkdir(parents=True, exist_ok=True)
            with open(folder / "commands.txt", "a") as f:
                f.write(command + "\n")
            return self.send({"ok": True})
        if self.path.startswith("/rules"):
            try:
                asked = json.loads(body or b"{}")
                name, value = str(asked.get("name", "")), str(asked.get("state", ""))
            except (ValueError, AttributeError):
                name, value = "", ""
            if name not in RULE_NAMES or value not in ("default", "on", "off"):
                return self.send({"error": "unknown rule"}, code=400)
            states = read_rules()
            states.pop(name, None)
            if value != "default":
                states[name] = value
            folder = cheats_dir()
            folder.mkdir(parents=True, exist_ok=True)
            temporary = folder / "rules.txt.tmp"
            temporary.write_text("".join(f"{n} {v}\n" for n, v in states.items()))
            temporary.replace(folder / "rules.txt")
            return self.send({"ok": True})
        if self.path.startswith("/select"):
            try:
                chosen = str(json.loads(body or b"{}").get("build", ""))
            except ValueError:
                chosen = ""
            if chosen not in VERSIONS:
                return self.send({"error": "unknown game version"}, code=400)
            if game_running():
                return self.send({"error": "Close the game before switching versions."})
            with lock:
                if state["building"]:
                    return self.send({"error": "Wait for the build to finish."})
                state["version"], state["result"] = chosen, ""
            return self.send({"ok": True})
        if self.path.startswith("/play"):
            if state["building"] or not game_path().exists():
                return self.send({"error": "Build first."})
            if not game_running():
                # The game's output, including the crash handler's call stack, goes to
                # game.log in the build directory; the run before is kept as game.prev.log.
                log = current_build() / "game.log"
                if log.exists():
                    log.replace(log.with_name("game.prev.log"))
                with open(log, "wb") as out:
                    game = subprocess.Popen([str(game_path())], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
            return self.send({"ok": True})
        return self.send({"error": "unknown"}, code=404)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=0, help="port to serve on (default: any free one)")
    ap.add_argument("--no-open", action="store_true", help="do not open the browser")
    ap.add_argument("--version", choices=sorted(VERSIONS), default="2.7.7", help="game version selected at start")
    args = ap.parse_args()
    state["version"] = args.version
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url = f"http://127.0.0.1:{server.server_address[1]}/?t={TOKEN}"
    print(f"Mods page: {url}\nClose the page (or press Ctrl-C here) to stop.", flush=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    if not args.no_open:
        webbrowser.open(url)
    try:
        while True:
            time.sleep(1)
            with lock:
                gone = state["seen"] and not state["building"] and time.time() - state["last_seen"] > 20
            if gone:
                print("The page was closed; stopping.")
                break
    except KeyboardInterrupt:
        pass
    server.shutdown()


if __name__ == "__main__":
    sys.exit(main())
