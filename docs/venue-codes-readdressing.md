# Venue Gecko code re-addressing (Neutral Spawns)

Extends the UCF re-addressing pipeline (`docs/ucf-readdressing.md`) to a second
competitive Gecko code the venue wants: **Neutral Spawns** ("neutral starts").
Same problem, same faithful solution: the code is authored at vanilla GALE01
v1.02 addresses; our tournament build is a *shifted* decomp DOL, so every
absolute address it bakes in must be re-resolved to this build's symbols. The
asm bytes are kept **exactly**; only the fields that encode an absolute address
are re-encoded.

The generator is the same script, now generalized:
`tools/gen_ucf_codes.py` emits **both** the UCF 0.8 codes and the Neutral Spawns
code into `build/GALE01/ucf_codes.gecko.txt` (filename kept so the existing ninja
`gen_ucf` wiring and the UCF output are undisturbed). Each is a separate Dolphin
`$<name>` block; both are listed under `[Gecko]` / `[Gecko_Enabled]`.

---

## 1. The code

Source: `#External/NeutralSpawn/NeutralSpawn.asm` in
`Ishiiruka/.../GameSettings/GALE01r2.ini`, a single Gecko **C2** hook:

```
C216E510 00000098    # insert PPC asm at vanilla 0x8016E510, 0x98 (152) lines
```

Authoritative bytes confirmed byte-for-byte against Nintendont: the code is the
**first** code inside `kernel/gecko/g_mods_stealth.bin`,
`g_mods_tournament.bin` and `g_mods_friendlies.bin` (all three identical for the
leading 1224 bytes = 8-byte C2 header + 152×8 payload). It spawns players at
fixed neutral positions per stage at match start.

Payload structure (304 words):
* words **0..191** — code (main routine + helper subroutines),
* words **192..295** — the neutral-position float table (per-stage spawn
  coordinates), read **PC-relative**, never an absolute address,
* words **296..303** — the register-relative epilogue (`lmw`/`lwz`/`mtlr` off
  r1) + branch-back placeholder.

Every `b`/`bc`/`bl` in the two code regions targets **within the payload**
(verified programmatically) — **no relative branch reaches into game code**, so
nothing needs a source port; the whole hook is a faithful byte-for-byte
re-address.

---

## 2. Address map (decoded hook)

C2 insert `0x8016E510` → `fn_8016E2BC + 0x254`. Every payload reference resolves
to a decomp symbol at delta 0 (via `config/GALE01/symbols.txt`), then to this
build's address (via `build/GALE01/main.elf`). Shifted values below are from one
build — **they change every build; the script regenerates them.**

| Field (payload word) | kind | vanilla → symbol → shifted (example) |
|---|---|---|
| C2 header | C2 | `0x8016E510` `fn_8016E2BC+0x254` → `0x801708BC` |
| w4/w5 | lis/ori | `0x8016B41C` `gm_IsCurrently1PMode_inline` → `0x8016D7C8` |
| w12/w13 | lis/lwz **@ha** | `0x80479D30` `state_machine` → `0x8047FC00` |
| w17/w18 | lis/lwz **@ha** | `0x80479D30` `state_machine` → `0x8047FC00` |
| w28/w29 | lis/ori | `0x8003241C` `Player_GetPlayerSlotType` → `0x80033638` |
| w49/w50 | lis/ori | `0x8003241C` `Player_GetPlayerSlotType` → `0x80033638` |
| w56/w57 | lis/ori | `0x80033370` `Player_GetTeam` → `0x8003458C` |
| w79/w80 | lis/ori | `0x8003241C` `Player_GetPlayerSlotType` → `0x80033638` |
| w86/w87 | lis/ori | `0x80033370` `Player_GetTeam` → `0x8003458C` |
| w121 | **r13** load | `0x804D49E8` `selected_stage` → `0x804DB8B0` |
| w143/w144 | lis/ori | `0x80032768` `Player_80032768` → `0x80033984` |
| w157/w158 | lis/ori | `0x80224E64` `Stage_80224E64` → `0x80227904` |
| w163/w164 | lis/ori | `0x80032768` `Player_80032768` → `0x80033984` |
| w170/w171 | lis/ori | `0x800326CC` `Player_LoadPlayerCoords` → `0x800338E8` |
| w175 | **r2→r13** load | `0x804D6DC8` `un_804D6DC8` → `0x804DDD10` |
| w178 | **r2→r13** load | `0x804D6DD8` `un_804D6DD8` → `0x804DDD20` |
| w180 | **r2→r13** load | `0x804D6DAC` `un_804D6DAC` → `0x804DDCF4` |
| w182/w183 | lis/ori | `0x80033094` `Player_SetFacingDirection` → `0x800342B0` |

**Not addresses (kept verbatim):** the entire float table (words 192..295) and
constants like `0x40A00000` are PC-relative data, never touched.

### New re-encoding kinds vs. UCF

UCF only needed `lis/ori` (unsigned) and `lwz disp(r13)`. Neutral Spawns adds
two:

* **`lis + lwz` with a signed displacement (`@ha`/`@l`)** — w12/w13 and w17/w18
  build `state_machine`'s pointer as `lis r3,addr@ha ; lwz r3,addr@l(r3)`. Unlike
  the `lis/ori` (unsigned) pairing, the low half is a *signed* displacement, so
  the high half carries the `+1` when bit 15 of the low half is set. The tool
  re-encodes both halves with the `@ha`/`@l` rule.

* **SDA2 (`r2`) loads of `.sbss` globals** — w175/w178/w180 load small-data
  globals via `lfs fX, disp(r2)`. See §3.

---

## 3. The one field that needed more than an immediate tweak: r2 → r13

The three `lfs fX, disp(r2)` loads (w175/w178/w180) read `.sbss` small-data
globals (`un_804D6DAC/C8/D8`). In **vanilla**, that region sat within a signed
16-bit displacement of **both** SDA bases (`_SDA_BASE_`=`0x804DB6A0` /
`_SDA2_BASE_`=`0x804DC4E0`), so the author was free to reach it from `r2`.

Our decomp build **spreads the small-data sections apart**. In the build shown:

```
.sbss    0x804DD2A0 .. 0x804DE929      (holds these three globals)
_SDA_BASE_  (r13) = 0x804E2560         -> .sbss is within reach  (disp ~ -0x4850)
_SDA2_BASE_ (r2)  = 0x804E6940         -> .sbss is 0x8C30 away   (OVERFLOWS s16)
```

So the shifted `.sbss` globals are reachable from **r13 but not from r2** — a
pure immediate-only re-encode of the `r2` load is **impossible** (the signed
displacement overflows). Because `.sbss` is exactly what `_SDA_BASE_` (r13) is
for, the tool re-expresses the **same global** as an r13/`_SDA_BASE_`-relative
load: it recomputes the displacement against r13 and rewrites the base-register
field `rA` from `r2` (2) to `r13` (13). The effective address is byte-verified to
be the identical symbol; only these three words change a field beyond the
immediate, and that is the sole such case in either code.

This is the R12 "recompute SDA loads against the shifted `_SDA_BASE_`" step,
applied to loads the stock code happened to encode via the read-only SDA2 base.
Everything else (13 `lis` pairs, the `state_machine` `@ha` pointer, the
`selected_stage` r13 load, all branches, the float table) was cleanly
remappable.

> The vanilla reconstruction (§4) keeps these three loads as `r2` (they fit in
> vanilla), so byte-fidelity to the source blob is preserved; only the *shifted*
> emission rebases them.

---

## 4. Faithfulness proof

Same two guarantees as UCF, now covering this code too:

* **`selfcheck()`** (runs on every invocation) — re-encoding every fixup of
  every hook (UCF + Neutral Spawns) with its *vanilla* address must reproduce
  the stock template word exactly.
* **byte reconstruction** — rebuild the blob with vanilla addresses and
  byte-compare to the real source:

  ```
  python tools/gen_ucf_codes.py --verify-bin    P:/Projects/Nintendont/kernel/gecko/g_ucf.bin
  # verify[UCF 0.8 (re-addressed)]: OK -- byte-identical (1072 bytes)

  python tools/gen_ucf_codes.py --verify-ns-bin P:/Projects/Nintendont/kernel/gecko/g_mods_stealth.bin
  # verify[Neutral Spawns (re-addressed)]: OK -- byte-identical to first 1224 bytes
  ```

  (Neutral Spawns is the leading code of a larger `g_mods_*.bin`, so its check
  compares the first 1224 bytes; UCF's `g_ucf.bin` is exactly the four hooks, so
  its check compares the whole file.)

---

## 5. Generation & build wiring

Unchanged from UCF (`docs/ucf-readdressing.md` §3–4). `tools/gen_ucf_codes.py`
now iterates code **groups** (UCF, Neutral Spawns); the ninja `gen_ucf` rule and
the output path `build/GALE01/ucf_codes.gecko.txt` are the same, so a plain
`python -m ninja` links the DOL and regenerates **both** codes with this build's
addresses. Resolve r2 as well as r13:

```
python tools/gen_ucf_codes.py                    # both codes, build/GALE01/main.elf
python tools/gen_ucf_codes.py --elf X --out Y
python tools/gen_ucf_codes.py --verify-bin    <g_ucf.bin>
python tools/gen_ucf_codes.py --verify-ns-bin <g_mods_stealth.bin>
```

## 6. Applying / testing in Dolphin

Same manual, explicit step as UCF (`docs/ucf-readdressing.md` §5): the generated
codes are **not** committed into `GALE01r2.ini`. To test, copy the
`$Neutral Spawns (re-addressed)` block from section **(b)** of
`build/GALE01/ucf_codes.gecko.txt` into `[Gecko]`, add its name under
`[Gecko_Enabled]`, and leave the pre-existing vanilla `#External/NeutralSpawn/...`
entry **disabled** (it crashes the shifted DOL — the same R12 problem UCF has).
Hardware (Nintendont) application is still the open R12/session-8 work.
