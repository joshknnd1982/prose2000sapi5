# Prose 2000 SAPI 5

A 32-bit and 64-bit SAPI 5 speech engine for the emulated Telesensory Systems Prose 2000,
so its voices can be used by any Windows application that speaks — screen readers, reading
tools, anything on the Windows speech stack.

Three English voices. No SAPI 4 anywhere, and the engine itself reads no registry at all.

## What this is

The Prose 2000 was a 1982-era hardware speech synthesiser from Telesensory Systems; the
firmware here is the 1987 Speech Plus revision. `ProseHost.exe` runs that original firmware
under emulation — it is a self-contained 64-bit console program that imports only
`KERNEL32` and `msvcrt`, from Andre Louis's NVDA add-on
([OnjLouis/prose2000](https://github.com/OnjLouis/prose2000)), itself derived from MAME's
Prose 2000 driver.

This project wraps that emulator in a SAPI 5 engine.

## The three voices

| Voice | Firmware | F0 | Character |
|---|---|---|---|
| Prose 2000 | `ESC[0V` | 130 Hz | The default voice |
| Prose 2000 Deep | `ESC[1V` | 105 Hz | Lower and heavier |
| Prose 2000 High | `ESC[2V` | 170 Hz | Higher and lighter |

The add-on this builds on documents *one* voice and hardcodes `ESC[0V`. The voice selector
was found by pulling the firmware's command dispatch table out of ROM `u45` at offset
`0x0cec6`. See [docs/PARAMETERS.md](docs/PARAMETERS.md) for the full command set, including
the ones that are deliberately not exposed because they can silence speech.

English only. The firmware contains one set of English letter-to-sound rules and no other
language, so no other languages are possible.

## Architecture

Both bitnesses use the *same* out-of-process design:

```
32-bit application ──> Prose2000SAPI5.dll (x86) ──┐
                                                  ├──> ProseHost.exe (x64) ──> firmware
64-bit application ──> Prose2000SAPI5.dll (x64) ──┘        PR2K stdio protocol
```

`ProseHost.exe` is 64-bit only, but a 32-bit process spawns it perfectly well under WOW64.
That means **no 32-bit helper process and no COM surrogate** — unlike most dual-architecture
speech engines, where the engine DLL is one bitness and the other half has to be bridged.
It also means a crash in the emulator cannot take down the application that was speaking.

64-bit Windows is required, because the emulator has no 32-bit build. The 32-bit interface
is still installed and still works; it is there for 32-bit *applications*.

Audio is 10 kHz, 16-bit, mono — what the firmware produces, and not configurable.

## Building

Needs CMake, Visual Studio 2022 Build Tools (x86 and x64), and Inno Setup 6.

```bat
build_all.bat
```

or, for the parts:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_all.ps1 -Test
```

`-Test` runs the render harness, both SAPI harnesses and the accessibility check against the
staged layout. `-SkipInstaller` stops after staging. The installer lands in
`dist\Prose2000SAPI5_Setup_<version>.exe`.

## Layout

```
src/                      the SAPI 5 engine and the emulator client
  prose_host.cpp          process lifetime and the PR2K protocol
  prose_tts_engine.cpp    ISpTTSEngine: SAPI's model mapped onto the firmware
  prose_voices.cpp        the voice and parameter catalogue
  prose_settings.cpp      per-user settings, shared with the configuration utility
tools/
  prose_config.cpp        the configuration utility
  prose_render.cpp        renders WAV files with no SAPI involved
  prose_sapitest.cpp      drives the DLL through SAPI without registering it
  prose_speak.cpp         speaks through the registered stack
  prose_*.py              the firmware probes that established the command set
  check_config_a11y.ps1   reads the utility's controls through MSAA
  check_installer_a11y.ps1 walks the installer's pages through MSAA
bin/prose2000/            ProseHost.exe and the six firmware images
installer/                the Inno Setup script
```

## Testing

```bash
tools\prose_safety.py
```

Proves no exposed parameter can silence speech. **Run it before exposing any new firmware
parameter.**

```bash
build_x64\bin\Release\prose_sapitest.exe --dll build_x64\bin\Release\Prose2000SAPI5.dll
```

Drives the whole SAPI path — enumerate, bind a token, `Speak`, collect events — without
registering anything and without administrator rights. This is the test to run when a
registered install misbehaves and the question is whether the engine or the registration is
at fault.

## Configuration

`Prose2000Config.exe` trims rate, pitch and volume on top of whatever the application asks
for, and exposes the extra firmware parameters. Settings live under
`HKCU\Software\Prose 2000 SAPI5`, so changing them needs no administrator rights, and the
engine re-reads them at the start of every utterance — a change is audible on the next thing
spoken, with no restart.

Every setting is a drop-down list rather than a slider, deliberately: a Win32 trackbar
reports its position to MSAA as a percentage, so a control offering -10..+10 announces "+5"
as "seventy-five percent". Drop lists announce their item text. `check_config_a11y.ps1`
enforces this, along with every control having an accessible name and being reachable by
Tab.

## Logging

Both interfaces, the configuration utility and the tools all log to:

```
%LOCALAPPDATA%\Prose2000 SAPI5\Logs
```

One file per component per process, flushed on every line so a crash mid-utterance still
leaves the lines that explain it. `PROSE2000_LOG_LEVEL` takes `off`, `error`, `warn`,
`info`, `debug` or `trace`; `PROSE2000_LOG_DIR` moves the folder. The installer writes its
own log and keeps a copy as `install.log` beside the program.

`PROSE2000_DATA_DIR` overrides where the engine looks for `ProseHost.exe` and `roms\`,
which is how the tools run out of a build tree.

## Licensing

The Prose 2000 hardware and firmware were developed by Telesensory Systems Inc. and Speech
Plus. **The firmware remains proprietary** and is not covered by any source licence here.
The ROM images are packaged from an existing NVDA add-on; nothing in this repository grants
a licence to them. Use this only where you are entitled to use that firmware.

`ProseHost.exe` is by Andre Louis and carries its own licence (`bin/prose2000/LICENSE.txt`),
with portions from MAME under BSD-3-Clause.
