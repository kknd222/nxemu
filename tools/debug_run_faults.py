"""Bounded append-only fatal watcher; starts after this run's startup log exists."""
from pathlib import Path
import time

class RunFatalWatcher:
    def __init__(self, log, budget=65536):
        self.path = Path(log)
        self.budget = budget
        if budget < 128: raise ValueError('budget must be >=128')
        self.offset = self.path.stat().st_size if self.path.exists() else 0
        self.pending = b''
        self.first = None
        self.bytes_read = 0
        self.rotations = 0
        self.scan_ns = 0
        self.identity = self.path.stat().st_ino if self.path.exists() else None

    def poll(self):
        if self.first: return self.first
        started = time.perf_counter_ns()
        try:
            stat = self.path.stat()
            if stat.st_size < self.offset or (self.identity is not None and stat.st_ino != self.identity):
                self.offset = 0; self.pending = b''; self.rotations += 1
            self.identity = stat.st_ino
            with self.path.open('rb') as stream:
                stream.seek(self.offset); raw = stream.read(self.budget)
            self.offset += len(raw); self.bytes_read += len(raw)
            lines = (self.pending + raw).split(b'\n')
            self.pending = lines.pop()[-4096:]
            for line in lines:
                if b'Userspace PANIC!' in line or b'Emulated program broke execution!' in line:
                    self.first = {'kind': 'guest_panic' if b'Userspace PANIC!' in line else 'guest_break',
                                  'line': line.decode('utf-8', errors='replace')[:2048],
                                  'log': str(self.path), 'offset_after_read': self.offset,
                                  'bytes_read': self.bytes_read, 'rotations': self.rotations}
                    break
        except OSError:
            pass
        finally:
            self.scan_ns += time.perf_counter_ns() - started
        return self.first
