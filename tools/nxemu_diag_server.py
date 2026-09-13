#!/usr/bin/env python3
"""Persistent localhost diagnostics API for an NXEmu installation."""

from __future__ import annotations

import argparse
import json
import os
import re
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import psutil


MILESTONES = (
    (re.compile(r"title_id=([0-9A-Fa-f]{16})"), "rom_identified"),
    (re.compile(r"loaded module main"), "program_loaded"),
    (re.compile(r"PatchRomFS"), "romfs_ready"),
    (re.compile(r"NVDEC video stream started"), "video_started"),
    (re.compile(r"SwkbdTrace Initialize"), "keyboard_opened"),
    (re.compile(r"SwkbdTrace SubmitNormalText"), "keyboard_submitted"),
    (re.compile(r"SaveTrace EnsureSaveData begin"), "save_requested"),
    (re.compile(r"SaveTrace EnsureSaveData end success"), "save_ready"),
    (re.compile(r"OpenSaveDataFileSystem"), "save_filesystem_opened"),
    (re.compile(r"SaveTrace (CreateFile|OpenFile|Write)"), "save_io"),
    (re.compile(r"SaveTrace (Flush|Commit)"), "save_committed"),
    (re.compile(r"<(Critical|Error)>"), "error"),
)

STAGE_EVENTS = {
    "rom_identified", "program_loaded", "romfs_ready", "video_started",
    "keyboard_opened", "keyboard_submitted", "save_requested", "save_ready",
    "save_filesystem_opened", "save_io", "save_committed",
}


class State:
    def __init__(self, root: Path, title_id: str):
        self.root = root
        self.log = root / "user" / "log" / "nxemu_log.txt"
        self.events_file = root / "user" / "log" / "nxemu_events.jsonl"
        self.save = root / "user" / "nand" / "user" / "save" / "0000000000000000"
        self.title_id = title_id.upper()
        self.lock = threading.Lock()
        self.events: deque[dict] = deque(maxlen=10000)
        self.seq = 0
        self.stage = "idle"
        self.offset = 0
        self.pending = b""
        self.started = time.time()

    def emit(self, kind: str, **data) -> None:
        event = {"seq": self.seq, "host_time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                 "event": kind, **data}
        self.seq += 1
        with self.lock:
            self.events.append(event)
            if kind in STAGE_EVENTS:
                self.stage = kind
            self.events_file.parent.mkdir(parents=True, exist_ok=True)
            with self.events_file.open("a", encoding="utf-8") as out:
                out.write(json.dumps(event, ensure_ascii=False) + "\n")
                out.flush()

    def scan_line(self, line: str) -> None:
        for pattern, kind in MILESTONES:
            if pattern.search(line):
                self.emit(kind, line=line)
                return

    def watch(self) -> None:
        # Read the existing session once so the API immediately has context.
        if self.log.exists():
            data = self.log.read_bytes()
            for line in data.decode("utf-8", "replace").splitlines():
                self.scan_line(line)
            self.offset = len(data)
        self.emit("watcher_started", log=str(self.log))
        while True:
            try:
                size = self.log.stat().st_size
                if size < self.offset:
                    self.offset = 0
                    self.pending = b""
                    self.emit("log_rotated")
                if size > self.offset:
                    with self.log.open("rb") as src:
                        src.seek(self.offset)
                        chunk = src.read()
                    self.offset += len(chunk)
                    lines = (self.pending + chunk).split(b"\n")
                    self.pending = lines.pop()
                    for raw in lines:
                        self.scan_line(raw.rstrip(b"\r").decode("utf-8", "replace"))
            except FileNotFoundError:
                self.offset = 0
                self.pending = b""
            except Exception as exc:
                self.emit("watch_error", message=repr(exc))
            time.sleep(0.25)

    def process_status(self) -> dict:
        matches = []
        for proc in psutil.process_iter(("pid", "name", "create_time", "memory_info")):
            try:
                if proc.info["name"] and proc.info["name"].lower() == "nxemu.exe":
                    matches.append({"pid": proc.pid, "started": proc.info["create_time"],
                                    "rss": proc.info["memory_info"].rss})
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
        return {"running": bool(matches), "processes": matches}

    def save_status(self) -> dict:
        roots = list(self.save.glob(f"*/{self.title_id}")) if self.save.exists() else []
        files = []
        for root in roots:
            files.extend({"path": str(p), "size": p.stat().st_size,
                          "mtime": p.stat().st_mtime} for p in root.rglob("*") if p.is_file())
        return {"roots": [str(p) for p in roots], "file_count": len(files), "files": files}


class Handler(BaseHTTPRequestHandler):
    state: State

    def send_json(self, value, status=200):
        body = json.dumps(value, ensure_ascii=False, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        request = urlparse(self.path)
        query = parse_qs(request.query)
        if request.path in ("/", "/health"):
            return self.send_json({"ok": True, "uptime_seconds": time.time() - self.state.started})
        if request.path == "/status":
            return self.send_json({"stage": self.state.stage, **self.state.process_status(),
                                   "save": self.state.save_status()})
        if request.path == "/events":
            since = int(query.get("since", [0])[0])
            with self.state.lock:
                events = [event for event in self.state.events if event["seq"] >= since]
            return self.send_json({"next": self.state.seq, "events": events})
        if request.path == "/log/tail":
            count = min(max(int(query.get("lines", [200])[0]), 1), 5000)
            lines = self.state.log.read_text(encoding="utf-8", errors="replace").splitlines()
            return self.send_json({"lines": lines[-count:]})
        self.send_json({"error": "not found"}, 404)

    def log_message(self, fmt, *args):
        return


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(r"D:\NXEmu"))
    parser.add_argument("--title-id", default="0100F08026D0C000")
    parser.add_argument("--port", type=int, default=32180)
    args = parser.parse_args()
    state = State(args.root, args.title_id)
    Handler.state = state
    threading.Thread(target=state.watch, name="nxemu-log-watch", daemon=True).start()
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
