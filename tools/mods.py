#!/usr/bin/env python3
"""A small local page for choosing mods, building, and starting the game.

    python3 tools/mods.py            (or double-click Mods.command)

It serves one page on this computer only (127.0.0.1) and opens it in the browser.
The mods are listed in config/mods.json; each is a CMake option of this project
(see cmake/DecompSources.cmake). Ticking a box and pressing Build runs the same two
commands you would type: `cmake -B build/<name> -D<option>=<value> ...` (or
`cmake --preset macos ...` for a build directory not configured yet) and
`cmake --build build/<name>`. The page stops this program when it is closed.

Every configured build directory under build/ (such as build/macos) can be chosen
on the page; --build picks the one selected at start.
"""
import argparse
import json
import os
import pathlib
import re
import secrets
import subprocess
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_BUILD = ROOT / "build" / "macos"
TOKEN = secrets.token_urlsafe(16)

lock = threading.Lock()
state = {"log": [], "building": False, "result": "", "last_seen": time.time(), "seen": False, "build": DEFAULT_BUILD}
game = None


def mods():
    return json.loads((ROOT / "config" / "mods.json").read_text())


def cache_text(build_dir):
    cache = build_dir / "CMakeCache.txt"
    return cache.read_text(errors="replace") if cache.exists() else ""


def builds():
    """Build directories that can be chosen: every configured one with a game folder
    set, plus the preset's directory even before it is first configured."""
    found = {}
    for cache in sorted((ROOT / "build").glob("*/CMakeCache.txt")):
        text = cache.read_text(errors="replace")
        game_dir = re.search(r"^D3_GAME_DIR:[A-Z]+=(.*)$", text, re.M)
        if game_dir and game_dir.group(1):
            found[cache.parent] = game_dir.group(1)
    found.setdefault(DEFAULT_BUILD, "")
    result = []
    for path, game_dir in found.items():
        try:
            shown = str(pathlib.Path(game_dir).relative_to(ROOT)) if game_dir else "game"
        except ValueError:
            shown = game_dir
        result.append({"id": path.name, "label": f"build/{path.name} (game files: {shown})"})
    return result


def pick_build(name):
    for b in builds():
        if b["id"] == name:
            return ROOT / "build" / name
    return None


def current_build():
    with lock:
        return state["build"]


def game_path():
    return current_build() / "d3"


def save_dir():
    found = re.search(r"^D3_SAVE_DIR:[A-Z]+=(.*)$", cache_text(current_build()), re.M)
    return pathlib.Path(found.group(1)) if found and found.group(1) else ROOT / "save"


def cheats_dir():
    return save_dir() / "cheats"


def cheats_status():
    try:
        return json.loads((cheats_dir() / "status.json").read_text())
    except (OSError, ValueError):
        return None


# What the page may ask the game to do (src/mods/cheats.cpp reads the same lines).
CHEAT = re.compile(r"^(level ([1-9]|[1-6][0-9]|70)|paragon [1-9][0-9]{0,3}|item [A-Za-z0-9_\-]{1,127}( ([1-9]|[1-4][0-9]|50))?|gold [1-9][0-9]{0,10}|cube|rifts ([1-9]|[1-9][0-9]|1[0-4][0-9]|150)|cache [1-5]( ([1-9]|1[0-9]|20))?)$")


def current_values():
    """What the build directory is configured with now."""
    values = {}
    text = cache_text(current_build())
    for m in mods():
        found = re.search(rf"^{re.escape(m['id'])}:[A-Z]+=(.*)$", text, re.M)
        values[m["id"]] = found.group(1) if found else ""
    return values


def say(line):
    with lock:
        state["log"].append(line.rstrip("\n"))


def build(build_dir, values):
    try:
        args = [f"-D{k}={v}" for k, v in values.items()]
        relative = str(build_dir.relative_to(ROOT))
        # An existing build directory keeps its own settings (decomp, game files);
        # only one not configured yet needs the preset.
        configure = ["cmake", "-B", relative] if cache_text(build_dir) else ["cmake", "--preset", "macos"]
        for command in (configure + args, ["cmake", "--build", relative]):
            say("$ " + " ".join(command))
            p = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
            for line in p.stdout:
                say(line)
            if p.wait() != 0:
                say(f"Failed (exit {p.returncode}).")
                with lock:
                    state["result"] = "failed"
                return
        say("Build finished.")
        with lock:
            state["result"] = "ok"
    except FileNotFoundError as e:
        say(f"Could not run {e.filename}: is CMake installed?")
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
  :root { --bg:#f6f4ef; --card:#fffdf8; --ink:#1f1b16; --dim:#6b6257; --line:#ddd6c8; --accent:#9a3412; --ok:#166534; --bad:#b91c1c; --log:#17140f; --logink:#e9e2d3; }
  @media (prefers-color-scheme: dark) { :root { --bg:#17140f; --card:#211d17; --ink:#efe8da; --dim:#a79c8b; --line:#3a3329; --accent:#f59e5b; --ok:#86efac; --bad:#fca5a5; --log:#0e0c09; --logink:#d9d1c0; } }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--ink); font:16px/1.5 -apple-system, "Helvetica Neue", sans-serif; }
  main { max-width:720px; margin:0 auto; padding:32px 16px 48px; }
  h1 { font:600 28px/1.2 Georgia, serif; margin:0 0 4px; }
  .sub { color:var(--dim); margin:0 0 24px; }
  .mod { display:grid; grid-template-columns:auto 1fr auto; gap:4px 14px; align-items:center; background:var(--card); border:1px solid var(--line); border-radius:10px; padding:16px; margin-bottom:12px; }
  .mod input[type=checkbox] { width:22px; height:22px; accent-color:var(--accent); }
  .mod label { font-weight:600; }
  .mod p { grid-column:2 / 4; margin:0; color:var(--dim); font-size:14px; }
  .mod select, .mod input[type=text] { font:inherit; padding:6px 8px; border:1px solid var(--line); border-radius:6px; background:var(--bg); color:var(--ink); width:150px; }
  .mod :disabled { opacity:.45; }
  .row { display:flex; gap:12px; align-items:center; flex-wrap:wrap; margin:20px 0 12px; }
  button { font:inherit; font-weight:600; padding:10px 20px; border-radius:8px; border:1px solid var(--line); background:var(--card); color:var(--ink); cursor:pointer; }
  button.primary { background:var(--accent); border-color:var(--accent); color:#fff; }
  @media (prefers-color-scheme: dark) { button.primary { color:#1a1208; } }
  button:disabled { opacity:.5; cursor:default; }
  #status { color:var(--dim); }
  #status.ok { color:var(--ok); } #status.bad { color:var(--bad); }
  pre { background:var(--log); color:var(--logink); border-radius:10px; padding:14px; height:300px; overflow:auto; font:12px/1.45 ui-monospace, Menlo, monospace; white-space:pre-wrap; word-break:break-word; margin:0; }
  .note { color:var(--dim); font-size:13px; margin-top:14px; }
  h2 { font:600 20px/1.2 Georgia, serif; margin:36px 0 6px; }
  table { border-collapse:collapse; width:100%; background:var(--card); border:1px solid var(--line); border-radius:10px; overflow:hidden; }
  th, td { text-align:left; padding:8px 14px; border-bottom:1px solid var(--line); font-size:15px; }
  th { color:var(--dim); font-weight:600; font-size:13px; }
  tr:last-child td { border-bottom:0; }
  #cheats input[type=text], #cheats input[type=search], #cheats select { font:inherit; padding:8px 10px; border:1px solid var(--line); border-radius:6px; background:var(--card); color:var(--ink); }
  #items td.n { color:var(--dim); font:12px ui-monospace, Menlo, monospace; }
  #items .legendary { color:#c2410c; } #items .set { color:#15803d; }
  @media (prefers-color-scheme: dark) { #items .legendary { color:#fb923c; } #items .set { color:#4ade80; } }
  #items button { padding:4px 12px; }
  kbd { font:13px ui-monospace, Menlo, monospace; background:var(--bg); border:1px solid var(--line); border-bottom-width:2px; border-radius:5px; padding:1px 6px; }
</style></head><body><main>
<h1>Mods</h1>
<p class="sub">Tick what you want, then build. Nothing ticked builds the game exactly as it shipped.</p>
<div class="mod" id="versionbox" hidden><label for="version">Game version</label><select id="version" style="width:auto"></select>
  <p>Which build directory to build and start. Each has its own decomp, game files and mods.</p></div>
<form id="mods"></form>
<div class="row">
  <button class="primary" id="build">Build</button>
  <button id="play">Start game</button>
  <span id="status"></span>
</div>
<pre id="log" aria-label="Build output"></pre>
<section id="cheats" hidden>
<h2>Cheats</h2>
<p class="sub" style="margin-bottom:12px" id="cheatstate">Start the game and load a hero.</p>
<div class="row" style="margin-top:0">
  <button data-cheat="level 70">Level 70</button>
  <input type="text" inputmode="numeric" id="paragon" value="100" aria-label="Paragon levels to add" style="width:80px">
  <button id="addparagon">Add paragon levels</button>
  <input type="text" inputmode="numeric" id="gold" value="10000000" aria-label="Gold to add" style="width:110px">
  <button id="addgold">Add gold</button>
</div>
<div class="row">
  <button data-cheat="cube">Unlock Kanai's Cube</button>
  <button data-cheat="rifts 70">Greater rifts to 70 (opens Challenge Rifts)</button>
  <input type="text" inputmode="numeric" id="caches" value="1" aria-label="Number of caches" style="width:50px">
  <span>Horadric Caches from act</span>
  <button data-act="1">I</button><button data-act="2">II</button><button data-act="3">III</button><button data-act="4">IV</button><button data-act="5">V</button>
</div>
<div class="row">
  <input type="search" id="itemsearch" placeholder="Find an item: name or type" aria-label="Find an item" style="flex:1;min-width:220px">
  <select id="rarity" aria-label="Rarity"><option value="">All items</option><option value="legendary">Legendary</option><option value="set">Set</option><option value="legset" selected>Legendary and set</option></select>
  <label><input type="checkbox" id="current" checked> Hide old versions</label>
</div>
<table id="items"></table>
<p class="note" id="itemnote"></p>
<pre id="cheatlog" aria-label="What the game did" style="height:120px"></pre>
</section>
<h2>Controls</h2>
<p class="sub" style="margin-bottom:12px">The game shows Switch button names on screen. On the keyboard they are these keys; a connected controller also works.</p>
<table>
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
</table>
<p class="note">This page runs on your computer only. Closing it stops the helper program; a game you started keeps running.</p>
</main><script>
const T = new URLSearchParams(location.search).get("t");
const $ = id => document.getElementById(id);
let mods = [], shown = 0, drawn = false, drawnBuild = null;
function field(m, value) {
  const on = value !== "";
  const v = on ? value : m.default;
  let input;
  if (m.type === "toggle") {
    input = "<span></span>";
  } else if (m.type === "choice") {
    const all = m.choices.includes(v) ? m.choices : m.choices.concat([v]);
    const label = c => (m.labels && m.choices.indexOf(c) >= 0) ? m.labels[m.choices.indexOf(c)] : c;
    input = `<select data-v="${m.id}">` + all.map(c => `<option value="${c}"${c === v ? " selected" : ""}>${label(c)}</option>`).join("") + `</select>`;
  } else {
    input = `<input type="text" inputmode="decimal" data-v="${m.id}" value="${v}" aria-label="${m.title} value">`;
  }
  return `<div class="mod"><input type="checkbox" id="c_${m.id}" data-c="${m.id}"${on ? " checked" : ""}>
    <label for="c_${m.id}">${m.title}</label>${input}<p>${m.description}</p></div>`;
}
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
const esc = t => t.replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
function drawItems() {
  if (!items) return;
  if (!items.length) { $("items").innerHTML = ""; $("itemnote").textContent = "The item list appears once the game has started with this mod."; return; }
  const words = $("itemsearch").value.toLowerCase().split(/\s+/).filter(Boolean);
  const r = $("rarity").value, current = $("current").checked;
  const hits = items.filter(i => i.display &&
    (!r || (r === "legset" ? i.rarity : i.rarity === r)) && (!current || !i.old) &&
    words.every(w => (i.display + " " + i.name + " " + i.type).toLowerCase().includes(w)));
  $("items").innerHTML = `<tr><th>Item</th><th>Type</th><th>Game name</th><th></th></tr>` + hits.slice(0, 80).map(i =>
    `<tr><td class="${i.rarity}">${esc(i.display)}</td><td>${esc(i.type)}</td><td class="n">${esc(i.name)}</td>` +
    `<td><button data-item="${esc(i.name)}">Give</button></td></tr>`).join("");
  $("itemnote").textContent = hits.length > 80 ? `Showing 80 of ${hits.length}. Type more to narrow it down.` : `${hits.length} items.`;
}
function drawCheats(s) {
  $("cheats").hidden = !s.cheats;
  if (!s.cheats) return;
  if (!cheatsShown) { cheatsShown = true; loadItems(); }
  const st = s.cheat_status, fresh = st && Date.now() / 1000 - st.updated < 10;
  $("cheatstate").textContent = !s.running && !fresh ? "Start the game and load a hero. Requests made now wait until you are in a game." :
    fresh && st.in_game ? `In game: level ${st.level}, paragon ${st.paragon}, gold ${(st.gold || 0).toLocaleString()}.` : "Waiting for you to enter a game; requests wait until then.";
  if (st && st.log) $("cheatlog").textContent = st.log.join("\n");
  if (items && !items.length && fresh) loadItems();
}
$("cheats").addEventListener("click", e => {
  const b = e.target.closest("button");
  if (!b) return;
  if (b.dataset.cheat) cheat(b.dataset.cheat);
  else if (b.id === "addparagon") cheat(`paragon ${parseInt($("paragon").value, 10) || 0}`);
  else if (b.id === "addgold") cheat(`gold ${parseInt($("gold").value, 10) || 0}`);
  else if (b.dataset.item) cheat(`item ${b.dataset.item}`);
  else if (b.dataset.act) cheat(`cache ${b.dataset.act} ${Math.min(20, Math.max(1, parseInt($("caches").value, 10) || 1))}`);
});
["itemsearch", "rarity", "current"].forEach(id => $(id).addEventListener("input", drawItems));
function sync() {
  document.querySelectorAll("[data-c]").forEach(c => { const v = document.querySelector(`[data-v="${c.dataset.c}"]`); if (v) v.disabled = !c.checked; });
}
async function poll() {
  let s;
  try { s = await (await fetch(`/state?t=${T}&from=${shown}`)).json(); }
  catch (e) { $("status").textContent = "The helper program has stopped. Run Mods.command again."; $("status").className = "bad"; return; }
  if (!drawn || s.build !== drawnBuild) {
    mods = s.mods;
    $("mods").innerHTML = mods.map(m => field(m, s.values[m.id] || "")).join("");
    if (!drawn) $("mods").addEventListener("change", sync);
    sync(); drawn = true; drawnBuild = s.build;
    $("version").innerHTML = s.builds.map(b => `<option value="${b.id}"${b.id === s.build ? " selected" : ""}>${b.label}</option>`).join("");
    $("versionbox").hidden = s.builds.length < 2;
  }
  $("version").disabled = s.building || s.running;
  drawCheats(s);
  if (s.lines.length) {
    const log = $("log");
    log.textContent += s.lines.join("\n") + "\n"; shown += s.lines.length; log.scrollTop = log.scrollHeight;
  }
  $("build").disabled = s.building;
  $("play").disabled = s.building || !s.built || s.running;
  $("play").textContent = s.running ? "Game is running" : "Start game";
  const st = $("status");
  if (s.building) { st.textContent = "Building…"; st.className = ""; }
  else if (s.result === "ok") { st.textContent = "Built. Ready to play."; st.className = "ok"; }
  else if (s.result === "failed") { st.textContent = "Build failed. See the output below."; st.className = "bad"; }
  else if (!s.built) { st.textContent = "Not built yet."; st.className = ""; }
  setTimeout(poll, s.building ? 400 : 1000);
}
$("build").addEventListener("click", async e => {
  e.preventDefault();
  const values = {};
  document.querySelectorAll("[data-c]").forEach(c => { const v = document.querySelector(`[data-v="${c.dataset.c}"]`); values[c.dataset.c] = c.checked ? (v ? v.value.trim() : "1") : ""; });
  const r = await (await fetch(`/build?t=${T}`, {method: "POST", body: JSON.stringify(values)})).json();
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
                        builds=builds(), build=current_build().name,
                        cheats=bool(values.get("D3_MOD_CHEATS")), cheat_status=cheats_status())
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
            threading.Thread(target=build, args=(current_build(), values), daemon=True).start()
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
        if self.path.startswith("/select"):
            try:
                chosen = pick_build(str(json.loads(body or b"{}").get("build", "")))
            except ValueError:
                chosen = None
            if chosen is None:
                return self.send({"error": "unknown build directory"}, code=400)
            if game_running():
                return self.send({"error": "Close the game before switching versions."})
            with lock:
                if state["building"]:
                    return self.send({"error": "Wait for the build to finish."})
                state["build"], state["result"] = chosen, ""
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
    ap.add_argument("--build", metavar="NAME", help="build directory under build/ to select at start, such as macos-277")
    args = ap.parse_args()
    if args.build:
        chosen = pick_build(args.build)
        if chosen is None:
            ap.error(f"build/{args.build} is not a configured build directory")
        state["build"] = chosen
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
