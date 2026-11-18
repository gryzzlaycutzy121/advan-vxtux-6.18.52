#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
vxtr_parse.py - baca dump ring trace vxtux_hook (spd_dump read_mem 0xAF800000).

Pemakaian:
    python3 vxtr_parse.py --dump trace_raw.bin [--map System.map] [--out golden.txt]
    python3 vxtr_parse.py --proc /proc/vxtuxtrace-copy.txt [--out golden.txt]

Format dump (little-endian arm64, sama dengan vxtux_hook.c):
    +0x0000  struct vxtr_hdr { u32 magic; u32 version; atomic_t count;
                               u32 overflow; u32 pad; }   (20 byte)
    +0x1000  struct vxtr_rec[65472] (64 byte per record)

Record (64 byte):
    u32 magic, u32 version, u32 crc, u32 seq, u16 event, u8 flags, u8 pad,
    u32 reserved, u64 arg0, u64 arg1, u64 caller, u64 t_order, u64 res

Kalau --map System.map diberikan, caller (return address) dipetakan ke nama
fungsi.  Tanpa map, caller dibiarkan mentah (hex) -- bandingkan lintas kernel
hanya valid dengan nama fungsi.

Keluaran (--out): satu baris per event, terurut seq:
    SEQ\tT_NS\tEVENT\tARG0_HEX\tARG1_HEX\tRES\tCALLER
"""
import argparse
import struct
import sys

VXTR_MAGIC = 0x56585452
VXTR_VERSION = 1
VXTR_HDR_SIZE = 0x1000
VXTR_REC_SIZE = 64
VXTR_MAX_REC = 65472

EVENTS = {
    1:  "CLK_GET", 2: "DEVM_CLK_GET", 3: "REG_GET", 4: "DEVM_REG_GET",
    5:  "DEVM_REG_OPT", 6: "RST_GET", 7: "DEVM_RST_GET", 8: "DEVM_RST_OPT",
    9:  "PINCTRL_GET", 10: "PINCTRL_STATE", 11: "IRQ_GET", 12: "IRQ_OPT",
    13: "IRQ_BYNAME", 14: "DMA_CHAN", 15: "IOREMAP", 16: "DRV_REG",
    17: "PROBE_ENTER", 18: "PROBE_RET", 19: "PM_ATTACH", 20: "REGMAP_WRITE",
    21: "REGMAP_READ", 22: "REGMAP_UPDATE",
}

HDR_FMT = "<IIiII"      # magic, version, count(signed), overflow, pad
REC_FMT = "<IIIIHBBIQQQQQ"   # magic,version,crc,seq,event,flags,pad,reserved,
                              # arg0,arg1,caller,t_order,res  (5 x u64)
REC_SIZE = struct.calcsize(REC_FMT)
assert REC_SIZE == 64, f"REC_FMT size {REC_SIZE} != 64 (harus cocok dgn struct C)"


def parse_dump(data, sysmap=None):
    """data: bytes dari read_mem.  return (list_of_records, hdr_info)."""
    if len(data) < VXTR_HDR_SIZE:
        raise ValueError(f"dump terlalu kecil: {len(data)} byte")
    magic, version, count, overflow, _pad = struct.unpack_from(HDR_FMT, data, 0)
    if magic != VXTR_MAGIC:
        raise ValueError(f"magic tidak cocok: 0x{magic:08x} (bukan VXTR)")
    if count < 0:
        count = count & 0xffffffff
    n = min(count, VXTR_MAX_REC)
    recs = []
    base = VXTR_HDR_SIZE
    for s in range(count - n + 1, count + 1):
        off = base + ((s - 1) % VXTR_MAX_REC) * VXTR_REC_SIZE
        if off + REC_SIZE > len(data):
            break
        m, v, crc, seq, ev, flags, pad, resv, a0, a1, caller, t, res = \
            struct.unpack_from(REC_FMT, data, off)
        if m != VXTR_MAGIC:
            continue
        # res disimpan dua's-complement di u64 -> kembalikan ke signed
        if res >= (1 << 63):
            res -= (1 << 64)
        recs.append({
            "seq": seq, "event": ev, "flags": flags, "arg0": a0, "arg1": a1,
            "caller": caller, "t": t, "res": res, "crc": crc,
        })
    info = {"count": count, "overflow": overflow, "version": version,
            "max_rec": VXTR_MAX_REC}
    return recs, info


def parse_proc(text):
    """Parse /proc/vxtuxtrace (format: T ev=a0 a1 res caller) -> list dict."""
    recs = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("phys="):
            continue
        # "<t> ev=<ev> a0=<a0> a1=<a1> res=<res> <caller>"
        try:
            parts = line.split()
            t = int(parts[0])
            ev = int(parts[1].split("=")[1])
            a0 = int(parts[2].split("=")[1], 16)
            a1 = int(parts[3].split("=")[1], 16)
            res = int(parts[4].split("=")[1])
            caller = parts[5] if len(parts) > 5 else "?"
            recs.append({"seq": len(recs) + 1, "event": ev, "flags": 0,
                         "arg0": a0, "arg1": a1, "caller": caller,
                         "t": t, "res": res, "crc": 0})
        except (ValueError, IndexError):
            continue
    return recs


def load_sysmap(path):
    """System.map -> dict {addr(int): name}.  Ambil symbol terkecil terdekat
    (caller adalah return address, bisa jatuh di tengah fungsi)."""
    syms = []
    with open(path, errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) == 3:
                try:
                    syms.append((int(parts[0], 16), parts[2]))
                except ValueError:
                    pass
    syms.sort()
    addrs = [s[0] for s in syms]

    def resolve(addr):
        import bisect
        i = bisect.bisect_right(addrs, addr) - 1
        if i < 0:
            return hex(addr)
        return syms[i][1]

    return resolve


def fmt_record(r, resolve=None):
    name = EVENTS.get(r["event"], f"EV{r['event']}")
    caller = resolve(r["caller"]) if resolve else f"0x{r['caller']:x}"
    return (f"{r['seq']}\t{r['t']}\t{name}\t"
            f"0x{r['arg0']:x}\t0x{r['arg1']:x}\t{r['res']}\t{caller}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", help="file biner hasil spd_dump read_mem 0xAF800000")
    ap.add_argument("--proc", help="salinan /proc/vxtuxtrace (teks)")
    ap.add_argument("--map", help="System.map untuk resolve caller")
    ap.add_argument("--out", help="tulis keluaran ke file (default stdout)")
    args = ap.parse_args()

    resolve = load_sysmap(args.map) if args.map else None
    recs = []
    info = {}
    if args.dump:
        with open(args.dump, "rb") as f:
            recs, info = parse_dump(f.read(), None)
    elif args.proc:
        with open(args.proc, errors="replace") as f:
            recs = parse_proc(f.read())
    else:
        ap.error("butuh --dump atau --proc")

    lines = []
    lines.append(f"# count={info.get('count', len(recs))} "
                 f"overflow={info.get('overflow', 0)} "
                 f"records={len(recs)}")
    for r in recs:
        lines.append(fmt_record(r, resolve))
    out = "\n".join(lines) + "\n"
    if args.out:
        with open(args.out, "w") as f:
            f.write(out)
        print(f"ditulis {len(recs)} record -> {args.out}")
    else:
        sys.stdout.write(out)


if __name__ == "__main__":
    main()
