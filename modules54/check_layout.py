#!/usr/bin/env python3
"""check_layout.py -- Gate: pastikan __this_module di .ko dibangun dengan
layout struct module VENDOR (offset init=0x178, exit=0x338), bukan vanilla
(0x150/0x338). Kalau offset init 0x150, kernel stok membaca mod->init NULL
dan init tidak akan pernah dipanggil (insmod rc=0, initstate=live, tanpa
proc entry, tanpa printk). Exit != 0 = build ditolak.
"""
import struct
import sys

INIT_OFF = 0x178
EXIT_OFF = 0x338


def analyze(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF':
        return None, "bukan ELF"
    e_shoff = struct.unpack_from('<Q', d, 40)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 58)

    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        s = struct.unpack_from('<IIQQQQIIQQ', d, o)
        secs.append(dict(name=s[0], type=s[1], flags=s[2], addr=s[3],
                         off=s[4], size=s[5], link=s[6], info=s[7],
                         align=s[8], entsize=s[9], idx=i))

    shstr = secs[e_shstrndx]
    strs = d[shstr['off']:shstr['off'] + shstr['size']]

    def nm(n):
        e = strs.find(b'\0', n)
        return strs[n:e].decode('latin1') if e >= 0 else ''

    for s in secs:
        s['name'] = nm(s['name'])

    symtab = next((s for s in secs if s['type'] == 2), None)
    if symtab is None:
        return None, "tidak ada .symtab"

    strsec = secs[symtab['link']]
    sstr = d[strsec['off']:strsec['off'] + strsec['size']]
    ent = symtab['entsize'] or 24

    syms = []
    for i in range(symtab['size'] // ent):
        o = symtab['off'] + i * ent
        st_name, st_info, st_other, st_shndx, st_value, st_size = \
            struct.unpack_from('<IBBHQQ', d, o)
        e = sstr.find(b'\0', st_name)
        name = sstr[st_name:e].decode('latin1') if st_name else ''
        syms.append(dict(name=name, shndx=st_shndx, info=st_info))

    # relokasi di .rela.gnu.linkonce.this_module
    relasec = next((s for s in secs if s['name'] == '.rela.gnu.linkonce.this_module'), None)
    if relasec is None:
        return None, "tidak ada .rela.gnu.linkonce.this_module"

    # offset relokasi thd __this_module = offset field .init/.exit
    # (bukan nama symbol: di .ko final symbol sudah inline, nama bisa kosong)
    roffs = []
    for i in range(relasec['size'] // 24):
        o = relasec['off'] + i * 24
        r_off, r_info, r_add = struct.unpack_from('<QQq', d, o)
        typ = r_info & 0xffffffff
        roffs.append(r_off)
    roffs.sort()

    init = next((r for r in roffs if r < 0x200), -1)   # .init area
    exit_ = next((r for r in roffs if r >= 0x300), -1)  # .exit area
    return {'init_module': init, 'cleanup_module': exit_}, None


def main():
    if len(sys.argv) < 2:
        print("usage: check_layout.py <file.ko>")
        return 2
    path = sys.argv[1]
    offs, err = analyze(path)
    if err:
        print(f"GAGAL: {err}")
        return 1
    init = offs.get('init_module', -1)
    exit_ = offs.get('cleanup_module', -1)
    print(f"  init offset = 0x{init:x}, exit offset = 0x{exit_:x} "
          f"(vendor: 0x{INIT_OFF:x}/0x{EXIT_OFF:x})")
    bad = []
    if init != INIT_OFF:
        bad.append(f"init=0x{init:x} != 0x{INIT_OFF:x}")
    if exit_ != EXIT_OFF:
        bad.append(f"exit=0x{exit_:x} != 0x{EXIT_OFF:x}")

    # vermagic HARUS cocok kernel device: 5.4.210 (tanpa suffix)
    d = open(path, 'rb').read()
    verm = None
    for token in (b'vermagic=5.4.210', b'vermagic=5.4.254'):
        i = d.find(token)
        if i >= 0:
            j = d.find(b'\0', i)
            verm = d[i:j if j >= 0 else i + 64].decode('latin1')
            break
    if not verm or not verm.startswith('vermagic=5.4.210'):
        bad.append(f"vermagic={verm!r} != 5.4.210")
    else:
        print(f"  vermagic OK: {verm}")

    if bad:
        print("  GAGAL: " + "; ".join(bad) +
              " -> mod->init terbaca NULL / ditolak kernel")
        return 1
    print("  OK: layout struct module vendor + vermagic 5.4.210")
    return 0


if __name__ == '__main__':
    sys.exit(main())
