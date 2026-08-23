"""Safety gate for the extra firmware parameters.

A parameter is only fit to expose if, once set, the synthesiser keeps speaking. Anything
that can silence a later utterance is disqualified outright: this engine drives a screen
reader, and going quiet is worse than sounding wrong.

For each parameter and each value the test speaks several utterances in a row on one host,
mixing in the other parameters, and fails the parameter if any utterance comes back empty.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prose_protocol import ProseHost  # noqa: E402

ESC = chr(27)
TEXT = "The quick brown fox jumps over the lazy dog."
BASE = "%s[0V%s[150r%s[85p%s[0a" % (ESC, ESC, ESC, ESC)

# id -> (command letter, values to try)
PARAMS = {
    "expression": ("g", (0, 5, 50, 125, 250)),
    "phrasing":   ("s", (0, 5, 50, 125, 250)),
    "softness":   ("i", (0, 1)),
    "tone":       ("I", (0, 1)),
    "emphasis":   ("P", (0, 1)),
}

# Parameters that are known to be unsafe and are deliberately not exposed by the engine.
# They stay in the sweep so the test keeps proving they are still unsafe: if one of these
# ever comes back clean the firmware has been misread, and if a parameter outside this set
# fails then something that IS exposed has started silencing speech.
KNOWN_UNSAFE = {
    "tone": "ESC[1I returns nothing for every second utterance afterwards, and only "
            "restarting the firmware clears it",
}


def utterances_ok(host, prefixes, gen_start):
    """Speak once per prefix on one host. Returns list of byte counts."""
    sizes = []
    gen = gen_start
    for prefix in prefixes:
        try:
            pcm = host.speak(BASE + prefix + TEXT, gen)
        except Exception as error:
            sizes.append("ERROR:%s" % error)
            gen += 1
            continue
        sizes.append(len(pcm))
        gen += 1
    return sizes


def main():
    verdicts = {}
    for name, (letter, values) in PARAMS.items():
        print("=== %s (ESC[N%s) ===" % (name, letter))
        safe = True
        for value in values:
            prefix = "%s[%d%s" % (ESC, value, letter)
            # Set it, then speak several more times both with and without it, which is
            # exactly what an application does when it changes a setting mid-session.
            with ProseHost() as host:
                sizes = utterances_ok(host, [prefix, prefix, "", prefix, ""], 1)
            bad = [s for s in sizes if not isinstance(s, int) or s == 0]
            status = "ok" if not bad else "SILENCED"
            if bad:
                safe = False
            print("  %s=%-4d %-9s %s" % (letter, value, status, sizes))
        verdicts[name] = safe
        print("  -> %s\n" % ("safe to expose" if safe else "NOT SAFE - can silence speech"))

    # Also test them combined, because that is what a user with several sliders will do.
    print("=== all safe parameters together ===")
    combo = "".join("%s[%d%s" % (ESC, v, PARAMS[n][0])
                    for n, v in (("expression", 125), ("phrasing", 125), ("softness", 1))
                    if verdicts.get(n))
    with ProseHost() as host:
        sizes = utterances_ok(host, [combo, combo, "", combo], 1)
    print("  %s -> %s" % (combo.replace(ESC, "ESC"), sizes))
    print()

    print("=" * 70)
    failures = 0
    for name, safe in verdicts.items():
        expected_unsafe = name in KNOWN_UNSAFE
        if safe and not expected_unsafe:
            print("%-12s EXPOSE" % name)
        elif not safe and expected_unsafe:
            print("%-12s DROP (as expected: %s)" % (name, KNOWN_UNSAFE[name]))
        elif not safe and not expected_unsafe:
            print("%-12s REGRESSION - an exposed parameter can silence speech" % name)
            failures += 1
        else:
            print("%-12s UNEXPECTED - listed as unsafe but passed; re-check the firmware"
                  % name)
            failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
