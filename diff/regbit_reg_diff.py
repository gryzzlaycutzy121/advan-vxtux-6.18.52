#!/usr/bin/env python3
"""
regbit_reg_diff.py — diff konstanta REGISTER sejati (offset regmap + mask).

Hanya konstanta yang muncul sebagai ARGUMEN fungsi akses register yang dihitung:
  regmap_write(_, OFFSET, VAL)      -> OFFSET (register), VAL (nilai)
  regmap_update_bits(_, OFFSET, MASK, VAL) -> OFFSET, MASK (bit mask), VAL
  regmap_read(_, OFFSET, _)         -> OFFSET
  writel(VAL, BASE + OFFSET)        -> OFFSET
  readl(BASE + OFFSET)              -> OFFSET
  sc27xx regmap wrappers: sama pola argumen.

Ini bukti "persis by register dan bit": himpunan OFFSET register & MASK bit
yang dipakai driver di port harus SUPERSET dari stock (porting tidak boleh
menghapus akses register, hanya menambah/adaptasi API).

Keluaran: regbit_reg_report.json
"""
import os, re, json, subprocess, collections

STOCK_SRC = r"E:/Advan/out/odm54"
PORT_SRC = r"E:/vxtux_build_repo/6.18.52-VXTux-UMS512"
STOCK_KO = r"E:/Advan/stock/vendor_modules"
PORT_KO = r"E:/vxtux_build_repo/dl_kos_242/extracted"

REGFN = r"(?:regmap_write|regmap_update_bits|regmap_read|regmap_update_bits_base|" \
        r"regmap_write_bits|writel|readl|writeb|readb|sc27xx_regmap_write|" \
        r"sc2730_regmap_write|sprd_regmap_write|sprd_regmap_update_bits)"

def norm(n):
    return re.sub(r'[^a-z0-9]', '', n.lower().replace('.ko', ''))

def find_source(name, roots):
    for root in roots:
        for dirpath, _, files in os.walk(root):
            for f in files:
                if f.endswith('.c') and norm(f.replace('.c', '')) == name:
                    return os.path.join(dirpath, f)
    return None

def reg_consts(path):
    try:
        t = open(path, encoding='utf-8', errors='replace').read()
    except OSError:
        return None
    offs, masks, vals = set(), set(), set()
    # regmap_write(regmap, OFF, VAL) / update_bits(regmap, OFF, MASK, VAL)
    for m in re.finditer(r'regmap_(?:write|update_bits|update_bits_base|read)\s*\(', t):
        # ambil sampai ');' atau maksimal 4 argumen
        seg = t[m.end():m.end()+400]
        args = split_args(seg)
        if len(args) >= 2:
            c = parse_const(args[1])
            if c is not None: offs.add(c)
        if 'update_bits' in m.group(0) and len(args) >= 3:
            c = parse_const(args[2])
            if c is not None: masks.add(c)
    # writel/readl pola: writel(VAL, BASE + OFF)
    for m in re.finditer(r'\b(?:writel|readl|writeb|readb)\s*\(', t):
        seg = t[m.end():m.end()+200]
        args = split_args(seg)
        # argumen kedua berisi base + OFFSET
        if len(args) >= 2:
            for c in find_offsets(args[1]):
                offs.add(c)
    return {"off": offs, "mask": masks}

def split_args(seg):
    """Pisahkan argumen di seg sampai tutup kurung seimbang; return list string."""
    args, depth, cur, i = [], 0, "", 0
    while i < len(seg):
        ch = seg[i]
        if ch in '([': depth += 1; cur += ch
        elif ch in ')]':
            depth -= 1
            if depth < 0: break
            cur += ch
        elif ch == ',' and depth == 0:
            args.append(cur); cur = ""
        else:
            cur += ch
        i += 1
    args.append(cur)
    return args

def parse_const(s):
    s = s.strip()
    m = re.match(r'^(0[xX][0-9a-fA-F]+|[0-9]+)\b', s)
    if m:
        try: return int(m.group(1), 0)
        except ValueError: return None
    return None

def find_offsets(expr):
    """Cari OFFSET dalam ekspresi base + OFF / base->reg / &base[OFF]."""
    out = []
    # BASE + 0xOFF
    for m in re.finditer(r'\+[ ]*(0[xX][0-9a-fA-F]+|[0-9]+)\b', expr):
        try: out.append(int(m.group(1), 0))
        except ValueError: pass
    # &base[OFF] atau base[OFF]
    for m in re.finditer(r'\[[ ]*(0[xX][0-9a-fA-F]+|[0-9]+)[ ]*\]', expr):
        try: out.append(int(m.group(1), 0))
        except ValueError: pass
    return out

def main():
    stock_ko = {norm(f): f for f in os.listdir(STOCK_KO) if f.endswith('.ko')}
    port_ko = {}
    for root, d, fs in os.walk(PORT_KO):
        for f in fs:
            if f.endswith('.ko'):
                port_ko.setdefault(norm(f), os.path.join(root, f))
    pairs = sorted(set(stock_ko) & set(port_ko))
    report = []
    stats = collections.Counter()
    for k in pairs:
        ss = find_source(k, [STOCK_SRC])
        ps = find_source(k, [PORT_SRC])
        if not ss or not ps:
            report.append({"mod": k, "verdict": "NO-SOURCE-MATCH"})
            stats["NO-SOURCE-MATCH"] += 1
            continue
        a, b = reg_consts(ss), reg_consts(ps)
        if a is None or b is None:
            report.append({"mod": k, "verdict": "NO-SOURCE-MATCH"})
            stats["NO-SOURCE-MATCH"] += 1
            continue
        # register offset & mask stock yang HILANG di port
        m_off = sorted(a["off"] - b["off"])
        m_mask = sorted(a["mask"] - b["mask"])
        n_off = len(a["off"]); n_mask = len(a["mask"])
        if not m_off and not m_mask:
            verdict = "REGISTER-PRESERVED"
        else:
            verdict = "REG-MISSING"
        report.append({"mod": k, "verdict": verdict,
                       "stock_src": ss, "port_src": ps,
                       "n_off": n_off, "n_mask": n_mask,
                       "miss_off": m_off[:24], "miss_mask": m_mask[:24],
                       "n_miss_off": len(m_off), "n_miss_mask": len(m_mask)})
        stats[verdict] += 1
    print(json.dumps({"pairs": len(pairs), "stats": dict(stats)}, indent=2))
    for r in report:
        v = r["verdict"]
        if v == "REGISTER-PRESERVED":
            print(f"OK    {r['mod']:34s} off={r['n_off']:>3} mask={r['n_mask']:>3}")
        else:
            if v == "NO-SOURCE-MATCH":
                print(f"NONE  {r['mod']:34s} no source match")
                continue
            print(f"CHECK {r['mod']:34s} miss_off={r.get('n_miss_off',0):>3} "
                  f"miss_mask={r.get('n_miss_mask',0):>3} off={r['n_off']} mask={r['n_mask']} "
                  f"mo={r.get('miss_off',[])[:5]} mm={r.get('miss_mask',[])[:5]}")
    with open(r"E:/vxtux_build_repo/regbit_reg_report.json", "w") as fh:
        json.dump(report, fh, indent=2, default=str)

if __name__ == "__main__":
    main()
