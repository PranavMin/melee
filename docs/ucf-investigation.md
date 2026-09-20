# UCF (Universal Controller Fix) — read-only investigation

Scope: answers design.md **R12**'s open item — porting UCF into the melee decomp
build *from source* so its addresses resolve at link time (instead of Slippi
Nintendont applying vanilla-1.02 address patches that crash our shifted DOL).
Investigation only; **no code was changed.** Paths in this repo are relative to
its root; Nintendont paths are under `P:\Projects\Nintendont`. Every line number
was verified against branch `reporter` in each repo.

Reference for the reproduction target is Slippi Nintendont's UCF codeset
(`common/config/MeleeCodes.c`, `kernel/gecko/g_ucf.bin`, applied by
`kernel/Patch.c`). Those addresses are vanilla GALE01 v1.02 and useless to us
directly, but the **logic and hook locations translate to decomp symbols**, which
is what this document maps.

**Governing constraint (CLAUDE.md):** *never modify a matched function.* New logic
goes in a new TU wired via data-table/wrapper edits — exactly like the existing
tournament code (`GS_CSS` row swap in gmscdata.c, menu-think swap in mnmain.c).
§4 holds every recommendation to that and flags each place the "obvious" edit
would touch a matched body.

---

## Q1 — What UCF actually does

UCF 0.8 is the standard tournament version. Nintendont's own menu text
(`common/config/MeleeCodes.c:79-97`) describes it as: *"Will enable UCF 0.8.
Includes dashback, shield drop, and wiggle out of tumble."* So the task's "two
parts" is slightly incomplete — the shipped 0.8 codeset is **three functional
fixes plus one cosmetic**. The compiled blob is `kernel/gecko/g_ucf.bin`
(1096 bytes), four Gecko **C2** ("insert PowerPC asm at this address") codes.
Decoding the C2 headers and resolving each vanilla-1.02 address against this
repo's `config/GALE01/symbols.txt`:

| C2 hook (vanilla) | lines | Decomp function it lands in | Purpose |
|---|---|---|---|
| `C2 80 0C 9A 44` | 0x2B | `ftCo_Turn_IASA` +0x4C (fn @0x800C99F8, src/melee/ft/kinds/ftCommon/ftCo_Turn.c:97) | **Dashback** |
| `C2 80 09 98 A4` | 0x26 | `ftCo_80099894` +0x10 (fn @0x80099894, src/melee/ft/kinds/ftCommon/ftCo_Escape.c:228) | **Shield-drop** |
| `C2 80 09 08 F4` | 0x17 | `ftCo_DamageFall_IASA` +0xCC (fn @0x80090828, src/melee/ft/kinds/ftCommon/ftCo_DamageFall.c:113) | **Wiggle out of tumble** |
| `C2 80 26 62 D0` | 0x1A | `mnCharSel_802640A0` +0x2230 (src/melee/mn/mncharsel.c) | Draw "UCF 0.8" text on CSS (cosmetic) |

(The blob ends with the ASCII string `UCF 0.8` at file offset 0x410, consumed by
the CSS-text code. UCF 0.84, `g_ucf_084.bin`, additionally hooks
`Fighter_Spaghetti_8006AD10` +0x750, `ftCo_SquatRv_CheckInput`, and `ftCo_80093240`
— out of scope; the venue runs 0.8.)

**What each fix modifies.** All three functional fixes touch the *same* piece of
fighter state — the per-axis "frames since the analog stick last crossed its
smash threshold" counter — and the answer is the shape they all share:

```
(stick coordinate past a cardinal threshold) AND (active_timer.lstick.<axis> < a small window)
```

- **Dashback** (`ftCo_Turn.c:160-171`, `fn_800C9C2C`, called from `ftCo_Turn_IASA`
  at ftCo_Turn.c:128-135):
  `fp->input.lstick[0].x * facing_after >= p_ftCommonData->dash_smash_stick_threshold`
  **and** `fp->active_timer.lstick.x < p_ftCommonData->dash_smash_window`.
  Passing → `ftCo_Dash_Enter` (a dash); failing → the fighter stays in Turn (a
  slow tilt-turn / "failed dashback"). Thresholds: `dash_smash_stick_threshold`
  (ft/types.h:134, +0x3C) and `dash_smash_window` (ft/types.h:140, +0x40).
- **Shield-drop** (`ftCo_Escape.c:198-206`, `inlineB0`, reached from the Guard
  state — see Q2): `fp->input.lstick[0].y <= p_ftCommonData->x314` **and**
  `fp->active_timer.lstick.y < p_ftCommonData->x318`. Passing → `ftCo_80099894`
  → `EscapeN`/platform drop-through. Thresholds: `x314` (Y coord, ft/types.h:359)
  and `x318` (Y window, ft/types.h:360).
- **Wiggle out of tumble** (`ftCo_DamageFall.c:124-129`):
  `ABS(fp->input.lstick[0].x) >= p_ftCommonData->x210` **and**
  `fp->active_timer.lstick.x < p_ftCommonData->x214`. Passing → `ftCo_Fall_Enter`
  (regain control out of tumble). Thresholds: `x210`/`x214` (ft/types.h:288-289).

The vanilla failure mode all three share is explained by how `active_timer.lstick`
is computed (Q2): the timer only resets to 0 (i.e. registers a *fresh* crossing)
when the **previous** frame's stick value was *not* past the deadzone. On worn or
fast-moving sticks the crossing frame is mis-sampled, the timer never lands inside
the window, and the input is dropped. **UCF does not change the threshold
constants** — it inserts asm that re-derives the crossing from the raw analog
value (it reads both `fp->input.lstick` at fp+0x620 and the raw pad copy at
`HSD_PadLibData`/master-status 0x804C1F78) and stamps the fighter's active-input
state so the downstream dash / drop / tumble decision fires when it should. That
is why it is delivered as inserted code, not a value poke.

---

## Q2 — Where the affected logic lives in the decomp

**Controller / analog layer** — `src/sysdolphin/baselib/controller.c`:
- `HSD_PadRenewMasterStatus` (controller.c:346) is the per-poll pipeline:
  `HSD_PadClamp` (194) → `HSD_PadADConvert` (274) → `HSD_PadScale` (290) →
  `HSD_PadCrossDir` (304). `HSD_PadScale` (controller.c:294-295) produces the
  normalized floats `nml_stickX/Y = stickX / scale_stick` that the fighter layer
  copies into `fp->input.lstick`. Raw `stickX/stickY` (s8, ±80-ish) live in
  `HSD_PadMasterStatus[]` (controller.c:15, `.bss:0x804C1FAC`); `HSD_PadLibData`
  is `.bss:0x804C1F78` (the base UCF's dashback asm loads). These are the decomp
  equivalents of the raw values UCF re-reads.

**Fighter input struct** — `src/melee/ft/types.h`:
- `input` block at **fp+0x620** (ft/types.h:1408-1416): `Vec2 lstick[3]`
  (3-frame history; `lstick[0]` = current, `lstick[1]` = previous), `cstick[3]`,
  `triggers[3]`, `held_buttons[3]`, `pressed_buttons`, `released_buttons`.
- **`active_timer`** at **fp+0x670** (`FtInputTimers`, ft/types.h:1421). Its
  doc-comment is exactly the UCF subject: *"Frames since each analog input last
  crossed its threshold: the stick smash deadzones."* Siblings `active_sticky`
  (+0x673), `active_duration` (+0x676), `activity_timer` (+0x679).
  `FtInputTimers` is defined at ft/types.h:1298-1301.

**Where `active_timer.lstick` is computed** — `src/melee/ft/fighter.c`, inside
`Fighter_Spaghetti_8006AD10` (fighter.c:1781, the per-fighter input proc):
- fighter.c:1914-1968 (X axis) and 1976+ (Y axis). Per frame: if `lstick[0].x`
  and `lstick[1].x` are **both** past `horizontal_stick_smash_deadzone`
  (ft/types.h:96) → `active_timer.lstick.x++` (capped 254); if `lstick[0]` is past
  but `lstick[1]` is not (**the fresh-crossing frame**) → `active_timer.lstick.x = 0`
  (fighter.c:1936, 1961); if neutral → `= 254` (fighter.c:1967). Y axis uses
  `vertical_stick_smash_deadzone` (ft/types.h:99). This previous-frame-gated reset
  is the exact vanilla logic UCF compensates for.

**The three fighter-state consumers UCF hooks:**
- **Dashback** — `ftCo_Turn.c:97` `ftCo_Turn_IASA`, decision in `fn_800C9C2C`
  (ftCo_Turn.c:160-171); enters `ftCo_Dash_Enter` (ftCo_Turn.c:133).
- **Shield-drop** — predicate `inlineB0` (ftCo_Escape.c:198-206); reached via
  `ftCo_80099794` (ftCo_Escape.c:208) / `ftCo_8009980C` (ftCo_Escape.c:218), which
  are called from the **Guard (shield) state IASA** (`ftCo_Guard.c:473,543`) and
  also from Wait (`ftCo_Wait.c`) and AppealS (`ftCo_AppealS.c:130`). The shared
  entry `ftCo_80099894` (ftCo_Escape.c:228) is where UCF's single hook sits — the
  choke point through every shield/platform-drop path.
- **Tumble wiggle** — `ftCo_DamageFall_IASA` (ftCo_DamageFall.c:113-133).

**Dispatch of these IASAs** — the fighter runs a per-frame GObj proc pipeline set
up in `Fighter_*` init at fighter.c:901-909:
`Fighter_8006A1BC`(prio 0) … **`Fighter_Spaghetti_8006AD10`(prio 3, input+timers)**
→ **`Fighter_procUpdate`(prio 4, runs the motion-state IASA/anim/phys/coll
callbacks)** → … Each motion state's callbacks come from the source data table
`MotionState ftData_MotionStateList[ftCo_MS_Count]` (src/melee/ft/ftmotionstates.c:133);
e.g. `ftCo_MS_Turn` at ftmotionstates.c:333-342 lists `ftCo_Turn_IASA` as its IASA
(ftmotionstates.c:338), and `ftCo_MS_DamageFall` lists `ftCo_DamageFall_IASA`
(ftmotionstates.c:558). This table is the key lever for Q4.

---

## Q3 — How Slippi Nintendont applies UCF (reference)

- **Selection.** `common/config/MeleeCodes.c:41-46` binds the "UCF" option to the
  blob `g_ucf` / `g_ucf_size` (from `kernel/gecko/g_ucf.h`, generated from
  `g_ucf.bin`). `cfLineItem` (MeleeCodes.c:99-106) defaults value `2` = UCF on.
  The venue's set is this "Controller Fix = UCF" plus the Convenience/Tournament,
  stage, safety options in the same file.
- **Format.** The blob is raw Gecko codes. UCF's four entries are all **C2**
  (insert-asm-at-address): `C2 aaaaaa NN` header (address low 3 bytes, `NN` = line
  count) followed by `NN` 8-byte lines of PowerPC that the Gecko codehandler
  branches to from `0x8000_0000|address`. That is why the addresses are baked in
  and vanilla-only.
- **Application.** `kernel/Patch.c:3260-3418`: when `MeleeVersion` is set, Patch
  installs the Gecko **codehandler** (Patch.c:3279-3336) and then, for each
  configured line item, `memcpy`s the selected option's `code`/`codeLen` into the
  GCT region and `sync_after_write`s it (Patch.c:3394-3418). The codehandler
  applies the C2 hooks at boot. No per-code address fix-up happens — the addresses
  are trusted as-is.
- **Version gating (the R12 crash mechanism).** `GetMeleeVersion` (Patch.c:145-169)
  keys **only on the disc header**: `GAME_ID` (0x0) and the version byte at
  `0x00000007`. `GALE`/`GALJ` with `gameVers == 2` → `MELEE_VERSION_NTSC_2`
  (Patch.c:158-160). Our patched ISO keeps a vanilla GALE01 v1.02 header (required
  to boot as Melee), so on hardware Nintendont detects NTSC 1.02 and applies the
  **vanilla-addressed** UCF C2 hooks to our **shifted decomp DOL** → the hooks
  branch into wrong code → boot crash. Confirms design.md R12 exactly. This is why
  UCF must be built into the DOL from source (link-time addresses) and *not* left
  to Nintendont for our build.

**Reproduction takeaway:** we reproduce UCF's *logic* (raw-stick re-derivation of
the smash crossing → stamp `active_timer.lstick`) at the *same three decomp
symbols* the C2 hooks land in (Q1 table). We do **not** carry the CSS-text code
(cosmetic; if wanted it is a separate SIS-text overlay like the tournament menu,
not part of the fix).

---

## Q4 — Cleanest matched-safe integration path

The logic must run **after** the input timers are computed
(`Fighter_Spaghetti_8006AD10`, prio 3) and **before** the state IASA consumes them
(`Fighter_procUpdate`, prio 4). Two matched-safe shapes exist; a hybrid is
recommended.

### Shape B (recommended) — wrap the IASA pointers in `ftData_MotionStateList`

`ftData_MotionStateList` (ftmotionstates.c:133) is a **source-level data table**,
the same class of object the project already edits (`mn_803EB6B0` menu table,
`GS_CSS` scene row). Swapping an IASA function pointer there is a data-table edit,
not a function-body edit. New TU `src/melee/ft/ucf.c` provides thin wrappers that
run the UCF re-derivation, then tail-call the vanilla IASA (which we only *call*):

- **Dashback:** ftmotionstates.c:338 `ftCo_Turn_IASA` → `ucf_Turn_IASA`. One entry.
- **Tumble:** ftmotionstates.c:558 `ftCo_DamageFall_IASA` → `ucf_DamageFall_IASA`.
  One entry.
- **Shield-drop:** the check is reached from **several** states (Guard
  ftCo_Guard.c:473/543, Wait, AppealS ftCo_AppealS.c:130), so faithfully covering
  it means wrapping each of those states' IASA entries in the table (their IASAs
  call `ftCo_8009980C`). The wrapper widens the drop decision — practically, by
  correcting `active_timer.lstick.y` (below) *before* calling the vanilla IASA, so
  the inlined `inlineB0` predicate passes. This is more surface than dashback/
  tumble and is the main correctness watch-item.

Each wrapper's body corrects the relevant `fp->active_timer.lstick.<axis>` (and,
if needed, nudges `fp->input.lstick[0]`) from the raw/normalized stick, then calls
the original IASA. Because the correction targets the shared timer state, the
vanilla decision code runs unchanged and simply sees the corrected input — the
closest structural match to what the C2 asm does. **Zero matched-function edits.**

Entry points called from: the `ftData_MotionStateList` rows (data), for
`ucf_Turn_IASA`, `ucf_DamageFall_IASA`, and the guard/wait/appeal IASA wrappers.

### Shape A (cleanest logically, but needs an install lever) — one corrector proc

Since all three fixes read the *same* `active_timer.lstick`, a single function
`ucf_CorrectInputs(Fighter_GObj*)` that recomputes both axes' crossings from the
raw stick and overwrites `active_timer.lstick.x/y` once per fighter per frame
reproduces **all three at one point** — no per-state wrapping, no shield-drop
enumeration risk. The catch is *installing* it between prio 3 and prio 4:

- The proc registrations at fighter.c:901-909 are in a **matched function**, so
  adding `HSD_GObj_SetupProc(gobj, &ucf_CorrectInputs, 3)` there is a **matched
  edit** — flagged, not allowed under CLAUDE.md without a documented exception.
- A matched-safe alternative is a single global GObj proc installed once at
  match-scene enter (via a scene-table wrap like the existing `GS_CSS` row) that
  iterates the fighter GObj list each frame. Its ordering relative to the
  per-fighter prio-3/4 procs must be proven correct (gobj-class priority), which
  is the risk that makes this less clean than Shape B despite the smaller code.

### Recommendation

Ship **Shape B** for **dashback and tumble** — each is a one-pointer data-table
swap of a single common motion state, trivially matched-safe and faithful. For
**shield-drop**, prefer factoring the timer correction into a shared
`ucf_CorrectInputs` helper and calling it from the guard/wait/appeal IASA
wrappers (still data-table swaps, no matched edits), rather than taking the
matched exception at fighter.c:904 for Shape A. Only fall back to a documented
one-line matched exception (register `ucf_CorrectInputs` at prio 3) if wrapping
the shield-capable states proves to miss a path.

**Matched-safe flags:**
- `ftData_MotionStateList` — **data**, editable (swap IASA pointers). ✅
- `ftCo_Turn_IASA` / `ftCo_DamageFall_IASA` / guard IASAs / `fn_800C9C2C` /
  `inlineB0` / `Fighter_Spaghetti_8006AD10` / the `HSD_Pad*` pipeline — **matched;
  only called, never edited.** ✅
- fighter.c:901-909 proc registration — **matched**; Shape A's direct install
  would edit it. ⚠ Avoid; use Shape B.

**Caveat — per-character overrides.** `ftData_MotionStateList` is the *common*
list; a character whose own motion-state table overrides Turn/DamageFall/Guard
would bypass the wrap. Turn, DamageFall and Guard are standard common states used
by all of the cast, so risk is low, but this must be verified per character before
trusting the port (build-time check: confirm no `Ft_Kind_*` MotionState table
re-points these IASAs).

---

## Q5 — Scope estimate

**Medium.** Not large: the fix is three small, well-localized decision points, all
reading one shared piece of state, and the hook mechanism (data-table IASA swap)
already exists in this tree. Not small: it is *gameplay-critical* asm-faithful
work where "close" is a bug, and it touches the input path every frame.

Main risks:
1. **Getting the dashback crossing logic exactly right (highest risk).** The value
   of UCF is precisely that it registers dashbacks vanilla drops; a re-derivation
   that is subtly off is worse than no UCF for competitive play (design's own
   note). The source of truth is `g_ucf.bin`'s asm at the four addresses — it
   should be disassembled and reproduced behaviorally, then diffed against real
   UCF, not re-derived from a mental model of "widen the window."
2. **Shield-drop's multiple entry states.** Enumerating every state that can reach
   `ftCo_80099894` (Guard, Wait, AppealS, and any character-specific ones) and
   wrapping all of them — miss one and shield-drop is inconsistent.
3. **Matching / hook fidelity.** Shape B keeps us in the sanctioned data-table
   pattern; Shape A's cleaner single-point form costs a matched edit or a
   proc-ordering proof. Pick B unless a path forces otherwise.
4. **Testability in Dolphin.** Verifiable in Slippi Dolphin frame-by-frame
   (per design §9.3) — feed known stick trajectories, compare dash-vs-tilt and
   drop-vs-dodge outcomes against a UCF-on reference build. This is doable without
   a Wii, which de-risks iteration, but a rigorous test needs recorded-input
   playback to hit the exact borderline analog values UCF targets.
5. **`p_ftCommonData` values are data-file, not source.** The thresholds
   (`dash_smash_*`, `x314/x318`, `x210/x214`, deadzones) live in loaded ft data,
   so the port must read them via `p_ftCommonData`, not hardcode — and must not
   assume UCF changes them (it does not).

---

## Summary

- **What UCF 0.8 changes:** three functional fixes — **dashback**
  (`ftCo_Turn.c` `fn_800C9C2C`/`ftCo_Turn_IASA`), **shield-drop**
  (`ftCo_Escape.c` `inlineB0`, reached from the Guard IASA) and **wiggle-out-of-
  tumble** (`ftCo_DamageFall_IASA`) — plus a cosmetic CSS "UCF 0.8" label. All
  three read one shared state: `fp->active_timer.lstick.{x,y}` (fp+0x670, "frames
  since the stick crossed the smash deadzone", computed in
  `Fighter_Spaghetti_8006AD10`, fighter.c:1914-1968) against `p_ftCommonData`
  thresholds. UCF inserts asm that re-derives the crossing from the raw analog
  stick (`HSD_PadLibData` @0x804C1F78) so legitimate inputs vanilla's
  previous-frame deadzone check drops are honored.
- **Decomp hook points:** the IASA callbacks in the source data table
  `ftData_MotionStateList` (ftmotionstates.c:133) — `ftCo_Turn_IASA` (row
  ftmotionstates.c:338), `ftCo_DamageFall_IASA` (row ftmotionstates.c:558), and the
  Guard/Wait/AppealS IASAs for shield-drop.
- **Recommended shape:** a new TU `src/melee/ft/ucf.c` holding thin IASA wrappers
  that correct `active_timer.lstick` from the raw stick and tail-call the vanilla
  IASA, wired by **swapping the IASA pointers in `ftData_MotionStateList`** — a
  data-table edit, no matched-function bodies touched, matching the existing
  `GS_CSS`/`mn_803EB6B0` pattern. Dashback and tumble are one-pointer swaps each;
  shield-drop shares a `ucf_CorrectInputs` helper across the shield-capable states.
  A single-proc variant is logically cleaner but needs a matched edit at
  fighter.c:904 (flagged, avoid).
- **Scope:** medium. Correctness of the dashback re-derivation and full coverage
  of the shield-drop entry states are the dominant risks; disassemble `g_ucf.bin`
  and diff behavior against a real UCF build rather than re-deriving. Testable in
  Slippi Dolphin without a Wii.
