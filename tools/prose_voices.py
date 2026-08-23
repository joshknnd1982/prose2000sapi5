"""Map the firmware's ESC[<n>V voice selector exhaustively.

The NVDA driver always sends ESC[0V, so only voice 0 has ever been heard through it.
This sweeps every value, groups byte-identical renders together, and writes one WAV per
distinct voice so they can be listened to side by side.
"""

import array
import hashlib
import math
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prose_protocol import ProseHost, write_wav, ROOT  # noqa: E402

ESC = chr(27)
OUT = os.path.join(ROOT, "samples", "voices")
TEXT = ("This is the Prose two thousand. The quick brown fox "
        "jumps over the lazy dog.")


def f0_of(pcm):
    a = array.array("h")
    a.frombytes(pcm)
    sr, est = 10000, []
    for s in range(0, len(a) - 1024, 512):
        fr = [float(x) for x in a[s:s + 1024]]
        if max(max(fr), -min(fr)) < 2000:
            continue
        m = sum(fr) / len(fr)
        fr = [x - m for x in fr]
        for lag in range(25, 220):
            n = 1024 - lag
            num = sum(fr[i] * fr[i + lag] for i in range(n))
            d = math.sqrt(sum(fr[i] * fr[i] for i in range(n))
                          * sum(fr[i + lag] * fr[i + lag] for i in range(n)))
            if d and num / d > 0.45:
                est.append(sr / float(lag))
                break
    return statistics.median(est) if est else 0.0


def rms_of(pcm):
    a = array.array("h")
    a.frombytes(pcm)
    return math.sqrt(sum(float(x) * x for x in a) / len(a)) if a else 0.0


def main():
    os.makedirs(OUT, exist_ok=True)
    letter = sys.argv[1] if len(sys.argv) > 1 else "V"
    values = range(0, 16)

    results = []
    for n in values:
        try:
            with ProseHost() as host:
                pcm = host.speak("%s[%d%s" % (ESC, n, letter) + TEXT, 1)
        except Exception as error:
            print("  %s=%-3d ERROR %s" % (letter, n, error))
            continue
        if not pcm:
            print("  %s=%-3d no audio" % (letter, n))
            continue
        digest = hashlib.sha1(pcm).hexdigest()[:12]
        results.append((n, digest, pcm))
        print("  %s=%-3d %6.2fs  F0=%6.1fHz  RMS=%6.0f  sha=%s"
              % (letter, n, len(pcm) / 20000.0, f0_of(pcm), rms_of(pcm), digest))

    groups = {}
    for n, digest, pcm in results:
        groups.setdefault(digest, {"values": [], "pcm": pcm})["values"].append(n)

    print()
    print("distinct renders: %d" % len(groups))
    for index, (digest, info) in enumerate(groups.items()):
        vals = info["values"]
        pcm = info["pcm"]
        name = "%s%02d_value_%d" % (letter, index, vals[0])
        path = os.path.join(OUT, name + ".wav")
        write_wav(path, pcm)
        print("  voice %d: %s=%s  F0=%.1fHz  %.2fs  -> %s"
              % (index, letter, ",".join(str(v) for v in vals),
                 f0_of(pcm), len(pcm) / 20000.0, os.path.basename(path)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
