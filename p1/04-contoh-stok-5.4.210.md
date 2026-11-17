# Contoh implementasi dari STOK 5.4.210 (biner proprietary) — dipakai P1

Semua di bawah ini **dibaca dari device ini**, bukan dari tebakan:
`Advan/stock/Image` (identik byte-per-byte dengan slice kernel `boot_a.bin`),
`Advan/stock/kallsyms.txt`, `Advan/stock/stock.config`, dan DTB stok
(`tmp_stock_vb.dts`).

## 1. DT stok — angka asli (jadi acuan, bukan karangan)

```
reserved-memory {
  ddrbist-mem@0x80000000      reg <0 0x80000000 0 0x1000>
  sysdumpinfo-mem@80001000    reg <0 0x80001000 0 0x1000>     <- 4 KiB
  uboot_log-mem               reg <0 0x9de80000 0 0x80000>    <- 512 KiB
  sysdump-uboot@9f000000      reg <0 0x9f000000 0 0x1000000>  <- 16 MiB
  rebootescrow@0xfff70000     reg <0 0xfff70000 0 0x10000>  no-map
  ramoops@fff80000            reg <0 0xfff80000 0 0x40000>
      record-size  = <0x8000>   (32 KiB)
      console-size = <0x8000>   (32 KiB)
      pmsg-size    = <0x8000>   (32 KiB)
};
chosen { stdout-path = "serial1:115200n8";
         bootargs = "earlycon console=ttyS1,115200n8 loop.max_part=7 loglevel=1
                     init=/init root=/dev/ram0 rw printk.devkmsg=on
                     ... initcall_debug=1 androidboot.init_fatal_panic=true"; }
```

Aliases stok: `sysdump_reserved = "/reserved-memory/sysdumpinfo-mem@80001000"`,
`uboot_log_reserved`, `pstore_reserved = "/reserved-memory/ramoops@fff80000"`.

## 2. Simbol sysdump di biner stok (bukti driver-nya apa)

```
sysdump_early_init            (early_initcall)
sysdump_magic_setup           (__setup  -> early param)
sysdump_reflag_setup          (__setup  -> early param)
get_sprd_sysdump_info_paddr
sprd_sysdump_write / _read / _open      (proc/sysctl surface)
sysdump_panic_event           (notifier_block sysdump_panic_event_nb)
sysdump_log_notifier          (notifier log dump)
sysdump_event / sysdump_connect / sysdump_disconnect
sysdump_ipi                   (menyadarkan/menghentikan CPU lain)
sprd_sysdump_extra            (data, diekspor)
sprd_hang_debug_printf, hang_detect_thread, hang_info[]   (SPRD_NATIVE_HANG_MONITOR)
```

String di Image (urutan alur yang terbaca):

```
sprd-sysdump: *  Sysdump enter, preparing debug info to dump ...  *
sprd-sysdump: vaddr is %p, paddr is %p.
sprd-sysdump: KTXT [0x%lx--0x%lx]        <- alamat teks kernel ikut disimpan
sprd-sysdump: KTXT VERIFY... / SHA1:
sprd-sysdump: [%s]vmcore info init end!
sprd-sysdump: *  Try to reboot system ... *
sprd-sysdump: get sprd_sysdump_info_paddr failed!!!.
```

Config stok: `CONFIG_SPRD_SYSDUMP=y`, `CONFIG_SPRD_MINI_SYSDUMP=y`,
`CONFIG_SPRD_NATIVE_HANG_MONITOR=y`, `CONFIG_SPRD_HANG_DEBUG_UART=y`,
`CONFIG_SPRD_HANG_RESET=y`, `CONFIG_SPRD_HANG_TRIGGER=y`,
`CONFIG_PSTORE=y` + `PSTORE_CONSOLE` + `PSTORE_RAM`.

**Kontrak yang dipakai stok:** saat panic → `sysdump_panic_event` menulis
info (4 KiB `sysdumpinfo-mem`) + isi dump ke `sysdump-uboot` (16 MiB), lalu
warm reboot supaya isi DRAM selamat; `spd_dump` membacanya sebagai partisi
`uboot_log` / `sysdumpdb`.

## 3. Apa yang bisa / tidak bisa dipindah ke 6.18.52

| Hal | Status di tree 6.18.52-VXTux-UMS512 | Tindakan P1 |
|---|---|---|
| `ramoops` | ada, pool 256 KiB | zona disesuaikan (lihat tabel) |
| pstore console/pmsg/ram | `CONFIG_PSTORE*` =y | dipertahankan, dikunci di `build_p1.sh` |
| Driver `sprd_sysdump` | **tidak ada** di tree (bukan upstream; source 5.4 di mesin ini juga tidak memuatnya) | tidak dipalsukan; kontraknya diganti `vxtux_logrec` + ramoops |
| `sysdumpinfo-mem` 4 KiB | tidak ada node | tidak ditambah (tanpa driver = node mati) |
| `androidboot.init_fatal_panic=true` | belum ada di cmdline | **ditambahkan** ke P1 (init gagal → panic, bukan hang senyap) |
| Hang monitor | tidak ada | `softlockup_panic=1` + `panic=15`/`0` sebagai padanan |
| UART console | `console=ttyS1,115200n8` (sama dengan stok) | dipertahankan |

### Penyimpangan yang disengaja (dan alasannya)

| Zona | Stok | P1 | Alasan |
|---|---|---|---|
| record-size | 32 KiB | 64 KiB | dmesg satu boot penuh tanpa `initcall_debug` muat 64 KiB |
| console-size | 32 KiB | 128 KiB | `loglevel=8` mencetak paling banyak di 10 detik pertama; 32 KiB wrap sebelum init |
| pmsg-size | 32 KiB | 32 KiB | sama |
| `no-map` | tidak ada | ada | tanpa `no-map` region bisa ikut dipetakan ulang/di-recycle |

Total tetap 256 KiB (0x40000) seperti stok; semua zona pangkat dua karena
`fs/pstore/ram_core.c` menolak ukuran lain.

## 4. Yang diambil dari pendekatan stok untuk P1

1. **`androidboot.init_fatal_panic=true`** — stok memakainya; tanpa ini
   kegagalan init bisa berakhir sebagai hang senyap (persis gejala layar
   hitam yang sedang dikejar).
2. **Warm reboot setelah dump** — stok `Try to reboot system`; padanannya
   `panic=15`. Itu sebabnya P1 punya dua varian: `panic=0` (beku, untuk
   memotret layar) dan `panic=15` (panen log otomatis).
3. **`initcall_debug=1` ada di cmdline stok** — jadi banjir initcall di layar
   video itu **perilaku stok**, bukan bug porting. P1 melepasnya supaya pesan
   fatal terlihat.
4. **`loglevel=1` di stok** vs `loglevel=8` di P1: stok menyembunyikan layar
   dan mengandalkan UART/pstore; P1 justru butuh layar.
