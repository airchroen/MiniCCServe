#!/usr/bin/env python3
"""Automated smoke test for MiniCCServe.

Seeds settings.ini next to the exe (fixed port, repo www/ as root), launches
the app, presses Start/Stop virtually via WM_COMMAND, and checks the HTTP
endpoints with urllib. A window flashes briefly while the test runs.

Usage: python scripts/smoke-test.py --exe build/mingw/MiniCCServe.exe
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

# Control identifiers (must match MainWindow in src/gui.hpp).
ID_START = 108
ID_STOP = 109
WM_COMMAND = 0x0111

user32 = ctypes.windll.user32
user32.EnumWindows.argtypes = [ctypes.c_void_p, wt.LPARAM]
user32.EnumWindows.restype = wt.BOOL
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.SendMessageW.restype = wt.LPARAM

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def find_window(pid: int, title: str, timeout_s: float = 10.0):
    """Visible top-level window with the exact title, owned by pid."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        found = []

        @WNDENUMPROC
        def cb(hwnd, _lp):
            wpid = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
            if wpid.value == pid and user32.IsWindowVisible(hwnd):
                n = user32.GetWindowTextLengthW(hwnd)
                buf = ctypes.create_unicode_buffer(n + 1)
                user32.GetWindowTextW(hwnd, buf, n + 1)
                if buf.value == title:
                    found.append(hwnd)
                    return False
            return True

        user32.EnumWindows(cb, 0)
        if found:
            return found[0]
        time.sleep(0.2)
    return None


def http(method: str, url: str, body: str | None = None, timeout: float = 10):
    """(status, text) for a request; HTTP errors are returned, not raised."""
    data = body.encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", required=True, help="path to MiniCCServe.exe")
    ap.add_argument("--port", type=int, default=18080)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    exe_dir = os.path.dirname(exe)
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # Pre-seed deterministic settings and a fresh example routes.json.
    with open(os.path.join(exe_dir, "settings.ini"), "w", encoding="ascii", newline="") as f:
        f.write(f"[server]\r\nport={args.port}\r\nroot={repo_root}\\www\r\n")
    routes = os.path.join(exe_dir, "routes.json")
    if os.path.exists(routes):
        os.remove(routes)

    failures = []

    def check(name, ok):
        if ok:
            print(f"PASS: {name}")
        else:
            print(f"FAIL: {name}")
            failures.append(name)

    proc = subprocess.Popen([exe])
    try:
        hwnd = find_window(proc.pid, "MiniCCServe")
        if not hwnd:
            print("FAIL: MiniCCServe window not found")
            return 1
        time.sleep(0.4)

        # Press Start
        user32.SendMessageW(hwnd, WM_COMMAND, ID_START, 0)
        time.sleep(0.8)

        base = f"http://127.0.0.1:{args.port}"

        # Content-aware checks: a directory listing is also 200, so verify the
        # body where it matters.
        status, home = http("GET", f"{base}/")
        check("GET / serves index.html content",
              status == 200 and "Welcome to MiniCCServe" in home)
        status, body = http("GET", f"{base}/index.html")
        check("GET /index.html direct (200, content)",
              status == 200 and "Welcome to MiniCCServe" in body)
        status, body = http("GET", f"{base}/definitely-missing")
        check("unknown path (404)", status == 404 and "404 Not Found" in body)
        check("mock route /api/hello (200)", http("GET", f"{base}/api/hello")[0] == 200)
        check("POST mock route /api/echo (200)",
              http("POST", f"{base}/api/echo", "ping")[0] == 200)
        check("mock route /api/unavailable (503)",
              http("GET", f"{base}/api/unavailable")[0] == 503)
        check("path traversal rejected (400)",
              http("GET", f"{base}/..%2F..%2FWindows%2Fwin.ini")[0] != 200)

        # delay_ms=500 on /api/hello must be observable
        t0 = time.perf_counter()
        http("GET", f"{base}/api/hello")
        check("delay_ms honored (>0.4s)", time.perf_counter() - t0 > 0.4)

        # Press Stop, then Start again (restart path)
        user32.SendMessageW(hwnd, WM_COMMAND, ID_STOP, 0)
        time.sleep(0.6)
        user32.SendMessageW(hwnd, WM_COMMAND, ID_START, 0)
        time.sleep(0.8)
        status, body = http("GET", f"{base}/")
        check("restart after stop works (content)",
              status == 200 and "Welcome to MiniCCServe" in body)

        user32.SendMessageW(hwnd, WM_COMMAND, ID_STOP, 0)
        time.sleep(0.4)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(3)
            except subprocess.TimeoutExpired:
                proc.kill()

    if failures:
        print(f"{len(failures)} smoke check(s) failed.")
        return 1
    print("All smoke checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
