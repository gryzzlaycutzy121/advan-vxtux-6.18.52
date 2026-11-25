#!/usr/bin/env python3
"""
regbit_macro2.py — BUKTI "persis by register dan bit" (macro + include resolution).

Fix vs regbit_macro.py:
  1. Resolve #include "xxx.h" -> header file -> definisi macro di situ.
     (offset register vendor hampir selalu di header, bukan .c)
  2. Klasifikasi jujur:
       HARNESS-MISS     : source stock MEMANG panggil regmap/writel tapi
                          offset tak tertangkap -> belum terbukti, bukan bukti
       NO-REG-EVIDENCE  : source stock memang tak punya akses register
                          (mis. lewat framework PWM/regulator) -> memang tak
                          ada yang bisa dibandingkan; bukan "lolos"
  3. Compare juga BIT MASK (arg ke-3 update_bits) dan nilai literal.

Verdict:
  REGISTER-EXACT    : offset set & mask set IDENTIK
  REGISTER-SUPERSET : port punya semua offset+mask stock ( boleh tambah)
  REGISTER-DIVERGENT : port kehilangan offset/mask stock -> DEFECT
"""
import os, re, json, collections

STOCK_SRC = r"E:/Advan/out/odm54"
PORT_SRC = r"E:/vxtux_build_repo/6.18.52-VXTux-UMS512"
STOCK_KO = r"E:/Advan/stock/vendor_modules"
PORT_KO = r"E:/vxtux_build_repo/dl_kos_242/extracted"

def norm(n):
    return re.sub(r'[^a-z0-9]', '', n.lower().replace('.ko', ''))

def build_index(root):
    cfiles = collections.defaultdict(list)
    headers = collections.defaultdict(list)
    all_headers = []
    mkmods = collections.defaultdict(set)
    for dirpath, dirs, files in os.walk(root):
        for f in files:
            if f.endswith('.c'):
                cfiles[norm(f[:-2])].append(os.path.join(dirpath, f))
            elif f.endswith('.h'):
                p = os.path.join(dirpath, f)
                headers[norm(f[:-2])].append(p)
                all_headers.append(p)
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
    return cfiles, headers, all_headers, mkmods

def resolve_sources(mod_raw, cfiles, mkmods):
    out = []
    for o in mkmods.get(mod_raw, set()):
        base = o.replace('\\', '').replace('\t', '').strip().split('/')[-1]
        out.extend(cfiles.get(norm(base), []))
    if not out:
        out = list(cfiles.get(norm(mod_raw), []))
    return sorted(set(out))

INC_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)

def resolve_includes(srcs, headers, all_headers):
    """Header yang di-#include oleh source; cari di dir yang sama / parent /
    include/ linux/ yang unik di tree."""
    out = []
    hdr_by_name = collections.defaultdict(list)
    for h in all_headers:
        hdr_by_name[os.path.basename(h)].append(h)
    for s in srcs:
        try:
            t = open(s, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for m in INC_RE.finditer(t):
            inc = m.group(1)
            base = os.path.basename(inc)
            sd = os.path.dirname(s)
            cands = []
            # 1) relatif ke dir source & parent
            for up in range(0, 4):
                cands.append(os.path.normpath(os.path.join(sd, *(['..'] * up), inc)))
            # 2) index unik di tree
            cands.extend(hdr_by_name.get(base, []))
            for c in cands:
                if os.path.isfile(c):
                    out.append(c)
                    break
    return sorted(set(out))

DEF_RE = re.compile(r'^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)'
                    r'(?:\s*\(([^)]*)\))?\s+(.+?)\s*(?:/\*.*)?$')

def collect_defines(paths):
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
                continue
            body = body.strip()
            bm = re.match(r'^(0[xX][0-9a-fA-F]+|\d+)[uUlL]*$', body)
            if bm:
                try:
                    d[name] = int(bm.group(1), 0)
                    continue
                except ValueError:
                    pass
            am = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)$', body)
            if am:
                d.setdefault(name, ('ALIAS', am.group(1)))
    for _ in range(4):
        for k, v in list(d.items()):
            if isinstance(v, tuple) and v[0] == 'ALIAS' and v[1] in d \
               and not isinstance(d[v[1]], tuple):
                d[k] = d[v[1]]
    return {k: v for k, v in d.items() if not isinstance(v, tuple)}

def resolve_expr(expr, defs):
    expr = expr.strip()
    lm = re.match(r'^(0[xX][0-9a-fA-F]+|\d+)[uUlL]*$', expr)
    if lm:
        try:
            return int(lm.group(1), 0)
        except ValueError:
            pass
    skip = {'base', 'regmap', 'map', 'priv', 'tsen', 'dev', 'ctx', 'chip',
            'adc', 'pmic', 'sc27xx', 'sprd', 'ctrl', 'x', 'r', 'u', 'd',
            'm', 'p', 'c', 'v', 'w', 't', 's', 'h', 'l', 'b', 'n', 'a',
            'common', 'offset', 'index', 'reg', 'val', 'value', 'mask'}
    for name in re.findall(r'[A-Za-z_][A-Za-z0-9_]*', expr):
        if name in defs and name not in skip:
            return defs[name]
    for m in re.finditer(r'\+\s*(0[xX][0-9a-fA-F]+|\d+)[uUlL]*', expr):
        try:
            return int(m.group(1), 0)
        except ValueError:
            pass
    return None

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
             'sprd_pmic_write', 'sprd_pmic_read', 'pmic_write', 'pmic_read',
             'sc27xx_pmic_read', 'sc27xx_pmic_write', 'regmap_raw_read',
             'regmap_raw_write')

def reg_access(paths, defs):
    offs, masks, calls, ncall = set(), set(), set(), 0
    for p in paths:
        try:
            t = open(p, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for fn in REG_CALLS:
            for m in re.finditer(re.escape(fn) + r'\s*\(', t):
                calls.add(fn); ncall += 1
                args = split_args(t[m.end():m.end()+500])
                if fn.startswith('regmap_'):
                    if len(args) >= 2:
                        v = resolve_expr(args[1], defs)
                        if v is not None: offs.add(v)
                    if 'update_bits' in fn and len(args) >= 3:
                        v = resolve_expr(args[2], defs)
                        if v is not None: masks.add(v)
                else:
                    if len(args) >= 2:
                        v = resolve_expr(args[1], defs)
                        if v is not None: offs.add(v)
    return offs, masks, calls, ncall

def main():
    print("index stock...", flush=True)
    s_c, s_h, s_hh, s_m = build_index(STOCK_SRC)
    print("index port...", flush=True)
    p_c, p_h, p_hh, p_m = build_index(PORT_SRC)

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
            print(f"NONE  {mod_raw:34s} src={len(ss)}/{len(ps)}", flush=True)
            continue
        s_inc = resolve_includes(ss, s_h, s_hh)
        p_inc = resolve_includes(ps, p_h, p_hh)
        sdef = collect_defines(ss + s_inc)
        pdef = collect_defines(ps + p_inc)
        so, sm, sc, sn = reg_access(ss, sdef)
        po, pm, pc, pn = reg_access(ps, pdef)
        mo = sorted(so - po)
        mm = sorted(sm - pm)
        if sn > 0 and not so and not sm:
            verdict = "HARNESS-MISS"
        elif sn == 0:
            verdict = "NO-REG-EVIDENCE"
        elif not mo and not mm:
            verdict = "REGISTER-EXACT" if (so == po and sm == pm) else "REGISTER-SUPERSET"
        else:
            verdict = "REGISTER-DIVERGENT"
        report.append({"mod": mod_raw, "verdict": verdict,
                       "n_stock_src": len(ss), "n_port_src": len(ps),
                       "n_stock_hdr": len(s_inc), "n_port_hdr": len(p_inc),
                       "stock_calls_n": sn, "port_calls_n": pn,
                       "stock_off": sorted(so), "port_off": sorted(po),
                       "stock_mask": sorted(sm), "port_mask": sorted(pm),
                       "miss_off": mo, "miss_mask": mm,
                       "stock_calls": sorted(sc), "port_calls": sorted(pc),
                       "stock_srcs": ss, "port_srcs": ps})
        stats[verdict] += 1
        tag = {"REGISTER-EXACT": "EXACT ", "REGISTER-SUPERSET": "SUPER ",
               "REGISTER-DIVERGENT": "DIVERG", "NO-REG-EVIDENCE": "NOEVID",
               "HARNESS-MISS": "HMISS "}[verdict]
        print(f"{tag} {mod_raw:34s} calls={sn}/{pn} off={len(so)}->{len(po)} "
              f"mask={len(sm)}->{len(pm)} missO={mo[:4]} missM={mm[:4]}", flush=True)
    print(json.dumps({"pairs": len(pairs), "stats": dict(stats)}, indent=2))
    with open(r"E:/vxtux_build_repo/regbit_macro2.json", "w") as fh:
        json.dump(report, fh, indent=2, default=str)

if __name__ == "__main__":
    main()
