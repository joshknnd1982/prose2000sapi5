"""Does the firmware always acknowledge a cancel?

The engine waits for a CANCELLED reply before reusing the host, and falls back to killing
and restarting the emulator if one does not arrive. That fallback is expensive, so this
checks how often it is actually needed: cancel at a spread of points through an utterance
and record what comes back, with a timeout so a silent host shows up as such instead of
hanging the probe.
"""

import os
import queue
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prose_protocol import (HEADER, MAGIC, HOST, ROMS, control_prefix,  # noqa: E402
                            SPEAK, CANCEL, QUIT, READY, AUDIO, DONE, ERROR, CANCELLED)

NAMES = {READY: "READY", AUDIO: "AUDIO", DONE: "DONE", ERROR: "ERROR",
         CANCELLED: "CANCELLED"}
TEXT = ("This is a long sentence that the listener will interrupt part of the way "
        "through, exactly as a screen reader user does every time they press an arrow key.")
CREATE_NO_WINDOW = 0x08000000


class Host:
    """Same shape as the engine's client: a reader thread feeding a queue."""

    def __init__(self):
        self.proc = subprocess.Popen(
            [HOST, ROMS], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=0, creationflags=CREATE_NO_WINDOW)
        self.q = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        try:
            while True:
                head = self._exact(HEADER.size)
                magic, kind, gen, size = HEADER.unpack(head)
                payload = self._exact(size) if size else b""
                self.q.put((kind, gen, payload))
        except Exception:
            self.q.put((None, 0, b"pipe closed"))

    def _exact(self, n):
        parts, left = [], n
        while left:
            c = self.proc.stdout.read(left)
            if not c:
                raise EOFError
            parts.append(c)
            left -= len(c)
        return b"".join(parts)

    def send(self, kind, gen=0, payload=b""):
        self.proc.stdin.write(HEADER.pack(MAGIC, kind, gen, len(payload)) + payload)
        self.proc.stdin.flush()

    def get(self, timeout):
        try:
            return self.q.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self):
        try:
            self.send(QUIT)
            self.proc.wait(timeout=2)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass


def trial(wait):
    host = Host()
    if host.get(10) is None:
        print("  host never became ready")
        return
    host.send(SPEAK, 1, (control_prefix() + "warm up").encode())
    while True:
        msg = host.get(10)
        if msg is None or msg[0] in (DONE, ERROR, None):
            break

    host.send(SPEAK, 2, (control_prefix() + TEXT).encode())
    got, finished = 0, False
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        msg = host.get(max(0.001, deadline - time.monotonic()))
        if msg is None:
            break
        kind, gen, payload = msg
        if kind == AUDIO:
            got += len(payload)
        elif kind in (DONE, CANCELLED, ERROR, None):
            finished = True
            break

    start = time.monotonic()
    host.send(CANCEL, 2)
    replies = []
    while True:
        msg = host.get(2.0)
        if msg is None:
            replies.append("(silence)")
            break
        kind, gen, payload = msg
        replies.append(NAMES.get(kind, str(kind)))
        if kind in (CANCELLED, DONE, ERROR, None):
            break
    elapsed = (time.monotonic() - start) * 1000

    alive = host.proc.poll() is None
    audio_msgs = sum(1 for r in replies if r == "AUDIO")
    tail = [r for r in replies if r != "AUDIO"]
    print("  wait %5.0fms  utt_done=%-5s  ack in %6.0fms  %s%s  host=%s"
          % (wait * 1000, finished, elapsed,
             ("%d AUDIO then " % audio_msgs) if audio_msgs else "",
             ",".join(tail) or "-", "alive" if alive else "DEAD"))
    host.close()


def main():
    print("cancelling at points through an %d-character utterance:" % len(TEXT))
    for wait in (0.05, 0.20, 0.40, 0.50, 0.55, 0.60, 0.70, 0.90, 1.20):
        trial(wait)
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
