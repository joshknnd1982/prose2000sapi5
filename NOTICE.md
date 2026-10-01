# Licensing and attribution

Three different things ship in this repository under three different terms. Read this
before redistributing any of it.

## 1. The SAPI 5 engine, configuration utility, tools and installer

Everything under `src/`, `tools/`, `installer/`, `docs/`, and the build scripts.

Copyright (c) 2026 Josh Kennedy. Licensed under the **MIT License** — see
[LICENSE](LICENSE).

## 2. `ProseHost.exe` — the emulator

`bin/prose2000/ProseHost.exe` is **not** part of this project. It is taken unmodified from
Andre Louis's Prose 2000 NVDA add-on, [OnjLouis/prose2000](https://github.com/OnjLouis/prose2000).

Licensed under the **BSD 3-Clause License** — see `bin/prose2000/LICENSE.txt` for the full
text, which is reproduced verbatim as that license requires.

    Copyright (c) 2026, Andre Louis
    Copyright (c) 2026, David Sexton and contributors
    Portions copyright Carl, Christopher Toth, Aaron Giles, Vas Crabb,
    R. Belmont, byuu, and other MAME contributors

Portions of the emulator are adapted from MAME's Prose 2000 driver (by Jonathan Gevaryahu,
with thanks to Kevin Horton) and from David Sexton's DoubleTalk PC project.

## 3. The firmware ROMs — proprietary

`bin/prose2000/roms/*.u21`, `*.u22`, `*.u29`, `*.u44`, `*.u45`

These are the original Prose 2000 firmware images, **proprietary to Telesensory Systems
Inc. / Speech Plus**. They are *not* covered by the BSD license above, and they are not
covered by the MIT License either. The upstream add-on's own license file states this
plainly:

> The original Prose firmware is proprietary to Telesensory Systems Inc./Speech Plus and is
> not covered by this license.

They are included here for the same reason the upstream NVDA add-on includes them: without
them the emulator has nothing to run, and the hardware and its vendor have been gone for
decades. No claim of ownership is made over them, and no license to them is granted by this
repository. If you are the rights holder and want them removed, open an issue.

The installer released alongside this repository bundles these ROMs.

## Summary

| Component | License | Redistributable |
|---|---|---|
| `src/`, `tools/`, `installer/`, `docs/` | MIT | Yes, under the MIT License |
| `bin/prose2000/ProseHost.exe` | BSD 3-Clause | Yes, keep the notice |
| `bin/prose2000/roms/*` | Proprietary (Telesensory/Speech Plus) | No license granted |
