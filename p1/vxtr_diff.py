#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
vxtr_diff.py - bandingkan trace golden (5.4.210 ORI) vs replay (6.18.52 port).

Pemakaian:
    python3 vxtr_diff.py --golden golden.txt --replay replay.txt [--strict]

golden.txt / replay.txt = keluaran vxtr_parse.py (baris: SEQ T EVENT A0 A1 RES CALLER).

Yang dibandingkan per jenis event:
    * jumlah kejadian (golden vs replay)
    * nilai arg0/arg1 per urutan kejadian (clk/regulator/reset nama-hash,
      regmap_write reg+val, irq num, dsb.)
    * res (return value) untuk PROBE_RET / REGMAP_READ
    * urutan antar event (interleaving) -> mendeteksi "urutan power-up beda"

Keluaran: ringkasan (baris = MATCH/MISMATCH per event + total), lalu detail
baris yang tidak cocok.  Exit code 0 = semua cocok, 1 = ada mismatch.
"""
import argparse
import sys

EVENTS = {
    1:  "CLK_GET", 2: "DEVM_CLK_GET", 3: "REG_GET", 4: "DEVM_REG_GET",
    5:  "DEVM_REG_OPT", 6: "RST_GET", 7: "DEVM_RST_GET", 8: "DEVM_RST_OPT",
    9:  "PINCTRL_GET", 10: "PINCTRL_STATE", 11: "IRQ_GET", 12: "IRQ_OPT",
    13: "IRQ_BYNAME", 14: "DMA_CHAN", 15: "IOREMAP", 16: "DRV_REG",
    17: "PROBE_ENTER", 18: "PROBE_RET", 19: "PM_ATTACH", 20: "REGMAP_WRITE",
    21: "REGMAP_READ", 22: "REGMAP_UPDATE",
}


def load(path):
    recs = []
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 6:
                continue
            try:
                recs.append({
                    "seq": int(parts[0]), "t": int(parts[1]),
                    "event": parts[2], "a0": int(parts[3], 16),
                    "a1": int(parts[4], 16), "res": int(parts[5]),
                    "caller": parts[6] if len(parts) > 6 else "?",
                })
            except ValueError:
                continue
    return recs


def fmt_args(ev, a0, a1):
    """Format arguman sesuai jenis event (nama-hash vs numerik)."""
    name_events = {"CLK_GET", "DEVM_CLK_GET", "REG_GET", "DEVM_REG_GET",
                   "DEVM_REG_OPT", "RST_GET", "DEVM_RST_GET", "DEVM_RST_OPT",
                   "PINCTRL_STATE", "IRQ_BYNAME", "DMA_CHAN"}
    if ev in name_events:
        return f"name=0x{a0:x}"
    if ev == "REGMAP_WRITE":
        return f"reg=0x{a0:x} val=0x{a1:x}"
    if ev == "REGMAP_UPDATE":
        mask = a1 >> 32
        val = a1 & 0xffffffff
        return f"reg=0x{a0:x} mask=0x{mask:x} val=0x{val:x}"
    if ev == "REGMAP_READ":
        return f"reg=0x{a0:x} val=0x{a1:x}"
    if ev == "IRQ_GET" or ev == "IRQ_OPT":
        return f"irq={a0}"
    if ev == "IOREMAP":
        return f"off=0x{a0:x} size=0x{a1:x}"
    if ev == "PROBE_ENTER":
        return f"dev=0x{a0:x} drv=0x{a1:x}"
    if ev == "DRV_REG":
        return f"drv=0x{a0:x}"
    return f"a0=0x{a0:x} a1=0x{a1:x}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--golden", required=True)
    ap.add_argument("--replay", required=True)
    ap.add_argument("--strict", action="store_true",
                    help="anggap urutan antar event juga harus identik")
    args = ap.parse_args()

    g = load(args.golden)
    r = load(args.replay)
    print(f"# golden={len(g)} record, replay={len(r)} record")

    # 1. per jenis event: jumlah + nilai
    by_event = {}
    for src, tag in ((g, "golden"), (r, "replay")):
        for rec in src:
            by_event.setdefault(rec["event"], {})[tag] = \
                by_event.get(rec["event"], {}).get(tag, []) + [rec]

    mismatches = 0
    events_all = sorted(set(by_event.keys()) |
                        {r["event"] for r in r} | {g["event"] for g in g},
                        key=lambda e: EVENTS.get(e, 999))
    for ev in events_all:
        gl = by_event.get(ev, {}).get("golden", [])
        rl = by_event.get(ev, {}).get("replay", [])
        n = max(len(gl), len(rl))
        if len(gl) != len(rl):
            print(f"MISMATCH {ev}: golden={len(gl)} replay={len(rl)}")
            mismatches += 1
        for i in range(n):
            a = gl[i] if i < len(gl) else None
            b = rl[i] if i < len(rl) else None
            if a is None or b is None:
                present = a if a is not None else b
                assert present is not None
                missing = "replay" if a is None else "golden"
                print(f"  [{i}] {ev}: hilang di {missing} "
                      f"({fmt_args(ev, present['a0'], present['a1'])})")
                mismatches += 1
                continue
            # bandingkan arg0/arg1 (dan res untuk PROBE_RET/REGMAP_READ)
            key_a = (a["a0"], a["a1"], a["res"] if ev in ("PROBE_RET", "REGMAP_READ") else 0)
            key_b = (b["a0"], b["a1"], b["res"] if ev in ("PROBE_RET", "REGMAP_READ") else 0)
            if key_a != key_b:
                print(f"  [{i}] {ev}: golden {fmt_args(ev, *key_a[:2])} "
                      f"res={key_a[2]} | replay {fmt_args(ev, *key_b[:2])} "
                      f"res={key_b[2]}")
                mismatches += 1

    # 2. urutan antar event (interleaving) - opsional (--strict)
    if args.strict:
        seq_g = [(x["event"]) for x in g]
        seq_r = [(x["event"]) for x in r]
        if seq_g != seq_r:
            # temukan deviasi pertama
            for i, (x, y) in enumerate(zip(seq_g, seq_r)):
                if x != y:
                    print(f"MISMATCH urutan pada index {i}: "
                          f"golden={x} replay={y}")
                    mismatches += 1
                    break
            if len(seq_g) != len(seq_r):
                print(f"MISMATCH panjang urutan: golden={len(seq_g)} "
                      f"replay={len(seq_r)}")
                mismatches += 1

    if mismatches == 0:
        print("OK: semua event cocok (nilai + jumlah)")
        return 0
    print(f"TOTAL MISMATCH: {mismatches}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
