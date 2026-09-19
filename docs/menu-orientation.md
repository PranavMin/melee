# Menu orientation — read-only investigation (runbook session 5)

Scope: answers to the five orientation questions for the Tournament menu work
(`../tournament-reporter/docs/design.md` §6.1). Investigation only; no code was
written. All paths are relative to this repo's root; all line numbers were
verified against branch `reporter` (one commit ahead of upstream
`doldecomp/melee`: it adds only `CLAUDE.md` and `include/relay_proto.h`).

**Headline finding that affects the design:** this tree contains **no Slippi
code at all** (case-insensitive grep for `slippi` returns zero matches). §6.1's
"wraps the existing Slippi EXI device access" has nothing to wrap here —
Slippi's game-side EXI traffic lives in its injected ASM/gecko layer, not in
the decomp. `lbrelayexi.c` will be a small *new* EXI device driver written
against the vanilla Dolphin SDK EXI API (fully decompiled in this repo), and
the device itself must be implemented on the other side (session 7's Dolphin
EXI forwarder, session 8's Nintendont). See §2 below.

---

## 1. How a menu screen is registered, drawn, and given input

### The two-level scene system

- `struct GameMode` ("major scene") — src/melee/gm/types.h:109. Holds `kind`,
  `on_load`/`on_unload`/`on_init`, and a pointer to its `GameModeState[]`.
- `struct GameModeState` — src/melee/gm/types.h:90. One state of a mode:
  `{ id, preload, flags, on_enter, on_exit, GameSceneInfo info }`; the array is
  terminated by `{ -1 }`.
- `struct GameScene` ("minor scene") — src/melee/gm/types.h:121. The reusable
  handler: `{ kind, on_frame, on_enter, on_exit }` (`ASSERT_SIZE 0x14`,
  types.h:131).
- Enums: `GameModeKind` src/melee/gm/forward.h:19 (`GM_MENU`=1 :21,
  `GM_DEBUG_SOUND_TEST`=7 :27, `GM_TOURNAMENT`=0x1B :47); `GameSceneKind`
  src/melee/gm/forward.h:88 (`GS_MENU` :90, `GS_DEBUG_MENU` :96, `GS_CSS` :97).

### Registration: two global tables

- Minor-scene table `static GameScene scenes[]` — src/melee/gm/gmscdata.c:65,
  terminated by the `GS_COUNT` sentinel at gmscdata.c:374. Examples:
  `GS_MENU` row at :73–79, `GS_DEBUG_MENU` (with `on_frame = NULL`) at
  :108–114, `GS_CSS` at :115–121.
- Major-scene table `static GameMode modes[]` — src/melee/gm/gmscdata.c:383,
  sentinel at :744. Sound-test mode row at :432–439:
  `{ false, GM_DEBUG_SOUND_TEST, NULL, NULL, NULL, gm_Mode_DebugMenu_States }`.
- Accessors `gm_GetAllGameScenes` / `gm_GetAllGameModes` —
  src/melee/gm/gmscdata.c:754 / :759.
- Per-mode state arrays: `gm_Mode_DebugMenu_States[]` and
  `gm_Mode_Menu_States[]` — src/melee/gm/gmmenumode.c:29 / :45 (verified).

### The driver loop

`main()` → `gm_801A4510()` (src/melee/gm/gmmain.c:219 →
src/melee/gm/gm_1A3F.c:366) → `runGameMode` (gm_1A3F.c:324) →
`gm_801A4014(mode)` (gm_1A3F.c:156), which per state:

1. `state->on_enter(state)` (gm_1A3F.c:171–173)
2. `gm_FindGameSceneHandler(kind)` (gm_1A3F.c:182; linear scan at
   src/melee/gm/gmscene.c:250)
3. `scene->on_enter(info->enter_data)` (gm_1A3F.c:186–188)
4. `gm_801A4D34(scene->on_frame, info)` — the blocking per-frame loop
   (gm_1A3F.c:189; body at src/melee/gm/gmscene.c:271). Order inside the loop:
   `gm_EvaluateAllControllerInputs()` (gmscene.c:315–317), then `on_frame()`
   (gmscene.c:318–320), then `HSD_GObj_RunProcs()` (gmscene.c:347) — inputs are
   latched first, scene tick second, all GObj think procs last.
5. `scene->on_exit(info->exit_data)` then `state->on_exit(state)`
   (gm_1A3F.c:190–196).

A menu leaves by writing `exit_data->pending_mode` and calling `gm_801A4B60()`
(src/melee/gm/gmscene.c:177 — just sets the "break the frame loop" flag); the
state's `on_exit` (`onExit`, src/melee/gm/gmmenumode.c:243) then calls
`gm_SetPendingGameMode` / `gm_SetNewGameModePending`. Also
`gm_ChangeGameModeAfterCurrentScene` (gm_1A3F.c:271) and
`gm_SetNextGameModeStateId` (gm_1A3F.c:246) for picking the next state within
a mode.

### Worked example (a): the in-game Sound Test

The Data → Sound Test screen is *not* a scene; it's a submenu (`MenuKind`
`0x1B`) inside `GS_MENU`:

- Submenu registry: `MenuKindData mn_803EB6B0[0x22]` — src/melee/mn/mnmain.c:378;
  `struct MenuKindData` (anim, description SIS ids, `selection_count`, `think`)
  at src/melee/mn/types.h:570. `MenuKind` enum src/melee/mn/forward.h:84–120.
- Entered from the Data menu think: `case SEL_DATA_SOUND:` →
  `mnSoundTest_8024BEE0(1)` — src/melee/mn/mnmain.c:2237–2240.
- `mnSoundTest_8024BEE0` — src/melee/mn/mnsoundtest.c:853 (verified): sets
  `mn_804A04F0.cur_menu = 0x1B` (:861), loads model sections (:868–872), builds
  the screen GObj (:873), then spawns the think proc (:874):
  `HSD_GObj_SetupProc(GObj_Create(0,1,0x80), fn_8024B2B0, 0)`.
- Think/input: `fn_8024B2B0` — mnsoundtest.c:521. Reads
  `mnSoundTest_GetInputs()` (:549, = `mn_80229624(4)`), handles Back (:550–557),
  Up/Down view toggle (:558–561), Left/Right cursor with wraparound (:605–618),
  A to play (:584–600), and swaps itself for the submenu think `fn_8024AED0`
  (:620–626; fn at :366).
- Menu-to-menu transition helper `mn_80229894` — src/melee/mn/mnmain.c:691:
  set cooldown, swap `cur_menu`, rebuild panel via `mn_8022B3A0`, free old
  think GObj, spawn `mn_803EB6B0[new].think`.

### Worked example (b): the debug menu (DevelopText)

`GM_DEBUG_SOUND_TEST` uses scene `GS_DEBUG_MENU` (`on_frame = NULL` — all logic
is GObj procs). `onEnterDebug` (src/melee/gm/gmmenumode.c:61) points enter-data
at the row table `un_803F9FA4` (src/melee/if/soundtest.c:2236). The menu GObj is
built by `un_80304168` (src/melee/if/textlib_1.c:593–605); its per-frame proc
`fn_80303EF4` (textlib_1.c:484) redraws and dispatches input; raw pad read at
textlib_1.c:379 (`HSD_PadCopyStatus[0].trigger`) with stick→d-pad emulation in
`un_803039A4` (textlib_1.c:342). Text via `DevText_Create/Show/Print`
(src/melee/if/textlib.h:12–30, `struct DevText` src/melee/if/types.h:138).

### Drawing (JObj models + SIS text)

Canonical build routine `mnSoundTest_8024BCA0` — src/melee/mn/mnsoundtest.c:788:
`GObj_Create` (:804) → `HSD_JObjLoadJoint` (:806) → `HSD_GObjObject_80390A70`
(:807) → `GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x80)` (:808, the
render hookup) → anims (:809–812) → `HSD_GObj_SetupProc` (:813) →
`GObj_InitUserData` (:832) → SIS text box (:837–842). Text API in
src/sysdolphin/baselib/sislib.h: create :142, set string :138, printf-style
:134/:132, destroy :145, load bank :139 (`SdMenu.dat` loaded at
src/melee/mn/mnmain.c:2928–2932).

### Input layers

1. Raw HSD pad: `struct HSD_PadStatus` src/sysdolphin/baselib/controller.h:49;
   `extern HSD_PadStatus HSD_PadCopyStatus[4]` controller.h:109.
2. Combined map: `gm_EvaluateAllControllerInputs` — src/melee/gm/gm_1A36.c:106;
   synthesizes `PAD_CONFIRM`/`PAD_CANCEL`/`PAD_LR_START`/`PAD_ANY_*`
   (:118–128); slot 4 = OR of all ports (:131–140). Accessors
   `gm_GetButtonsTriggered` (:13) and repeat-stream `gm_801A36C0` (:18).
3. Menu bitfield: `mn_80229624` — src/melee/mn/mnmain.c:632 (verified):
   directions from the repeat stream (hold-to-scroll), confirm/cancel from
   triggers, gated by `mn_804D6BC8.cooldown`. Flags `MenuInput_*`
   src/melee/mn/inlines.h:12–25; wrappers `Menu_GetAllInputs` inlines.h:38–47.

### Recipe for the Tournament menu

Note there is already a full (Melee-native) tournament major scene to crib
from: `GM_TOURNAMENT` row gmscdata.c:600–607, `gm_Mode_Tournament_States[]`
src/melee/gm/gmtoumode.c:31, entered from the VS menu at
src/melee/mn/mnmain.c:2483–2488, with a real `on_frame`
(`gm_Scene_TouSetup_OnFrame`, src/melee/gm/gmtou_0.c:2849) that shows the
exit-to-menu pattern (:2860–2870).

Two implementation routes:

- **New submenu inside `GS_MENU`** (cheapest, matches the §6.1 "Main menu →
  Tournament" flow): add a `MenuKind` (forward.h:84), a row in
  `mn_803EB6B0[]` (mnmain.c:378 — bump the `[0x22]` bound), a think function
  modeled on `mn_8022CE6C` (mnmain.c:2211), and reach it from the VS or main
  menu think via `mn_80229894(kind, 0, 1)`. Screen model per
  `mnSoundTest_8024BEE0`.
- **New minor scene**: add `GameSceneKind` before `GS_COUNT` (forward.h:139),
  a `scenes[]` row before gmscdata.c:374, a `GameModeState[]` in the new TU,
  and (if a new mode) a `modes[]` row before gmscdata.c:744. On-enter loads
  archives/SIS (pattern gmtou_0.c:2895–2905), on-frame reads inputs and drives
  the list, exit via `gm_801A4B60()`.

The submenu route is recommended for `mntourney.c`: no scene-table edits, and
the loading/error screens are just think-function states.

---

## 2. EXI to the Slippi device: current state and available API

**There is no Slippi EXI code in this tree.** Branch `reporter` = upstream +
one commit adding `CLAUDE.md` and `include/relay_proto.h` (198 lines of
generated structs, no transport). Nothing in `src/` even includes
`dolphin/exi.h`; game code reaches EXI only through the CARD library (e.g.
`CARDProbe(chan)` at src/melee/lb/lbsnap.c:475). So `lbrelayexi.c` is a new
driver built on the vanilla SDK EXI library, which is fully decompiled and
linked into the game (config/GALE01/splits.txt:5258).

### The API

Public header libs/dolphin/include/dolphin/exi.h:1–29; implementation
libs/dolphin/src/dolphin/os/OSExi.c:

| Function | Def | Constraints (verified) |
|---|---|---|
| `EXILock` / `EXIUnlock` | OSExi.c:613 / :650 | chan<3; chan 0 has dev<3, chans 1–2 dev==0 (:622); busy → queued via callback (depth 3, :20–23) |
| `EXISelect` / `EXIDeselect` | OSExi.c:440 / :481 | freq < 6 (0=1 MHz … 5=32 MHz); must hold the lock |
| `EXIImm` | OSExi.c:130 | **len 1–4 bytes** (`MAX_IMM`, asserts at :141), type read/write/rw (:142) |
| `EXIImmEx` | OSExi.c:168 | any length/alignment, synchronous ≤4-byte chunks |
| `EXIDma` | OSExi.c:186 | **buffer 32-byte aligned, len a nonzero multiple of 32** (asserts at :195–197), type read *or* write only (:198); cache coherency is the caller's job |
| `EXISync` | OSExi.c:223 | busy-poll completion |
| `EXIAttach`/`EXIDetach` | OSExi.c:396 / :418 | chan 0/1 only (:403, :424) — memcard slots; not needed for chan-2-style devices |
| `EXIGetID` | OSExi.c:691 | canonical ID handshake at :716–726 |

Registers, if ever needed raw: `__EXIRegs` at
libs/dolphin/include/dolphin/hw_regs.h:11 (0xCC006800).

### Patterns to copy

- **Best template: `HIORead`/`HIOWrite`** — libs/dolphin/src/dolphin/hio/hio.c:209–234
  (verified) and :236–261: lock → select → 4-byte `EXIImm` command word →
  `EXIDma` payload → sync → deselect → unlock. Structurally exactly the
  `EXI_RELAY_REQ`/`EXI_RELAY_POLL` shape. Async variants at :263–311.
- **Fixed 64-byte DMA read with cache maintenance: `ReadSram`** —
  libs/dolphin/src/dolphin/os/OSRtc.c:89–112 (`DCInvalidateRange` before the DMA
  read). `WriteSram` (OSRtc.c:124–145) shows `EXIImmEx` for a non-32-aligned
  payload — the right precedent for sending odd-sized structs like the 8-byte
  header + small requests without padding.
- **The game's own EXI user (CARD)**: command via `EXIImmEx(chan, cmd, 1–5, 1)`
  (libs/dolphin/src/dolphin/card/CARDBios.c:336), bulk via
  `EXIDma(chan, buf, 512|128, mode, __CARDTxHandler)` (CARDBios.c:357–358);
  cache ops by callers (CARDRead.c:165 invalidate, CARDWrite.c:117 store).

### Buffer-size implications for §6.1

The 4 KB response buffer should be `static u8 buf[4096] ATTRIBUTE_ALIGN(32);`
so responses can come back via a single `EXIDma` read (4096 is already a
multiple of 32). Requests (max `report_score_req`/`end_set_req`, 28 bytes +
8-byte header) either go out via `EXIImmEx` (no alignment rule, trivially
correct) or get staged into a 32-aligned, 32-padded buffer for DMA. `EXIDma`
has no read+write mode, so a round trip is imm-command + DMA inside one
select/deselect window, exactly as hio.c does it. Remember
`DCStoreRange` before DMA writes and `DCInvalidateRange` before DMA reads —
the SDK does not do it for you.

Note for sessions 7/8: since the decomp has no Slippi device, the EXI command
ids for `EXI_RELAY_REQ`/`EXI_RELAY_POLL` and the channel/device/frequency to
use are *ours to define* on the Dolphin (Ishiiruka) and Nintendont sides; the
game side just performs the transactions above. Slippi's own device
conventions live in those codebases, not here.

---

## 3. CSS: selected character ids and per-frame input

One file implements the CSS for every mode:
src/melee/mn/mncharsel.c (~5.5k lines). Scene wiring: the `GS_CSS` row at
src/melee/gm/gmscdata.c:115–121 (verified) —
`{ GS_CSS, mnCharSel_Scene_OnFrame, mnCharSel_Scene_OnEnter, mnCharSel_Scene_OnExit, NULL }`.
Game modes reuse it and supply their own `CSSData*` enter-data
(e.g. `gmVsMelee_EnterCss`, src/melee/gm/gmvsmelee.c:119–127).

### Where the selected character id lives (the authoritative copy)

`CSSData->vs.start.players[port].ckind`:

- Chain: `CSSData` src/melee/mn/types.h:307–313 → `VsModeData` :264–274 →
  `StartMeleeData` :259–262 → `PlayerInitData players[4]` :97–134.
- Field: `s8 ckind; ///< ::CharacterKind` — src/melee/mn/types.h:98 (verified).
  This is the **external** character id (`CharacterKind`,
  src/melee/ft/forward.h:130–169: `CKind_Captain`=0 … `CKind_Ganon`=0x19,
  `ChKind_None`=0x21) — which is what design §5.4's `p1_char`/`p2_char` and the
  relay's `chars.ts` table should use. The internal id (`FighterKind`,
  ft/forward.h:92–128) is not stored anywhere in the CSS.
- Live pointer: `static CSSData* mnCharSel_804D6CB0` — mncharsel.c:73; for VS
  it points at `gmVsMelee_CssData` (src/melee/gm/gmvsmelee.c:23).
- Main write (A-press on an icon, verified): mncharsel.c:2649–2655 —
  `players[player_idx].ckind = icons[doors[door].sel_icon].char_kind;`
  Deselect writes `ChKind_None` at :2574–2576; unplug auto-pick at :2037–2038.
- Companions on the same struct: `.color` (costume) written mncharsel.c:1560,
  `.team` :2359, `.slot_type` (`Gm_PKind`: 0 human, 1 CPU, 3 closed) :2455.

UI-level state (not char ids): `mnCharSel_803F0DFC.doors[door].sel_icon`
(`CSSDoor` src/melee/mn/types.h:419–448; `sel_icon` :437 is a **grid index**
0–0x18, 0x19 = none) → convert via `icons[sel_icon].char_kind`
(`CSSIcon` types.h:400–412; table at mncharsel.c:161). Door↔port mapping:
`getPlayerForDoor` mncharsel.c:2014–2023 (VS: door == port; 1P: door 0 → the
human port). SelKind↔CKind conversions: `gm_SelKindToCKind` /
`gm_CKindToSelKind` — src/melee/gm/gm_1601.c:2081 / :2086, tables in
gm_1601.static.h:72/:82.

**Caveat:** `Player_GetPlayerCharacter` (src/melee/pl/player.c:395–403) reads
`player_slots[]`, which is only populated at match start
(src/melee/gm/gmvs.c:1573–1579) — it is stale during the CSS. Read
`mnCharSel_804D6CB0->vs.start.players[port].ckind` instead.

### Per-frame input handling

- Scene tick: `mnCharSel_Scene_OnFrame` — mncharsel.c:5348 (runs once per CSS
  frame after inputs are latched; does scene transitions and B-to-exit, no
  stick reading).
- Real per-port input: `mnCharSel_CursorThink` — mncharsel.c:2366, one GObj per
  cursor (proc registered at :4474). Pad reads: 1P path :2402–2405, VS path
  :2421–2424 (`HSD_PadCopyStatus[port].trigger/.button` + stick delta).

### Hook points for the score keybinds (§6.1: Z + D-pad)

- **Best: top of `mnCharSel_Scene_OnFrame`** (after :5355–5357) — once per
  frame, same TU as all the state (`mnCharSel_804D6CB0`, doors, icons), runs
  after `gm_EvaluateAllControllerInputs()`. Read Z+D-pad per port from
  `HSD_PadCopyStatus[port]` or `gm_GetButtonsTriggered(port)` here, snapshot
  `ckind` per port, and drive the `lbtourney.c` state machine. Beware the
  early `return` at :5423 (B-to-exit) if hooking the bottom instead.
- Per-port raw-input alternative: inside `mnCharSel_CursorThink` right after
  the pad reads (:2405 / :2424).
- On-change (selection committed): the single commit point :2649–2655, or the
  per-door refresh chokepoint `mnCharSel_8025DB34` (:1193).
- The score overlay (`MANGO 2 – 1 ZAIN`, `!`/`✗`) can be its own GObj + SIS
  text created from the hook, following the §1 drawing pattern.

---

## 4. Game-end sequence and the results struct (v2 GAME_END hook)

### Call path

Per-frame VS scene tick `gm_Scene_Vs_OnFrame` — src/melee/gm/gmvs.c:1546, a
4-state machine on `VsSceneState::unk_0` (documented
src/melee/gm/types.h:1259–1263: 0 = match running, 1 = "GAME!", 2 = 1P tally,
3 = exit).

- The match *ends* in `fn_8016CFE0` (gmvs.c:1341): it sets
  `state.match_result = gm_GetMatchOutcome()` (:1394/:1405; outcome logic at
  gmvs.c:874/:730/:781, `MatchOutcome` enum src/melee/gm/forward.h:142–163),
  then runs the end block at :1409–1413 (`fn_8016C7F0`, HUD teardown,
  `state.unk_0 = 1`). LRA+Start / Z-retry enter the same path via
  `fn_8016CF4C` (gmvs.c:1293; call sites :1371, :1384).
- Existing callback at the "GAME!" instant: `fn_8016C7F0` (gmvs.c:1020) calls
  `controller.start.on_match_end(match_result)` if set
  (`StartMeleeRules::on_match_end`, src/melee/mn/types.h:248). **But the
  results struct is not yet filled at that point** — only the outcome is.
- Results are **finalized** in `gm_Scene_Vs_OnExit` — src/melee/gm/gmvs.c:2092
  (verified): copies `controller.state.x24C` into the exit-data's `MatchEnd`,
  stamps `is_teams`/`outcome`, then `gm_80166378(&data->xC)` fills every
  standing, guarded by `state.match_over` (:2098–2110). (1P modes may finalize
  earlier in `fn_8016D634`, gmvs.c:1497; VS skips it.)
- Afterwards: `gmVsMelee_ExitVs` (src/melee/gm/gmvsmelee.c:209) picks Results
  vs Sudden Death from `exit->match_end`, and the results screen reads it via
  `gmVsMelee_ResultsEnterData` (gmvsmelee.c:28 →
  src/melee/gm/gmresult.c:1727/:1745).

### The structs

- **`struct MatchEnd`** — src/melee/gm/types.h:606–624 (verified), size 0x227C.
  Key fields: `outcome` (0x04), `match_kind` (0x05), `is_teams` (0x06),
  `frame_count` (0x08), `n_winners` (0x0D), `winners[6]` (0x10, **port indices
  of the winners**), `player_standings[6]` (0x58).
- **`struct MatchPlayerData`** — src/melee/gm/types.h:539–599, 0xA8 per port:
  `pkind` (`Gm_PKind`, 3 = empty port), **`ckind` (per-port `CharacterKind`)**,
  `ftkind` (resolves Zelda/Sheik), costume bits, `is_big_loser` (misleading
  name — it is the **placement index**, 0 = 1st), `stocks`, `percent`, `team`,
  `kills[4]`, `score`.
- Filled by `gm_80166378(MatchEnd*)` — src/melee/gm/gm_1601.c:2888–3022:
  per-port `pkind`/`ckind`/`ftkind`/`stocks`/`team`/`percent` at :2907–2923,
  scores :3013, then the placement/winner passes :3017–3021 (`fn_80165AC0` at
  :2723 computes placements and `winners[]`/`n_winners` at :2752–2756).
- **Stage id is NOT in `MatchEnd`.** It lives in `StartMeleeRules.stkind`
  (u16, src/melee/mn/types.h:218), read via `gm_GetStKind()`
  (src/melee/gm/gmvs.c:147) or `controller.start.stkind`. Both halves live in
  one object: `static VsSceneController controller` — gmvs.c:61 (struct
  src/melee/gm/types.h:1304–1308 = `{ VsSceneState state; StartMeleeRules
  start; }`; the live `MatchEnd` is `state.x24C`, types.h:1301). Accessors:
  `gmVs_GetSceneController` gmvs.c:63, `gm_GetStartMeleeRules` gmvs.c:73,
  `gm_8016B774()` → `&controller.state.x24C` gmvs.c:473.

### Recommended hook point

**`gm_Scene_Vs_OnExit`, after the `if/else` closes at gmvs.c:2112** — the one
place where, for every mode (`GS_VS`, `GS_SUDDEN_DEATH`, `GS_TRAINING` all
share this scene exit, gmscdata.c:84/:91/:98), the `MatchEnd` is fully
populated and untouched: winner ports (`winners[]`), per-port character
(`player_standings[port].ckind`), and stage (`controller.start.stkind`) all
readable in one struct + one field. Placing it after :2112 (not inside the
`if`) also covers the 1P case where `fn_8016D634` finalized earlier. Two
caveats: `on_match_end` fires at the "GAME!" *moment* but with stale results;
and the scene exit is skipped entirely on a reset-button exit
(gm_1A3F.c:190).

---

## 5. Free RAM and static-buffer conventions (design R4)

### Free RAM

- The arena does **not** run to 0x81800000. build/GALE01/ldscript.lcf:29–30
  (verified): `__ArenaLo = (_db_stack_addr + 0x1f) & ~0x1f;` and
  `__ArenaHi = 0x81700000;` — the top 1 MiB of MEM1 is reserved (FST/OS).
- From the built `main.elf`: static image ends at `.sdata2` end 0x804DEC00;
  stacks bring `__ArenaLo` to 0x804F0C00 (0x804EEC00 on a non-debug boot,
  libs/dolphin/src/dolphin/os/OS.c:173–177). **Arena = 0x804F0C00–0x81700000 ≈
  18.06 MiB** (~18,937 KB).
- The game prints it every boot: `OSReport("# Arena Size %d MB\n", ...)` —
  src/melee/gm/gmmain.c:191 (verified; `arena_size` captured at gmmain.c:146
  after the 48 MB-devkit adjustment at :143–145). The SDK also prints
  `Arena : 0x… - 0x…` at OS.c:233–235. Expected output: `# Arena Size 18 MB`.
- After XFB ×2 (1.2 MiB), GX FIFO (256 KiB, gmmain.c:149), and the 512 KiB
  audio heap (src/sysdolphin/baselib/initialize.c:100–184), the
  general-purpose heap is **~16.1 MiB**. There is also a fully-populated but
  never-read `HSD_MemReport` (initialize.c:32, fields written at
  :81/:100/:167–170/:184) — a ready-made hook if we ever want a live figure.
- No linker map is generated by default; `python configure.py --map && ninja`
  would emit `build/GALE01/main.elf.MAP` for per-object .bss attribution.
  build/GALE01/report.json is decomp-progress only (no memory data).

**Verdict on R4:** the ~8 KB of tournament statics (64×64 B set list + 4 KB
EXI buffer) is noise against an 18 MiB arena — the budget is fine.

### Static-buffer conventions

- Dominant pattern: file-scope `static`, address-derived names, .bss/.sbss.
  Where original .bss ordering must be preserved, statics are hoisted into a
  sibling `<module>.static.h` (29 such files; e.g. included at
  src/melee/lb/lbaudio_ax.c:9).
- Fixed text buffers: src/melee/lb/lbaudio_ax.static.h:98–100
  (`static char cur_hps_stem[0x40]` etc.).
- Large block-scope static work buffers with a size guard:
  src/melee/lb/lbaudio_ax.c:2122–2136 — `static u8 lbl_80433C64[53*1024];`
  plus `HSD_ASSERT(…, computed < 53*1024)` and passing `sizeof(buf)` instead of
  a repeated literal. Good idiom for the 4 KB relay buffer.
- DMA-facing buffers use `ATTRIBUTE_ALIGN(32)` (defined
  src/Runtime/platform.h:60–64): e.g.
  `static u8 garbage[…] ATTRIBUTE_ALIGN(32);` at
  src/sysdolphin/baselib/video.c:9. `ATTRIBUTE_ALIGN(8)` sightings are mostly
  a decomp layout trick, not a hardware need.
- New-file guidance: since `lbrelayexi.c`/`lbtourney.c`/`mntourney.c` are new
  (unmatched) TUs, plain `static` buffers at file scope are fine — no
  `.static.h` needed; use `ATTRIBUTE_ALIGN(32)` on the EXI response buffer and
  `_Static_assert` the struct sizes against `include/relay_proto.h` (which
  already carries its own size asserts).

---

## Things learned that should flow back into the design doc

1. **§6.1 "wraps the existing Slippi EXI device access" is wrong for this
   tree** — there is no Slippi game-side code in the decomp. `lbrelayexi.c` is
   a new, small EXI driver on vanilla SDK primitives (template: hio.c), and
   the EXI command ids + channel/device for the relay device must be defined
   in the Ishiiruka (session 7) and Nintendont (session 8) forks. This doesn't
   change the architecture, only the wording and where the device contract
   lives.
2. **Character ids:** the CSS stores the **external** `CharacterKind`
   (0 = Captain Falcon … 0x19 = Ganondorf, 26 playable). §5.4's "Melee internal
   character id" should be pinned down as *external* id in `protocol.yaml`'s
   comments and in the relay's `chars.ts`, or the table will be built against
   the wrong numbering. (`MatchEnd` at game end also stores `ckind` external +
   `ftkind` internal, so v2 GAME_END can stay in external ids too.)
3. **R4 is resolved:** ~18 MiB arena (~16 MiB heap); the game prints
   `# Arena Size 18 MB` at boot. 8 KB of tournament statics is negligible.
4. Melee already ships a native tournament mode (`GM_TOURNAMENT`,
   `gmtou*.c`) — useful as the closest full-scene precedent, and a reason to
   name our menu distinctly in UI strings to avoid confusion.
