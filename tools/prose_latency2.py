"""How much responsiveness is left after trimming?

Trimming removed the silence that used to be *played*. What remains is the wall-clock time
the firmware spends generating that pad before any speech comes out of it - the engine has
to wait for it, because it cannot emit audio it has not received.

Measures:
  1. time from SPEAK until the chunk containing the first real speech sample arrives
     (this is the engine's true floor today), and
  2. whether splitting long text at sentence boundaries lowers that floor, since the pad
     appears to scale with how much text the firmware was handed at once.
"""

import array
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prose_protocol import ProseHost, control_prefix, SPEAK, AUDIO, DONE  # noqa: E402

RATE = 10000
GATE_WINDOW = 100
SPEECH_RMS = 600.0

PARAGRAPH = (
    "The Prose two thousand was built by Telesensory Systems in the nineteen eighties. "
    "It used a formant synthesizer descended from the work of Dennis Klatt. "
    "Its firmware still runs today inside an emulator. "
    "This paragraph exists to measure how long the engine takes to say its first word."
)


def rms(frame):
    return (sum(float(x) * x for x in frame) / len(frame)) ** 0.5 if frame else 0.0


def first_speech_sample(pcm):
    """Index of the first sample belonging to a window that is unambiguously speech."""
    a = array.array("h")
    a.frombytes(pcm)
    for w in range(len(a) // GATE_WINDOW):
        if rms(a[w * GATE_WINDOW:(w + 1) * GATE_WINDOW]) >= SPEECH_RMS:
            return w * GATE_WINDOW
    return None


def time_to_speech(host, text, gen):
    """Wall clock until the engine could emit the first speech sample."""
    payload = (control_prefix() + text).encode("ascii", "replace")
    start = time.monotonic()
    host.send(SPEAK, gen, payload)

    audio = bytearray()
    first_msg = None
    speech_at = None
    while True:
        kind, g, data = host.recv()
        if g != gen:
            continue
        if kind == AUDIO:
            if first_msg is None:
                first_msg = time.monotonic() - start
            audio += data
            if speech_at is None and first_speech_sample(bytes(audio)) is not None:
                speech_at = time.monotonic() - start
        elif kind in (DONE, 104, 105):
            total = time.monotonic() - start
            return first_msg, speech_at, total, bytes(audio)


def main():
    with ProseHost() as host:
        gen = 1
        time_to_speech(host, "warm up", gen)
        gen += 1

        print("=== time from SPEAK to the first speech sample being available ===")
        print("%-46s %9s %11s %9s" % ("phrase", "1st msg", "TO SPEECH", "synth"))
        for text in ["OK", "Cancel", "Documents folder",
                     "The quick brown fox jumps over the lazy dog.", PARAGRAPH]:
            first, speech, total, pcm = time_to_speech(host, text, gen)
            gen += 1
            pad = first_speech_sample(pcm)
            print("%-46s %8.0fms %10.0fms %8.0fms   (pad %.0f ms of audio)"
                  % (text[:44], (first or 0) * 1000, (speech or 0) * 1000, total * 1000,
                     1000.0 * (pad or 0) / RATE))

        print()
        print("=== does splitting a paragraph at sentences start it sooner? ===")
        first, speech, total, pcm = time_to_speech(host, PARAGRAPH, gen)
        gen += 1
        print("whole paragraph in one go : first speech at %6.0f ms" % ((speech or 0) * 1000))

        sentences = [s.strip() for s in re.split(r"(?<=[.!?])\s+", PARAGRAPH) if s.strip()]
        wall_start = time.monotonic()
        per_sentence = []
        for i, sentence in enumerate(sentences):
            _f, s_at, tot, spcm = time_to_speech(host, sentence, gen)
            gen += 1
            per_sentence.append((sentence, s_at, tot, len(spcm) / 20000.0))
            if i == 0:
                print("first sentence only       : first speech at %6.0f ms  <-- what the "
                      "listener waits" % ((s_at or 0) * 1000))
        print()
        print("  %-52s %9s %9s %8s" % ("sentence", "to speech", "synth", "audio"))
        playable = 0.0
        synth_total = 0.0
        for sentence, s_at, tot, audio_secs in per_sentence:
            synth_total += tot
            playable += audio_secs
            print("  %-52s %8.0fms %8.0fms %7.2fs"
                  % (sentence[:50], (s_at or 0) * 1000, tot * 1000, audio_secs))
        print()
        print("  total synthesis %.2fs for %.2fs of audio (%.1fx real time)"
              % (synth_total, playable, playable / synth_total if synth_total else 0))
        print("  -> later sentences synthesise far faster than they play, so they can be")
        print("     produced while the first one is still being heard: no gap.")
        print("  wall clock for the whole split run: %.2fs" % (time.monotonic() - wall_start))
    return 0


if __name__ == "__main__":
    sys.exit(main())
