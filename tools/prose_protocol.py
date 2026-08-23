"""Render Prose 2000 speech to WAV files by driving ProseHost.exe directly.

This is a development/verification harness: it speaks to the emulated firmware
over the same binary protocol the SAPI 5 engine uses, with no SAPI 4 and no
registry involvement whatsoever.
"""

import argparse
import os
import struct
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HOST = os.path.join(ROOT, "bin", "prose2000", "ProseHost.exe")
ROMS = os.path.join(ROOT, "bin", "prose2000", "roms")

MAGIC = 0x4B325250          # 'PR2K'
SPEAK, CANCEL, QUIT = 1, 2, 3
READY, AUDIO, DONE, ERROR, CANCELLED = 101, 102, 103, 104, 105
HEADER = struct.Struct("<IIII")
CREATE_NO_WINDOW = 0x08000000

SAMPLE_RATE = 10000
CHANNELS = 1
SAMPLE_WIDTH = 2

# Native firmware ranges, as used by the NVDA driver.
RATE_MIN, RATE_DEFAULT, RATE_MAX = 50, 150, 250      # words per minute
PITCH_MIN, PITCH_DEFAULT, PITCH_MAX = 50, 85, 200    # Hz-ish firmware units
ATTEN_MIN, ATTEN_MAX = 0, 15                         # 0 = loudest


class HostError(RuntimeError):
    pass


class ProseHost:
    """Owns one ProseHost.exe process and its framed stdio protocol."""

    def __init__(self, host=HOST, roms=ROMS):
        self.host = host
        self.roms = roms
        self.proc = None

    def start(self):
        if not os.path.isfile(self.host):
            raise HostError("missing host: %s" % self.host)
        self.proc = subprocess.Popen(
            [self.host, self.roms],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
            creationflags=CREATE_NO_WINDOW,
        )
        kind, _gen, payload = self.recv()
        if kind != READY:
            raise HostError("host did not report ready: %r %r" % (kind, payload))
        return self

    def _read_exact(self, size):
        parts, remaining = [], size
        while remaining:
            chunk = self.proc.stdout.read(remaining)
            if not chunk:
                raise HostError("host closed the pipe")
            parts.append(chunk)
            remaining -= len(chunk)
        return b"".join(parts)

    def recv(self):
        magic, kind, gen, size = HEADER.unpack(self._read_exact(HEADER.size))
        if magic != MAGIC:
            raise HostError("bad magic 0x%08x" % magic)
        return kind, gen, (self._read_exact(size) if size else b"")

    def send(self, kind, generation=0, payload=b""):
        self.proc.stdin.write(HEADER.pack(MAGIC, kind, generation, len(payload)) + payload)
        self.proc.stdin.flush()

    def speak(self, text, generation=1):
        """Send text, collect PCM until DONE. Returns raw 16-bit mono bytes."""
        self.send(SPEAK, generation, text.encode("ascii", "replace"))
        audio = bytearray()
        while True:
            kind, gen, payload = self.recv()
            if gen != generation:
                continue
            if kind == AUDIO:
                audio += payload
            elif kind == DONE:
                return bytes(audio)
            elif kind == CANCELLED:
                return bytes(audio)
            elif kind == ERROR:
                raise HostError(payload.decode("utf-8", "replace"))

    def stop(self):
        if self.proc is None:
            return
        try:
            self.send(QUIT)
            self.proc.wait(timeout=3)
        except Exception:
            self.proc.kill()
        finally:
            self.proc = None

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()


def control_prefix(rate=RATE_DEFAULT, pitch=PITCH_DEFAULT, atten=ATTEN_MIN):
    """Build the firmware's native escape prefix for rate/pitch/volume."""
    esc = chr(27)
    return "%s[%dr%s[0V%s[%dp%s[%da" % (esc, rate, esc, esc, pitch, esc, atten)


def write_wav(path, pcm):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(SAMPLE_WIDTH)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)
    return len(pcm) / float(SAMPLE_RATE * SAMPLE_WIDTH * CHANNELS)


def main():
    ap = argparse.ArgumentParser(description="Render Prose 2000 speech to WAV")
    ap.add_argument("--out", default=os.path.join(ROOT, "samples"))
    ap.add_argument("--text", default=None, help="render just this text")
    ap.add_argument("--rate", type=int, default=RATE_DEFAULT)
    ap.add_argument("--pitch", type=int, default=PITCH_DEFAULT)
    ap.add_argument("--atten", type=int, default=ATTEN_MIN)
    args = ap.parse_args()

    if args.text is not None:
        jobs = [("custom", args.text, args.rate, args.pitch, args.atten)]
    else:
        jobs = [
            ("00_prose2000_default",
             "This is the Prose two thousand, speaking through the Microsoft "
             "Speech A P I version five. There is one English male voice.",
             RATE_DEFAULT, PITCH_DEFAULT, ATTEN_MIN),
            ("01_rate_slow", "This is the slowest speaking rate, fifty words per minute.",
             RATE_MIN, PITCH_DEFAULT, ATTEN_MIN),
            ("02_rate_default", "This is the default speaking rate, one hundred fifty words per minute.",
             RATE_DEFAULT, PITCH_DEFAULT, ATTEN_MIN),
            ("03_rate_fast", "This is the fastest speaking rate, two hundred fifty words per minute.",
             RATE_MAX, PITCH_DEFAULT, ATTEN_MIN),
            ("04_pitch_low", "This is the lowest pitch setting.",
             RATE_DEFAULT, PITCH_MIN, ATTEN_MIN),
            ("05_pitch_default", "This is the default pitch setting.",
             RATE_DEFAULT, PITCH_DEFAULT, ATTEN_MIN),
            ("06_pitch_high", "This is the highest pitch setting.",
             RATE_DEFAULT, PITCH_MAX, ATTEN_MIN),
            ("07_volume_full", "This is full volume.",
             RATE_DEFAULT, PITCH_DEFAULT, ATTEN_MIN),
            ("08_volume_quiet", "This is the quietest volume setting.",
             RATE_DEFAULT, PITCH_DEFAULT, ATTEN_MAX),
            ("09_numbers_and_punctuation",
             "The Prose 2000 was made in 1982. It costs $1,995.50; that is a lot!",
             RATE_DEFAULT, PITCH_DEFAULT, ATTEN_MIN),
        ]

    failures = 0
    with ProseHost() as host:
        generation = 1
        for name, text, rate, pitch, atten in jobs:
            try:
                pcm = host.speak(control_prefix(rate, pitch, atten) + text, generation)
                generation += 1
                if not pcm:
                    print("FAIL %-28s produced no audio" % name)
                    failures += 1
                    continue
                path = os.path.join(args.out, name + ".wav")
                seconds = write_wav(path, pcm)
                print("OK   %-28s %6.2fs  %7d bytes  %s" % (name, seconds, len(pcm), path))
            except Exception as error:
                print("FAIL %-28s %s" % (name, error))
                failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
