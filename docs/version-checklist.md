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

## 3. Text & layout (SIS menu text)

- [ ] **Title and bottom hint bar are centered** and sit fully inside the border; text fits.
      *Centering x-values are hardcoded estimates per string — nudge if off.*
- [ ] **No missing/garbage glyphs.** *The SIS font is ASCII-only with no `(` `)` `/`
      glyphs — those render blank/"V" (the old `(L/R)` filter showed a stray "V";
      changed to `L-R`). Use hyphens, never parens/slashes/em-dashes in on-screen strings.*
- [ ] Set-list rows read correctly: `> ROUND  NAME VS NAME  BOx` with proper spacing.

## 4. Gecko codes (UCF + Neutral Spawns)

- [ ] Game **does not crash at boot** from the codes. *If it crashes only with codes
      enabled, the `.ini` addresses don't match this DOL — regenerate (section 0).*
- [ ] **UCF feels right**: dashback, shield-drop, and wiggle-out-of-tumble behave like
      UCF 0.8 (not vanilla). See `ucf-investigation.md`.
- [ ] **Neutral spawns** present on stages.
- [ ] Only the **two re-addressed codes** are enabled in `[Gecko_Enabled]` — the
      vanilla-1.02-addressed codes (Required/Recommended) stay disabled or they crash the
      shifted DOL.

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
