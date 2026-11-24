#!/usr/bin/env python3
"""
regbit_diff.py — register/bit-level diff antara stock .ko (5.4.210 OEM)
dan port .ko (6.18.52), berbasis disassembly AArch64.

Metode:
  Untuk tiap pasangan modul (normalized-name match):
  1. llvm-objdump -d -> daftar instruksi.
  2. Ekstrak "fingerprint register/bit":
       - MMIO_OFF : set offset [xN,#imm] pada ldr/str (register offset akses MMIO/struct)
       - IMM      : set konstanta mov/movz/movk/add/sub/cmp
       - MASK     : set mask pada and/orr/eor/tst (bit mask)
       - FN       : set nama fungsi .text
  3. Bandingkan: stock vs port.
     Verdict per modul:
       REGISTER-EXACT  : himpunan offset & mask kunci identik (dalam noise API)
       DIFF            : ada perbedaan yang perlu ditinjau
       NO-FN           : tidak ada fungsi sebanding (modul beda peran)

  Catatan: build beda kernel/ABI -> offset struct kernel boleh beda; yang
  harus identik adalah offset register MMIO vendor dan bit mask vendor.
  Karena itu verdict utama memakai MASK + konstanta vendor yang stabil.
"""
import sys, os, re, json, subprocess, collections

OBJDUMP = r"E:/bfkrt/LLVM-12-win64/bin/llvm-objdump.exe"
STOCK = r"E:/Advan/stock/vendor_modules"
PORT = r"E:/vxtux_build_repo/dl_kos_242/extracted"

def norm(n):
    return re.sub(r'[^a-z0-9]', '', n.lower().replace('.ko', ''))

def disasm(path):
    r = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", path],
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        return None
    return r.stdout

def parse_insn(text):
    """Kembalikan list (fn, mnemo, operands)."""
    out = []
    cur = None
    for line in text.splitlines():
        if re.match(r'^[0-9a-f]+ <.*>:$', line.strip()):
            cur = line.split('<')[1].rstrip('>:')
            continue
        m = re.match(r'^\s+[0-9a-f]+:\s*([a-z][a-z0-9]*)\s*(.*)$', line)
        if m:
            out.append((cur, m.group(1), m.group(2)))
    return out

def fingerprint(insns):
    mmio_off = set()
    imm = set()
    mask = set()
    fns = set()
    for fn, mn, op in insns:
        fns.add(fn)
        if mn in ("ldr", "str", "ldrb", "strb", "ldrh", "strh", "ldrsw"):
            m = re.search(r'\[x[0-9]+, #(0x[0-9a-f]+|[0-9]+)\]', op)
            if m:
                try:
                    v = int(m.group(1), 0)
                    mmio_off.add(v)
                except ValueError:
                    pass
        if mn in ("mov", "movz", "movk", "add", "sub", "cmp", "cmn", "adds", "subs"):
            for t in re.findall(r'#(0x[0-9a-f]+|[0-9]+)', op):
                try:
                    imm.add(int(t, 0))
                except ValueError:
                    pass
        if mn in ("and", "orr", "eor", "ands", "bic", "tst"):
            for t in re.findall(r'#(0x[0-9a-f]+|[0-9]+)', op):
                try:
                    mask.add(int(t, 0))
                except ValueError:
                    pass
    return {"mmio_off": mmio_off, "imm": imm, "mask": mask, "fns": fns}

def sig(s):
    return f"fns={len(s['fns'])} off={len(s['mmio_off'])} imm={len(s['imm'])} mask={len(s['mask'])}"

def verdict(a, b):
    # inti register/bit: offset MMIO & bit mask harus overlap kuat
    off_i = len(a["mmio_off"] & b["mmio_off"])
    mask_i = len(a["mask"] & b["mask"])
    off_a, off_b = len(a["mmio_off"]), len(b["mmio_off"])
    mask_a, mask_b = len(a["mask"]), len(b["mask"])
    if off_a == 0 and off_b == 0:
        return "NO-MMIO"
    # skor: berapa % offset & mask yang sama (di sisi terkecil)
    off_lo = min(off_a, off_b) or 1
    mask_lo = min(mask_a, mask_b) or 1
    off_r = off_i / off_lo
    mask_r = mask_i / mask_lo
    if off_r >= 0.8 and mask_r >= 0.8:
        return "REGISTER-EXACT"
    if off_r >= 0.5 and mask_r >= 0.5:
        return "PARTIAL"
    return "DIFF"

def main():
    stock = {norm(f): f for f in os.listdir(STOCK) if f.endswith('.ko')}
    port = {}
    for root, d, fs in os.walk(PORT):
        for f in fs:
            if f.endswith('.ko'):
                port.setdefault(norm(f), os.path.join(root, f))
    pairs = sorted(set(stock) & set(port))
    report = []
    stats = collections.Counter()
    for k in pairs:
        sp, pp = os.path.join(STOCK, stock[k]), port[k]
        ta, tb = disasm(sp), disasm(pp)
        if ta is None or tb is None:
            report.append({"mod": stock[k], "port": pp, "verdict": "NO-DISASM",
                           "stock": None, "port": None})
            stats["NO-DISASM"] += 1
            continue
        fa, fb = fingerprint(parse_insn(ta)), fingerprint(parse_insn(tb))
        v = verdict(fa, fb)
        report.append({"mod": stock[k], "port": pp, "verdict": v,
                       "stock": sig(fa), "port": sig(fb),
                       "off_i": len(fa["mmio_off"] & fb["mmio_off"]),
                       "mask_i": len(fa["mask"] & fb["mask"])})
        stats[v] += 1
    print(json.dumps({"pairs": len(pairs), "stats": dict(stats)}, indent=2))
    for r in report:
        print(f"{r['verdict']:16s} {r['mod']:40s} {r.get('stock','')[:60]} | {r.get('port','')[:60]}")
    with open(r"E:/vxtux_build_repo/regbit_report.json", "w") as fh:
        json.dump(report, fh, indent=2, default=str)

if __name__ == "__main__":
    main()
