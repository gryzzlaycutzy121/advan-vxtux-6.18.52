#!/usr/bin/env python3
"""cpio newc (070701) multi-archive reader/writer.

Kernel Linux (init/initramfs.c unpack_to_rootfs) SETELAH membaca TRAILER!!!
melanjutkan scan archive berikutnya di buffer yang sama. Archive initramfs VXTux
adalah beberapa archive newc yang digabung (hasil concat), jadi parser harus
mendukung itu — parser yang break di TRAILER akan kehilangan ~83 modul.

Offset dipatuhi ketat (walk by offset, bukan find magic) supaya false-positive
"070701" di dalam data biner .ko tidak merusak state parser.

  python cpio_tool2.py list    <file.cpio>            # ringkas
  python cpio_tool2.py extract <file.cpio> <outdir>
  python cpio_tool2.py pack    <indir> <out.cpio>
"""
import os
import stat
import sys

MAGIC = b"070701"


def _hdr(buf, off):
    if off + 110 > len(buf) or buf[off:off + 6] != MAGIC:
        return None
    v = [int(buf[off + 6 + i * 8:off + 14 + i * 8], 16) for i in range(13)]
    return v  # ino,mode,uid,gid,nlink,mtime,filesize,devmaj,devmin,rdevmaj,rdevmin,namesize,check


def walk(buf):
    """Yield (name, data, mode) untuk semua entri di semua archive newc."""
    off = 0
    n = len(buf)
    while off + 110 <= n:
        v = _hdr(buf, off)
        if v is None:
            off += 4  # resync: mungkin archive berikutnya tak 4-byte aligned
            continue
        namesize, filesize = v[11], v[6]
        if namesize == 0 or namesize > 4096 or filesize > 2 * 1024 * 1024 * 1024:
            off += 4
            continue
        name_start = off + 110
        name = buf[name_start:name_start + namesize - 1].decode("utf-8", "replace")
        data_off = (name_start + namesize + 3) & ~3
        if data_off + filesize > n:
            off += 4
            continue
        data = buf[data_off:data_off + filesize]
        mode = v[1]
        yield name, data, mode
        off = (data_off + filesize + 3) & ~3
        if name == "TRAILER!!!":
            off = (off + 3) & ~3  # lanjut ke archive berikutnya (kernel semantics)


def list_archive(path):
    buf = open(path, "rb").read()
    files = 0
    dirs = 0
    kos = 0
    for name, data, mode in walk(buf):
        if stat.S_ISDIR(mode):
            dirs += 1
        else:
            files += 1
            if name.endswith(".ko"):
                kos += 1
    print(f"-- {files} file, {dirs} dir, {kos} .ko, {len(buf)} byte --")


def extract(path, outdir):
    buf = open(path, "rb").read()
    count = 0
    for name, data, mode in walk(buf):
        target = os.path.join(outdir, name.lstrip("/"))
        if stat.S_ISDIR(mode):
            os.makedirs(target, exist_ok=True)
        elif stat.S_ISLNK(mode):
            os.makedirs(os.path.dirname(target) or outdir, exist_ok=True)
            try:
                os.symlink(data.decode(), target)
            except OSError:
                pass
        elif stat.S_ISREG(mode):
            os.makedirs(os.path.dirname(target) or outdir, exist_ok=True)
            with open(target, "wb") as f:
                f.write(data)
            os.chmod(target, stat.S_IMODE(mode) | 0o600)
        count += 1
    print(f"diekstrak {count} entri ke {outdir}")


def _pad(s, n=4):
    return s + b"\0" * ((n - len(s) % n) % n)


def pack(indir, out, mode_map=None):
    entries = []
    for root, dirs, files in os.walk(indir):
        for name in dirs + files:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, indir)
            arc = rel.replace(os.sep, "/")
            entries.append((arc, full))
    entries.sort()
    entries.insert(0, (".", indir))
    blob = bytearray()
    ino = 1
    for arc, full in entries:
        st = os.lstat(full)
        mode = st.st_mode
        if mode_map and arc in mode_map:
            mode = (mode & ~0o7777) | mode_map[arc]
        data = b""
        if stat.S_ISREG(mode):
            data = open(full, "rb").read()
        elif stat.S_ISLNK(mode):
            data = os.readlink(full).encode()
        name = arc.encode() + b"\0"
        hdr = MAGIC + b"".join(f"{x:08x}".encode() for x in (
            ino, mode, 0, 0, 1, int(st.st_mtime), len(data),
            0, 0, 0, 0, len(name), 0))
        blob += _pad(hdr + name)
        blob += _pad(bytes(data))
        ino += 1
    name = b"TRAILER!!!\0"
    hdr = MAGIC + b"".join(f"{x:08x}".encode() for x in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0))
    blob += _pad(hdr + name)
    open(out, "wb").write(bytes(blob))
    print(f"pack {len(entries)} entri -> {out} ({len(blob)} byte)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    if sys.argv[1] == "list":
        list_archive(sys.argv[2])
    elif sys.argv[1] == "extract":
        extract(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == "pack":
        pack(sys.argv[2], sys.argv[3])
