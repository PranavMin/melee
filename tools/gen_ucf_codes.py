#!/usr/bin/env python3
###
# Regenerate the venue Gecko codes, re-addressed for THIS build's shifted DOL.
#
# Background (see docs/ucf-readdressing.md + docs/venue-codes-readdressing.md and
# docs/ucf-investigation.md):
#   Several competitive Gecko codes we ship are authored at vanilla GALE01 v1.02
#   addresses. Our decomp DOL is SHIFTED (design.md R11/R12), so applying the
#   vanilla-addressed hooks crashes -- the asm itself is fine, only its hardcoded
#   absolute addresses are wrong. Two such codes are handled here:
#     * UCF 0.8       -- 4 C2 hooks from Nintendont/kernel/gecko/g_ucf.bin.
#     * Neutral Spawns -- 1 C2 hook (0x8016E510, 0x98 lines) shared by
#                        g_mods_{stealth,tournament,friendlies}.bin, also present
#                        verbatim as `#External/NeutralSpawn/NeutralSpawn.asm` in
#                        Ishiiruka .../GameSettings/GALE01r2.ini.
#
# This tool keeps each code's EXACT asm bytes and only re-addresses the fields
# that encode an absolute vanilla address:
#   * the C2 insert address (a function+offset) of every hook,
#   * every lis/ori pair that builds an absolute data or function pointer,
#   * every lis + signed-displacement load (@ha/@l) that builds a data pointer,
#   * every `lwz/... rX, disp(r13)` small-data  (SDA)  load of a game global,
#   * every `lfs/... fX, disp(r2)`  small-data2 (SDA2) load of a game constant.
# Each vanilla address was resolved to a decomp SYMBOL (+delta) via
# config/GALE01/symbols.txt; this tool re-resolves those symbols to the current
# build's addresses in build/GALE01/main.elf and re-encodes just those fields.
# Relative branches (b/bc/bl) inside every payload are self-contained (verified:
# they only ever target within their own payload) and are NEVER touched; the
# copied original (overwritten) instruction is register-relative and never
# touched. Nothing is re-derived -- the emitted codes are byte-for-byte the real
# codes except for the re-addressed immediate fields.
#
# Stdlib only (matches the repo's tooling style; the ELF32-BE symtab is parsed
# here directly, no external deps).
#
# Usage:
#   python tools/gen_ucf_codes.py                 # uses build/GALE01/main.elf
#   python tools/gen_ucf_codes.py --elf X --out Y
#   python tools/gen_ucf_codes.py --verify-bin \
#       P:/Projects/Nintendont/kernel/gecko/g_ucf.bin        # prove UCF fidelity
#   python tools/gen_ucf_codes.py --verify-ns-bin \
#       P:/Projects/Nintendont/kernel/gecko/g_mods_stealth.bin  # prove NS fidelity
###

import argparse
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# Templates -- the EXACT words of the source blobs. Each hook carries its full
# C2 payload (the last word of each payload is the codehandler's branch-back
# placeholder, kept verbatim). `header` and `fixups` name the fields that encode
# an absolute vanilla address; everything else is emitted unchanged. Word indices
# are 0-based into the payload word list.
#   fixup kinds:
#     ("abs",    hi, lo, symbol, delta, vanilla) -- lis rX,@h ; ori rX,rX,@l   (unsigned OR)
#     ("abs_ha", hi, lo, symbol, delta, vanilla) -- lis rX,@ha ; <mem> rX,@l(rX) (signed disp)
#     ("sda",    idx,    symbol,        vanilla) -- <mem> rX, disp(r13)  (_SDA_BASE_)
#     ("sda2",   idx,    symbol,        vanilla) -- <mem> fX, disp(r2)   (_SDA2_BASE_)
# `vanilla` values are the addresses these fields hold in the stock blob; they
# are used only to self-check the templates and to reconstruct the vanilla blob
# in --verify mode. Re-encoding a fixup with its vanilla address must reproduce
# the original word (asserted at startup) -- that is the fidelity guarantee.
# ---------------------------------------------------------------------------

# r13 (_SDA_BASE_) and r2 (_SDA2_BASE_) in stock GALE01 v1.02.
#   0x804DB6A0 - 0x514C == 0x804D6554 == p_ftCommonData (every UCF SDA load).
#   0x804DC4E0 is the stock SDA2 base reached by the Neutral Spawns lfs loads
#   (confirmed: each resolves to a decomp float object symbol at delta 0, and the
#   selfcheck below re-derives the stock 0xA8E8/0xA8F8/0xA8CC displacements).
VANILLA_R13 = 0x804DB6A0
VANILLA_R2 = 0x804DC4E0

UCF_HOOKS = [
    {
        "name": "UCF 0.8 dashback",
        "purpose": "dashback (ftCo_Turn_IASA)",
        "header": ("ftCo_Turn_IASA", 0x4C),
        "header_vanilla": 0x800C9A44,
        "fixups": [
            ("sda", 11, "p_ftCommonData", 0x804D6554),
            ("abs", 23, 24, "HSD_PadLibData", 0, 0x804C1F78),
            ("abs", 45, 46, "Player_GetEntityAtIndex", 0, 0x8003418C),
            ("abs", 68, 69, "gmMain_8046B108", 0, 0x8046B108),
        ],
        "words": [
            0xD01F002C, 0x7C0802A6, 0x90010004, 0x9421FF20, 0xBE8100B0, 0x48000121,
            0x7FC802A6, 0xC03F0894, 0xC05E0000, 0xFC011040, 0x40820118, 0x808DAEB4,
            0xC03F0620, 0xFC200A10, 0xC044003C, 0xFC011040, 0x41800100, 0x887F0670,
            0x2C030002, 0x408000F4, 0x887F221F, 0x54600739, 0x408200E8, 0x3C60804C,
            0x60631F78, 0x8BA30001, 0x387DFFFE, 0x889F0618, 0x4800008D, 0x7C7C1B78,
            0x7FA3EB78, 0x889F0618, 0x4800007D, 0x7C7C1850, 0x7C6319D6, 0x2C0315F9,
            0x408100B0, 0x38000001, 0x901F2358, 0x901F2340, 0x809F0004, 0x2C04000A,
            0x40A20098, 0x887F000C, 0x38800001, 0x3D808003, 0x618C418C, 0x7D8903A6,
            0x4E800421, 0x2C030000, 0x41820078, 0x8083002C, 0x80841ECC, 0xC03F002C,
            0xD0240018, 0xC05E0004, 0xFC011040, 0x4181000C, 0x38600080, 0x48000008,
            0x3860007F, 0x98640006, 0x48000048, 0x7C852378, 0x3863FFFF, 0x2C030000,
            0x40800008, 0x38630005, 0x3C808046, 0x6084B108, 0x1C630030, 0x7C841A14,
            0x1C65000C, 0x7C841A14, 0x88640002, 0x7C630774, 0x4E800020, 0x4E800021,
            0x40000000, 0x00000000, 0xBA8100B0, 0x800100E4, 0x382100E0, 0x7C0803A6,
            0x60000000, 0x00000000,
        ],
    },
    {
        "name": "UCF 0.8 shield-drop",
        "purpose": "shield-drop (ftCo_80099894)",
        "header": ("ftCo_80099894", 0x10),
        "header_vanilla": 0x800998A4,
        "fixups": [
            ("sda", 9, "p_ftCommonData", 0x804D6554),
        ],
        "words": [
            0x7C0802A6, 0x90010004, 0x9421FF20, 0xBE8100B0, 0x7C7E1B78, 0x83FE002C,
            0x480000DD, 0x7FA802A6, 0xC03F063C, 0x806DAEB4, 0xC0030314, 0xFC010040,
            0x408100E4, 0xC03F0620, 0x48000071, 0xD0210090, 0xC03F0624, 0x48000065,
            0xC0410090, 0xEC4200B2, 0xEC210072, 0xEC21102A, 0xC05D000C, 0xFC011040,
            0x418000B4, 0x889F0670, 0x2C040003, 0x408100A8, 0xC01D0010, 0xC03F0624,
            0xFC000840, 0x40800098, 0xBA8100B0, 0x800100E4, 0x382100E0, 0x7C0803A6,
            0x8061001C, 0x83E10014, 0x38210018, 0x38630008, 0x7C6803A6, 0x4E800020,
            0xFC000A10, 0xC03D0000, 0xEC000072, 0xC03D0004, 0xEC000828, 0xFC00001E,
            0xD8010080, 0x80610084, 0x38630002, 0x3C004330, 0xC85D0014, 0x6C638000,
            0x90010080, 0x90610084, 0xC8210080, 0xEC011028, 0xC03D0000, 0xEC200824,
            0x4E800020, 0x4E800021, 0x42A00000, 0x37270000, 0x43300000, 0x3F800000,
            0xBF4CCCCD, 0x43300000, 0x80000000, 0x7FC3F378, 0x7FE4FB78, 0xBA8100B0,
            0x800100E4, 0x382100E0, 0x7C0803A6, 0x00000000,
        ],
    },
    {
        "name": "UCF 0.8 tumble",
        "purpose": "wiggle out of tumble (ftCo_DamageFall_IASA)",
        "header": ("ftCo_DamageFall_IASA", 0xCC),
        "header_vanilla": 0x800908F4,
        "fixups": [
            ("sda", 8, "p_ftCommonData", 0x804D6554),
            ("abs", 12, 13, "HSD_PadLibData", 0, 0x804C1F78),
        ],
        "words": [
            0x7C0802A6, 0x90010004, 0x9421FF20, 0xBE8100B0, 0x2C030001, 0x4082008C,
            0xC03F0628, 0xFC200A10, 0x806DAEB4, 0xC0430210, 0xFC011040, 0x40800074,
            0x3F80804C, 0x639C1F78, 0x8BDC0001, 0x387EFFFE, 0x889F0618, 0x48000029,
            0x7C7D1B78, 0x7FC3F378, 0x889F0618, 0x48000019, 0x7C7D1850, 0x7C6319D6,
            0x388015F9, 0x7C041800, 0x48000038, 0x7C852378, 0x3863FFFF, 0x2C030000,
            0x40800008, 0x38630005, 0x809C0008, 0x1C630030, 0x7C841A14, 0x1C65000C,
            0x7C841A14, 0x88640002, 0x7C630774, 0x4E800020, 0xBA8100B0, 0x800100E4,
            0x382100E0, 0x7C0803A6, 0x60000000, 0x00000000,
        ],
    },
    {
        "name": "UCF 0.8 CSS label",
        "purpose": "cosmetic 'UCF 0.8' text on CSS (mnCharSel_802640A0)",
        "header": ("mnCharSel_802640A0", 0x2230),
        "header_vanilla": 0x802662D0,
        "fixups": [
            ("abs", 8, 9, "HSD_SisLib_803A6754", 0, 0x803A6754),
            ("abs", 25, 26, "HSD_SisLib_803A6B98", 0, 0x803A6B98),
            ("abs", 33, 34, "HSD_SisLib_803A7548", 0, 0x803A7548),
        ],
        "words": [
            0x7C0802A6, 0x90010004, 0x9421FF20, 0xBE8100B0, 0x48000089, 0x7FC802A6,
            0x38600000, 0x38800000, 0x3D80803A, 0x618C6754, 0x7D8903A6, 0x4E800421,
            0x7C7F1B78, 0x38800001, 0x989F0049, 0x38800001, 0x989F004A, 0xC03E000C,
            0xD03F0024, 0xD03F0028, 0x7FE3FB78, 0x48000059, 0x7C8802A6, 0xC03E0000,
            0xC05E0004, 0x3D80803A, 0x618C6B98, 0x7D8903A6, 0x4E800421, 0x7C641B78,
            0x7FE3FB78, 0xC03E0008, 0xC05E0008, 0x3D80803A, 0x618C7548, 0x7D8903A6,
            0x4E800421, 0x48000024, 0x4E800021, 0x42180000, 0xC3898000, 0x3EE66666,
            0x3DCCCCCD, 0x4E800021, 0x55434620, 0x302E3800, 0xBA8100B0, 0x800100E4,
            0x382100E0, 0x7C0803A6, 0x38980000, 0x00000000,
        ],
    },
]

# ---------------------------------------------------------------------------
# Neutral Spawns -- the single C2 hook at vanilla 0x8016E510 (0x98 lines).
# Source bytes: the first code of Nintendont/kernel/gecko/g_mods_stealth.bin
# (identical in g_mods_tournament.bin and g_mods_friendlies.bin, and in the
# `#External/NeutralSpawn/NeutralSpawn.asm` block of GALE01r2.ini). Spawns the
# players at fixed neutral positions per stage at match start.
#
# Decoded (all references resolve to a decomp symbol at delta 0 unless noted):
#   C2 insert 0x8016E510 -> fn_8016E2BC + 0x254
#   abs   call  0x8016B41C -> gm_IsCurrently1PMode_inline  (w4/w5)
#   abs_ha data 0x80479D30 -> state_machine                (w12/w13, w17/w18)
#   abs   call  0x8003241C -> Player_GetPlayerSlotType      (w28/w29, w49/w50, w79/w80)
#   abs   call  0x80033370 -> Player_GetTeam                (w56/w57, w86/w87)
#   sda   load  0x804D49E8 -> selected_stage (r13)          (w121)
#   abs   call  0x80032768 -> Player_80032768               (w143/w144, w163/w164)
#   abs   call  0x80224E64 -> Stage_80224E64                (w157/w158)
#   abs   call  0x800326CC -> Player_LoadPlayerCoords        (w170/w171)
#   sda2  load  0x804D6DC8 -> un_804D6DC8 (r2)              (w175)
#   sda2  load  0x804D6DD8 -> un_804D6DD8 (r2)              (w178)
#   sda2  load  0x804D6DAC -> un_804D6DAC (r2)              (w180)
#   abs   call  0x80033094 -> Player_SetFacingDirection      (w182/w183)
# Words 192..295 are the neutral-position float table (PC-relative, not touched);
# words 296..303 are the register-relative epilogue (not touched). Every
# b/bc/bl in the two code regions targets within the payload (verified) -- no
# relative branch reaches into game code, so the whole hook is a faithful
# byte-for-byte re-address.
NEUTRAL_SPAWN_HOOKS = [
    {
        "name": "Neutral Spawns",
        "purpose": "neutral starts (fn_8016E2BC insert; per-stage fixed spawns)",
        "header": ("fn_8016E2BC", 0x254),
        "header_vanilla": 0x8016E510,
        "fixups": [
            ("abs", 4, 5, "gm_IsCurrently1PMode_inline", 0, 0x8016B41C),
            ("abs_ha", 12, 13, "state_machine", 0, 0x80479D30),
            ("abs_ha", 17, 18, "state_machine", 0, 0x80479D30),
            ("abs", 28, 29, "Player_GetPlayerSlotType", 0, 0x8003241C),
            ("abs", 49, 50, "Player_GetPlayerSlotType", 0, 0x8003241C),
            ("abs", 56, 57, "Player_GetTeam", 0, 0x80033370),
            ("abs", 79, 80, "Player_GetPlayerSlotType", 0, 0x8003241C),
            ("abs", 86, 87, "Player_GetTeam", 0, 0x80033370),
            ("sda", 121, "selected_stage", 0x804D49E8),
            ("abs", 143, 144, "Player_80032768", 0, 0x80032768),
            ("abs", 157, 158, "Stage_80224E64", 0, 0x80224E64),
            ("abs", 163, 164, "Player_80032768", 0, 0x80032768),
            ("abs", 170, 171, "Player_LoadPlayerCoords", 0, 0x800326CC),
            ("sda2", 175, "un_804D6DC8", 0x804D6DC8),
            ("sda2", 178, "un_804D6DD8", 0x804D6DD8),
            ("sda2", 180, "un_804D6DAC", 0x804D6DAC),
            ("abs", 182, 183, "Player_SetFacingDirection", 0, 0x80033094),
        ],
        "words": [
            0x7C0802A6, 0x90010004, 0x9421FF20, 0xBE8100B0, 0x3D808016, 0x618CB41C,
            0x7D8903A6, 0x4E800421, 0x2C030000, 0x40820480, 0x2C1C0005, 0x40800478,
            0x3C608048, 0x80639D30, 0x5463443E, 0x2C03021C, 0x41820464, 0x3C608048,
            0x80639D30, 0x5463443E, 0x2C03010F, 0x41820450, 0x887F24D0, 0x2C030001,
            0x41820054, 0x3B200000, 0x3B400000, 0x7F43D378, 0x3D808003, 0x618C241C,
            0x7D8903A6, 0x4E800421, 0x2C030003, 0x41820010, 0x7C1CD000, 0x41820014,
            0x3B390001, 0x3B5A0001, 0x2C1A0004, 0x4081FFD0, 0x7F83E378, 0x7F24CB78,
            0x88BF24D0, 0x48000115, 0x480003F4, 0x3B400000, 0x3B000000, 0x3B200000,
            0x7F23CB78, 0x3D808003, 0x618C241C, 0x7D8903A6, 0x4E800421, 0x2C030003,
            0x41820024, 0x7F23CB78, 0x3D808003, 0x618C3370, 0x7D8903A6, 0x4E800421,
            0x7C03D000, 0x40820008, 0x3B180001, 0x3B390001, 0x2C190004, 0x4180FFBC,
            0x2C180001, 0x41820398, 0x2C180002, 0x41810390, 0x3B5A0001, 0x2C1A0003,
            0x4180FF98, 0x3B200000, 0x3B410080, 0x3B000000, 0x3AC00000, 0x3AE00000,
            0x7EE3BB78, 0x3D808003, 0x618C241C, 0x7D8903A6, 0x4E800421, 0x2C030003,
            0x41820028, 0x7EE3BB78, 0x3D808003, 0x618C3370, 0x7D8903A6, 0x4E800421,
            0x7C03C800, 0x4082000C, 0x7EF8D1AE, 0x3B180001, 0x3AF70001, 0x2C170004,
            0x4180FFB8, 0x3B390001, 0x2C190003, 0x4180FFA4, 0x3B200000, 0x7C79D0AE,
            0x7C03E000, 0x41820010, 0x3B390001, 0x2C190004, 0x4180FFEC, 0x7F83E378,
            0x7F24CB78, 0x88BF24D0, 0x48000009, 0x480002E8, 0x7C0802A6, 0x90010004,
            0x9421FF20, 0xBE8100B0, 0x7C7F1B78, 0x7C9E2378, 0x7CBD2B78, 0x48000121,
            0x7F8802A6, 0x80CD9348, 0x38A00000, 0x807C0000, 0x2C03FFFF, 0x4182005C,
            0x7C033000, 0x4182000C, 0x3B9C0044, 0x4BFFFFE8, 0x3B9C0004, 0x1C7D0020,
            0x7F9C1A14, 0x1C7E0008, 0x7F9C1A14, 0x38810080, 0xC03C0000, 0xD0240000,
            0xC03C0004, 0xD0240004, 0x38600000, 0x90640008, 0x7FE3FB78, 0x3D808003,
            0x618C2768, 0x7D8903A6, 0x4E800421, 0x48000054, 0x2C1D0001, 0x4182000C,
            0x7FC3F378, 0x48000014, 0x4800023D, 0x7C6802A6, 0x7C63F0AE, 0x48000004,
            0x38810080, 0x3D808022, 0x618C4E64, 0x7D8903A6, 0x4E800421, 0x7FE3FB78,
            0x38810080, 0x3D808003, 0x618C2768, 0x7D8903A6, 0x4E800421, 0x48000004,
            0x7FE3FB78, 0x38810080, 0x3D808003, 0x618C26CC, 0x7D8903A6, 0x4E800421,
            0xC0210080, 0xC002A8E8, 0xFC010040, 0x4081000C, 0xC022A8F8, 0x48000008,
            0xC022A8CC, 0x7FE3FB78, 0x3D808003, 0x618C3094, 0x7D8903A6, 0x4E800421,
            0xBA8100B0, 0x800100E4, 0x382100E0, 0x7C0803A6, 0x4E800020, 0x4E800021,
            0x00000020, 0xC2700000, 0x41200000, 0x42700000, 0x41200000, 0xC1A00000,
            0x41200000, 0x41A00000, 0x41200000, 0xC2700000, 0x41200000, 0xC1A00000,
            0x41200000, 0x42700000, 0x41200000, 0x41A00000, 0x41200000, 0x0000001F,
            0xC21B3333, 0x420CCCCD, 0x421B3333, 0x420CCCCD, 0x00000000, 0x41000000,
            0x00000000, 0x4279999A, 0xC21B3333, 0x420CCCCD, 0xC21B3333, 0x40A00000,
            0x421B3333, 0x420CCCCD, 0x421B3333, 0x40A00000, 0x00000008, 0xC2280000,
            0x41D4CCCD, 0x42280000, 0x41E00000, 0x00000000, 0x423B999A, 0x00000000,
            0x409CCCCD, 0xC2280000, 0x41D4CCCD, 0xC2280000, 0x40A00000, 0x42280000,
            0x41E00000, 0x42280000, 0x40A00000, 0x0000001C, 0xC23A6666, 0x4214CCCD,
            0x423D999A, 0x42153333, 0x00000000, 0x40E00000, 0x00000000, 0x426A0000,
            0xC23A6666, 0x4214CCCD, 0xC23A6666, 0x40A00000, 0x423D999A, 0x42153333,
            0x423D999A, 0x40A00000, 0x00000002, 0xC2250000, 0x41A80000, 0x42250000,
            0x41D80000, 0x00000000, 0x40A80000, 0x00000000, 0x42400000, 0xC2250000,
            0x41A80000, 0xC2250000, 0x40A00000, 0x42250000, 0x41D80000, 0x42250000,
            0x40A00000, 0x00000003, 0xC2200000, 0x42000000, 0x42200000, 0x42000000,
            0x428C0000, 0x40E00000, 0xC28C0000, 0x40E00000, 0xC2200000, 0x42000000,
            0xC2200000, 0x40A00000, 0x42200000, 0x42000000, 0x42200000, 0x40A00000,
            0xFFFFFFFF, 0x4E800021, 0x00030102, 0xBA8100B0, 0x800100E4, 0x382100E0,
            0x7C0803A6, 0x881F24D0, 0x60000000, 0x00000000,
        ],
    },
]

# Code groups: each becomes one Dolphin `$<code_label>` entry (one or more C2
# hooks concatenated) plus its own annotated address map.
GROUPS = [
    {
        "code_label": "UCF 0.8 (re-addressed)",
        "source": "Nintendont/kernel/gecko/g_ucf.bin (four C2 hooks; UCF 0.8)",
        "hooks": UCF_HOOKS,
    },
    {
        "code_label": "Neutral Spawns (re-addressed)",
        "source": "first C2 code of Nintendont/kernel/gecko/g_mods_*.bin "
                   "(== GALE01r2.ini NeutralSpawn.asm)",
        "hooks": NEUTRAL_SPAWN_HOOKS,
    },
]


# ---------------------------------------------------------------------------
# ELF32 big-endian .symtab reader (name -> value), stdlib only.
# ---------------------------------------------------------------------------
def load_elf_symbols(path):
    data = path.read_bytes()
    if data[:4] != b"\x7fELF":
        die(f"{path} is not an ELF file")
    if data[4] != 1 or data[5] != 2:
        die(f"{path} is not ELF32 big-endian (class={data[4]} data={data[5]})")
    e_shoff = struct.unpack_from(">I", data, 0x20)[0]
    e_shentsize = struct.unpack_from(">H", data, 0x2E)[0]
    e_shnum = struct.unpack_from(">H", data, 0x30)[0]
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (_name, typ, _flags, _addr, offset, size, link, _info,
         _align, entsz) = struct.unpack_from(">IIIIIIIIII", data, off)
        secs.append((typ, offset, size, link, entsz))
    syms = {}
    for typ, offset, size, link, entsz in secs:
        if typ != 2 or entsz == 0:  # SHT_SYMTAB
            continue
        stroff = secs[link][1]
        for j in range(size // entsz):
            o = offset + j * entsz
            st_name, st_value, _st_size, _info, _other, _shndx = struct.unpack_from(
                ">IIIBBH", data, o)
            if st_name == 0 or st_value == 0:
                continue
            end = data.index(b"\0", stroff + st_name)
            nm = data[stroff + st_name:end].decode("ascii", "replace")
            syms.setdefault(nm, st_value)
    return syms


def die(msg):
    print(f"gen_ucf_codes: error: {msg}", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
# Re-encoding helpers. Each returns the modified word(s); only the immediate
# (low 16 bits) changes, the opcode/register fields are preserved verbatim.
# ---------------------------------------------------------------------------
def enc_abs(hi_word, lo_word, addr):
    """lis rX, addr@h ; ori rX, rX, addr@l  (unsigned OR pairing)."""
    hi = (hi_word & 0xFFFF0000) | ((addr >> 16) & 0xFFFF)
    lo = (lo_word & 0xFFFF0000) | (addr & 0xFFFF)
    return hi, lo


def enc_abs_ha(hi_word, lo_word, addr):
    """lis rX, addr@ha ; <mem> rX, addr@l(rX)  (signed-displacement pairing).
    The low half is a signed 16-bit displacement, so the high half carries the
    +1 carry when bit15 of the low half is set (the standard @ha convention)."""
    lo = addr & 0xFFFF
    ha = ((addr - (lo - 0x10000 if lo & 0x8000 else lo)) >> 16) & 0xFFFF
    hi = (hi_word & 0xFFFF0000) | ha
    lo_w = (lo_word & 0xFFFF0000) | lo
    return hi, lo_w


def enc_sda(word, addr, base):
    """<mem> rX, (addr - base)(rBase)  -- signed 16-bit displacement."""
    disp = (addr - base) & 0xFFFF
    return (word & 0xFFFF0000) | disp


def enc_c2_header(addr):
    return 0xC2000000 | (addr & 0x01FFFFFF)


def resolve_hook(hook, sym, bases, *, vanilla=False):
    """Return (c2_word, n, words[], addrmap[]) for one hook.
    `bases` = {"r13": ..., "r2": ...} (shifted, or vanilla when vanilla=True).
    vanilla=True re-encodes with the stock addresses (must reproduce the
    template exactly); otherwise uses the shifted addresses from `sym`."""
    words = list(hook["words"])
    addrmap = []

    # C2 header
    hsym, hdelta = hook["header"]
    if vanilla:
        haddr = hook["header_vanilla"]
    else:
        haddr = _need(sym, hsym, hook) + hdelta
    c2 = enc_c2_header(haddr)
    addrmap.append(("C2 insert", f"{hsym}+0x{hdelta:X}", hook["header_vanilla"], haddr))

    for fx in hook["fixups"]:
        kind = fx[0]
        if kind in ("abs", "abs_ha"):
            _, hi_idx, lo_idx, s, delta, van = fx
            addr = van if vanilla else _need(sym, s, hook) + delta
            enc = enc_abs if kind == "abs" else enc_abs_ha
            words[hi_idx], words[lo_idx] = enc(words[hi_idx], words[lo_idx], addr)
            tag = f"{s}+0x{delta:X}" if delta else s
            addrmap.append((f"lis/lo w{hi_idx}/{lo_idx}", tag, van, addr))
        elif kind == "sda":
            _, idx, s, van = fx
            addr = van if vanilla else _need(sym, s, hook)
            words[idx] = enc_sda(words[idx], addr, bases["r13"])
            addrmap.append((f"r13 load w{idx}", s, van, addr))
        elif kind == "sda2":
            # r2/_SDA2_BASE_ relative load in the stock blob. The target is a
            # small-data (.sbss) global that vanilla could reach from BOTH r13
            # and r2 (the two SDA bases overlapped its region). Our shifted build
            # spreads the small-data sections apart: the global is still in reach
            # of r13 but the shifted _SDA2_BASE_ (r2) is now too far (the signed
            # 16-bit displacement overflows). When that happens we re-express the
            # SAME global as an r13/_SDA_BASE_-relative load (rewrite rA 2->13);
            # this is the one field that needs more than an immediate tweak. The
            # vanilla path always keeps r2 (it fits), so template/verify fidelity
            # to the source bytes is preserved.
            _, idx, s, van = fx
            if vanilla:
                words[idx] = enc_sda(words[idx], van, bases["r2"])
                addrmap.append((f"r2 load w{idx}", s, van, van))
                continue
            addr = _need(sym, s, hook)
            d2 = addr - bases["r2"]  # true (unmasked) difference
            if -0x8000 <= d2 <= 0x7FFF:
                words[idx] = enc_sda(words[idx], addr, bases["r2"])
                addrmap.append((f"r2 load w{idx}", s, van, addr))
            else:
                d13 = addr - bases["r13"]
                if not (-0x8000 <= d13 <= 0x7FFF):
                    die(f"w{idx} {s}: 0x{addr:08X} unreachable from r2 "
                        f"(disp {d2:#x}) or r13 (disp {d13:#x}); not remappable")
                w = enc_sda(words[idx], addr, bases["r13"])
                w = (w & ~(0x1F << 16)) | (13 << 16)  # rewrite base reg rA: r2 -> r13
                words[idx] = w
                addrmap.append((f"r2->r13 load w{idx}", s, van, addr))
        else:
            die(f"unknown fixup kind {kind}")
    return c2, len(hook["words"]) // 2, words, addrmap


def _need(sym, name, hook):
    if name not in sym:
        die(f"symbol '{name}' not found in ELF (hook {hook['name']})")
    return sym[name]


def all_hooks():
    for g in GROUPS:
        for h in g["hooks"]:
            yield h


VANILLA_BASES = {"r13": VANILLA_R13, "r2": VANILLA_R2}


def selfcheck():
    """Re-encoding every fixup with its vanilla address must reproduce the
    stock template word exactly -- proves the fixup table (indices/kinds) is
    correct and that the tool only ever changes the addressed fields."""
    for hook in all_hooks():
        c2, n, words, _ = resolve_hook(hook, {}, VANILLA_BASES, vanilla=True)
        if enc_c2_header(hook["header_vanilla"]) != c2:
            die(f"selfcheck: C2 header mismatch in {hook['name']}")
        if words != hook["words"]:
            for i, (a, b) in enumerate(zip(words, hook["words"])):
                if a != b:
                    die(f"selfcheck: word {i} in {hook['name']} "
                        f"{a:08X} != template {b:08X}")


# ---------------------------------------------------------------------------
# Emit
# ---------------------------------------------------------------------------
def build_group(group, sym, bases):
    """Return (raw_lines, code_lines, addrmaps) for one code group."""
    raw_lines = []
    code_lines = []
    addrmaps = []
    for hook in group["hooks"]:
        c2, n, words, addrmap = resolve_hook(hook, sym, bases)
        addrmaps.append((hook, addrmap))
        raw_lines.append(f"  * {hook['purpose']}")
        raw_lines.append(f"    {c2:08X} {n:08X}")
        code_lines.append(f"{c2:08X} {n:08X}")
        for i in range(0, len(words), 2):
            a, b = words[i], words[i + 1]
            raw_lines.append(f"    {a:08X} {b:08X}")
            code_lines.append(f"{a:08X} {b:08X}")
        raw_lines.append("")
    return raw_lines, code_lines, addrmaps


def render(sym, bases):
    r13, r2 = bases["r13"], bases["r2"]
    out = []
    out.append("# Venue Gecko codes, re-addressed for this build's shifted DOL.")
    out.append("# Generated by tools/gen_ucf_codes.py -- do not edit by hand.")
    out.append("# Faithful to the source blobs (UCF 0.8 g_ucf.bin; Neutral Spawns")
    out.append("# NeutralSpawn.asm / g_mods_*.bin); only absolute address fields")
    out.append("# were re-resolved to this build's symbols.")
    out.append(f"# _SDA_BASE_  (r13) this build = 0x{r13:08X}")
    out.append(f"# _SDA2_BASE_ (r2)  this build = 0x{r2:08X}")
    out.append("")

    groups_rendered = []
    for group in GROUPS:
        raw_lines, code_lines, addrmaps = build_group(group, sym, bases)
        groups_rendered.append((group, raw_lines, code_lines, addrmaps))

    out.append("# ---- Address map (vanilla -> symbol -> this build) ----")
    for group, _raw, _code, addrmaps in groups_rendered:
        out.append(f"#  == {group['code_label']} ==")
        for hook, addrmap in addrmaps:
            out.append(f"#   {hook['name']}:")
            for what, tag, van, sh in addrmap:
                out.append(f"#     {what:16} {tag:30} 0x{van:08X} -> 0x{sh:08X}")
    out.append("")

    out.append("# ============ (a) Raw code listing ============")
    for group, raw_lines, _code, _am in groups_rendered:
        out.append(f"# ---- {group['code_label']} ----")
        out.append(f"#   source: {group['source']}")
        out.extend("# " + ln if ln else "#" for ln in raw_lines)
    out.append("")

    out.append("# ============ (b) Dolphin GameSettings INI blocks ============")
    out.append("# Drop these into Sys/GameSettings/GALE01r2.ini (see")
    out.append("# docs/ucf-readdressing.md and docs/venue-codes-readdressing.md).")
    out.append("[Gecko]")
    for group, _raw, code_lines, _am in groups_rendered:
        out.append(f"${group['code_label']}")
        out.extend(code_lines)
    out.append("[Gecko_Enabled]")
    for group, _raw, _code, _am in groups_rendered:
        out.append(f"${group['code_label']}")
    out.append("")
    return "\n".join(out)


def verify_group(group, bin_path, *, whole_file):
    """Reconstruct one group's blob from the templates (vanilla addresses) and
    byte-compare to the real source bytes -- ultimate fidelity proof.
    whole_file=True compares the entire file (UCF g_ucf.bin is exactly the four
    hooks); otherwise compares only the leading len(blob) bytes (the Neutral
    Spawns hook is the first code of a larger g_mods_*.bin)."""
    blob = bytearray()
    for hook in group["hooks"]:
        c2, n, words, _ = resolve_hook(hook, {}, VANILLA_BASES, vanilla=True)
        blob += struct.pack(">II", c2, n)
        for w in words:
            blob += struct.pack(">I", w)
    real = Path(bin_path).read_bytes()
    ref = real if whole_file else real[:len(blob)]
    label = group["code_label"]
    if whole_file and len(real) != len(blob):
        print(f"verify[{label}]: MISMATCH length ({len(blob)} vs {len(real)} bytes)",
              file=sys.stderr)
        return False
    if bytes(blob) == ref:
        scope = "byte-identical" if whole_file else f"byte-identical to first {len(blob)} bytes of"
        print(f"verify[{label}]: OK -- reconstructed blob is {scope} {bin_path} "
              f"({len(blob)} bytes)")
        return True
    print(f"verify[{label}]: MISMATCH", file=sys.stderr)
    for i in range(0, min(len(blob), len(ref)), 4):
        a = struct.unpack_from(">I", blob, i)[0]
        b = struct.unpack_from(">I", ref, i)[0]
        if a != b:
            print(f"  @0x{i:X}: reconstructed {a:08X} != real {b:08X}", file=sys.stderr)
    return False


def group_by_label(sub):
    for g in GROUPS:
        if sub.lower() in g["code_label"].lower():
            return g
    return None


def main():
    ap = argparse.ArgumentParser(description="Re-address venue Gecko codes (UCF + Neutral Spawns) for the shifted DOL.")
    ap.add_argument("--elf", type=Path, default=REPO_ROOT / "build/GALE01/main.elf",
                    help="linked ELF to read shifted symbols from")
    ap.add_argument("--out", type=Path, default=REPO_ROOT / "build/GALE01/ucf_codes.gecko.txt",
                    help="output file for the re-addressed codes")
    ap.add_argument("--sda-symbol", default="_SDA_BASE_",
                    help="symbol giving r13 in the ELF (default _SDA_BASE_)")
    ap.add_argument("--sda2-symbol", default="_SDA2_BASE_",
                    help="symbol giving r2 in the ELF (default _SDA2_BASE_)")
    ap.add_argument("--verify-bin", type=Path, default=None,
                    help="path to stock g_ucf.bin; reconstruct UCF group + byte-compare (whole file) and exit")
    ap.add_argument("--verify-ns-bin", type=Path, default=None,
                    help="path to a g_mods_*.bin; reconstruct Neutral Spawns group + byte-compare (leading bytes) and exit")
    args = ap.parse_args()

    selfcheck()  # template fidelity is checked on every run

    if args.verify_bin or args.verify_ns_bin:
        ok = True
        if args.verify_bin:
            ok &= verify_group(group_by_label("UCF"), args.verify_bin, whole_file=True)
        if args.verify_ns_bin:
            ok &= verify_group(group_by_label("Neutral Spawns"), args.verify_ns_bin,
                               whole_file=False)
        sys.exit(0 if ok else 1)

    if not args.elf.exists():
        die(f"ELF not found: {args.elf} (build the DOL first)")
    sym = load_elf_symbols(args.elf)
    if args.sda_symbol not in sym:
        die(f"'{args.sda_symbol}' not in ELF symbols; cannot resolve r13 for SDA loads")
    if args.sda2_symbol not in sym:
        die(f"'{args.sda2_symbol}' not in ELF symbols; cannot resolve r2 for SDA2 loads")
    bases = {"r13": sym[args.sda_symbol], "r2": sym[args.sda2_symbol]}

    text = render(sym, bases)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")
    print(text)
    print(f"\ngen_ucf_codes: wrote {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
