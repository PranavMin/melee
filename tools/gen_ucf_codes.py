#!/usr/bin/env python3
###
# Regenerate the UCF 0.8 Gecko codes, re-addressed for THIS build's shifted DOL.
#
# Background (see docs/ucf-readdressing.md and docs/ucf-investigation.md):
#   Slippi Nintendont ships UCF 0.8 as four Gecko "C2" (insert-PowerPC-asm) hooks
#   in kernel/gecko/g_ucf.bin. Every address they bake in is vanilla GALE01 v1.02.
#   Our decomp DOL is SHIFTED, so applying the vanilla-addressed hooks crashes
#   (design.md R12 / R11). The asm itself is fine -- only its hardcoded addresses
#   are wrong.
#
# This tool keeps UCF's EXACT asm bytes and only re-addresses the fields that
# encode an absolute vanilla address:
#   * the C2 insert address (a function+offset) of each of the 4 hooks,
#   * every lis/ori pair that builds an absolute data or function pointer,
#   * every `lwz rX, disp(r13)` small-data (SDA) load of a game global.
# Each vanilla address was resolved to a decomp SYMBOL (+delta) via
# config/GALE01/symbols.txt; this tool re-resolves those symbols to the current
# build's addresses in build/GALE01/main.elf and re-encodes just those fields.
# Relative branches (b/bc/bl) inside every payload are self-contained and are
# NEVER touched; the copied original (overwritten) instruction is register-
# relative and is never touched. Nothing is re-derived -- the emitted codes are
# byte-for-byte the real UCF except for the re-addressed immediate fields.
#
# Stdlib only (matches the repo's tooling style; the ELF32-BE symtab is parsed
# here directly, no external deps).
#
# Usage:
#   python tools/gen_ucf_codes.py                 # uses build/GALE01/main.elf
#   python tools/gen_ucf_codes.py --elf X --out Y
#   python tools/gen_ucf_codes.py --verify-bin \
#       P:/Projects/Nintendont/kernel/gecko/g_ucf.bin   # prove byte-fidelity
###

import argparse
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# UCF 0.8 templates -- the EXACT words of Nintendont/kernel/gecko/g_ucf.bin.
# Each hook carries its full C2 payload (the last word of each payload is the
# codehandler's branch-back placeholder, kept verbatim). `header` and `fixups`
# name the fields that encode an absolute vanilla address; everything else is
# emitted unchanged. Word indices are 0-based into the payload word list.
#   fixup kinds:
#     ("abs", hi_idx, lo_idx, symbol, delta) -- lis rX,hi ; ori rX,rX,lo
#     ("sda", idx, symbol)                   -- lwz/... rX, disp(r13)
# `vanilla` values are the addresses these fields hold in the stock blob; they
# are used only to self-check the templates and to reconstruct the vanilla blob
# in --verify mode. Re-encoding a fixup with its vanilla address must reproduce
# the original word (asserted at startup) -- that is the fidelity guarantee.
# ---------------------------------------------------------------------------

# r13 (_SDA_BASE_) in stock GALE01 v1.02. 0x804DB6A0 - 0x514C == 0x804D6554 ==
# p_ftCommonData, which is exactly what every UCF SDA load reaches (confirmed by
# the downstream `lfs f2, 0x3C(r4)` = p_ftCommonData->dash_smash_stick_threshold).
VANILLA_R13 = 0x804DB6A0

HOOKS = [
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


def enc_sda(word, addr, r13):
    """lwz/... rX, (addr - r13)(r13)."""
    disp = (addr - r13) & 0xFFFF
    return (word & 0xFFFF0000) | disp


def enc_c2_header(addr):
    return 0xC2000000 | (addr & 0x01FFFFFF)


def resolve_hook(hook, sym, r13, *, vanilla=False):
    """Return (c2_word, n, words[], addrmap[]) for one hook.
    vanilla=True re-encodes with the stock addresses (must reproduce the
    template exactly); otherwise uses the shifted addresses from `sym`."""
    words = list(hook["words"])
    addrmap = []

    # C2 header
    hsym, hdelta = hook["header"]
    if vanilla:
        haddr = hook["header_vanilla"]
    else:
        if hsym not in sym:
            die(f"symbol '{hsym}' not found in ELF (hook {hook['name']})")
        haddr = sym[hsym] + hdelta
    c2 = enc_c2_header(haddr)
    addrmap.append(("C2 insert", f"{hsym}+0x{hdelta:X}", hook["header_vanilla"], haddr))

    for fx in hook["fixups"]:
        if fx[0] == "abs":
            _, hi_idx, lo_idx, s, delta, van = fx
            addr = van if vanilla else _need(sym, s, hook) + delta
            words[hi_idx], words[lo_idx] = enc_abs(words[hi_idx], words[lo_idx], addr)
            tag = f"{s}+0x{delta:X}" if delta else s
            addrmap.append((f"lis/ori w{hi_idx}/{lo_idx}", tag, van, addr))
        elif fx[0] == "sda":
            _, idx, s, van = fx
            addr = van if vanilla else _need(sym, s, hook)
            words[idx] = enc_sda(words[idx], addr, r13)
            addrmap.append((f"r13 load w{idx}", s, van, addr))
        else:
            die(f"unknown fixup kind {fx[0]}")
    return c2, len(hook["words"]) // 2, words, addrmap


def _need(sym, name, hook):
    if name not in sym:
        die(f"symbol '{name}' not found in ELF (hook {hook['name']})")
    return sym[name]


def selfcheck():
    """Re-encoding every fixup with its vanilla address must reproduce the
    stock template word exactly -- proves the fixup table (indices/kinds) is
    correct and that the tool only ever changes the addressed fields."""
    for hook in HOOKS:
        c2, n, words, _ = resolve_hook(hook, {}, VANILLA_R13, vanilla=True)
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
def build_codes(sym, r13):
    raw_lines = []       # annotated raw listing
    code_lines = []      # bare "AAAAAAAA BBBBBBBB" lines (all hooks)
    all_addrmaps = []
    for hook in HOOKS:
        c2, n, words, addrmap = resolve_hook(hook, sym, r13)
        all_addrmaps.append((hook, addrmap))
        raw_lines.append(f"  * {hook['purpose']}")
        raw_lines.append(f"    {c2:08X} {n:08X}")
        code_lines.append(f"{c2:08X} {n:08X}")
        for i in range(0, len(words), 2):
            a, b = words[i], words[i + 1]
            raw_lines.append(f"    {a:08X} {b:08X}")
            code_lines.append(f"{a:08X} {b:08X}")
        raw_lines.append("")
    return raw_lines, code_lines, all_addrmaps


def render(sym, r13):
    raw_lines, code_lines, all_addrmaps = build_codes(sym, r13)
    out = []
    out.append("# UCF 0.8 Gecko codes, re-addressed for this build's shifted DOL.")
    out.append("# Generated by tools/gen_ucf_codes.py -- do not edit by hand.")
    out.append("# Faithful to Nintendont/kernel/gecko/g_ucf.bin; only absolute")
    out.append("# address fields were re-resolved to this build's symbols.")
    out.append(f"# _SDA_BASE_ (r13) this build = 0x{r13:08X}")
    out.append("")
    out.append("# ---- Address map (vanilla -> symbol -> this build) ----")
    for hook, addrmap in all_addrmaps:
        out.append(f"#  {hook['name']}:")
        for what, tag, van, sh in addrmap:
            out.append(f"#    {what:16} {tag:26} 0x{van:08X} -> 0x{sh:08X}")
    out.append("")
    out.append("# ============ (a) Raw code listing ============")
    out.extend("# " + ln if ln else "#" for ln in raw_lines)
    out.append("")
    out.append("# ============ (b) Dolphin GameSettings INI blocks ============")
    out.append("# Drop these into Sys/GameSettings/GALE01r2.ini (see docs/ucf-readdressing.md).")
    out.append("[Gecko]")
    out.append("$UCF 0.8 (re-addressed)")
    out.extend(code_lines)
    out.append("[Gecko_Enabled]")
    out.append("$UCF 0.8 (re-addressed)")
    out.append("")
    return "\n".join(out)


def verify_against(bin_path, r13_vanilla=VANILLA_R13):
    """Reconstruct the stock blob from the templates (vanilla addresses) and
    byte-compare to the real g_ucf.bin -- ultimate fidelity proof."""
    blob = bytearray()
    for hook in HOOKS:
        c2, n, words, _ = resolve_hook(hook, {}, r13_vanilla, vanilla=True)
        blob += struct.pack(">II", c2, n)
        for w in words:
            blob += struct.pack(">I", w)
    real = Path(bin_path).read_bytes()
    if bytes(blob) == real:
        print(f"verify: OK -- reconstructed blob is byte-identical to {bin_path} "
              f"({len(real)} bytes)")
        return True
    print(f"verify: MISMATCH ({len(blob)} vs {len(real)} bytes)", file=sys.stderr)
    for i in range(0, min(len(blob), len(real)), 4):
        a = struct.unpack_from(">I", blob, i)[0]
        b = struct.unpack_from(">I", real, i)[0]
        if a != b:
            print(f"  @0x{i:X}: reconstructed {a:08X} != real {b:08X}", file=sys.stderr)
    return False


def main():
    ap = argparse.ArgumentParser(description="Re-address UCF 0.8 Gecko codes for the shifted DOL.")
    ap.add_argument("--elf", type=Path, default=REPO_ROOT / "build/GALE01/main.elf",
                    help="linked ELF to read shifted symbols from")
    ap.add_argument("--out", type=Path, default=REPO_ROOT / "build/GALE01/ucf_codes.gecko.txt",
                    help="output file for the re-addressed codes")
    ap.add_argument("--sda-symbol", default="_SDA_BASE_",
                    help="symbol giving r13 in the ELF (default _SDA_BASE_)")
    ap.add_argument("--verify-bin", type=Path, default=None,
                    help="path to stock g_ucf.bin; reconstruct+byte-compare and exit")
    args = ap.parse_args()

    selfcheck()  # template fidelity is checked on every run

    if args.verify_bin:
        ok = verify_against(args.verify_bin)
        sys.exit(0 if ok else 1)

    if not args.elf.exists():
        die(f"ELF not found: {args.elf} (build the DOL first)")
    sym = load_elf_symbols(args.elf)
    if args.sda_symbol not in sym:
        die(f"'{args.sda_symbol}' not in ELF symbols; cannot resolve r13 for SDA loads")
    r13 = sym[args.sda_symbol]

    text = render(sym, r13)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")
    print(text)
    print(f"\ngen_ucf_codes: wrote {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
