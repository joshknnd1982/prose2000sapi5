# The Prose 2000 firmware command set

Everything here was established by measurement against the emulated firmware, not from
documentation. The NVDA add-on this project builds on uses four of the seventeen commands
and documents a single voice; both of those turned out to be understatements.

The tools that produced these results are in `tools/`:

| Tool | What it does |
|---|---|
| `prose_protocol.py` | The `PR2K` stdio protocol, as a small Python harness |
| `prose_voices.py` | Sweeps `ESC[<n>V` and groups byte-identical renders |
| `prose_params.py` | Sweeps every command letter and classifies its effect |
| `prose_safety.py` | The gate: proves a parameter cannot silence later speech |

## Where the command table lives

The dispatch table is in the firmware image `roms/v3.4.1__2000__3.u45` at offset `0x0cec6`,
stored as a sorted run of ASCII letters:

```
ACINPVafgilprstvx
```

Seventeen commands. Each is invoked as `ESC [ <number> <letter>`. The parser consumes any
sequence of that shape and silently discards the ones it does not implement, which is why
probing the escape space alone cannot tell a real command from a typo — sending an unknown
sequence produces no audio and no error.

## The three voices

`ESC[<n>V` selects a voice. Values above 2 clamp.

| Voice | Command | Measured F0 | Notes |
|---|---|---|---|
| Prose 2000 | `ESC[0V` | 130 Hz | The default. The only one the NVDA add-on ever plays. |
| Prose 2000 Deep | `ESC[1V` | 105 Hz | Lower and heavier. |
| Prose 2000 High | `ESC[2V` | 170 Hz | Higher and lighter. |

Duration is identical across all three for the same text, so the letter-to-sound rules and
timing are unchanged — only the vocal tract differs. Formant peaks move as well as F0,
which is what distinguishes these from a simple pitch offset.

A fourth render appears at large values of `n`, but the threshold moves with the text
(`n>=10` for one sentence, `n>=250` for another), the F0 is identical to voice 3, and the
difference is a flat +200 bytes of padding. It is a clamping artifact, not a voice.

## The three controls SAPI maps onto

| Purpose | Command | Range | Default |
|---|---|---|---|
| Rate | `ESC[<n>r` | 50–250 words per minute | 150 |
| Pitch | `ESC[<n>p` | 50–200 firmware units | 85 |
| Attenuation | `ESC[<n>a` | 0–15, **0 is loudest** | 0 |

Attenuation is coarse — sixteen steps across roughly 13 dB — so the engine pins it at 0 and
applies SAPI's volume percentage to the samples instead. A sixteen-step volume slider does
not feel continuous.

## Extra parameters the engine exposes

These are offered in the configuration utility. Names are descriptive: the firmware's own
names for them are not recorded anywhere available, so each is labelled by what it
measurably does.

| Setting | Command | Range | Default | Measured behaviour |
|---|---|---|---|---|
| Expression | `ESC[<n>g` | 0–250 | 0 | Widens the intonation contour and lowers F0 as it rises; 2.66× F0 spread, 1.54× duration |
| Phrasing | `ESC[<n>s` | 0–250 | 0 | Lengthens phrase boundaries without touching F0; 1.51× duration |
| Softness | `ESC[<n>i` | 0–1 | 0 | Timbre toggle, slightly softer delivery |
| Emphasis | `ESC[<n>P` | 0–1 | 1 | `0` slows the delivery and lowers F0 |

## Commands that are deliberately **not** exposed

| Command | Why not |
|---|---|
| `ESC[<n>I` ("tone") | **Silences speech.** Sounds fine on the utterance that sets it, then returns nothing at all for every second utterance afterwards. No following command clears it; only restarting the firmware does. |
| `ESC[<n>t`, `ESC[<n>L` | Can wedge the firmware past its own utterance time limit, so the host has to be killed and restarted. |
| `ESC[<n>N` | Emits a fixed ~3.2 seconds regardless of the text given. Looked like a spell-out mode; it is not — "cat" and "hello" both produce 3.2 s. A diagnostic mode of some kind. |
| `ESC[<n>f` | Values above 3 produce no audio at all. |
| `ESC[<n>A` | Non-monotonic and erratic: values 0, 1, 2, 10, 25, 50, 85 are identical to baseline while 3, 5 and 100+ differ. |
| `ESC[<n>v` | Behaves identically to `r`; appears to be an alias for rate. |
| `ESC[<n>C`, `ESC[<n>l`, `ESC[<n>x` | No measurable effect at any value. |

`tone` is the important one. This engine drives a screen reader, and going quiet is a worse
failure than sounding wrong, so `tools/prose_safety.py` exists purely to prove that no
exposed parameter can do it. It keeps sweeping `tone` as a known-unsafe control: if `tone`
ever passes, the firmware has been misread; if anything else fails, something that *is*
exposed has started silencing speech.

**Run `prose_safety.py` before exposing any new parameter.**

## The firmware pads the head of every utterance

This is the single biggest thing affecting how responsive the voice feels, and it is not
synthesis speed. Measured from sending `SPEAK`:

| Phrase | First audio message | Leading silence in that audio | Total wait |
|---|---|---|---|
| "OK" | 16 ms | 232 ms | 248 ms |
| "Cancel" | 15 ms | 355 ms | 370 ms |
| "Documents folder" | 32 ms | 446 ms | **478 ms** |
| "The quick brown fox jumps over the lazy dog." | 31 ms | 672 ms | 703 ms |
| A long paragraph | 47 ms | 1082 ms | 1129 ms |

Synthesis is fast. The delay is *silence the firmware puts at the front of the audio*, and
it scales with the length of the utterance — so it is worst exactly when a screen reader is
announcing something short and wants to be instant.

The engine trims it (`SilenceTrimmer` in `src/prose_host.cpp`), bringing the head down to
under 10 ms.

Finding the head is not "first sample above a threshold". The firmware emits a brief fixed
marker — peak 247, RMS about 198 — at the point the pad ends, and on longer utterances a
further stretch of *true digital silence* follows it before any speech. A single-sample test
stops on that marker and then treats the silence behind it as an internal pause worth
keeping, which trims almost nothing. The trimmer instead looks for the first 10 ms window
whose RMS clears 600 — unambiguously speech, well above the 198 marker — and then walks back
over quieter windows down to RMS 100 so the attack of the first phoneme is not clipped.

`tools/prose_latency.py` measures this, and `prose_sapitest` asserts the head stays under
100 ms so it cannot regress.

### Where the remaining latency goes

With the pad trimmed, the engine sits on the emulator's own floor:

| | Time from `Speak()` to first audio |
|---|---|
| Warm engine (the steady state) | **31–47 ms** |
| Cold, first utterance after the voice is selected | 109–125 ms |

The cold figure is the firmware boot (~93 ms), which is why it is started on a background
thread as soon as `SetObjectToken` runs rather than on the first utterance.

The raw protocol delivers a usable speech sample 16–47 ms after `SPEAK`, so the SAPI layer
and the trimmer together add essentially nothing. Two things were checked and found *not*
to help:

- **Splitting long text at sentence boundaries.** The pad is emitted almost instantly
  rather than generated in real time, so a paragraph reaches its first speech sample in
  47 ms — exactly the same as its first sentence alone. Splitting buys nothing and costs
  prosody.
- **Buffering less before the onset is found.** The firmware sends the whole pad in its
  first message or two, so the trimmer almost never waits for more audio.

### Cancelling

Interruption matters more than cold start for a screen reader, because every keypress
cancels whatever is being said. Measured:

- A cancel issued while the firmware is genuinely speaking is acknowledged in **62–78 ms**,
  and the next utterance's audio arrives ~15 ms later.
- A cancel for an utterance that has *already finished* is never acknowledged at all. That
  is correct — there is nothing left to stop — but it means the engine has to fall back to
  a timeout.

That timeout is on the path between a keypress and the next thing spoken, so it was cut
from 1500 ms to 400 ms, and it no longer kills the emulator: the host is idle and healthy
in this case, and restarting it used to cost a further ~93 ms boot for nothing.

## Two behaviours worth knowing

**Firmware settings persist across utterances.** The emulator models real hardware, and the
hardware kept whatever it was last told. The engine therefore restates the *complete*
parameter set on every utterance rather than only what changed — see `build_prefix` in
`src/prose_host.cpp`. Sending a partial set lets a value set once leak into everything
spoken afterwards; that is how the `tone` fault was first seen, as an utterance falling
silent because of a parameter set three utterances earlier.

**Renders are reproducible only from a fresh process.** Repeated utterances within one host
process differ slightly, because DSP state carries over. Byte equality is only a valid test
oracle if each render starts from a fresh `ProseHost.exe`.

## There are no index marks

The firmware has no `\mrk=N\` equivalent — nothing reports how far into an utterance it has
got. SAPI bookmark and boundary events therefore have to be recovered by splitting the
utterance at the point the event belongs to, and reading off how much audio has been
written so far.

Splitting is not free. Every utterance gets 0.3–0.6 s of padding, so a separate utterance
per word inflates the audio by **93%** and sounds nothing like continuous speech. The
engine's compromise:

* **Bookmarks and sentence starts** close the current segment and open the next, so their
  offsets are exact. These are what applications use to track speech position.
* **Word boundaries** are interpolated across the audio the segment actually produced, in
  proportion to where each word began in the text. The offsets are approximate; the audio
  is correct.
