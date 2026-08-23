"""Classify every firmware command letter by what it actually does.

Sweeps each letter from the ROM command table over a wide value range and reports whether
it behaves like a rate, pitch, volume or timbre control, so the SAPI 5 layer can expose
the ones that carry real meaning.
"""

import array
import hashlib
import math
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prose_protocol import ProseHost  # noqa: E402

ESC = chr(27)
TEXT = "The quick brown fox jumps over the lazy dog."
LETTERS = os.environ.get("PROSE_LETTERS", "ACINPVafgilprstvx")
VALUES = (0, 1, 2, 3, 5, 10, 25, 50, 85, 100, 150, 200, 250)


def analyse(pcm):
    a = array.array("h")
    a.frombytes(pcm)
    if not a:
        return None
    rms = math.sqrt(sum(float(x) * x for x in a) / len(a))
    sr, est = 10000, []
    for s in range(0, len(a) - 1024, 1024):
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
    return {"secs": len(a) / 10000.0, "rms": rms,
            "f0": statistics.median(est) if est else 0.0,
            "sha": hashlib.sha1(pcm).hexdigest()[:10]}


def spread(values):
    values = [v for v in values if v]
    if len(values) < 2:
        return 0.0
    lo, hi = min(values), max(values)
    return (hi / lo) if lo > 0 else 0.0


def main():
    print("text: %r" % TEXT)
    print("values swept: %s\n" % (VALUES,))
    summary = []
    for ch in LETTERS:
        rows = []
        for n in VALUES:
            try:
                with ProseHost() as host:
                    pcm = host.speak("%s[%d%s" % (ESC, n, ch) + TEXT, 1)
            except Exception as error:
                rows.append((n, None, str(error)))
                continue
            stats = analyse(pcm)
            rows.append((n, stats, None if stats else "no audio"))

        print("=== ESC[N%s ===" % ch)
        ok = [(n, r) for n, r, _e in rows if r]
        for n, r, err in rows:
            if err:
                print("  N=%-4d ERROR %s" % (n, err[:60]))
            else:
                print("  N=%-4d %6.2fs  F0=%6.1fHz  RMS=%6.0f  %s"
                      % (n, r["secs"], r["f0"], r["rms"], r["sha"]))

        distinct = len({r["sha"] for _n, r in ok})
        dur = spread([r["secs"] for _n, r in ok])
        f0s = spread([r["f0"] for _n, r in ok])
        rmss = spread([r["rms"] for _n, r in ok])
        traits = []
        if dur > 1.15:
            traits.append("RATE-like (%.2fx duration)" % dur)
        if f0s > 1.15:
            traits.append("PITCH-like (%.2fx F0)" % f0s)
        if rmss > 1.30:
            traits.append("VOLUME-like (%.2fx RMS)" % rmss)
        if distinct > 1 and not traits:
            traits.append("TIMBRE-only (%d distinct renders)" % distinct)
        print("  -> %d distinct renders; %s\n"
              % (distinct, ", ".join(traits) if traits else "no effect"))
        summary.append((ch, distinct, traits))

    print("=" * 72)
    print("%-6s %-9s %s" % ("cmd", "distinct", "behaviour"))
    for ch, distinct, traits in summary:
        print("%-6s %-9d %s" % ("ESC[N" + ch, distinct,
                                ", ".join(traits) if traits else "no effect"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
