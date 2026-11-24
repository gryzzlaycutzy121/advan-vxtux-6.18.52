#!/usr/bin/env python3
"""
regbit_src_diff.py — diff register/bit level SOURCE.

Bandingkan himpunan konstanta numerik (offset register MMIO + bit mask + nilai)
antara source driver stock 5.4.210 dan port 6.18.52.

Kunci: porting tidak boleh MENGHILANGKAN konstanta vendor yang dipakai driver.
  - REGISTER-PRESERVED : semua konstanta stock ada di port (superset)  -> bukti
                         driver menulis register/mask yang sama.
  - CONST-MISSING      : ada konstanta stock yang hilang -> TINJAU (bisa jadi
                         path/feature dihapus, atau bug port).
  - NO-SOURCE-MATCH    : file source tidak ketemu di salah satu sisi.

Keluaran: regbit_src_report.json + ringkasan stdout.
"""
import os, re, json, subprocess, collections

STOCK_SRC = r"E:/Advan/out/odm54"
PORT_SRC = r"E:/vxtux_build_repo/6.18.52-VXTux-UMS512"
STOCK_KO = r"E:/Advan/stock/vendor_modules"
PORT_KO = r"E:/vxtux_build_repo/dl_kos_242/extracted"

def norm(n):
    return re.sub(r'[^a-z0-9]', '', n.lower().replace('.ko', ''))

def find_source(name, roots):
    """name = nama modul (mis 'sc27xx_tsensor_thermal'). Cari *.c di roots."""
    for root in roots:
        for dirpath, _, files in os.walk(root):
            for f in files:
                if f.endswith('.c') and norm(f.replace('.c', '')) == name:
                    return os.path.join(dirpath, f)
    return None

def consts(path):
    try:
        t = open(path, encoding='utf-8', errors='replace').read()
    except OSError:
        return None
    c = set()
    for m in re.finditer(r'\b(?:0[xX][0-9a-fA-F]+|[0-9]+)(?:[uUlL]*)\b', t):
        try:
            v = int(m.group(0), 0)
        except ValueError:
            continue
        if 0 < v < 0x100000000:
            c.add(v)
    return c

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
        # cari source di stock dan port
        ss = find_source(k, [STOCK_SRC])
        ps = find_source(k, [PORT_SRC])
        if not ss or not ps:
            report.append({"mod": k, "verdict": "NO-SOURCE-MATCH",
                           "stock_src": ss, "port_src": ps})
            stats["NO-SOURCE-MATCH"] += 1
            continue
        cs, cp = consts(ss), consts(ps)
        if cs is None or cp is None:
            report.append({"mod": k, "verdict": "NO-SOURCE-MATCH"})
            stats["NO-SOURCE-MATCH"] += 1
            continue
        missing = sorted(cs - cp)
        if not missing:
            verdict = "REGISTER-PRESERVED"
        else:
            verdict = "CONST-MISSING"
        report.append({"mod": k, "verdict": verdict,
                       "stock_src": ss, "port_src": ps,
                       "n_stock": len(cs), "n_port": len(cp),
                       "n_common": len(cs & cp),
                       "missing": missing[:20],
                       "n_missing": len(missing)})
        stats[verdict] += 1
    print(json.dumps({"pairs": len(pairs), "stats": dict(stats)}, indent=2))
    for r in report:
        if r["verdict"] == "REGISTER-PRESERVED":
            print(f"OK    {r['mod']:38s} stock={r['n_stock']:>4} port={r['n_port']:>4} "
                  f"common={r['n_common']:>4}")
        else:
            print(f"CHECK {r['mod']:38s} missing={r.get('n_missing','?'):>3} "
                  f"stock={r.get('n_stock','-'):>4} port={r.get('n_port','-'):>4} "
                  f"miss={r.get('missing',[])[:6]}")
    with open(r"E:/vxtux_build_repo/regbit_src_report.json", "w") as fh:
        json.dump(report, fh, indent=2, default=str)

if __name__ == "__main__":
    main()
