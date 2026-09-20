# Boot + main-menu investigation (kiosk R14)

Read-only investigation for the Tournament Reporter kiosk
(`../tournament-reporter/docs/design.md` §6.1 / R14). No code was changed. All
paths are relative to this repo root; line numbers verified against branch
`reporter`.

Terminology used below for "matched-safe":

- **matched-body edit** — editing the C body of a decompiled/matched function.
  Forbidden by CLAUDE.md (the one accepted exception is `bootOnLoad` in the boot
  TU `gmboot.c`).
- **data-table edit** — changing a `static`/global data array (e.g.
  `mn_803EB6B0[]`, `scenes[]`, `modes[]`). Not a function body; already used
  freely in this fork (the tournament work bumped `mn_803EB6B0` to `[0x24]`,
  added rows, and repointed the `MENU_KIND_MAIN` think). Treated as allowed.
- **our-TU code** — `mn/mntourney.c`, `lb/lbtourney.c`, `lb/lbrelayexi.c`: new
  unmatched files we own.
- **asset edit** — changing a `.dat`/archive (model, texture, matanim, SIS text
  bank). Heaviest; called out explicitly wherever required.

---

## Problem 1 — "No memory card" prompt on every boot

### Where the prompt comes from

Boot runs `GM_BOOT` (`modes[]` row `gmscdata.c:710-717`,
`gm_Mode_Boot_States` `gmboot.c:33`), whose single state uses the **`GS_MEMCARD`
minor scene** (`scenes[]` row `gmscdata.c:359-365`):
`gm_Scene_MemCard_OnFrame` / `_OnEnter` / `_OnExit` in `gmscmemcard.c`.

The prompt is produced entirely by the decision state-machine in the **matched**
`gm_Scene_MemCard_OnFrame` (`gmscmemcard.c:268-463`). The decisive branch is
state `tickDecision_0` (`gmscmemcard.c:294-304`):

```
case 0:
    temp_r29 = lb_8001CBBC();            // gmscmemcard.c:296
    gmMainLib_8015FA34(temp_r29);
    if (temp_r29 == 0 || temp_r29 == 2)  // Ready / _2  -> silent exit
        enter_data.decision = 0x14;      // tickDecision_20 -> gm_801A4B60()
    else
        enter_data.decision = 1;         // -> gm_801AF250() draws the prompt
```

`lb_8001CBBC` (`lbcardgame.c:175-187`) returns `LbCardResult_Invalid` when the
card status is not `LbCardStatus_0`, else probes with `lb_8001BD34`; a missing or
unformatted card yields a value that is **not** `0`/`2`, so boot falls into
`decision = 1`. `decision 1` calls `gm_801AF250` (`gmscmemcard.c:198-266`), which
switches on `lb_8001C87C()` (`lbcardgame.c:123`) and calls `gm_801AEE6C` to build
the "no card / corrupt data / create file?" panels. **This decision-0 branch is
the decomp equivalent of the GALE01r2 "Skip Memcard Prompt" Gecko code** — that
code forces the silent-exit path so the prompt panels are never built.

### Is there a data flag to skip it? — No clean one

- The scene enter-data is `load_data` (`gmboot.c:30`), whose fields map onto the
  scene's `enterData.unk0` (`gmscmemcard.c:18-22,48-55`). `bootOnLoad` already
  writes it (`gmboot.c:51-61`). But the initial decision is chosen by
  `checkUnk0()` (`gmscmemcard.c:465-471`) which only ever returns `tickDecision_0`
  or `tickDecision_1` — **both enter the machine**. No field on `load_data`
  short-circuits to the exit (`decision 20`).
- The only in-machine early-out is `lb_8001CBBC()` returning `0`/`2`, which we
  cannot force without a valid card present.

So there is **no pure data flag** that suppresses the prompt.

### Recommendation (matched-safe, ranked)

**A. Preferred — repoint the boot state to a trivial pass-through scene (no
matched-body edit).**
Do it the same way `menu-orientation.md` §1 adds a scene:

1. Add a `GameSceneKind` before `GS_COUNT` (`gm/forward.h`).
2. Add a `scenes[]` row for it before the `GS_COUNT` sentinel (`gmscdata.c`,
   data-table edit) whose `on_frame` is a 3-line function *in our TU* that just
   calls `gm_801A4B60()` (break the frame loop) — no card probe, no panels.
3. In the accepted boot-exception TU, change `gm_Mode_Boot_States[0].info.kind`
   from `GS_MEMCARD` to the new kind (`gmboot.c:40`). This is a data edit inside
   the file that already carries the boot exception.

Nothing matched is touched. **Trade-off:** boot then skips the card save/load
probe entirely. For this kiosk that is consistent with the design — it forces all
rules/unlocks live every menu pass (`forceKioskDefaults`, `mntourney.c:470-485`)
and never writes a save — but if any later code assumes the boot-time card init
ran, prefer option B.

**B. If the card init must still run — one unavoidable matched one-liner.**
The only seamless skip that keeps the card probe is to force the silent-exit at
the decision point: in `gm_Scene_MemCard_OnFrame`, make the `decision 0` branch
take `enter_data.decision = 0x14` unconditionally (or gate the prompt on a kiosk
flag) at **`gmscmemcard.c:298-303`**. This is a matched-body edit; cite it as a
second accepted exception on par with the `bootOnLoad` one. (A kiosk flag read
here could be set from `bootOnLoad`, keeping the edit to a single `if`.)

Recommendation: take **A** — it needs no matched edit and matches the kiosk's
"defaults are forced, nothing is saved" model.

---

## Problem 2(a) — Kill the main-menu "blip" before the tournament screen

### Why the blip happens

Boot reaches `GM_MENU`; its state on-enter `onEnter` (**matched**,
`gmmenumode.c:72-241`) picks the first submenu. At boot `force_main_menu` is 0 and
the previous mode is not in the switch, so it hits `default` →
`menu_kind = MENU_KIND_MAIN`, `hovered_selection = SEL_MAIN_1P`
(`gmmenumode.c:236-239`).

`mnMain_Scene_OnEnter` (`mnmain.c:2775-2981`) then, for the default case, spawns
the menu think and **builds the main panel** `mn_8022B3A0(0)`
(`mnmain.c:2971-2977`), and sets `mn_804D6BC8.cooldown = 0x14` (**20 frames**,
`mnmain.c:2786`).

The `MENU_KIND_MAIN` think is our wrapper `mnTourney_MainMenuThink`
(`mn_803EB6B0[0].think`, `mnmain.c:381-388` / `mntourney.c:487-508`). It only
auto-enters the tournament screen once `cooldown == 0` (`mntourney.c:495`);
until then it delegates to vanilla `mn_8022DB10`. So the real main menu is drawn
and animated for ~20 frames before the swap — **that is the blip** (not a single
frame).

### Why we cannot just enter `MENU_KIND_TOURNAMENT` at on-enter

`MENU_KIND_TOURNAMENT` (index 35) is a **SIS-text overlay pseudo-menu**, not a
panel menu: its `mn_803EB6B0[35]` row is `{ NULL, 0, mnTourney_DescIndices, 1,
mnTourney_Think }` (`mnmain.c:628-635`) — **`anim_loop == NULL`**. If `onEnter`
set `data->menu_kind = MENU_KIND_TOURNAMENT`, `mnMain_Scene_OnEnter`'s default
case would call `mn_8022B3A0(0)`, which dereferences
`mn_803EB6B0[cur_menu].anim_loop` at `mnmain.c:1433-1435` and builds the shared
`MenMainConTop` panel with `option_count = 1` — i.e. read through NULL and draw a
bogus panel. So there is **no `force_tournament_menu` analogue that would work**:
the obstacle is the panel build in the matched `mnMain_Scene_OnEnter`, and the
tournament screen has no panel model to build.

### Recommendation (matched-safe)

Keep the `MENU_KIND_MAIN` entry, but remove the visible frames from *our* code
(`mnTourney_MainMenuThink`, our TU) on the boot/auto-enter pass:

1. **Enter on frame 1, not frame 20.** On the auto-enter pass, force
   `mn_804D6BC8.cooldown = 0` so the swap happens the first time the think runs
   instead of after the 20-frame countdown. Gate this on the arm-flag from
   Problem 3 so a user who backs out still gets the normal responsive main menu.
2. **Suppress the one main-panel frame.** The panel GObj was already created by
   `mnMain_Scene_OnEnter` before any think runs. Because think procs run in
   `HSD_GObj_RunProcs()` *before* the frame's GX render
   (`menu-orientation.md` §1), our think can hide the panel on that first pass so
   it never draws: hide the panel root JObj (set `JOBJ_HIDDEN`) — the panel proc
   is `fn_8022AFEC` (`mnmain.h:66`), reachable by walking the GObj list, or
   captured once — then spawn `mnTourney_Think` as it already does
   (`mntourney.c:501-504`). Everything stays in our TU + data tables.

**If a strictly zero-cost, zero-frame path is wanted instead:** the only fully
seamless option is one matched edit in `mnMain_Scene_OnEnter`'s default case
(`mnmain.c:2970-2977`) — on a kiosk "boot into tournament" flag, skip
`mn_8022B3A0(0)` and spawn `mnTourney_Think` directly. Flag it as a matched
exception if chosen; the our-TU hide-the-panel approach above avoids it.

---

## Problem 2(b) — Add a visible, selectable "Tournament" option to the main menu

### Where the main-menu items are defined

The main menu (`MENU_KIND_MAIN`, index 0) has **5 options**: `SEL_MAIN_1P=0`,
`SEL_MAIN_VS=1`, `SEL_MAIN_TOY=2`, `SEL_MAIN_SETTINGS=3`, `SEL_MAIN_DATA=4`
(`mn/forward.h:135-139`). The pieces:

| Piece | Where | Notes |
|---|---|---|
| Menu descriptor | `mn_803EB6B0[0]` `mnmain.c:379-388` | `{ anim_loop=mn_803EB3FC, start_frame=0, desc=mn_803EB660, selection_count=0x05, think=mnTourney_MainMenuThink }` (struct `MenuKindData` `mn/types.h:570-576`) |
| Option count | `selection_count` field above (`0x05`) | bounds cursor movement (`mnmain.c:2680`, `2733-2736`) and panel build (`mnmain.c:1436`) |
| Cursor positions | `mn_803EB3FC[5]` `mnmain.c:294-298` | one `AnimLoopSettings` per option |
| Description SIS ids | `mn_803EB660[] = {0x81..0x85, 0}` `mnmain.c:356` | from `SdMenu.dat`/`SdMenu.usd` bank (`mnmain.c:2947-2951`) |
| Option→joint map | `mn_803EAE68[]` `mnmain.c:96-98` | `option_jobjs[i] = tree[mn_803EAE68[i]]` (`mnmain.c:1474`) |
| **Option row + label graphics** | archive **`MnMaAll`** (`mnmain.c:2798`) | model `MenMainConTop_Top` (option joints) + `MenMainCursor_Top` matanim (label textures). Label shown = matanim frame `start_frame + 2*sel` (`GetSelectionFrameOffset` `mnmain.c:1387-1390`, applied `mnmain.c:1494-1496`) |
| Selection dispatch | `mn_8022DB10` **matched** `mnmain.c:2666-2738` | `switch (hovered_selection)` maps `SEL_MAIN_*` → submenu `MenuKind` (`mnmain.c:2690-2711`), then rebuilds panel + spawns that kind's think |

Note the prompt's premise is slightly off: the **row of options is not drawn from
`SdMenu.dat`** — that file is the SIS *text* bank (descriptions). The visible
option row/labels are 3-D model + material-animation data in the **`MnMaAll`**
archive (`MenMainConTop_Top` + `MenMainCursor_Top`).

### Cost of a genuine 6th visible option

A native-looking "Tournament" row in the 1P/VS/Trophies/… list requires **all** of:

1. **Asset edit (unavoidable) — `MnMaAll`:** add a 6th option joint to
   `MenMainConTop_Top` (so `mn_803EAE68[5]=0x9` resolves to a real joint) and a
   "Tournament" label texture as a new material-animation frame (frame `10`) in
   `MenMainCursor_Top`. Without this, cursor position 6 has no drawn row/label —
   an invisible option. There is no spare pre-authored slot: all 5 existing
   labels are in use.
2. **Data edits (allowed):** `selection_count` `0x05`→`0x06`
   (`mnmain.c:384`); a 6th entry in `mn_803EB3FC[]` (`mnmain.c:294-298`); a 6th id
   in `mn_803EB660[]` (`mnmain.c:356`, plus a "Tournament" string in the
   `SdMenu.dat`/`.usd` SIS bank — another small asset edit if a description line
   is wanted).
3. **Dispatch:** map the new selection to the tournament screen. This can **avoid
   a matched-body edit**: the `MENU_KIND_MAIN` think is already our wrapper
   `mnTourney_MainMenuThink`. Let it own the 6th selection — on `Confirm` while
   `hovered_selection == 5`, spawn `mnTourney_Think` itself (as its Z shortcut
   already does, `mntourney.c:495-505`) and **do not** delegate to `mn_8022DB10`
   for that value. Only delegate selections 0-4 and cursor movement. This is
   essential because `mn_8022DB10`'s confirm `switch` (`mnmain.c:2690-2711`) has
   no `case 5` and would leave `menu_kind` uninitialized.

### Recommendation

- If a **visible native row** is a hard requirement: it costs one **`MnMaAll`
  asset edit** (6th joint + "Tournament" label matanim) plus the data edits above;
  dispatch stays matched-safe via the `mnTourney_MainMenuThink` wrapper. Flag the
  asset edit as unavoidable.
- **Least-invasive alternative that needs no asset edit:** keep 5 visible rows and
  drive tournament entry from our wrapper without a new drawn row — e.g. the
  existing Z shortcut, or repurpose the dispatch of an existing slot in the
  wrapper. This fully avoids assets and matched edits but does **not** satisfy the
  "visible option in the row" ask. Choose per how important the drawn label is
  versus avoiding the archive edit.

---

## Problem 3 — How boot-entry and user-back-out coexist

### The current conflict

Today `force_main_menu` is the *only* signal, and it is overloaded: it is set at
boot-equivalent re-entries and consumed by `onEnter` (`gmmenumode.c:95-100`) to
land on `MENU_KIND_MAIN`, after which `mnTourney_MainMenuThink` **always**
auto-enters the tournament screen once `cooldown == 0` (`mntourney.c:495`).
`force_main_menu` is raised from:

- CSS `END_SET` accepted — `lbtourney.c:247-249`
- CSS B-back kept-armed — `lbtourney.c:310` (`lbTourney_CSSFrame`)
- LR+Start reset-to-menu — `mn_8022F268` `mn_22EC.c:221-224` (from
  `mnMain_Scene_OnFrame` `mnmain.c:2746`)

Because the auto-enter is unconditional, pressing **B on the tournament screen**
(`exitToMainMenu`, `mntourney.c:283-289`, which calls
`mn_80229894(MENU_KIND_MAIN,0,3)` — a *within-scene* transition, no `GM_MENU`
re-enter) drops onto `MENU_KIND_MAIN`, whose think immediately re-enters
tournament: the B-back loops. That is the reported behaviour.

### Key distinction already present in the control flow

- **Auto-enter-desired** arrivals (boot, `END_SET`, CSS-back, LR+Start) all reach
  `MENU_KIND_MAIN` through a **fresh `GM_MENU` scene entry** (`onEnter` runs).
- **User-back-out** (`exitToMainMenu`) reaches `MENU_KIND_MAIN` **within the same
  GS_MENU scene** via `mn_80229894` — `onEnter` does *not* run.

So one explicit boolean cleanly separates them.

### Recommendation — an "auto-enter armed" flag in our TU

Add `static bool tm_auto_enter;` to `mntourney.c` (it is tournament-menu UI
state, alongside `tm_state` et al., `mntourney.c:54-64`). Semantics:

- **Arm** on every auto-enter-desired arrival:
  - boot: initialise it `true` (the kiosk always boots fresh into `GM_MENU`), or
    set it from the boot flag in Problem 1/2(a);
  - `END_SET` accept: set it where `force_main_menu` is raised
    (`lbtourney.c:247`);
  - CSS B-back: alongside `lbtourney.c:310`;
  - optionally LR+Start (`mn_22EC.c:221`) if that should also return to the
    tournament screen.
  Expose a tiny setter (`mnTourney_ArmAutoEnter()` in `mntourney.h`) so
  `lbtourney.c` sets it without touching menu internals.
- **Consume + disarm** in `mnTourney_MainMenuThink` (`mntourney.c:487-508`): only
  run the auto-enter block when `tm_auto_enter` is set, and clear it there. When
  it is clear, fall through to vanilla `mn_8022DB10` — the main menu simply stays
  up (with the 2(b) "Tournament" option available to re-enter).
- **Leave it clear** in `exitToMainMenu` (`mntourney.c:283-289`): a user B-back
  does not arm, so the main menu stays put. (`exitToMainMenu` is within-scene, so
  `onEnter` never re-arms it either.)

Resulting flow:

- **Boot / END_SET / CSS-back** → `GM_MENU` `onEnter` → `MENU_KIND_MAIN` →
  `mnTourney_MainMenuThink` sees armed → seamless tournament (Problem 2(a) makes
  it frame-clean).
- **B on tournament screen** → `exitToMainMenu` → `MENU_KIND_MAIN`, unarmed →
  stays on the real main menu.
- **"Tournament" main-menu option** (2(b)) → wrapper spawns `mnTourney_Think`
  directly; no arming needed.

The flag lives in `mntourney.c` because that TU owns both the auto-enter site and
the back-out site; `lbtourney.c` only needs the one-line setter. No matched-body
edits are required for the coexistence logic itself.

---

## Summary of required edits by type

| Ask | Data-table / our-TU (allowed) | Matched-body edit | Asset edit |
|---|---|---|---|
| 1. Skip memcard prompt (option A) | new `GS_` kind + `scenes[]` row + tiny our-TU `on_frame`; repoint `gm_Mode_Boot_States[0].info.kind` (`gmboot.c:40`) | none | none |
| 1. Skip memcard prompt (option B) | optional kiosk flag from `bootOnLoad` | **1 line** `gmscmemcard.c:298-303` | none |
| 2(a). No blip | force `cooldown=0` + hide panel in `mnTourney_MainMenuThink` | none (our-TU path) | none |
| 2(a). Zero-frame variant | — | `mnMain_Scene_OnEnter` default case `mnmain.c:2970-2977` | none |
| 2(b). Visible "Tournament" row | `selection_count`, `mn_803EB3FC`, `mn_803EB660`; dispatch in wrapper | none (wrapper owns dispatch) | **`MnMaAll`** joint + label matanim (+ `SdMenu` string) |
| 3. Coexistence flag | `tm_auto_enter` in `mntourney.c` + setter; arm at `lbtourney.c:247`,`:310` | none | none |

Only two edits are genuinely unavoidable and heavier than data/our-TU work, and
both are optional depending on product choices: the single matched line for
Problem 1 option B (avoidable via option A), and the `MnMaAll` asset edit for the
visible main-menu row in 2(b) (avoidable only by dropping the "visible row"
requirement).
