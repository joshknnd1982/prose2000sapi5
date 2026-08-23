"""Where does the delay between asking for speech and hearing it come from?

Separates the two candidates:
  * synthesis latency - wall-clock time from sending SPEAK to the first audio byte, and
  * leading silence   - silent samples at the head of the audio the firmware returns.

The second one is the one a listener actually experiences as lag, because it is played.
"""

import array
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prose_protocol import ProseHost, control_prefix, MAGIC, SPEAK, AUDIO, DONE  # noqa: E402

RATE = 10000
THRESHOLD = 300

PHRASES = [
    "OK",
    "Cancel",
    "Button",
    "File menu",
    "Documents folder",
    "The quick brown fox",
    "The quick brown fox jumps over the lazy dog.",
    "This is a much longer sentence, of the sort a screen reader would read out "
    "when the user asks it to read a whole paragraph of continuous text.",
]


def first_sound_index(pcm):
    a = array.array("h")
    a.frombytes(pcm)
    for i, v in enumerate(a):
        if abs(v) > THRESHOLD:
            return i
    return len(a)


def speak_timed(host, text, gen):
    """Returns (seconds to first AUDIO message, seconds to DONE, pcm)."""
    payload = (control_prefix() + text).encode("ascii", "replace")
    start = time.monotonic()
    host.send(SPEAK, gen, payload)

    audio = bytearray()
    first_audio_at = None
    while True:
        kind, g, data = host.recv()
        if g != gen:
            continue
        if kind == AUDIO:
            if first_audio_at is None:
                first_audio_at = time.monotonic() - start
            audio += data
        elif kind == DONE:
            return first_audio_at, time.monotonic() - start, bytes(audio)
        elif kind in (104, 105):
            return first_audio_at, time.monotonic() - start, bytes(audio)


def main():
    print("%-46s %9s %9s %9s %9s" % ("phrase", "1st msg", "lead sil", "TOTAL LAG", "audio"))
    print("-" * 90)
    with ProseHost() as host:
        gen = 1
        # One warm-up so process start-up is not charged to the first phrase.
        speak_timed(host, "warm up", gen)
        gen += 1

        for text in PHRASES:
            first, total, pcm = speak_timed(host, text, gen)
            gen += 1
            lead_samples = first_sound_index(pcm)
            lead_secs = lead_samples / float(RATE)
            # What the listener waits: time until the first audio byte arrives, plus the
            # silence at the head of it that still has to be played before anything is heard.
            perceived = (first or 0.0) + lead_secs
            print("%-46s %8.3fs %8.3fs %8.3fs %8.2fs"
                  % (text[:44], first or 0.0, lead_secs, perceived,
                     len(pcm) / 20000.0))

    print()
    print("'1st msg'   : wall clock from SPEAK to the first AUDIO message")
    print("'lead sil'  : silent samples at the head of the returned audio")
    print("'TOTAL LAG' : what the listener waits before hearing anything")
    return 0


if __name__ == "__main__":
    sys.exit(main())
