#!/usr/bin/env python3
"""
regbit_final.py — diff register/bit level-SOURCE (index sekali jalan).

Perf: walk tiap tree HANYA SEKALI, bangun index:
  Makefile -> daftar modul -> daftar objek -> file .c

Verdict per modul:
  REGISTER-PRESERVED : union konstanta register/mask stock tercakup port
  REG-MISSING        : ada akses register stock yang hilang di port
  NO-SOURCE          : source modul tidak ter-resolve di salah satu sisi
"""
import os, re, json, collections, sys

STOCK_SRC = r"E:/Advan/out/odm54"
PORT_SRC = r"E:/vxtux_build_repo/6.18.52-VXTux-UMS512"
STOCK_KO = r"E:/Advan/stock/vendor_modules"
PORT_KO = r"E:/vxtux_build_repo/dl_kos_242/extracted"

def norm(n):
    return re.sub(r'[^a-z0-9]', '', n.lower().replace('.ko', ''))

def build_index(root):
    """walk sekali; kembalikan (cfiles: dict normname->list path,
    makefiles: dict mod_raw -> list objname)."""
    cfiles = collections.defaultdict(list)
    mkmods = collections.defaultdict(set)
    for dirpath, dirs, files in os.walk(root):
        for f in files:
            if f.endswith('.c'):
                cfiles[norm(f[:-2])].append(os.path.join(dirpath, f))
        if 'Makefile' in files:
            mk = os.path.join(dirpath, 'Makefile')
            try:
                t = open(mk, encoding='utf-8', errors='replace').read()
            except OSError:
                continue
            # composite: <mod>-y := obj1.o obj2.o ...  (multi baris via \)
            for m in re.finditer(r'^\s*([A-Za-z0-9_\-./]+)-y\s*[+]?=\s*(.*?)(?=^\s*[A-Za-z0-9_\-./]+\s*-y\s*[+]?=|^\s*[A-Za-z0-9_\-./]+\s*-objs|^\s*obj-\$|^[A-Za-z]|\Z)',
                                 t, re.M | re.S):
                mkmods[m.group(1)].update(objs_from(m.group(2)))
            # non-composite: <mod>.o
            for m in re.finditer(r'^\s*(?:obj-\$\(CONFIG_[A-Z0-9_]+\)\s*\+?=\s*|obj-m\s*\+?=\s*|obj-y\s*\+?=\s*)([A-Za-z0-9_\-./]+)\.o\b',
                                 t, re.M):
                mkmods[m.group(1)].add(m.group(1))
    return cfiles, mkmods

def objs_from(body):
    return [x.group(1) for x in re.finditer(r'([A-Za-z0-9_\-./]+)\.o', body)]

def resolve(mod_raw, cfiles, mkmods):
    objs = mkmods.get(mod_raw, set())
    out = []
    for o in objs:
        o = o.replace('\\', '').replace('\t', '').strip()
        base = o.replace('.o', '')
        base = base.split('/')[-1]   # pakai basename utk ketemu lintas dir
        if norm(base) in cfiles:
            out.extend(cfiles[norm(base)])
    return sorted(set(out))

def reg_consts_all(paths):
    offs, masks = set(), set()
    for path in paths:
        try:
            t = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for m in re.finditer(r'regmap_(?:write|update_bits|update_bits_base|read)\s*\(', t):
            args = split_args(t[m.end():m.end()+400])
            if len(args) >= 2:
                c = pconst(args[1])
                if c is not None: offs.add(c)
            if 'update_bits' in m.group(0) and len(args) >= 3:
                c = pconst(args[2])
                if c is not None: masks.add(c)
        for m in re.finditer(r'\b(?:writel|readl|writeb|readb)\s*\(', t):
            args = split_args(t[m.end():m.end()+200])
            if len(args) >= 2:
                for c in find_offsets(args[1]):
                    offs.add(c)
    return offs, masks

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

def pconst(s):
    m = re.match(r'\s*(0[xX][0-9a-fA-F]+|[0-9]+)\b', s)
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
    print("indexing stock...", flush=True)
    s_c, s_m = build_index(STOCK_SRC)
    print("indexing port...", flush=True)
    p_c, p_m = build_index(PORT_SRC)
    print(f"stock: {len(s_c)} c-files, {len(s_m)} mods ; port: {len(p_c)}, {len(p_m)}",
          flush=True)

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
        ss = resolve(mod_raw, s_c, s_m)
        ps = resolve(mod_raw, p_c, p_m)
        if not ss or not ps:
            report.append({"mod": mod_raw, "verdict": "NO-SOURCE",
                           "n_stock_src": len(ss), "n_port_src": len(ps)})
            stats["NO-SOURCE"] += 1
            continue
        ao, am = reg_consts_all(ss)
        bo, bm = reg_consts_all(ps)
        mo = sorted(ao - bo); mm = sorted(am - bm)
        verdict = "REGISTER-PRESERVED" if not mo and not mm else "REG-MISSING"
        report.append({"mod": mod_raw, "verdict": verdict,
                       "n_stock_src": len(ss), "n_port_src": len(ps),
                       "stock_off": len(ao), "port_off": len(bo),
                       "stock_mask": len(am), "port_mask": len(bm),
                       "n_miss_off": len(mo), "n_miss_mask": len(mm),
                       "miss_off": mo[:24], "miss_mask": mm[:24],
                       "stock_srcs": ss, "port_srcs": ps})
        stats[verdict] += 1
        v = report[-1]
        if verdict == "REGISTER-PRESERVED":
            print(f"OK    {mod_raw:34s} src={len(ss)}/{len(ps)} "
                  f"off={len(ao)}->{len(bo)} mask={len(am)}->{len(bm)}")
        else:
            print(f"CHECK {mod_raw:34s} src={len(ss)}/{len(ps)} "
                  f"miss_off={len(mo)} miss_mask={len(mm)} mo={mo[:5]} mm={mm[:5]}")
    print(json.dumps({"pairs": len(pairs), "stats": dict(stats)}, indent=2))
    with open(r"E:/vxtux_build_repo/regbit_final.json", "w") as fh:
        json.dump(report, fh, indent=2, default=str)

if __name__ == "__main__":
    main()
