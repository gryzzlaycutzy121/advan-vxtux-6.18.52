# P1 — percobaan "panic atau watchdog?" untuk 6.18.52-VXTux-UMS512

Tujuan: **memisahkan** dua kemungkinan yang saat ini setara karena layar mati
tanpa teks panic — (A) boot benar-benar panic, atau (B) menggantung lalu
direset watchdog. Caranya: panic tidak me-reboot (`panic=0`), banjir
`initcall_debug` dilepas, pesan fatal tetap di layar (`keep_bootcon`) dan tetap
tersimpan (ramoops + vxtuxlog + UART).

**P1 hanya aktif lewat skrip build.** `.config` di tree sudah dikembalikan ke
keadaan normal (`CONFIG_CMDLINE=""`, `p1/.config.pre-p1`), jadi build biasa
tidak berubah perilaku. Skrip yang menyuntik cmdline P1 saat build.

| file | isi |
|---|---|
| `p1-cmdline.txt` | cmdline P1 (sumber kebenaran; dibaca skrip build & packer) |
| `0001-vxtux-p1-ramoops-kmsgdump.patch` | patch kernel: zona ramoops + `kmsg_dump` di `vxtux_logrec` |
| `build_p1.sh` | build di Linux/CI: config P1 → Image → `boot_p1.img`, config dipulihkan |
| `pack_p1.mjs` | **packer**: header + ramdisk dari boot image yang sudah terbukti load |
| `p1-build.yml` | workflow GitHub Actions (dispatch; unggah Image/DTB/boot_p1.img) |
| `dump_all.bat` | **dump semua kanal** via `spd_dump` (mode download) |
| `dump_all_adb.sh` | **dump semua kanal** via `adb` (device hidup) |
| `log_report.mjs` | gabungkan jadi `00-SEMUA.txt` + `01-RINGKAS.txt` |
| `dump_p1.bat` | dump ringkas (ramoops + vxtuxlog + cache + sysdumpdb) |
| `dump_extract.mjs` | ekstrak teks & record dari hasil `read_mem` |
| `04-contoh-stok-5.4.210.md` | contoh implementasi dari biner stok 5.4.210 + pemetaan |
| `out/pack-test.img` | bukti packing (kernel lama; bukan P1 — lihat §3) |

## 1. Kenapa P1 wajib rebuild (bukan cuma ganti boot.img)

`CONFIG_CMDLINE_FORCE=y` → kernel **membuang** bootargs LK/DT dan memakai
`CONFIG_CMDLINE` yang ditanam saat build (`drivers/of/fdt.c`). Cmdline di
header boot image hanya dibaca LK, bukan kernel.

## 2. Verifikasi parameter yang dipakai P1

| Parameter | Bukti di tree 6.18.52 |
|---|---|
| `printk.always_kmsg_dump=1` | **ADA** — `kernel/printk/printk.c:4686-4687` `static bool always_kmsg_dump; module_param_named(always_kmsg_dump, …)` → boot param `printk.always_kmsg_dump=` |
| `keep_bootcon` | param inti printk (boot console tidak dilepas) |
| `no_console_suspend` | param inti printk |
| `softlockup_panic=1` | `kernel/watchdog.c` |
| `androidboot.init_fatal_panic=true` | diambil dari cmdline **stok 5.4.210** (`tmp_stock_vb.dts`), ditangani init |
| ramoops zona pangkat dua | `fs/pstore/ram_core.c` menolak ukuran lain |

Kalau `always_kmsg_dump` suatu saat hilang, padanannya: `sysrq` + panic notifier
(`vxtux_logrec` sudah jadi `kmsg_dumper`) — jadi jalur dump tidak bergantung
pada satu parameter.

## 3. Build + packing

```bash
# Linux/CI
bash p1/build_p1.sh
# -> p1/out/Image, p1/out/boot_p1.img
```

Packing memakai **template boot image yang sudah terbukti load**, bukan
`out/base_boot` (yang memang tidak ada):
```bash
node p1/pack_p1.mjs \
  out/6.18.52-VXTux-UMS512/boot_artix53_panic.img \
  p1/out/Image p1/out/boot_p1.img \
  --cmdline-file p1/p1-cmdline.txt
```
Cara kerjanya: header (v4 / `header_size` 1584) dan ramdisk disalin apa adanya
dari template; hanya `kernel_size` + blob kernel yang diganti, cmdline ditulis
di offset 64 (yang dicetak LK). Hasilnya diverifikasi: magic, `header_size`,
`header_version`, kernel di offset 4096, ukuran page-aligned, dan ramdisk
byte-identik.

**Hasil nyata (CI):**
| Item | Nilai |
|---|---|
| run | https://github.com/gryzzlaycutzy121/advan-vxtux-6.18.52/actions/runs/44022757221 |
| kernelrelease | `6.18.52-VXTux-UMS512` |
| compiler | `aarch64-linux-gnu-gcc 11.4.0` (ubuntu-22.04) |
| Image | 40.813.056 B, sha256 `fc1be9d7f7d995e749618ea467e697bcc80767a1b6fcdc1a271742a1d84bae2c` |
| boot_p1.img | 42.311.680 B (10.330 page), ramdisk 1.490.813 B dari template |

Verifikasi yang dijalankan: `sha256sum -c Image.sha256` OK; magic ARM64 di
`0x38`; string `panic=0`, `keep_bootcon`, `printk.always_kmsg_dump=1`,
`androidboot.init_fatal_panic=true`, `vxtuxlog=0xAFC00000` semuanya ADA;
`initcall_debug=1` dan `panic=15` tidak ada; kernel di offset 4096 punya
sha256 identik dengan `Image`.

Catatan: `p1/out/pack-test.img` (peninggalan percobaan awal) memakai Image
lama, jangan dipakai. `boot_p1.img` yang sah adalah yang di atas.

Pipeline CI-nya: 6.18.52 build penuh ~26 menit di runner 2-core; workflow
menulis `p1/out/diag.txt` + `status.txt` lalu meng-commit `Image.gz` +
`build-report.txt` kembali ke branch `p1-build` (artefak Actions butuh token
admin untuk diunduh, jadi jalur commit ini yang dipakai).

Kenapa tidak di mesin Windows ini: `HOSTCC=clang` → `fixdep.c: 'sys/types.h'
file not found`; `HOSTCC=gcc` (mingw) → `mkdir() too many arguments`, `getline`
tidak ada; WSL/Docker tidak ada; MSYS64 tanpa compiler. Karena itu CI:
salin `p1/p1-build.yml` → `.github/workflows/`, Actions → **p1-build** → Run.

## 4. Flash

```bat
fastboot flash boot_a p1\out\boot_p1.img
fastboot reboot
```
Mode download (device mati, tahan VOL DOWN, colok USB):
```bat
cd /d E:\spd_flash
spd_dump_new.exe --wait 300 exec_addr 0x3ee8 ^
  fdl fdl1-dl.bin 0x5500 fdl fdl2-dl.bin 0x9efffe00 exec ^
  baudrate 4000000 blk_size 65535 path E:\spd_dump_logs ^
  write_part boot_a E:\vxtux_build_repo\p1\out\boot_p1.img reset
```
Varian B (panen log otomatis): build dengan `panic=15`.

## 5. Dump SEMUA log

### a) Perangkat mati / layar beku → mode download
```bat
dump_all.bat          rem RAM + partisi log  (~31 MB, 1-2 menit)
dump_all.bat full     rem + partisi cache 128 MB (~7-8 menit)
```
Yang diambil:

| Kanal | Sumber | Isi |
|---|---|---|
| ramoops | `read_mem 0xfff80000 0x40000` | pstore record + console + pmsg |
| vxtuxlog | `read_mem 0xAFC00000 0x400000` | snapshot `kmsg_dump` (2 slot, crc) |
| uboot_log (RAM) | `read_mem 0x9de80000 0x80000` | log LK: boot reason, cmdline |
| sysdumpinfo | `read_mem 0x80001000 0x1000` | struct sysdump stok |
| sysdump-uboot | `read_mem 0x9f000000 0x1000000` | area dump vendor/LK |
| uboot_log, sysdumpdb, misc | `read_part` | partisi log LK & dump |
| cache (opsional) | `read_part cache 0x8000000` | `/cache/vxtux-logs/*.txt` |

Hasil di `E:\spd_dump_logs\p1-<tanggal>_<jam>\`: `*.bin` (mentah),
`*.text.txt`, `*.rec-<seq>-r<reason>.txt`, lalu **`00-SEMUA.txt`** (gabungan
semua kanal) dan **`01-RINGKAS.txt`** (hitungan + baris penting per penanda:
panic, oops/WARN, watchdog/lockup, initcall gagal, deferred, pstore, init
userspace, storage, display, firmware, reboot).

### b) Perangkat hidup / adb jalan
```bash
bash p1/dump_all_adb.sh
```
Mengambil `dmesg`, `/proc/cmdline`, `uname -a`, `console-ramoops-0`,
`dmesg-ramoops-0`, `/proc/vxtuxlog`, `/sys/kernel/debug/devices_deferred`,
`/proc/last_kmsg`, daftar `/cache/vxtux-logs` (+ pull bila bisa), dan properti
slot/serialno/verity. Laporannya sama: `01-RINGKAS.txt`.

### c) Manual per kanal
`p1\dump_p1.bat` masih ada untuk dump ringkas (ramoops + vxtuxlog + cache +
sysdumpdb), dan `dump_extract.mjs` bisa dipakai langsung pada satu berkas:
```
node p1\dump_extract.mjs E:\spd_dump_logs\p1-...\a1_ramoops.bin
```

## 6. Tabel keputusan

| Gejala | Kesimpulan | Lanjut |
|---|---|---|
| Layar berhenti di `Kernel panic` / `Failed to execute` | jalur A: panic | baca `PC is at` / `Call trace` |
| Layar hitam, diam >60 s | jalur B: hang → watchdog | P5: isolasi panel/DSI |
| Layar menetap di baris terakhir, tanpa panic | hang di deferred probe | `devices_deferred`, `initcall_blacklist` |
| `p1_vxtuxlog…` ada, `reason=dump` | dump non-panic terekam | cocokkan dengan layar |

## 7. Isi patch

1. `ums512-1h10-vxtux.dts` — ramoops: record 128K→64K, console 64K→128K
   (total tetap 256K, pangkat dua). Zona console 64K terbukti wrap sebelum init.
2. `drivers/vxtux/debug/vxtux_logrec.c` — daftar sebagai `kmsg_dumper`:
   snapshot ke RAM `0xAFC00000` untuk **semua** alasan dump (OOPS/REBOOT/HALT),
   bukan hanya panic.

## 8. Rollback

```bash
cp p1/.config.pre-p1 6.18.52-VXTux-UMS512/.config
cd 6.18.52-VXTux-UMS512 && git apply -R ../../p1/0001-vxtux-p1-ramoops-kmsgdump.patch
```
