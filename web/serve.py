#!/usr/bin/env python3
"""CHIPGENE web updater - local firmware hub for the browser.

One command:  python3 web/serve.py
Opens http://127.0.0.1:8123 with cards for every firmware branch. Each card
builds the branch with the Pico SDK and flashes a connected module over USB
(no BOOTSEL button - the running firmware reboots itself into the bootloader).

Stdlib only. Serves on 127.0.0.1 only; branch names are whitelisted by regex
before touching git; nothing is exposed to the network.
"""
import glob
import json
import os
import re
import shutil
import subprocess
import threading
import time
import uuid
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

REPO_URL = os.environ.get("CHIPGENE_REPO", "https://github.com/andrixb/CHIPMODS.git")
SDK_PATH = os.environ.get("PICO_SDK_PATH", str(Path.home() / "pico-sdk"))
PORT = int(os.environ.get("CHIPGENE_WEBPORT", "8123"))
ROOT = Path(__file__).resolve().parent          # <repo>/web
REPO_LOCAL = ROOT.parent                          # local checkout ("local" channel)
WEB_DIR = ROOT
OUT_DIR = WEB_DIR / "firmware"                    # built uf2s land here
BRANCH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,63}$")

JOBS = {}
LOCK = threading.Lock()


# ---------------------------------------------------------------- utilities
def run(cmd, log, cwd=None, env=None):
    """Stream a command's output into the job log; return exit code."""
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.Popen(cmd, cwd=cwd, env=e, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1)
    for line in p.stdout:
        log.append(line.rstrip())
        trim_log(log)
    return p.wait()


def trim_log(log, keep=400):
    if len(log) > keep:
        del log[: len(log) - keep]


def job_update(job_id, **kw):
    with LOCK:
        JOBS[job_id].update(kw)


def list_ports():
    return sorted(Path(p).name for p in glob.glob("/dev/cu.usbmodem*"))


def board_state():
    return {
        "ports": list_ports(),
        "bootloader": Path("/Volumes/RPI-RP2").is_dir(),
        "circuitpy": Path("/Volumes/CIRCUITPY").is_dir(),
    }


def branches():
    out = subprocess.run(["git", "ls-remote", "--heads", REPO_URL],
                         capture_output=True, text=True, timeout=30).stdout
    names = []
    for line in out.splitlines():
        m = re.search(r"refs/heads/(.+)$", line)
        if m:
            names.append(m.group(1))
    return ["local"] + sorted(set(names))


def uf2_for(branch):
    """Prebuilt uf2 path for a channel, or None."""
    if branch == "local":
        hits = sorted((REPO_LOCAL / "build").glob("*.uf2"))
    else:
        hits = sorted(OUT_DIR.glob("%s*.uf2" % branch.replace("/", "_")))
    return str(hits[-1]) if hits else None


# ---------------------------------------------------------------- build
def do_build(job_id, branch):
    log = JOBS[job_id]["log"]
    if branch == "local":
        src = REPO_LOCAL
        build = src / "build"
    else:
        work = OUT_DIR / "src"
        if work.exists():
            shutil.rmtree(work)
        log.append("cloning %s (%s)..." % (REPO_URL, branch))
        rc = run(["git", "clone", "--depth", "1", "--branch", branch,
                  REPO_URL, str(work)], log)
        if rc != 0:
            raise RuntimeError("clone failed")
        src, build = work, work / "build"

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    if not build.exists():
        log.append("configuring cmake...")
        rc = run(["cmake", "-S", str(src), "-B", str(build),
                  "-DPICO_SDK_PATH=" + SDK_PATH, "-DPICO_BOARD=pico",
                  "-DCMAKE_BUILD_TYPE=Release"], log)
        if rc != 0:
            raise RuntimeError("cmake configure failed")
    log.append("building...")
    rc = run(["cmake", "--build", str(build), "-j8"], log)
    if rc != 0:
        raise RuntimeError("build failed")
    uf2s = sorted(build.glob("*.uf2"))
    if not uf2s:
        raise RuntimeError("no uf2 produced")
    dst = OUT_DIR / ("%s_%s.uf2" % (branch.replace("/", "_"),
                                    time.strftime("%Y%m%d-%H%M%S")))
    shutil.copy(uf2s[-1], dst)
    JOBS[job_id]["uf2"] = str(dst)
    size_kb = dst.stat().st_size // 1024
    log.append("built %s (%d KB)" % (dst.name, size_kb))


# ---------------------------------------------------------------- flash
def enter_bootloader(log):
    """Reboot the module into the UF2 bootloader, no button needed."""
    if Path("/Volumes/RPI-RP2").is_dir():
        return True
    ports = glob.glob("/dev/cu.usbmodem*")
    if Path("/Volumes/CIRCUITPY").is_dir():
        log.append("CircuitPython detected - rebooting via REPL")
        if not ports:
            raise RuntimeError("no serial port to reach the REPL")
        p = ports[0]
        subprocess.run(["stty", "-f", p, "raw", "115200", "-echo"], check=False)
        with open(p, "r+b", buffering=0) as f:
            f.write(b"\x03")
            time.sleep(1)
            f.write(b"import microcontroller\r\n")
            time.sleep(0.3)
            f.write(b"microcontroller.on_next_reset(microcontroller.RunMode.UF2)\r\n")
            time.sleep(0.3)
            f.write(b"microcontroller.reset()\r\n")
        time.sleep(0.5)
    elif ports:
        log.append("1200-baud touch on %s -> bootloader" % ports[0])
        subprocess.run(["stty", "-f", ports[0], "1200"], check=False)
    else:
        raise RuntimeError("module not found on USB - plug it in "
                           "(or hold BOOTSEL while connecting)")
    for _ in range(30):
        if Path("/Volumes/RPI-RP2").is_dir():
            return True
        time.sleep(1)
    raise RuntimeError("bootloader volume RPI-RP2 never appeared")


def do_flash(job_id, uf2):
    log = JOBS[job_id]["log"]
    if not Path(uf2).is_file():
        raise RuntimeError("uf2 missing: " + uf2)
    enter_bootloader(log)
    log.append("writing %s ..." % Path(uf2).name)
    shutil.copy(uf2, "/Volumes/RPI-RP2/")
    for _ in range(15):
        time.sleep(0.5)
        if not Path("/Volumes/RPI-RP2").is_dir():
            break
    log.append("flashed - the module reboots into the new firmware")


# ---------------------------------------------------------------- job mgr
def start_job(kind, branch, flash):
    job_id = uuid.uuid4().hex[:10]
    with LOCK:
        JOBS[job_id] = {"id": job_id, "kind": kind, "branch": branch,
                        "state": "running", "stage": "build", "log": [],
                        "uf2": None, "error": None, "started": time.time()}

    def work():
        try:
            do_build(job_id, branch)
            if flash:
                job_update(job_id, stage="flash")
                do_flash(job_id, JOBS[job_id]["uf2"])
            job_update(job_id, state="done")
        except Exception as exc:
            job_update(job_id, state="error", error=str(exc))

    threading.Thread(target=work, daemon=True).start()
    return job_id


# ---------------------------------------------------------------- HTTP
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def read_json(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            html = (WEB_DIR / "index.html").read_bytes()
            self.send(200, html, "text/html; charset=utf-8")
        elif self.path == "/api/board":
            self.send(200, board_state())
        elif self.path == "/api/branches":
            try:
                self.send(200, {"branches": branches()})
            except Exception as exc:
                self.send(200, {"branches": ["local"], "error": str(exc)})
        elif self.path == "/api/uf2s":
            items = []
            for p in sorted(OUT_DIR.glob("*.uf2"), reverse=True)[:20]:
                items.append({"name": p.name, "kb": p.stat().st_size // 1024})
            self.send(200, {"uf2s": items})
        elif self.path.startswith("/api/job/"):
            jid = self.path.split("/")[-1]
            with LOCK:
                job = JOBS.get(jid)
                if not job:
                    self.send(404, {"error": "no such job"})
                    return
                out = dict(job)
                out["log"] = "\n".join(job["log"][-120:])
            self.send(200, out)
        else:
            self.send(404, {"error": "not found"})

    def do_POST(self):
        body = self.read_json()
        if self.path == "/api/build":
            br = body.get("branch", "")
            if not BRANCH_RE.match(br):
                self.send(400, {"error": "bad branch name"})
                return
            self.send(200, {"job": start_job("build", br, False)})
        elif self.path == "/api/buildflash":
            br = body.get("branch", "")
            if not BRANCH_RE.match(br):
                self.send(400, {"error": "bad branch name"})
                return
            self.send(200, {"job": start_job("buildflash", br, True)})
        elif self.path == "/api/flashfile":
            name = os.path.basename(body.get("name", ""))
            uf2 = (OUT_DIR / name).resolve()
            if not str(uf2).startswith(str(OUT_DIR.resolve())) or not uf2.is_file():
                self.send(400, {"error": "unknown firmware file"})
                return
            job_id = uuid.uuid4().hex[:10]
            with LOCK:
                JOBS[job_id] = {"id": job_id, "kind": "flash", "branch": name,
                                "state": "running", "stage": "flash",
                                "log": [], "uf2": str(uf2), "error": None,
                                "started": time.time()}

            def work():
                try:
                    do_flash(job_id, str(uf2))
                    job_update(job_id, state="done")
                except Exception as exc:
                    job_update(job_id, state="error", error=str(exc))

            threading.Thread(target=work, daemon=True).start()
            self.send(200, {"job": job_id})
        else:
            self.send(404, {"error": "not found"})


def main():
    missing = []
    if not Path(SDK_PATH).is_dir():
        missing.append("Pico SDK at " + SDK_PATH)
    if not shutil.which("cmake"):
        missing.append("cmake")
    if missing:
        print("WARNING: build channel unavailable, missing: "
              + ", ".join(missing))
    url = "http://127.0.0.1:%d" % PORT
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print("CHIPGENE updater:", url)
    print("  repo :", REPO_URL)
    print("  sdk  :", SDK_PATH)
    if not os.environ.get("CHIPGENE_NOOPEN"):
        threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
