#!/usr/bin/env python3
"""A small local page for choosing mods, building, and starting the game.

    python3 tools/mods.py            (or double-click Mods.command)

It serves one page on this computer only (127.0.0.1) and opens it in the browser.
The mods are listed in config/mods.json; each is a CMake option of this project
(see cmake/DecompSources.cmake). Ticking a box and pressing Build runs the same two
commands you would type: `cmake --preset macos -D<option>=<value> ...` and
`cmake --build build/macos`. The page stops this program when it is closed.
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
BUILD = ROOT / "build" / "macos"
GAME = BUILD / "d3"
TOKEN = secrets.token_urlsafe(16)

lock = threading.Lock()
state = {"log": [], "building": False, "result": "", "last_seen": time.time(), "seen": False}
game = None


def mods():
    return json.loads((ROOT / "config" / "mods.json").read_text())


def current_values():
    """What the build directory is configured with now."""
    values = {}
    cache = BUILD / "CMakeCache.txt"
    text = cache.read_text() if cache.exists() else ""
    for m in mods():
        found = re.search(rf"^{re.escape(m['id'])}:[A-Z]+=(.*)$", text, re.M)
        values[m["id"]] = found.group(1) if found else ""
    return values


def say(line):
    with lock:
        state["log"].append(line.rstrip("\n"))


def build(values):
    try:
        args = [f"-D{k}={v}" for k, v in values.items()]
        for command in (["cmake", "--preset", "macos"] + args, ["cmake", "--build", "build/macos"]):
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
</style></head><body><main>
<h1>Mods</h1>
<p class="sub">Tick what you want, then build. Nothing ticked builds the game exactly as it shipped.</p>
<form id="mods"></form>
<div class="row">
  <button class="primary" id="build">Build</button>
  <button id="play">Start game</button>
  <span id="status"></span>
</div>
<pre id="log" aria-label="Build output"></pre>
<p class="note">This page runs on your computer only. Closing it stops the helper program; a game you started keeps running.</p>
</main><script>
const T = new URLSearchParams(location.search).get("t");
const $ = id => document.getElementById(id);
let mods = [], shown = 0, drawn = false;
function field(m, value) {
  const on = value !== "";
  const v = on ? value : m.default;
  let input;
  if (m.type === "choice") {
    const all = m.choices.includes(v) ? m.choices : m.choices.concat([v]);
    input = `<select data-v="${m.id}">` + all.map(c => `<option${c === v ? " selected" : ""}>${c}</option>`).join("") + `</select>`;
  } else {
    input = `<input type="text" inputmode="decimal" data-v="${m.id}" value="${v}" aria-label="${m.title} value">`;
  }
  return `<div class="mod"><input type="checkbox" id="c_${m.id}" data-c="${m.id}"${on ? " checked" : ""}>
    <label for="c_${m.id}">${m.title}</label>${input}<p>${m.description}</p></div>`;
}
function sync() {
  document.querySelectorAll("[data-c]").forEach(c => { document.querySelector(`[data-v="${c.dataset.c}"]`).disabled = !c.checked; });
}
async function poll() {
  let s;
  try { s = await (await fetch(`/state?t=${T}&from=${shown}`)).json(); }
  catch (e) { $("status").textContent = "The helper program has stopped. Run Mods.command again."; $("status").className = "bad"; return; }
  if (!drawn) {
    mods = s.mods;
    $("mods").innerHTML = mods.map(m => field(m, s.values[m.id] || "")).join("");
    $("mods").addEventListener("change", sync); sync(); drawn = true;
  }
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
  document.querySelectorAll("[data-c]").forEach(c => { values[c.dataset.c] = c.checked ? document.querySelector(`[data-v="${c.dataset.c}"]`).value.trim() : ""; });
  const r = await (await fetch(`/build?t=${T}`, {method: "POST", body: JSON.stringify(values)})).json();
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
            body.update(mods=mods(), values=current_values(), built=GAME.exists(), running=game_running())
            return self.send(body)
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
            threading.Thread(target=build, args=(values,), daemon=True).start()
            return self.send({"ok": True})
        if self.path.startswith("/play"):
            if state["building"] or not GAME.exists():
                return self.send({"error": "Build first."})
            if not game_running():
                game = subprocess.Popen([str(GAME)], cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            return self.send({"ok": True})
        return self.send({"error": "unknown"}, code=404)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=0, help="port to serve on (default: any free one)")
    ap.add_argument("--no-open", action="store_true", help="do not open the browser")
    args = ap.parse_args()
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
