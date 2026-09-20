# UCF re-addressing pipeline

Answers **design.md R12** with the user's chosen approach (2026-09-20):
**re-address, don't reimplement.** UCF 0.8 crashes on our build only because its
Gecko hook addresses are vanilla GALE01 v1.02 and our decomp DOL is *shifted*
(R11/R12). The asm itself is correct. So we keep UCF's **exact bytes**
(`Nintendont/kernel/gecko/g_ucf.bin`) and remap only the fields that encode an
absolute vanilla address, then apply it as ordinary Gecko codes exactly like
vanilla. Byte-for-byte faithful, zero re-derivation.

The build-step script `tools/gen_ucf_codes.py` regenerates the re-addressed
codes every build (addresses shift whenever our code changes) and writes
`build/GALE01/ucf_codes.gecko.txt`.

> This supersedes the source-port plan in `docs/ucf-investigation.md` §Q4 (new
> TU `ucf.c` + `ftData_MotionStateList` wraps). That investigation remains the
> reference for *what UCF does*; this document is the shipped mechanism.

> **The same pipeline now also re-addresses the venue "Neutral Spawns" code.**
> `tools/gen_ucf_codes.py` was generalized to emit both codes into the same
> output file (`build/GALE01/ucf_codes.gecko.txt`), each as its own Dolphin
> `$<name>` block. See `docs/venue-codes-readdressing.md` for the decoded Neutral
> Spawns hook, its address map, and the one field there (three `.sbss` loads)
> that needed an r2→r13 base rebase because the shifted `_SDA2_BASE_` can no
> longer reach them.

---

## 1. The four decoded hooks

`g_ucf.bin` is 1072 bytes: four Gecko **C2** ("insert PowerPC asm at address")
codes, back to back, then the ASCII string `UCF 0.8` consumed by the CSS-text
code. A C2 code is `C2 <addr24> <N>` followed by `N` 8-byte lines of PPC; the
codehandler branches into the payload from `0x80000000|addr` and the payload's
last line is the branch-back placeholder. Each 8-byte line is **two** 4-byte
instructions.

| # | C2 header (vanilla) | Lines | Lands in (decomp symbol) | Purpose |
|---|---|---|---|---|
| 1 | `C2 0C9A44` | 0x2B | `ftCo_Turn_IASA` +0x4C | **dashback** |
| 2 | `C2 0998A4` | 0x26 | `ftCo_80099894` +0x10 | **shield-drop** |
| 3 | `C2 0908F4` | 0x17 | `ftCo_DamageFall_IASA` +0xCC | **wiggle out of tumble** |
| 4 | `C2 2662D0` | 0x1A | `mnCharSel_802640A0` +0x2230 | cosmetic "UCF 0.8" CSS label |

All four payloads were fully disassembled. **Every branch (`b`/`bc`/`bl`) in
every payload is PC-relative and lands inside its own payload** — no payload
branches to game code, so there is no un-remappable relative-branch hook. The
copied original (overwritten) instruction in each payload is register-relative
(e.g. hook 1 begins `stfs f0, 0x2C(r31)`) and the hooked functions are untouched
matched functions, so it needs no remap. The only things that reference an
absolute vanilla address are the C2 insert points and the data/function
references listed below.

---

## 2. Every remapped address

Three kinds of absolute reference are remapped; **nothing else is touched.**

1. **C2 insert address** — a function+offset; re-encoded into the C2 header.
2. **`lis rX,hi` + `ori rX,rX,lo` pairs** — build an absolute data or function
   pointer (functions are then called via `mtctr`/`bctrl`). Both immediate
   halves re-encoded.
3. **`lwz rX, disp(r13)` small-data (SDA) loads** — every UCF payload loads
   `p_ftCommonData` via r13. In stock 1.02 `r13 = _SDA_BASE_ = 0x804DB6A0` and
   `-0x514C(r13) = 0x804D6554 = p_ftCommonData` (confirmed downstream by
   `lfs f2, 0x3C(r4)` = `p_ftCommonData->dash_smash_stick_threshold`). Our
   build's `_SDA_BASE_` differs **and the SDA layout is not a uniform shift**, so
   the displacement is recomputed as `p_ftCommonData(shifted) - _SDA_BASE_(shifted)`.
   This is the one non-obvious remap: leaving the stock `-0x514C` would read the
   wrong SDA slot and break/crash UCF.

Each vanilla address was resolved to a symbol (+delta) via
`config/GALE01/symbols.txt`, then to this build's address via
`build/GALE01/main.elf`. Example values below are from one build — **they change
every build; the script regenerates them.** (`_SDA_BASE_` = `0x804E2440` in the
build shown.)

| Hook | Field (payload word) | vanilla → symbol (+delta) → shifted |
|---|---|---|
| dashback | C2 header | `0x800C9A44` `ftCo_Turn_IASA+0x4C` → `0x800CB5EC` |
| dashback | r13 load w11 | `0x804D6554` `p_ftCommonData` → `0x804DD484` |
| dashback | lis/ori w23/24 | `0x804C1F78` `HSD_PadLibData` → `0x804C8E38` |
| dashback | lis/ori w45/46 | `0x8003418C` `Player_GetEntityAtIndex` → `0x800353A8` |
| dashback | lis/ori w68/69 | `0x8046B108` `gmMain_8046B108` → `0x80470F58` |
| shield-drop | C2 header | `0x800998A4` `ftCo_80099894+0x10` → `0x8009B2F0` |
| shield-drop | r13 load w9 | `0x804D6554` `p_ftCommonData` → `0x804DD484` |
| tumble | C2 header | `0x800908F4` `ftCo_DamageFall_IASA+0xCC` → `0x800922A8` |
| tumble | r13 load w8 | `0x804D6554` `p_ftCommonData` → `0x804DD484` |
| tumble | lis/ori w12/13 | `0x804C1F78` `HSD_PadLibData` → `0x804C8E38` |
| CSS label | C2 header | `0x802662D0` `mnCharSel_802640A0+0x2230` → `0x80269598` |
| CSS label | lis/ori w8/9 | `0x803A6754` `HSD_SisLib_803A6754` → `0x803ABA00` |
| CSS label | lis/ori w25/26 | `0x803A6B98` `HSD_SisLib_803A6B98` → `0x803ABE44` |
| CSS label | lis/ori w33/34 | `0x803A7548` `HSD_SisLib_803A7548` → `0x803AC7F4` |

**Not an address (kept verbatim):** hook 2 word 51 `lis r0, 0x4330` is the IEEE
signed-int→double magic constant (`0x43300000`), not a pointer — untouched.

**Result: all references were cleanly remappable.** No hook uses a relative
branch into game code, so none required a source port; the whole set stays a
faithful byte-for-byte re-address.

### What actually changes, byte for byte

Only the immediate (low 16 bits) of a remapped instruction changes; opcode and
register fields are preserved. `lis`/`ori` update `@h`/`@l`; the r13 load updates
its signed displacement; the C2 header updates its 24-bit address. Everything
else in all 1072 bytes is emitted unchanged. `tools/gen_ucf_codes.py` proves
this two ways on every run:

* **selfcheck()** — re-encoding each fixup with its *vanilla* address must
  reproduce the stock template word exactly (asserted at startup).
* **`--verify-bin`** — reconstruct the whole blob with vanilla addresses and
  byte-compare to the real `g_ucf.bin`:

  ```
  python tools/gen_ucf_codes.py --verify-bin P:/Projects/Nintendont/kernel/gecko/g_ucf.bin
  # verify: OK -- reconstructed blob is byte-identical ... (1072 bytes)
  ```

---

## 3. The script

`tools/gen_ucf_codes.py` (Python 3 stdlib only). It embeds the exact UCF 0.8
payload words with the remappable fields marked, parses `build/GALE01/main.elf`'s
ELF32-BE `.symtab` itself (no external deps), resolves the symbols to this
build's addresses, re-encodes only those fields, and emits:

* **(a)** an annotated raw code listing, and
* **(b)** ready-to-paste Dolphin `[Gecko]` / `[Gecko_Enabled]` INI blocks,

to `build/GALE01/ucf_codes.gecko.txt` (also printed to stdout). A vanilla→symbol
→shifted address map is included as a comment header.

```
python tools/gen_ucf_codes.py                     # default: build/GALE01/main.elf
python tools/gen_ucf_codes.py --elf X --out Y
python tools/gen_ucf_codes.py --verify-bin <g_ucf.bin>
```

---

## 4. Build wiring

`configure.py` appends a ninja rule right after `generate_build(config)` (the
dtk-generated `tools/project.py` is left untouched):

```
rule gen_ucf
  command = $python tools/gen_ucf_codes.py --elf $in --out $out
build build/GALE01/ucf_codes.gecko.txt: gen_ucf build/GALE01/main.elf | tools/gen_ucf_codes.py
default build/GALE01/ucf_codes.gecko.txt
```

Because it depends on `main.elf` and `default` statements are additive, a plain
`python -m ninja` links the DOL and then regenerates the codes with the current
build's addresses. The append is re-emitted on every reconfigure, so it survives
`build.ninja` regeneration. To regenerate by hand: `python -m ninja
build/GALE01/ucf_codes.gecko.txt`.

---

## 5. Applying / testing in Dolphin (dev loop)

The generated codes are **not** committed into `GALE01r2.ini` — that file is all
codes-disabled for R11, and enabling UCF is left as a manual, explicit step.

The Slippi Dolphin fork is Ishiiruka. Its game-settings file is
`Ishiiruka/Binary/x64/Sys/GameSettings/GALE01r2.ini` (the built exe reads this;
`Ishiiruka/Data/Sys/GameSettings/GALE01r2.ini` is the source-staged copy — update
both to keep them in sync). That file already contains the stock **vanilla-
addressed UCF 0.84** entries under `[Gecko]`; leave those **disabled** (they crash
the shifted DOL — that is the whole R11 problem).

To enable our re-addressed UCF 0.8 for a Dolphin test:

1. Rebuild so `build/GALE01/ucf_codes.gecko.txt` is current
   (`python -m ninja`).
2. Open that file and copy the block under **"(b) Dolphin GameSettings INI
   blocks"** — the `$UCF 0.8 (re-addressed)` code (its C2 lines) into the
   `[Gecko]` section of `GALE01r2.ini`, and the matching
   `$UCF 0.8 (re-addressed)` line into the `[Gecko_Enabled]` section (this is the
   line that turns it on — the enabled section on the fork currently has every
   entry commented out).
3. Do **not** enable any of the pre-existing vanilla `#External/UCF 0.84/...`
   entries.
4. Boot the shifted ISO in Dolphin and verify dashback / shield-drop / tumble
   frame-by-frame (design §9.3). Because the codes now carry this build's
   addresses, the hooks land in the right functions instead of crashing at boot.

Regenerate and re-paste whenever the melee code changes (addresses shift).

## 6. Hardware (venue) — still open

This covers the Dolphin dev loop. On hardware, Slippi Nintendont applies its own
built-in vanilla UCF, gated by `GetMeleeVersion()` on the spoofed disc header, so
it must instead be made to apply *this* re-addressed set (or have its UCF
disabled and these codes applied as a GCT). That is the R12 Nintendont / session
8 work and is not done here.
