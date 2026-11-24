#!/usr/bin/env python3
"""
regbit_macro.py — BUKTI "persis by register dan bit" (resolusi macro).

Kenapa macro: driver vendor defines offset register sebagai macro
  #define SC27XX_TSEN_CTRL0  0x0
 lalu memakai  regmap_update_bits(map, base + SC27XX_TSEN_CTRL0, MASK, VAL)
 sehingga offset TIDAK muncul sebagai literal di call site. Harness lama
 (literal-only) salah menyimpulkan "tidak ada akses register".

Metode:
  1. Parse semua #define dari source modul -> map NAMA -> NILAI (hex/dec).
    Ikuti #include header lokal (satu level) untuk mask/offset.
  2. Ekstrak call site akses register:
       regmap_write(map, OFF, VAL)
       regmap_update_bits(map, OFF, MASK, VAL)
       regmap_read(map, OFF, &v)
       writel(VAL, base + OFF) / readl(base + OFF)
     resolved: OFF = literal atau (base_expr + MACRO) -> macro tsb.
  3. Hasil per modul: (register offset set, bit mask set) untuk stock & port.
  4. Verdict:
       REGISTER-EXACT     : offset set IDENTIK dan mask set IDENTIK
       REGISTER-SUPERSET   : port punya semua offset+mask stock (tambah boleh)
       REGISTER-DIVERGENT  : port kehilangan offset/mask stock  -> DEFECT
       NO-REG-EVIDENCE     : tidak ada akses register yang bisa dibuktikan
"""
import os, re, json, collections, sys

STOCK_SRC = r"E:/Advan/out/odm54"
PORT_SRC = r"E:/vxtux_build_repo/6.18.52-VXTux-UMS512"
STOCK_KO = r"E:/Advan/stock/vendor_modules"
PORT_KO = r"E:/vxtux_build_repo/dl_kos_242/extracted"

def norm(n):
    return re.sub(r'[^a-z0-9]', '', n.lower().replace('.ko', ''))

# ---------- index tree sekali jalan ----------
def build_index(root):
    cfiles = collections.defaultdict(list)
    headers = collections.defaultdict(list)
    mkmods = collections.defaultdict(set)
    mkdir = {}
    for dirpath, dirs, files in os.walk(root):
        for f in files:
            if f.endswith('.c'):
                cfiles[norm(f[:-2])].append(os.path.join(dirpath, f))
            elif f.endswith('.h'):
                headers[norm(f[:-2])].append(os.path.join(dirpath, f))
        if 'Makefile' in files:
            mk = os.path.join(dirpath, 'Makefile')
            try:
                t = open(mk, encoding='utf-8', errors='replace').read()
            except OSError:
                continue
            for m in re.finditer(
                    r'^\s*([A-Za-z0-9_\-./]+)-y\s*[+]?=\s*(.*?)'
                    r'(?=^\s*[A-Za-z0-9_\-./]+\s*-y\s*[+]?=|^\s*[A-Za-z0-9_\-./]+\s*-objs'
                    r'|^\s*obj-\$|^[A-Za-z]|\Z)', t, re.M | re.S):
                mkmods[m.group(1)].update(
                    x.group(1) for x in re.finditer(r'([A-Za-z0-9_\-./]+)\.o', m.group(2)))
            for m in re.finditer(
                    r'^\s*(?:obj-\$\(CONFIG_[A-Z0-9_]+\)\s*\+?=\s*|obj-m\s*\+?=\s*'
                    r'|obj-y\s*\+?=\s*)([A-Za-z0-9_\-./]+)\.o\b', t, re.M):
                mkmods[m.group(1)].add(m.group(1))
    return cfiles, headers, mkmods

def resolve_sources(mod_raw, cfiles, mkmods):
    out = []
    for o in mkmods.get(mod_raw, set()):
        base = o.replace('\\', '').replace('\t', '').strip().split('/')[-1]
        out.extend(cfiles.get(norm(base), []))
    # fallback: file .c bernama sama persis
    if not out:
        out = list(cfiles.get(norm(mod_raw), []))
    return sorted(set(out))

# ---------- macro parsing ----------
DEF_RE = re.compile(r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)'
                    r'(?:\s*\(([^)]*)\))?\s+(.+?)\s*(?:/\*.*)?$')

def collect_defines(paths):
    """Nilai konstanta (bukan macro fungsi)."""
    d = {}
    for p in paths:
        try:
            t = open(p, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for line in t.splitlines():
            m = DEF_RE.match(line)
            if not m:
                continue
            name, params, body = m.group(1), m.group(2), m.group(3)
            if params is not None:
                continue          # macro fungsi: bukan konstanta
            body = body.strip()
            bm = re.match(r'^(0[xX][0-9a-fA-F]+|\d+)[uUlL]*$', body)
            if bm:
                try:
                    d[name] = int(bm.group(1), 0)
                except ValueError:
                    pass
            else:
                # konstanta dari macro lain: resolve 1 level
                om = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)$', body)
                if om:
                    d.setdefault(name, ('ALIAS', om.group(1)))
    # resolve alias + arithmetic sederhana
    for _ in range(3):
        for k, v in list(d.items()):
            if isinstance(v, tuple) and v[0] == 'ALIAS':
                if v[1] in d and not isinstance(d[v[1]], tuple):
                    d[k] = d[v[1]]
    return {k: v for k, v in d.items() if not isinstance(v, tuple)}

def resolve_expr(expr, defs):
    """Ambil konstanta register dari ekspresi offset.
    Kembalikan (nilai_literal | None, macro_name | None)."""
    expr = expr.strip()
    # literal murni
    lm = re.match(r'^(0[xX][0-9a-fA-F]+|\d+)[uUlL]*$', expr)
    if lm:
        try:
            return int(lm.group(1), 0), None
        except ValueError:
            pass
    # base + MACRO / MACRO + base / MACRO
    ids = re.findall(r'[A-Za-z_][A-Za-z0-9_]*', expr)
    # buang nama fungsi/struktur umum
    skip = {'base', 'regmap', 'map', 'priv', 'tsen', 'dev', 'ctx', 'chip',
            'adc', 'pmic', 'sc27xx', 'sprd', 'ctrl', 'x', 'r', 'map_',
            'u', 'd', 'm', 'p', 'c', 'v', 'w', 't', 's', 'h', 'l', 'b', 'n', 'a'}
    for name in ids:
        if name in defs and name not in skip:
            return defs[name], name
    # base + 0xOFF (literal di offset)
    for m in re.finditer(r'\+\s*(0[xX][0-9a-fA-F]+|\d+)[uUlL]*', expr):
        try:
            return int(m.group(1), 0), None
        except ValueError:
            pass
    return None, None

def split_args(seg):
    args, depth, cur = [], 0, ""
    for ch in seg:
        if ch in '([': depth += 1; cur += ch
        elif ch in ')]':
            depth -= 1
            if depth < 0: break
            cur += ch
        elif ch == ',' and depth == 0:
            args.append(cur); cur = ""
        else: cur += ch
    args.append(cur)
    return args

REG_CALLS = ('regmap_write', 'regmap_update_bits', 'regmap_update_bits_base',
             'regmap_read', 'regmap_read_base', 'writel', 'readl',
             'writeb', 'readb', 'writew', 'readw', 'sc27xx_regmap_write',
             'sprd_regmap_write', 'sprd_regmap_update_bits', 'sc2730_regmap_write',
             'sprd_pmic_write', 'sprd_pmic_read', 'pmic_write', 'pmic_read')

def reg_access(paths, defs):
    """Kembalikan (set offset, set mask, set of call names)."""
    offs, masks, calls = set(), set(), set()
    for p in paths:
        try:
            t = open(p, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for fn in REG_CALLS:
            for m in re.finditer(re.escape(fn) + r'\s*\(', t):
                calls.add(fn)
                args = split_args(t[m.end():m.end()+500])
                if fn.startswith('regmap_'):
                    if len(args) >= 2:
                        v, _ = resolve_expr(args[1], defs)
                        if v is not None: offs.add(v)
                    if 'update_bits' in fn and len(args) >= 3:
                        v, _ = resolve_expr(args[2], defs)
                        if v is not None: masks.add(v)
                else:
                    # writel(VAL, base+OFF) : offset di arg 2
                    if len(args) >= 2:
                        v, _ = resolve_expr(args[1], defs)
                        if v is not None: offs.add(v)
    return offs, masks, calls

def main():
    print("index stock...", flush=True)
    s_c, s_h, s_m = build_index(STOCK_SRC)
    print("index port...", flush=True)
    p_c, p_h, p_m = build_index(PORT_SRC)

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
        mod_raw = stock_ko[k][:-3]
        ss = resolve_sources(mod_raw, s_c, s_m)
        ps = resolve_sources(mod_raw, p_c, p_m)
        if not ss or not ps:
            report.append({"mod": mod_raw, "verdict": "NO-SOURCE",
                           "n_stock_src": len(ss), "n_port_src": len(ps)})
            stats["NO-SOURCE"] += 1
            print(f"NONE  {mod_raw:34s} src={len(ss)}/{len(ps)}")
            continue
        sdef = collect_defines(ss + [h for h in s_h.get(norm(mod_raw), [])])
        pdef = collect_defines(ps + [h for h in p_h.get(norm(mod_raw), [])])
        so, sm, sc = reg_access(ss, sdef)
        po, pm, pc = reg_access(ps, pdef)
        # mask stock yang hilang / offset stock yang hilang
        mo = sorted(so - po)
        mm = sorted(sm - pm)
        if not so and not sm:
            verdict = "NO-REG-EVIDENCE"
        elif not mo and not mm:
            verdict = "REGISTER-EXACT" if (so == po and sm == pm) else "REGISTER-SUPERSET"
        else:
            verdict = "REGISTER-DIVERGENT"
        report.append({"mod": mod_raw, "verdict": verdict,
                       "n_stock_src": len(ss), "n_port_src": len(ps),
                       "stock_off": sorted(so), "port_off": sorted(po),
                       "stock_mask": sorted(sm), "port_mask": sorted(pm),
                       "miss_off": mo, "miss_mask": mm,
                       "stock_calls": sorted(sc), "port_calls": sorted(pc),
                       "stock_srcs": ss, "port_srcs": ps})
        stats[verdict] += 1
        tag = {"REGISTER-EXACT": "EXACT ", "REGISTER-SUPERSET": "SUPER ",
               "REGISTER-DIVERGENT": "DIVERG", "NO-REG-EVIDENCE": "NOEVID"}[verdict]
        print(f"{tag} {mod_raw:34s} off={len(so)}->{len(po)} mask={len(sm)}->{len(pm)} "
              f"missO={mo[:4]} missM={mm[:4]}", flush=True)
    print(json.dumps({"pairs": len(pairs), "stats": dict(stats)}, indent=2))
    with open(r"E:/vxtux_build_repo/regbit_macro.json", "w") as fh:
        json.dump(report, fh, indent=2, default=str)

if __name__ == "__main__":
    main()
