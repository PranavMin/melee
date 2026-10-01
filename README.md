# LazyTO — Melee tournament module

This is the [LazyTO](https://github.com/PranavMin/tournament-reporter) fork of the
[Melee decompilation](https://github.com/doldecomp/melee). LazyTO lets players at a Melee
weekly pick, play and report their start.gg sets from the Wii itself.

This repo builds **`tournament.bin`**, the kiosk module. Venue Wiis keep running a stock
Melee 1.02 ISO. The LazyTO Nintendont loader copies the module into RAM at boot and patches
in a short list of hooks. Nothing on the disc changes.

The module provides the Tournament screen (set list, start, confirm), the CSS score banner and
binds, the L+R port claim, the Z+X handwarmer, auto-scoring from the end of each game, and the
EXI driver that talks to the relay.

## Build

```
python configure.py --non-matching   # once
python tools/build_module.py         # -> build/GALE01/tournament.bin
```

Details, the file format and the hook table are in
[docs/tournament-module.md](docs/tournament-module.md). The per-build manual QA list is
[docs/version-checklist.md](docs/version-checklist.md).

## The LazyTO repos

| Repo | Role |
|---|---|
| [tournament-reporter](https://github.com/PranavMin/tournament-reporter) | Relay on the venue's Raspberry Pi; talks to start.gg. Design docs and setup guides live here. |
| **melee** (this repo, branch `vanilla-module`) | `tournament.bin`, the kiosk module. |
| [Nintendont](https://github.com/PranavMin/Nintendont) | Wii loader: relay EXI device, module loader, relay discovery. |
| [Ishiiruka](https://github.com/PranavMin/Ishiiruka) | Slippi Dolphin with the relay forwarder, for development without a Wii. |

The wire protocol is defined once in `tournament-reporter/protocol.yaml`; this repo carries a
generated copy of `relay_proto.h`.

## Upstream

Everything outside the module's sources, `tools/build_module.py` and `docs/` is the upstream
decomp, merged periodically from `doldecomp/melee` `master`. The upstream project's own setup
guide is [docs/getting_started.md](docs/getting_started.md).
