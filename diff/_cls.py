import json
r=json.load(open("E:/vxtux_build_repo/regbit_src_report.json"))
def isyear(v):
    return 1900<=v<=2100
for r in r:
    if r["verdict"]!="CONST-MISSING": continue
    miss=r["missing"]
    real=[m for m in miss if not isyear(m)]
    print(f"{r['mod']:24s} n_miss={r['n_missing']:>3}  non-year={real}")
