"""How fast can speech be interrupted?

For a screen reader this matters more than cold-start latency. Every arrow-key press
cancels whatever is being said and starts something new, so the felt responsiveness of the
voice is really: cancel the current utterance, then hear the next one.

Measures the cancel round trip, and the full interrupt-to-next-speech time.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prose_protocol import (ProseHost, control_prefix, SPEAK, CANCEL,  # noqa: E402
                            AUDIO, DONE, ERROR, CANCELLED)

LONG = ("This is a long sentence that the listener will interrupt part of the way "
        "through, exactly as a screen reader user does every time they press an arrow key.")
NEXT = "Documents folder"


def drain_until_cancelled(host, timeout=3.0):
    """Time from sending CANCEL to the host confirming it stopped."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        kind, _gen, _payload = host.recv()
        if kind == CANCELLED:
            return time.monotonic(), True
        if kind in (DONE, ERROR):
            return time.monotonic(), False
    return time.monotonic(), False


def main():
    with ProseHost() as host:
        gen = 1
        host.speak(control_prefix() + "warm up", gen)
        gen += 1

        print("=== interrupting speech ===")
        print("%-14s %12s %14s %14s" % ("wait before", "cancel ack", "next utterance",
                                        "TOTAL"))
        for wait in (0.05, 0.15, 0.30, 0.60):
            # Start a long utterance and let it run for a while.
            host.send(SPEAK, gen, (control_prefix() + LONG).encode("ascii", "replace"))
            speaking_gen = gen
            gen += 1

            got = 0
            deadline = time.monotonic() + wait
            while time.monotonic() < deadline:
                kind, g, data = host.recv()
                if kind == AUDIO and g == speaking_gen:
                    got += len(data)
                elif kind in (DONE, CANCELLED, ERROR):
                    break

            # Interrupt it.
            t_cancel = time.monotonic()
            host.send(CANCEL, speaking_gen)
            t_acked, clean = drain_until_cancelled(host)
            cancel_ms = (t_acked - t_cancel) * 1000

            # Now say the next thing and see how long until its audio is available.
            t_next = time.monotonic()
            host.send(SPEAK, gen, (control_prefix() + NEXT).encode("ascii", "replace"))
            next_gen = gen
            gen += 1
            first_audio = None
            while True:
                kind, g, data = host.recv()
                if g != next_gen:
                    continue
                if kind == AUDIO and first_audio is None:
                    first_audio = time.monotonic()
                if kind in (DONE, CANCELLED, ERROR):
                    break
            next_ms = ((first_audio or time.monotonic()) - t_next) * 1000

            print("%-14s %10.0fms %12.0fms %12.0fms  %s"
                  % ("%.0f ms" % (wait * 1000), cancel_ms, next_ms,
                     cancel_ms + next_ms, "clean" if clean else "NOT ACKED"))

        print()
        print("cancel ack     : CANCEL sent -> host confirms it stopped")
        print("next utterance : SPEAK sent -> first audio of the new phrase")
        print("TOTAL          : what the user waits after pressing a key")
    return 0


if __name__ == "__main__":
    sys.exit(main())
