# Kiosk defaults debug — why items stay on and stages stay locked

Read-only investigation (branch `reporter`, no code changed). Follows up on
`docs/kiosk-and-defaults-investigation.md`, which had two wrong facts. Context:
`forceKioskDefaults()` (src/melee/mn/mntourney.c:469-483, called from
`mnTourney_MainMenuThink` mntourney.c:494) writes live `GameRules` +
`GamePrefs` + the character-unlock mask at menu time. Observed after testing:
stocks/timer correct, **items still on**, **not all stages unlocked**.

All line numbers verified against `reporter`.

## TL;DR — root causes

| Symptom | Cause | Field written | Correct write |
|---|---|---|---|
| Items still on | Wrong sentinel: `item_freq = 0` is the *lowest ON* frequency, not off. "Off" is **-1**. | `prefs->item_freq = 0` | `prefs->item_freq = -1` |
| Stages locked | Wrong mask entirely: `stage_mask` is the per-match random/legal **toggle** mask, not the **unlock** mask. Unlock lives in `save_data.x186A`. | `prefs->stage_mask = 0xFFFFFFFF` | call `gm_8016468C()` (keep the `stage_mask` write too) |
| Stocks/timer OK | `GameRules` (`x1850`) is a separate struct, not in save_data; read live at match build. | — | already correct |
| Characters | `0xFFFF` write is correct and survives (== `gm_80164F18()`); works. | — | already correct |

There is **no save-data reload** between the menu and the match (see Q3), so a
menu-time write is timing-safe. The bugs are wrong *values*/wrong *field*, not
timing.

---

## Memory map (why some writes stick and others "don't")

- `gmMainLib_GetGameRules()` → `&gmMainLib_804D3EE0->x1850` (gmmain_lib.c:144-147).
  A standalone struct, **not** inside save_data. Seeded once from
  `gmMainLib_DefaultGameRules` at boot (gmmain_lib.c:1308). This is why
  stocks/timer stick.
- `gmMainLib_GetGamePrefs()` → `&GetCardData()->save_data.x1CB0`
  (gmmain_lib.c:159-162). Lives **inside** save_data (the memory-card image).
- `gmMainLib_GetUnlockedCharactersBitmaskPtr()` → `&save_data.unlocked_characters`
  (gmmain_lib.c:926-929). Also inside save_data.
- Real stage-unlock mask: `save_data.x186A`, via `gmMainLib_8015EDA4()`
  (gmmain_lib.c:936-939). **Separate field from `GamePrefs.stage_mask`.**

`struct GamePrefs` (gm/types.h:133-141): `item_freq` is `u8` at +0, `item_mask`
`u64` at +8, `stage_mask` `u32` at +0x18.

---

## Q1 — Items: where spawning is actually decided, and why `item_freq = 0` fails

**The live prefs ARE what the match reads.** VS matches are assembled by
`gm_80167BC8(vs_data)` (gm_1601.c:3508). It runs at `gmVsMelee_EnterVs`
(src/melee/gm/gmvsmelee.c:177) — i.e. the GM_VS state right before the game
boots, *after* the CSS/SSS — and reads **live** `gmMainLib_GetGameRules()` /
`gmMainLib_GetGamePrefs()` (gm_1601.c:3516-3517), copying:
- `vs_data->start.rules.item_freq = (s8) prefs->item_freq;` (gm_1601.c:3586)
- enabled item-type set `start.rules.x20` from `prefs->item_mask` (gm_1601.c:3588-3596)

`start->rules` is then copied into the StartMeleeData (gmvsmelee.c:178). So the
value we set at the menu *does* propagate; it is neither re-loaded nor taken
from a different struct. (`gm_InitVsMode`/`gm_SetupRulesDefaults` at
gmvsmelee.c:103 only touch the VsModeData scratch `vs->start.rules`, which
`gm_80167BC8` overwrites from live prefs — not a factor.)

**The actual runtime gate — and the bug.** Items are enabled iff item frequency
is **not -1**:
- `Item_80266F3C` "Check if items are enabled" → `return gm_8016AE80() != -1;`
  (src/melee/it/item.c:80-87).
- `gm_8016AE80()` → `gm_GetStartMeleeRules()->item_freq` (src/melee/gm/gmvs.c:83-86).
- Reinforced in the spawner: `it_8026D324` bails only when `... == -1`
  (itspawn.c:475-477); the freq is also used as a *table index*
  `xFC[gm_8016AE80() * 2]` (itspawn.c:187, 439).

So `item_freq` semantics are **-1 = None/off**, **0..4 = ascending ON
frequencies**. `item_freq = 0` is the lowest *enabled* rate, not off — hence
items keep spawning. Confirmed by the item-switch menu, which maps its "None"
row to -1: `item_freq = data->x21 - 1` (mnitemsw.c:227, and inverse
`x21 = item_freq + 1` at mnitemsw.c:827), and by every vanilla no-item mode
hardcoding `-1` (gmvsmode.c:168, gmmultiman.c:300/1043, gmhomerun.c:114,
gm_1884.c:883).

The prior investigation's claim that "the first option (None) is 0" (and that
`item_freq != 0` gates items) is **wrong**; the gate is `!= -1`.

**Correct fix (matched-safe, our-TU data write):** in `forceKioskDefaults`
change `prefs->item_freq = 0;` → `prefs->item_freq = -1;`.
`GamePrefs.item_freq` is `u8`, so this stores `0xFF`; `gm_80167BC8` reads it back
as `(s8)` → `-1` (gm_1601.c:3586), so it survives correctly. `item_mask = 0` can
stay (belt-and-braces; with `item_freq = -1` the whole spawner is gated off
regardless of the enabled-type mask). No matched function is edited — we only
set a data field.

---

## Q2 — Stages: `stage_mask` is the toggle mask, not the unlock mask

**`stage_mask` is NOT the SSS unlock gate.** `GamePrefs.stage_mask` (types.h:140)
is the per-match **toggle / random-legal** mask:
- set by the VS stage-toggle menu `gm_801641E4(stage, enable)` (gm_1601.c:2132-2139),
- read by random-stage selection `gm_80164330` / `fn_801642A0` (gm_1601.c:2149-2205),
- `gm_IsStageUnlocked(stkind)` (gm_1601.c:2141-2147) reads the *same* `stage_mask`
  field, but its only callers are the toggle menu (mnstagesw.c:683) and
  data-delete (mndatadel.c:91) — **not** the stage-select screen.

**The real stage-unlock mechanism** is a different field, `save_data.x186A` (a
`u16`), via `gmMainLib_8015EDA4()`:
- predicate `gm_80164430(stkind)` (gm_1601.c:2228-2244) tests
  `(1 << unlock_bit) & *gmMainLib_8015EDA4()`, where `unlock_bit =
  getStageUnlockIndex(...)`; returns true if the stage isn't unlockable
  (`unlock_bit == NUM_UNLOCKABLE_STAGES`) or its bit is set.
- setters: `gm_80164504(stkind)` unlocks one (gm_1601.c:2246-2262);
  **`gm_8016468C()` unlocks ALL** — loops `i < 11`, `*ptr |= 1 << i`
  (gm_1601.c:2278-2286). `NUM_UNLOCKABLE_STAGES == 11`.

**The stage-select screen gates on `gm_80164430`, not `stage_mask`:**
`mnStageSel` sets each icon state `= gm_80164430(...stkind) ? 2 : 1`
(src/melee/mn/mnstagesel.c:531). That is exactly the stages that stayed locked.

**Mask width.** `stage_mask` is `u32`; toggle loops run to `0x1D` (29 slots)
(gm_1601.c:2156, 2189). The *unlock* space is only the 11 bits of `x186A`
(the unlockable stages: Battlefield, Final Destination, the three past/N64
stages, etc.). Writing `0xFFFFFFFF` to `stage_mask` therefore cannot unlock
anything — wrong field and, for unlock purposes, wrong width.

**Correct fix (matched-safe):** call the public helper **`gm_8016468C()`** from
`forceKioskDefaults` (declared gm/gm_1601.h:172 — we only *call* it; this is the
same routine the debug "unlock all" path uses at gmmain_lib.c:1271). Keep the
existing `prefs->stage_mask = 0xFFFFFFFF;` too, because that is the separate
toggle mask that makes every unlocked stage legal for random/enabled. Requires
`#include` of gm/gm_1601.h in mntourney.c (add if not already present).

---

## Q3 — Timing: nothing reloads GameRules/GamePrefs between menu and match

Save data (which contains `GamePrefs` *and* `unlocked_characters` *and* `x186A`)
is only (re)initialized by:
- `gmMainLib_8015FBA4()` — full boot init; callers: gmmain.c:172 (boot),
  gm_1A3F.c:219 (boot/`gm_801A4510`), mndatadel.c:145 (user erases save data).
- `gmMainLib_8015FA34()` / `gmMainLib_8015F600()` — memory-card load; caller
  gmscmemcard.c:297, i.e. the **GS_MEMCARD scene inside GM_BOOT**, before the
  menu. (Also soundtest.c.)

None of these run on GM_VS enter, CSS enter, or SSS enter. The card image is
read once at boot, *before* the main menu. Therefore a write made in
`mnTourney_MainMenuThink` survives untouched through START_SET → GM_VS → CSS →
SSS → `gm_80167BC8` at `gmVsMelee_EnterVs`. The live `GameRules`/`GamePrefs`
structs are read (not reloaded) at match-build time.

**Conclusion:** the menu is a perfectly safe place to force these values; the
observed failures are value/field bugs, not overwrites. The **latest** safe
point would be an init hook in our own TU that runs on GM_VS enter, but that is
unnecessary — the current menu-time write timing is fine. (Note the scratch
`gm_SetupRulesDefaults` at gmvsmelee.c:103 resets only the VsModeData copy, not
the live structs, and is always superseded by `gm_80167BC8`'s live read.)

---

## Q4 — Characters: the `0xFFFF` write is correct and works

CSS gates each icon on `gm_IsCKindUnlocked(ckind)` (mncharsel.c:4362, 4437,
4533; predicate at gm_1601.c:2310-2322), which tests
`*gmMainLib_GetUnlockedCharactersBitmaskPtr() & (1 << unlock_bit)`. There are 11
unlockable characters (`NUM_UNLOCKABLE_CHARACTERS`).

`forceKioskDefaults`' `*gmMainLib_GetUnlockedCharactersBitmaskPtr() = 0xFFFF;`
sets all 16 bits (11 needed) — functionally identical to the stock "unlock all"
routine `gm_80164F18()` (gm_1601.c:2438-2448, which ORs bits `0..10`). Since
save_data is not reloaded after the menu (Q3), this write survives to the CSS.
**Characters are genuinely unlocked; this part is not buggy.** (Optionally
replace the literal with a `gm_80164F18()` call for symmetry with the stage fix,
but it is not required.)

---

## Recommended `forceKioskDefaults` changes (all matched-safe, our-TU only)

Only data writes / calls to public accessors — no matched function body is
touched (consistent with CLAUDE.md and the existing data-table pattern):

```c
rules->mode = 1;              /* Stock            — OK, keep */
rules->stock_count = 4;       /* OK, keep */
rules->stock_time_limit = 8;  /* 8:00 in Stock    — OK, keep */

prefs->item_freq = -1;        /* FIX: -1 = off (was 0 = lowest ON) */
prefs->item_mask = 0;         /* keep (harmless with freq -1) */
prefs->stage_mask = 0xFFFFFFFF; /* keep: random/legal toggle mask */

gm_8016468C();                /* FIX: real stage UNLOCK (save_data.x186A) */
*gmMainLib_GetUnlockedCharactersBitmaskPtr() = 0xFFFF; /* OK, keep (== gm_80164F18) */
```

Add `#include "melee/gm/gm_1601.h"` to mntourney.c for `gm_8016468C` if absent.
No timing change needed — the menu-time write already survives to match start.
