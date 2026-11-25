#!/usr/bin/env python3
"""Sisip modul sc27xx_typec.ko versi FIX ke initramfs cpio 'padded' TANPA
menggeser offset entri lain.

Mengapa tidak repack dari nol:
  Archive initramfs yang dipakai boot LKVM (archive 'padded', 232 modul) punya
  layout/alignment spesifik. Repack penuh menggeser offset file dan memicu
  artefak ENOEXEC pada modul lain (mis. drxk.ko) saat load di LKVM. Modul fix
  disisipkan byte-level dengan PANJANG YANG SAMA (isi fix + zero-pad), sehingga
  seluruh offset & alignment entri lain tetap utuh.

Penggunaan:
  python splice_typec_fix.py <initramfs_padded.cpio.gz> <sc27xx_typec_fix.ko> <out.cpio.gz>
"""
import gzip
import hashlib
import sys

NAME = b"modules/drivers/usb/typec/sprd/sc27xx_typec.ko\x00"
MAGIC = b"070701"


def find_entry(buf, name):
    """Cari entri cpio newc lewat nama file ( plaintext di header)."""
    idx = buf.find(name)
    if idx < 0:
        raise SystemExit(f"entri {name!r} tidak ditemukan di archive")
    hdr = idx - 110
    if buf[hdr:hdr + 6] != MAGIC:
        raise SystemExit(f"header cpio tak valid di offset {hdr}")
    fields = [int(buf[hdr + 6 + i * 8:hdr + 14 + i * 8], 16) for i in range(13)]
    namesize, filesize = fields[11], fields[6]
    data_off = (idx + namesize + 3) & ~3
    return hdr, data_off, filesize


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    src, fixpath, out = sys.argv[1], sys.argv[2], sys.argv[3]

    raw = gzip.open(src, "rb").read()
    fix = open(fixpath, "rb").read()
    hdr, data_off, filesize = find_entry(raw, NAME)
    old = raw[data_off:data_off + filesize]
    print(f"archive      : {src} ({len(raw)} byte)")
    print(f"entri        : {NAME[:-1].decode()} hdr_off={hdr} data_off={data_off}")
    print(f"ukuran dalam : lama={filesize} fix={len(fix)}")
    print(f"md5 lama     : {hashlib.md5(old).hexdigest()}")
    print(f"md5 fix      : {hashlib.md5(fix).hexdigest()}")

    if len(fix) > filesize:
        raise SystemExit("modul fix lebih besar daripada entri lama — "
                         "perlu padding file, bukan zero-pad")

    new = raw[:data_off] + fix + b"\x00" * (filesize - len(fix)) + raw[data_off + filesize:]
    assert len(new) == len(raw), "ukuran archive berubah — offset lain bergeser"
    assert new[:data_off] == raw[:data_off], "header sebelum entri berubah"
    assert new[data_off + filesize:] == raw[data_off + filesize:], "data setelah entri berubah"

    # mtime=0 -> output gzip deterministik (bukan timestamp saat ini)
    with open(out, "wb") as raw_out:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw_out, mtime=0) as f:
            f.write(new)

    check = gzip.open(out, "rb").read()
    _, do2, fs2 = find_entry(check, NAME)
    blob = check[do2:do2 + fs2]
    ok = blob[:len(fix)] == fix
    print(f"tulis        : {out} ({len(open(out, 'rb').read())} byte gzip)")
    print(f"verifikasi   : isi fix identik = {ok}, ukuran entri tetap = {fs2 == filesize}")
    if not ok:
        raise SystemExit("verifikasi gagal")


if __name__ == "__main__":
    main()
