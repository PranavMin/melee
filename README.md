# Archived: the LazyTO kiosk moved to [PranavMin/lazyto](https://github.com/PranavMin/lazyto)

This fork of the [Melee decompilation](https://github.com/doldecomp/melee) used to hold the
LazyTO kiosk module, `tournament.bin`. On 2026-10-01 the kiosk's source moved into the
[lazyto](https://github.com/PranavMin/lazyto) repo, under
[kiosk/](https://github.com/PranavMin/lazyto/tree/main/kiosk). It now builds against the
unmodified decomp, which lazyto includes as a git submodule. This fork is no longer used or
updated.

**To build the kiosk,** follow
[docs/development.md](https://github.com/PranavMin/lazyto/blob/main/docs/development.md) in
lazyto.

**What is kept here:**

- Branch `vanilla-module`: the module's history up to the move (last commit `a85a996c0`).
- Tag `shifted-dol-final`: the retired build that compiled the kiosk into a rebuilt `main.dol`.
- [docs/history/](docs/history/): the investigation reports from that build.
