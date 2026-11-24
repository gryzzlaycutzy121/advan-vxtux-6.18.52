#!/usr/bin/env python3
"""
regbit_diff2.py — register/bit-level diff per-fungsi (v2).

Build beda kernel/ABI -> jumlah fungsi & inlining beda. Yang harus identik
adalah OPERASI REGISTER/BIT pada hardware:
  - offset [xN,#imm] pada ldr/str   (register offset MMIO / field struct)
  - mask pada and/orr/eor/tst       (bit mask)
  - konstanta mov/movz/movk         (nilai register)

Metode:
  1. Disasm kedua .ko (llvm-objdump --no-show-raw-insn).
  2. Per FUNGSI: kumpulkan offset/mask/imm.
  3. Cocokkan fungsi by nama; untuk fungsi yang ADA DI KEDUA SISI bandingkan.
  4. Verdict:
       - PARITY : semua fungsi yang match punya offset+mask identik
       - EQUIV  : sebagian kecil beda (perlu tinjau)
       - DIFF   : banyak fungsi sama yang operasi register/bit-nya beda
  5. Detail mismatch ke regbit_report.json.
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

def per_fn(insns):
    fnmap = collections.defaultdict(lambda: (set(), set(), set()))
    for fn, mn, op in insns:
        off, mask, imm = fnmap[fn]
        if mn in ("ldr", "str", "ldrb", "strb", "ldrh", "strh", "ldrsw"):
            for m in re.finditer(r'\[x[0-9]+, #(0x[0-9a-f]+|[0-9]+)\]', op):
                try: off.add(int(m.group(1), 0))
                except ValueError: pass
        if mn in ("and", "orr", "eor", "ands", "bic", "tst"):
            for t in re.findall(r'#(0x[0-9a-f]+|[0-9]+)', op):
                try: mask.add(int(t, 0))
                except ValueError: pass
        if mn in ("mov", "movz", "movk", "add", "sub", "cmp", "cmn"):
            for t in re.findall(r'#(0x[0-9a-f]+|[0-9]+)', op):
                try: imm.add(int(t, 0))
                except ValueError: pass
    return fnmap

def fn_equal(a, b):
    ao, am, ai = a
    bo, bm, bi = b
    off_eq = (ao == bo)
    mask_eq = (am == bm)
    imm_eq = (ai == bi) or (len(ai) >= 6 and len(bi) >= 6 and
                            len(ai & bi) >= max(3, int(0.6 * min(len(ai), len(bi)))))
    return off_eq, mask_eq, imm_eq

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
            report.append({"mod": stock[k], "verdict": "NO-DISASM"})
            stats["NO-DISASM"] += 1
            continue
        fa, fb = per_fn(parse_insn(ta)), per_fn(parse_insn(tb))
        common = sorted(set(fa) & set(fb))
        if not common:
            report.append({"mod": stock[k], "verdict": "NO-COMMON-FN",
                           "stock_fn": len(fa), "port_fn": len(fb)})
            stats["NO-COMMON-FN"] += 1
            continue
        mism = []
        for fn in common:
            oe, me, ie = fn_equal(fa[fn], fb[fn])
            if not (oe and me):
                mism.append({"fn": fn, "off_eq": oe, "mask_eq": me,
                             "stock_off": sorted(fa[fn][0]),
                             "port_off": sorted(fb[fn][0]),
                             "stock_mask": sorted(fa[fn][1]),
                             "port_mask": sorted(fb[fn][1])})
        if not mism:
            verdict = "PARITY"
        else:
            frac = len(mism) / len(common)
            verdict = "DIFF" if frac > 0.3 else "EQUIV"
        report.append({"mod": stock[k], "port": pp, "verdict": verdict,
                       "common_fn": len(common), "mismatch_fn": len(mism),
                       "stock_fn": len(fa), "port_fn": len(fb),
                       "detail": mism[:12]})
        stats[verdict] += 1
    print(json.dumps({"pairs": len(pairs), "stats": dict(stats)}, indent=2))
    for r in report:
        print(f"{r['verdict']:8s} {r['mod']:38s} common={r.get('common_fn','-'):>4} "
              f"mism={r.get('mismatch_fn','-'):>3} "
              f"(stock {r.get('stock_fn','-')} fn / port {r.get('port_fn','-')} fn)")
    with open(r"E:/vxtux_build_repo/regbit_report.json", "w") as fh:
        json.dump(report, fh, indent=2, default=str)

if __name__ == "__main__":
    main()
