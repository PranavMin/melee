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
- [ ] **No missing/garbage glyphs.** *The SIS text encoder (hsd_3A64.c) maps only
      these ASCII bytes: space `" ' , - . : 0-9 A-Z a-z`. Any other ASCII byte (`+ ( ) /
      [ ] % & * @ # $ = < > ? !`) is taken as a Shift-JIS lead byte and eats the next
      character - that was the stray "V" from `(L/R)` and the blank from `Z + X`. The
      glyphs DO exist in the font (HSD_SisLib_FontAtlas, 287 glyphs, sheet rendered
      2026-09-22): write them as 2-byte SJIS escapes in the string literal, e.g. `+` =
      `"{"` (0x817B), `(` `)` = `"i"` `"j"`, `/` = `"^"`, `!` = `"I"`,
      `?` = `"H"`, `x` (times) = `"~"`, `=` = `""`, `%` = `""`.
      There are NO controller-button glyphs in the font (digits, Latin, kana, symbols,
      24 kanji only) - icons need textures (design.md sec 12).*
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

## 7. CSS / SSS / in-match venue features (v22-v24, native)

- [ ] **D-pad UP/DOWN on the CSS toggles that port's rumble** with the vibration-menu
      pulse, and the **selection hand shakes** on every toggle (v23, `lbtourney.c`
      `kickHand`/`stepHand` via `mnCharSel_CursorHandOffset`). *Once a player has picked
      a nametag, Melee takes in-match rumble from the TAG's flag, not the port's
      (`gm_RumbleEnabledForPlayer`); the toggle and the tag pick keep both in step. If
      rumble "ignores" the toggle, that coupling regressed.*
- [ ] **Set list has the backdrop AND the menu border** around it (v25). *v22 hid
      both (barren), v23 hid the panel; the panel is now hidden only during the boot
      warm-up and shown again the moment the set list is up. If the main-menu row
      text ever shows through behind the list, that is the panel's item children -
      hide those, not the whole panel.*
- [ ] **Nametag dropdown lists the set's two tags first** (slots 0 and 1, 4 chars,
      A-Z/0-9 only, e.g. `MANG` / `ZAIN`; empty tag falls back to `P1`/`P2`). Picking one
      annotates the CSS score line with the port: `MANGO P1  0 - 0  P3 ZAIN`. *Written
      into persistent nametag slots 0/1 at START_SET (`writeNametag`), overwriting
      whatever the kiosk card had there. Friendlies (Z) leave the tags as they were.*
- [ ] **Button icons in the overlays (v35).** The set-list hint bar reads
      `(A) START  (Z) FRIENDLIES  (Y) REFRESH  (B) MENU` with real GameCube-coloured
      button discs (A green, B red, X/Y light grey, Z purple square, L/R grey squares,
      Start grey pill, C-stick yellow), the confirm/error hints `(A) YES (B) BACK`, the
      filter line `(L) (R)`, and the CSS hint `(Z)+(X) FOR HANDWARMER` / `(Z)+(X) CANCELS
      HANDWARMER`. *Mechanism: 4 shape glyphs appended to the SIS font atlas
      (`sislib_font_extra.inc` from `tools/gen_button_glyphs.py`, indices 287-290, SJIS
      0x8540-0x8543) + one row each in the three lookup tables in hsd_3A76.c (now
      `[0x248]`); `lbbuttonglyph.c` draws an icon as a coloured shape entry with the
      font's own letter over it, all positioned from the kerning table, and translates
      `+ ( ) / ! ?` to their SJIS pairs so callers write plain ASCII with `#A`-style
      markers. `lbButton_Measure` gives exact widths, so the title and hint bars are now
      centred by measurement, not by eye. Things to eyeball: letter centred in its disc,
      icon baseline level with the text, the hint bar not clipped at either edge, and
      the CSS hint still right-anchored where the user put it (right edge of the old
      `ZX FOR HANDWARMER`).*
- [ ] **CSS overlay layout (v31, tuned live by the user):** score `MANGO P1  0 - 0  P3
      ZAIN` top centre (x 188, y -4, 0.62); hint `Z + X FOR HANDWARMER` (the `+` is the SJIS escape `{`, v34) bottom right
      (x 456, y 446, 0.43); status (`SENDING... / SCORE SENT / SEND FAILED`) bottom left (x 2,
      y 446, 0.45). (v32)
      *`ZX`, not `Z + X`: the SIS font has no `+` (nor `(` `)` `/`). History: v25-v29
      were placed by rebuild-and-look; v30 added **layout tune mode** so that never
      happens again; **compiled out since v33** (`LB_TOURNEY_LAYOUT_TUNE 0` in lbtourney.c -
      set it to 1 to get it back). With it on, on the CSS with a set active, hold **L + R**: D-pad nudges the
      selected element 2 px (10 with Z), **X** cycles SCORE / HINT / STATUS, **Y** grows
      the scale by 0.02 (shrinks with Z), and a mid-screen readout `TUNE SCORE X 212 Y 4
      S 52` shows the live values for 3 s. Read them off and hardcode them in `el[]`
      (lbtourney.c). The rumble D-pad toggle is suppressed while the chord is held.*
- [ ] **SIS vertical rule (found fixing the icon letters, v36):** a glyph drawn at
      entry scale s lands **32*(1-s) px below the entry's y**. Each entry pushes its
      scale opcode at its start and pops it at its end, so every entry is measured as a
      32-unit line at the text's default scale 1.0 and bottom-aligned to it. That is the
      "~12 px lower" seen at 0.63 and why the icon letters (scale 0.58*s) sat 13 px
      low in v35. Position two entries of different scale relative to each other with
      this rule, never by eye.
- [ ] **Z + X on the CSS starts a handwarmer straight away** when every present player
      is ready: no stage select, the game begins on a random legal stage (`stage_sel`
      flipped to Random for that one transition so the CSS-exit code fills
      `force_stage_id` from `mnSelStageRandom()` and the SSS skips itself; restored to
      Choose on the first match frame). Not ready yet -> the press only arms/disarms the
      flag (top hint `HANDWARMER NEXT - NOT SCORED   Z AND X TO CANCEL`), and Start goes
      through the SSS as usual. In the game a `HANDWARMER m:ss` clock counts up **in the
      top-left corner** and **turns red past 1:00**, and **the HUD's own countdown is
      hidden** (v27, `ifTime_HideTimers()` re-asserted after every vanilla frame; the
      match timer still runs underneath, so the 8:00 limit still ends the game). Back on
      the CSS the flag is **cleared automatically**. *Informational: the C-stick score binds still decide
      what is scored. `forceKioskDefaults` pins `stage_sel = 0` so a memcard with
      "random stage" mode cannot skip the SSS for real games.*
- [ ] **SSS shows ONLY the six legal stages** from the first frame (BF, FD, FoD, YS,
      DL64, PS; every other icon hidden and unhoverable; the random icon stays). *v25,
      matching the venue Wii. Filter is by stage id (`sssIsLegal`), not `stage_mask`.*
- [ ] **Stage striking: X over a stage removes it** - icon gone, **no hover outline or
      preview left on the empty spot**, A there is refused with the buzz, random skips
      it. **Y puts all struck stages back** (v26, mis-strike recovery); strikes also
      reset on every SSS entry. *v24 kept struck stages hoverable so the outline
      lingered - user flagged it. If every legal stage is struck, random ignores the
      strikes rather than hanging.*
- [ ] **Sheik's nametag vanishes during Vanish** (up-B), Zelda's during Farore's Wind,
      and under a cloaking device: `fn_802FCC44` (ifnametag.c) now also hides the tag
      while the fighter's own `invisible` bit is set. Tags are hidden during the entry
      animation too (same bit) - expected.
