#!/usr/bin/env python3
"""
regbit_reg_diff2.py — diff register/bit level-SOURCE dengan resolusi modul
komposit (modul = banyak .c via Makefile -y list).

Alur per modul:
  1. Resolusi daftar source:
     stock : Makefile stock yang punya baris '<mod>.o' atau '<mod>-y :='
     port  : Makefile port yang sama
     tiap objek -> cari .c-nya di dir tersebut + subdir-nya
  2. Kumpulkan konstanta register (argumen regmap_write/update_bits/read,
     writel/readl BASE+OFF) dari SEMUA source modul (union).
  3. Bandingkan: konstanta register stock yang hilang di port = defect port.
     - REGISTER-PRESERVED : union port mencofer union stock (register/bit utuh)

Keluaran: regbit_reg_report.json (v2, termasuk modul komposit)
"""
import os, re, json, subprocess, collections

STOCK_SRC = r"E:/Advan/out/odm54"
PORT_SRC = r"E:/vxtux_build_repo/6.18.52-VXTux-UMS512"
STOCK_KO = r"E:/Advan/stock/vendor_modules"
PORT_KO = r"E:/vxtux_build_repo/dl_kos_242/extracted"

def norm(n):
    return re.sub(r'[^a-z0-9]', '', n.lower().replace('.ko', ''))

def _walk(root, pattern):
    """Cari semua path yang ekstensinya .c yang basename-nya = pattern (tanpa .c)."""
    hits = []
    for dirpath, _, files in os.walk(root):
        for f in files:
            if not f.endswith('.c'):
                continue
            if norm(f.replace('.c', '')) == pattern:
                hits.append(os.path.join(dirpath, f))
    return hits

def find_src_c(objname, dir_):
    """objname tanpa .o -> cari <obj>.c di dir_ atau subdir."""
    name = objname.replace('.o', '')
    hits = _walk(dir_, name)
    return hits

def find_makefiles(root):
    out = []
    for dirpath, _, files in os.walk(root):
        if 'Makefile' in files:
            out.append(os.path.join(dirpath, 'Makefile'))
    return out

def resolve_module_sources(mod, root):
    """Cari semua source .c yang membentuk modul 'mod' dalam tree root."""
    srcs = []
    for mk in find_makefiles(root):
        try:
            t = open(mk, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        d = os.path.dirname(mk)
        # baris 'mod.o' (kbuild non-composite)
        for m in re.finditer(r'^\s*(?:obj-\$\(CONFIG_[A-Z0-9_]+\)\s*\+?=\s*)?' +
                             re.escape(mod) + r'\.o\b', t, re.M):
            s = find_src_c(mod, d)
            srcs.extend(s)
        # baris 'mod-y := obj1.o obj2.o ...' (composite) — bisa multi baris
        for m in re.finditer(r'^\s*' + re.escape(mod) + r'-y\s*[+]?=\s*(.*?)(?=^\s*\S+\s*-y\s*[+]?=|^\s*\S+\s*-objs|^[^\s#\\]|\Z)',
                             t, re.M | re.S):
            body = m.group(1)
            for om in re.finditer(r'([A-Za-z0-9_\-./]+)\.o', body):
                srcs.extend(find_src_c(om.group(1), d))
        # 'mod-objs :=' varianta
        for m in re.finditer(r'^\s*' + re.escape(mod) + r'-objs\s*:=\s*(.*?)(?=^\s*\S+\s*-objs|^[^\s#\\]|\Z)',
                             t, re.M | re.S):
            body = m.group(1)
            for om in re.finditer(r'([A-Za-z0-9_\-./]+)\.o', body):
                srcs.extend(find_src_c(om.group(1), d))
    return sorted(set(srcs))

def reg_consts_all(paths):
    offs, masks = set(), set()
    for path in paths:
        try:
            t = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for m in re.finditer(r'regmap_(?:write|update_bits|update_bits_base|read)\s*\(', t):
            seg = t[m.end():m.end()+400]
            args = split_args(seg)
            if len(args) >= 2:
                c = parse_const(args[1])
                if c is not None: offs.add(c)
            if 'update_bits' in m.group(0) and len(args) >= 3:
                c = parse_const(args[2])
                if c is not None: masks.add(c)
        for m in re.finditer(r'\b(?:writel|readl|writeb|readb)\s*\(', t):
            seg = t[m.end():m.end()+200]
            args = split_args(seg)
            if len(args) >= 2:
                for c in find_offsets(args[1]):
                    offs.add(c)
    return offs, masks

def split_args(seg):
    args, depth, cur = [], 0, ""
    for ch in seg:
        if ch in '([':
            depth += 1; cur += ch
        elif ch in ')]':
            depth -= 1
            if depth < 0: break
            cur += ch
        elif ch == ',' and depth == 0:
            args.append(cur); cur = ""
        else:
            cur += ch
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
    out = []
    for m in re.finditer(r'\+[ ]*(0[xX][0-9a-fA-F]+|[0-9]+)\b', expr):
        try: out.append(int(m.group(1), 0))
        except ValueError: pass
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
        ko_name = stock_ko[k]           # mis. 'apsys-dvfs.ko'
        mod_raw = ko_name[:-3] if ko_name.endswith('.ko') else ko_name
        ss = resolve_module_sources(mod_raw, STOCK_SRC)
        ps = resolve_module_sources(mod_raw, PORT_SRC)
        if not ss or not ps:
            report.append({"mod": k, "verdict": "NO-SOURCE-MATCH",
                           "n_stock_src": len(ss), "n_port_src": len(ps)})
            stats["NO-SOURCE-MATCH"] += 1
            continue
        a_off, a_mask = reg_consts_all(ss)
        b_off, b_mask = reg_consts_all(ps)
        m_off = sorted(a_off - b_off)
        m_mask = sorted(a_mask - b_mask)
        if not m_off and not m_mask:
            verdict = "REGISTER-PRESERVED"
        else:
            verdict = "REG-MISSING"
        report.append({"mod": k, "verdict": verdict,
                       "n_stock_src": len(ss), "n_port_src": len(ps),
                       "stock_off": len(a_off), "port_off": len(b_off),
                       "stock_mask": len(a_mask), "port_mask": len(b_mask),
                       "n_miss_off": len(m_off), "n_miss_mask": len(m_mask),
                       "miss_off": m_off[:24], "miss_mask": m_mask[:24],
                       "stock_srcs": ss, "port_srcs": ps})
        stats[verdict] += 1
    print(json.dumps({"pairs": len(pairs), "stats": dict(stats)}, indent=2))
    for r in report:
        v = r["verdict"]
        if v == "REGISTER-PRESERVED":
            print(f"OK    {r['mod']:32s} src={r['n_stock_src']}/{r['n_port_src']} "
                  f"off={r['stock_off']}->{r['port_off']} mask={r['stock_mask']}->{r['port_mask']}")
        elif v == "NO-SOURCE-MATCH":
            print(f"NONE  {r['mod']:32s} no source resolved (stock {r['n_stock_src']}, port {r['n_port_src']})")
        else:
            print(f"CHECK {r['mod']:32s} src={r['n_stock_src']}/{r['n_port_src']} "
                  f"miss_off={r['n_miss_off']} miss_mask={r['n_miss_mask']} "
                  f"mo={r['miss_off'][:5]} mm={r['miss_mask'][:5]}")
    with open(r"E:/vxtux_build_repo/regbit_reg_report.json", "w") as fh:
        json.dump(report, fh, indent=2, default=str)

if __name__ == "__main__":
    main()