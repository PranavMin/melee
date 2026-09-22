# New-version check-yourself list

Manual checks to run **every time a new `SmashTournament-vN.iso` is produced**, each
one born from a bug we actually hit. If a check fails, the note says the usual cause
and where it's documented. **This is a living list — add a row whenever a new build
issue bites us.**

Legend: each item is something *you* verify by eye on the running build.

---

## 0. Build hygiene (only if you built the ISO yourself)

- [ ] Built with **`python configure.py --non-matching`** then `python -m ninja`.
      *Plain `configure.py` (matching) silently drops our new TUs → link error
      "undefined mnTourney_*/lbTourney_*". Our code only exists in the non-matching build.*
- [ ] **No non-ASCII in edited C files** before building (scan for em-dash U+2014,
      smart quotes, etc.). *MWCC parses source as Shift-JIS and errors on them.*
- [ ] **Gecko codes regenerated for THIS DOL** and synced into both `GALE01r2.ini`
      copies (Binary + Data). *Every code addresses the shifted DOL by symbol; addresses
      move whenever our code changes (e.g. `8E38→8EF8` between v8 and v9). A stale
      `.ini` = vanilla/old addresses on a new DOL = boot crash (R11/R12).*
      See `ucf-readdressing.md`, `venue-codes-readdressing.md`.
- [ ] If any on-screen text looks jammed/wrong after an edit, **clean-rebuild the .o**.
      *A stale `mntourney.o` once rendered "STARTPapa VS Hotel" / wrong confirm text.*

## 1. Boot & menu flow

- [ ] Boots **straight into the set-selection list** — no intro movie, no title
      sequence, no lingering main menu. *Intro/title skip lives in `bootOnLoad` (gmboot.c).*
- [ ] **No boot crash.** *v7 crashed (`Invalid read … PC=0x803442f0`, __SetSURegs) because
      it entered the set list on frame 1 before menu graphics initialized; v8 gates on
      `cooldown == 0`. See `kiosk-and-defaults-investigation.md`.*
- [ ] **No "missing memory card" popup.** *Provide a card: Dolphin Slot A = Memory Card,
      Slot B = the relay EXI device. This is config, not a code hack (the game edit was
      reverted). On hardware Nintendont emulates the card.*
- [ ] After an **END_SET upload**, it returns to the set list (doesn't sit on the CSS).
- [ ] **B on the CSS** returns to the set list.
- [ ] **Z on the set list** enters the CSS in friendlies mode (no set active, nothing reported).

## 2. Kiosk defaults — rules & unlocks (forced live at boot)

- [ ] Match rules are **4 stocks / 8:00 / items OFF**. *"Items off" is `item_freq = -1`
      (0xFF), not 0 — 0 is the lowest ON setting. Bug fixed; verify it stuck. See `defaults-debug.md`.*
- [ ] **All characters unlocked** on the CSS.
- [ ] **All stages unlocked** on stage select. *Real unlock is `gm_8016468C()`, not
      `stage_mask` (that's only the per-match legal-stage toggle). Bug fixed; verify.*
- [ ] **Random-stage set = the 6 singles legal stages** (BF, FD, FoD, YS, DL, PS).
      *`stage_mask = 0xE70000B0` (Magus "Singles Stages"), forced live in `forceKioskDefaults`.*
- [ ] **Music off + mono.** *Set at **boot**, NOT live at menu-enter. Music-off =
      `gmMainLib_DefaultGamePrefs.sound_balance = 100` (copied into live prefs at the boot
      init-copy). Mono = `OSSetSoundMode(0)` at boot. **Do NOT force `sound_balance` live in
      `forceKioskDefaults`** — writing it at menu-enter re-mixes the BGM mid scene-transition
      and crashes in the GX texture path (`__GXSetSUTexRegs`), deterministically, every boot
      (bisected v12-v15, 2026-09-21). Same caution for any audio-mixing setting: default
      template / boot only.*

## 3. Text & layout (SIS menu text)

- [ ] **Title and bottom hint bar are centered** and sit fully inside the border; text fits.
      *Centering x-values are hardcoded estimates per string — nudge if off.*
- [ ] **No missing/garbage glyphs.** *The SIS font is ASCII-only with no `(` `)` `/`
      glyphs — those render blank/"V" (the old `(L/R)` filter showed a stray "V";
      changed to `L-R`). Use hyphens, never parens/slashes/em-dashes in on-screen strings.*
- [ ] Set-list rows read correctly: `> ROUND  NAME VS NAME  BOx` with proper spacing.

## 4. Venue mods (UCF + Neutral Spawns) — native, NOT gecko

- [ ] **Gecko codes are OFF at the ISO level too** — right-click the ISO → Properties →
      Gecko Codes tab: nothing checked. *This per-ISO state overrides the global setting
      and was the cause of a `last_PC = 80001f18` boot crash on 2026-09-21 even with the
      global flag False. Our Ishiiruka is `Binary/x64/Slippi Dolphin.exe` (launched via
      Slippi Launcher) — it has no "Enable Cheats" checkbox in Config → General, so the
      ISO Properties dialog and the ini are the only two levers.*
- [ ] **`EnableCheats` is OFF in Ishiiruka's `User/Config/Dolphin.ini`.** *Cheats-on makes
      Slippi Ishiiruka install its own vanilla-addressed `Sys/bootloader.gct` into the
      codelist (GeckoCode.cpp:171-191), patching vanilla 1.02 addresses into our shifted
      DOL — the R11 crash, every boot. Debugger callstack 2026-09-21: gecko handler
      `0x80001f18` → heap garbage `0x81335ae0`, `HSD_ObjAllocAddFree` reading a trashed
      free-list pointer, all during `mnMain_Scene_OnEnter`. Gecko codes are therefore
      unusable in Ishiiruka for this build: every venue mod is compiled into the DOL.*
- [ ] **UCF feels right**: dashback, shield-drop, wiggle-out-of-tumble behave like UCF 0.8.
      *Native port in `lbucf.c` per `ucf-investigation.md` (data-table IASA wrappers in
      `ftData_MotionStateList`, installed once from `lbTourney_CSSFrame`, no matched edits).
      Feel-tested green 2026-09-21. Known gap: `ftCo_Wait_IASA`/`ftCo_DamageFall_IASA` are also
      called directly from a few attack/damage states, bypassing the table wrap.*
- [ ] **Neutral spawns** present on stages: on Battlefield a 2P match starts on the **left
      and right side platforms** (`±38.8, 35.2`), not centre/top. *Native port in
      `lbneutralspawn.c`, called from `fn_8016E2BC` (gmvs.c) at the asm's exact insertion
      point (`+0x254`, after `getSpawnPoint`, before the fighter is spawned from it) — a
      documented matched-function edit. Do NOT try a post-spawn data hook: `rules.on_match_start`
      fires after vanilla has already spawned every fighter, so a slot-pose write
      (`Player_80032768`) or even a live `cur_pos`/`coll_data` teleport there does nothing
      visible (v18/v19 — fighters stayed at vanilla `spawn_point` 2/3). FoD `±41.25` on the
      side platforms is the venue's real value (cross-checked against the ini asm), not a
      bug, even though it reads "further out" than vanilla FoD.*
- [ ] The `[Gecko_Enabled]` entries in `GALE01r2.ini` are inert (cheats off); the
      re-addressed UCF/Neutral blocks there are kept as reference only.

## 5. Relay & start.gg (before the game can list anything)

- [ ] **Relay is running** and its status page loads: http://localhost:8080 .
      *Restart it after a machine reboot.*
- [ ] Status page shows **`Cache: N sets` with N > 0**, and **no "preview-id set(s)
      dropped" warning**. *0 sets or a preview warning = a start.gg pool isn't started.
      Start it: `markSetInProgress` on any set materializes real numeric ids for the whole
      pool (R8). The relay drops preview (string-id) sets by design.*
- [ ] Status page shows the **correct event id** (1613010 for the test tournament).

## 6. Full set flow (end-to-end smoke test)

- [ ] Set list **populates from the relay** (matches the sets on start.gg). *An empty
      list while the relay shows sets = game↔relay protocol mismatch (e.g. an EXI command-byte
      skew from a DOL built before a protocol renumber) — rebuild game and forwarder together.*
- [ ] Select a set → **A starts it** → lands on the CSS.
- [ ] **C-stick score binds** work (Z + C-left = P1, C-right = P2, C-down = undo,
      C-up held = end set) from **any controller port**. *Binds are C-stick, not D-pad —
      some players have no D-pad.*
- [ ] **"SCORE SENT"** confirmation appears after a report; **start.gg reflects the score**.
- [ ] END_SET closes the set on start.gg and returns to the set list.
