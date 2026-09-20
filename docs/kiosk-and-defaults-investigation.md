# Kiosk flow + tournament defaults — read-only investigation

Scope: answers to design.md R14 (kiosk menu flow) and R12 (unlock-all +
default match rules, as decomp *source* edits) for the Tournament Reporter
(`../tournament-reporter/docs/design.md`). Investigation only; **no code was
changed.** All paths are relative to this repo's root; every line number was
verified against branch `reporter`.

Our new files: `src/melee/mn/mntourney.c` (Tournament/set-selection submenu),
`src/melee/lb/lbtourney.c` (CSS score overlay + set state),
`src/melee/lb/lbrelayexi.c` (EXI driver). The two wire-ups that already exist
(both **data-table** edits, not function-body edits):

- `mn_803EB6B0[0x24]` row 0 (`MENU_KIND_MAIN`) think is now
  `mnTourney_MainMenuThink` (src/melee/mn/mnmain.c:379-388); the array bound was
  bumped from `0x22` to `0x24`, and a new row 35 (`MENU_KIND_TOURNAMENT`, mn/forward.h:120)
  points its think at `mnTourney_Think` (mnmain.c:628-635).
- `GS_CSS` scene row now uses `lbTourney_CSSFrame` / `lbTourney_CSSExit`
  wrapping the vanilla `mnCharSel_Scene_OnEnter` (src/melee/gm/gmscdata.c:119-125).

**Governing constraint (CLAUDE.md):** *never modify a matched function.* The
existing pattern only edits **data tables** (scene/menu/rules initializers) to
point at, or seed values for, our three new TUs. Every recommendation below
holds to that; §6 flags each place where the "obvious" edit would instead touch
a matched function body and gives the matched-safe alternative.

---

## Group A — Kiosk flow (R14)

### The scene/menu machinery in one paragraph

`main()` → `gm_801A4510()` (src/melee/gm/gmmain.c:219). That routine inits every
mode, then chooses the **first major scene**: `GM_PROGRESSIVE_SCAN` or, normally,
**`GM_BOOT`** (src/melee/gm/gm_1A3F.c:381-387), and runs modes forever in the
loop at gm_1A3F.c:390-397, each mode returning the next. A menu screen is one
`MenuKind` submenu inside the `GM_MENU` major scene (scene `GS_MENU`); which
submenu is entered on `GM_MENU` enter is decided by `onEnter`
(src/melee/gm/gmmenumode.c:72-241). Menu-to-menu swaps inside `GS_MENU` go
through `mn_80229894(kind, sel, panel)` (src/melee/mn/mnmain.c:710-726), which
frees the current think GObj and spawns `mn_803EB6B0[kind].think`. Menu→other-mode
transitions use `mn_80229860(mode)` (mnmain.c:701-706, writes the scene
exit-data's `pending_mode` and calls `gm_801A4B60()`); a scene that is **not** a
menu (e.g. the CSS) leaves via `gm_ChangeGameModeAfterCurrentScene(mode)`
(declared gm/gm_1A3F.h:92) + `gm_801A4B60()`.

### Q1 — Boot directly into the Tournament menu

**Where the first scene is decided.**
1. Initial major mode: `gm_801A4510` sets `state_machine.routing.curr_mode = GM_BOOT`
   (src/melee/gm/gm_1A3F.c:386; the `GM_PROGRESSIVE_SCAN` branch is DTV-only).
2. `GM_BOOT`'s state array `gm_Mode_Boot_States` (src/melee/gm/gmboot.c:33-47)
   runs the `GS_MEMCARD` scene with `bootOnLoad`/`bootOnLeave`.
3. **`bootOnLoad`** picks the *next* mode (src/melee/gm/gmboot.c:49-60): if
   `gmMainLib_8046B0F0.skip_intro` → `GM_TITLE`, else `gm_801BF708(0)` and
   `GM_OPENING_MV` (the opening movie). `skip_intro` is set true only on a soft
   reset (`OSGetResetCode()==0x80000000`) in `gmMainLib_8015FCC0`
   (src/melee/gm/gmmain_lib.c:1331) — a cold boot always plays the movie.
4. **`bootOnLeave`** commits it: `gm_ChangeGameModeAfterCurrentScene(scene_data->mode_id)`
   (src/melee/gm/gmboot.c:81). The comment right above it (gmboot.c:80) names the
   classic *"boot to CSS"* Gekko code, which patches exactly this `mode_id` to a
   hardcoded `GM_VS` — i.e. this is the canonical boot-redirect point.
5. Eventually the chain reaches `GM_MENU`; `onEnter` (gmmenumode.c:72) picks the
   submenu. A fresh boot (no meaningful previous mode) hits the **default** case
   `MENU_KIND_MAIN` / `SEL_MAIN_1P` (gmmenumode.c:236-239), whose think is our
   `mnTourney_MainMenuThink`.

**Cleanest matched-safe hook — auto-enter from our own main-menu think.**
Because `mn_803EB6B0[MENU_KIND_MAIN].think` is *already ours*
(`mnTourney_MainMenuThink`, mntourney.c:455-477), the no-matched-edit kiosk boot
is: on the first frame the main-menu think runs in kiosk mode, execute the same
body as its existing Z-press branch (mntourney.c:459-474) without waiting for Z —
set `cur_menu = MENU_KIND_TOURNAMENT`, spawn `mnTourney_Think`, free the
main-menu GObj, and `sendList()`. This lands on the set list the instant the main
menu would appear, and touches only mntourney.c. It does **not** skip the opening
movie / title (those are chosen in the matched `bootOnLoad`).

**To also skip the intro/title** there is no data-table lever: the target mode is
chosen in code inside `bootOnLoad` (matched). Options, in order of preference:
- Set `gmMainLib_8046B0F0.skip_intro = true` from our code before the boot scene
  reads it — but the only safe writer runs after `gmMainLib_8015FCC0`, and this
  still routes to `GM_TITLE`, not straight to the menu.
- Accept the one-line matched edit in `bootOnLoad` (gmboot.c:55/58) to set
  `mode_id = GM_MENU` — this is precisely what the "boot to CSS" Gekko does, but
  it **violates the matched-function rule** (see §6). Recommended only if the
  team decides the boot TU may carry a documented exception.

**Recommendation:** ship the auto-enter-from-`mnTourney_MainMenuThink` approach
(fast to the set list, zero matched edits); treat "skip the movie" as a separate,
optional decision that costs one matched edit in `bootOnLoad`.

### Q2 — After END_SET, return to the set list and re-issue LIST_SETS

**Today:** `lbtourney.c pollRelay` just clears `has_set` on END_SET ok
(src/melee/lb/lbtourney.c:235-238); the station sits on the CSS.

**The CSS→menu transition to copy.** The vanilla CSS back-exit does exactly this
transition (src/melee/mn/mncharsel.c:5421-5422):
`gm_ChangeGameModeAfterCurrentScene(GM_MENU); gm_801A4B60();`. From the CSS you
must use `gm_ChangeGameModeAfterCurrentScene` (not `mn_80229860`, which needs a
menu scene's exit-data). Call these from lbtourney.c when END_SET completes.

**Landing on the Tournament submenu instead of the VS submenu.** After the
transition, `GM_MENU`'s `onEnter` runs. Previous mode is `GM_VS`, whose case
selects `MENU_KIND_VS` (gmmenumode.c:172-175) — not what we want, and we cannot
edit `onEnter` (matched). The matched-safe lever is **`force_main_menu`**:
`onEnter` checks `gmMainLib_GetGameRules()->force_main_menu` *first* and, if set,
forces `MENU_KIND_MAIN` and clears the flag (gmmenumode.c:94-100). It is an
ordinary writable data field (`struct GameRules` field 0, gm/types.h:222) and is
already set this way from ordinary code elsewhere
(`gmMainLib_GetGameRules()->force_main_menu = 1;`, src/melee/mn/mn_22EC.c:223).
So: before the transition, set `force_main_menu = 1`; `GM_MENU` then enters
`MENU_KIND_MAIN`, whose think is our `mnTourney_MainMenuThink`, which (per Q1's
kiosk auto-enter) drops straight into the Tournament submenu and calls
`sendList()` — re-issuing LIST_SETS for free.

**How the think proc gets spawned on re-entry.** Two equivalent routes, both
already in the tree:
- Let the `GM_MENU` scene spawn `mn_803EB6B0[MENU_KIND_MAIN].think`
  (=`mnTourney_MainMenuThink`) via its normal enter path, then have that think
  auto-enter (Q1).
- Or, once already inside `GS_MENU`, call `mn_80229894(MENU_KIND_TOURNAMENT, 0, 3)`
  (mnmain.c:710) — it frees the current think GObj and spawns
  `mn_803EB6B0[MENU_KIND_TOURNAMENT].think` = `mnTourney_Think` (mnmain.c:634).
  This is exactly the inverse of `exitToMainMenu`'s `mn_80229894(MENU_KIND_MAIN,0,3)`
  (mntourney.c:281-287). Note `mnTourney_Think` boots in `TM_OFF` and does
  nothing until something calls `sendList()`, so whichever route is used must
  kick a LIST_SETS (the Q1 auto-enter body already does).

### Q3 — CSS B/back routes to the Tournament menu when a set is active

**Where vanilla handles CSS B.** Inside the **matched** `mnCharSel_Scene_OnFrame`
(src/melee/mn/mncharsel.c:5348): the back predicate `mn_8022F218()` gates the
exit block at mncharsel.c:5415-5423 (`sfxBack`, teardown, then
`gm_ChangeGameModeAfterCurrentScene(GM_MENU); gm_801A4B60(); return;`).

**Where our hook sits.** `lbTourney_CSSFrame` (src/melee/lb/lbtourney.c:275-295)
is the `GS_CSS` `on_frame`; it runs our overlay/keybind logic and then **calls
`mnCharSel_Scene_OnFrame()` last** (lbtourney.c:294). Inputs are already latched
before `on_frame` (per docs/menu-orientation.md §1), so at the top of
`lbTourney_CSSFrame` we can read the same B press (via `gm_GetButtonsTriggered(4)`
/ `mn_8022F218`-equivalent) and, **when `has_set` is true**, set
`gmMainLib_GetGameRules()->force_main_menu = 1;` before falling through to
`mnCharSel_Scene_OnFrame()`. Vanilla then performs its own B→`GM_MENU`
transition *in the same frame*, but now `onEnter` sees `force_main_menu` and
lands on `MENU_KIND_MAIN` → our think → Tournament submenu. This needs **no edit
to the matched function** and reuses vanilla's teardown. (Alternatively, fully
pre-empt: detect B ourselves, run the transition, and `return` before calling
`mnCharSel_Scene_OnFrame` — but then we must replicate the teardown at
mncharsel.c:5416-5420, so the `force_main_menu`-then-fall-through route is
cleaner.)

**Kiosk-flow summary (cleanest path):** one mechanism serves all three. Make
`mnTourney_MainMenuThink` auto-enter the Tournament submenu (+`sendList`) in
kiosk mode (Q1). For any return to the menu from the CSS — END_SET ok (Q2) or B
(Q3) — set `gmMainLib_GetGameRules()->force_main_menu = 1` and use the CSS's
`gm_ChangeGameModeAfterCurrentScene(GM_MENU)` + `gm_801A4B60()` transition
(Q2 issues it from lbtourney's poll; Q3 lets vanilla issue it after we set the
flag). No matched-function edits; the intro-skip is the only piece that would
need one and is optional.

---

## Group B — Tournament defaults (R12, decomp source edits)

### Q4 — Unlock all characters and stages

**Character unlock.** `gm_IsCKindUnlocked(ckind)` (src/melee/gm/gm_1601.c:2310-2322)
is the predicate the "Unlock All Characters" Gekko patches. It reads the save-data
bitmask via `gmMainLib_GetUnlockedCharactersBitmaskPtr()`
(src/melee/gm/gmmain_lib.c:926-929 → `&save_data.unlocked_characters`, a `u16`,
gm/types.h:284) and returns true when the character's unlock bit is set *or* the
character is a starter (`unlock_bit == NUM_UNLOCKABLE_CHARACTERS`). There are
**11** unlockable characters (`NUM_UNLOCKABLE_CHARACTERS`, gm/gm_1601.h:12); the
bitmask is zeroed on save init (memzero of the trophy_count..x1F2C region,
gmmain_lib.c:1181-1183). `gm_UnlockCKind` (gm_1601.c:2324-2349) is the setter
(`*mask |= 1 << idx`).

**Stage unlock — already done by default.** `gm_IsStageUnlocked(stkind)`
(gm_1601.c:2141-2147) tests `(1<<stkind) & gmMainLib_GetGamePrefs()->stage_mask`.
`stage_mask` is the last field of `gmMainLib_DefaultGamePrefs` and is initialized
to **`U32_MAX`** (all stages unlocked) — src/melee/gm/gmmain_lib.c:53-55 with
`struct GamePrefs::stage_mask` at gm/types.h:140. So **stages need no change** on
a fresh save; the defaults already unlock them.

**Smallest source change for characters (matched-safe).** The direct edit —
making `gm_IsCKindUnlocked` `return true` — is a **matched-function edit and is
forbidden** (§6). The matched-safe equivalent is a one-time write of the mask
from our own code (e.g. at kiosk boot in `mnTourney_MainMenuThink`, or in an init
call from lbrelayexi/lbtourney):
`*gmMainLib_GetUnlockedCharactersBitmaskPtr() = 0xFFFF;` (11 bits needed; all-ones
is fine). This calls a public accessor (declared gm/gmmain_lib.h:130) — we only
*call* matched code, never edit it — and makes every `gm_IsCKindUnlocked` return
true thereafter. **Caveat:** the mask lives in memory-card/NAND save data, so on
a machine with existing save data the loaded value is what our write overrides at
runtime; writing it each boot (rather than relying on a data default) is the
robust choice.

### Q5 — Default match rules: 4 stocks, 8:00 timer, no items

**Where the live values come from.** VS matches are built in the rules-assembly
function in gm_1601.c: it reads `rules = gmMainLib_GetGameRules()` and
`prefs = gmMainLib_GetGamePrefs()` (src/melee/gm/gm_1601.c:3516-3517) and copies:
- `rules->mode` selects the match kind (gm_1601.c:3520-3550).
- **Timer source depends on mode:** in Time mode (`mode==0`) the timer is
  `rules->time_limit * 60` (gm_1601.c:3523-3525); in **Stock mode (`mode==1`) the
  timer is `rules->stock_time_limit * 60`** (gm_1601.c:3530-3532) — *not*
  `time_limit`.
- `rules->stock_count` → each player's starting stocks (gm_1601.c:3554).
- `prefs->item_freq` → item spawn rate (gm_1601.c:3586), and `prefs->item_mask`
  → which item types are enabled (gm_1601.c:3588-3596). Items are "on" iff
  `item_freq != 0`: `gm_8016AE80()` returns `item_freq` as a bool
  (src/melee/gm/gmvs.c:83-86), and the item-switch menu writes
  `item_freq = menu_index - 1` so the first option ("None") is 0
  (src/melee/mn/mnitemsw.c:227).

**The two data initializers to edit (both matched-safe).**

1. `GameRules gmMainLib_DefaultGameRules` (src/melee/gm/gmmain_lib.c:29-51),
   copied into the live rules at boot (gmmain_lib.c:1308). `struct GameRules` field
   layout at gm/types.h:221-244. Change:
   - `mode`: `0` → `1` (Stock) — gmmain_lib.c:32 (field +0x02).
   - `stock_count`: `3` → `4` — gmmain_lib.c:34 (field +0x04).
   - `stock_time_limit`: `0` → `8` — gmmain_lib.c:38 (field +0x08). **This is the
     8:00 timer for Stock mode**; setting `time_limit` (currently `2`, field
     +0x03, gmmain_lib.c:33) would *not* take effect in Stock mode.

2. `struct GamePrefs gmMainLib_DefaultGamePrefs` (src/melee/gm/gmmain_lib.c:53-55),
   copied into save data at init (gmmain_lib.c:1192). `struct GamePrefs` layout at
   gm/types.h:133-141. For "no items":
   - `item_freq`: `2` → `0` (None) — first initializer element, gmmain_lib.c:54
     (field +0).
   - `item_mask`: `U64_MAX` → `0` (no item types enabled) — second element,
     gmmain_lib.c:54 (field +0x08). Belt-and-braces with `item_freq=0`.

These are the decomp equivalents of the Gekko `Stock Mode` / `4 Stocks` /
`8 Minutes` / `No Items` writes. **Caveat (same as Q4):** defaults seed save data
only on (re)initialization; a machine with existing save data uses the stored
values. For a guaranteed kiosk, also write the live structs at boot
(`gmMainLib_GetGameRules()` / `gmMainLib_GetGamePrefs()`) from our code rather
than relying on the data defaults alone.

### Q6 — Risks / matched-function collisions (must be data/wrapper edits)

- **`gm_IsCKindUnlocked` (gm_1601.c:2310) and `gm_IsStageUnlocked`
  (gm_1601.c:2141) are matched functions.** The Gekko "unlock all" patches their
  return values; editing the bodies is forbidden. Use the mask-write approach
  (Q4) from our own TU. (Stages already default-unlocked, so no code needed
  there.)
- **`bootOnLoad` (gmboot.c:49) and `gm_801A4510` (gm_1A3F.c:366) are matched.**
  A true boot-straight-to-menu (skipping the movie) means editing `bootOnLoad`'s
  `mode_id` — a matched edit. The matched-safe path is the auto-enter from our
  own `mnTourney_MainMenuThink` (Q1), which accepts the intro/title playing first.
- **`gmMainLib_GetGameRules` / `gmMainLib_GetGamePrefs` / `mnCharSel_Scene_OnFrame`
  / `mn_80229894` / `mn_80229860` / `gm_ChangeGameModeAfterCurrentScene` are
  matched — but we only *call* them, never edit them.** All kiosk transitions
  (Q2/Q3) are built by calling these plus setting the `force_main_menu` **data
  field**; `onEnter` (gmmenumode.c:72, matched) is influenced only through that
  field, never edited.
- **Rule/pref changes are data-initializer edits** (`gmMainLib_DefaultGameRules`,
  `gmMainLib_DefaultGamePrefs` in gmmain_lib.c) — data, not functions — matching
  the existing pattern already used for `mn_803EB6B0` and the `GS_CSS` row.
- **Save-data override risk (Q4 + Q5):** all four defaults (unlock mask, item
  prefs, stage mask) are loaded from the memory card / NAND at boot; the data
  initializers only apply on a fresh/reset save. On a station with existing save
  data the stored values win. The robust kiosk answer is to *write the live
  structs at boot from our code* (mask + rules + prefs), not to rely on the data
  defaults alone. The data-default edits remain worthwhile as the correct
  fresh-save baseline.
- **`item_freq` vs `stock_time_limit` foot-guns:** in Stock mode the timer is
  `stock_time_limit`, not `time_limit` (gm_1601.c:3530); and items key off
  `item_freq != 0` (gmvs.c:85). Getting either field wrong silently produces the
  wrong ruleset. These are the two easiest mistakes to make and are called out
  above.

---

## Implementation-path summary

**Kiosk flow (R14).** One lever does all three, no matched edits:
1. In `mnTourney_MainMenuThink`, add a kiosk auto-enter that runs the existing
   Z-branch body (mntourney.c:459-474) on first frame without waiting for Z.
2. On END_SET ok, from `lbtourney.c` set
   `gmMainLib_GetGameRules()->force_main_menu = 1` and call
   `gm_ChangeGameModeAfterCurrentScene(GM_MENU); gm_801A4B60();` (the same pair
   vanilla CSS-B uses, mncharsel.c:5421-5422); the auto-enter then re-lists.
3. For CSS-B with a set active, set `force_main_menu = 1` at the top of
   `lbTourney_CSSFrame` and let the wrapped `mnCharSel_Scene_OnFrame` perform its
   normal B→`GM_MENU` transition.
Optional: skipping the intro movie needs a one-line matched edit in `bootOnLoad`
(gmboot.c) — flagged, not recommended unless the team allows a boot-TU exception.

**Defaults (R12).** Two data-initializer edits + one small boot-time write:
- `gmMainLib_DefaultGameRules` (gmmain_lib.c:29-51): `mode` 0→1, `stock_count`
  3→4, `stock_time_limit` 0→8.
- `gmMainLib_DefaultGamePrefs` (gmmain_lib.c:53-55): `item_freq` 2→0,
  `item_mask` U64_MAX→0. (Stages already default to all-unlocked via
  `stage_mask = U32_MAX`.)
- Characters: no data default exists for the unlock bitmask; write
  `*gmMainLib_GetUnlockedCharactersBitmaskPtr() = 0xFFFF;` once from our code at
  kiosk boot. To be robust against existing save data, also write the live
  `GameRules`/`GamePrefs` at boot rather than trusting the data defaults.

**Blockers / decisions for the team:**
- Whether the boot TU may carry a documented matched-function exception to skip
  the intro (otherwise the movie plays before the auto-enter).
- Confirm the kiosk should force live save-data values at boot (overriding a
  populated memory card), since the data-initializer defaults alone do not apply
  when save data already exists.
