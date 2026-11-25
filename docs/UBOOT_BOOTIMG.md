# Build Kernel untuk u-boot (boot.img Artix) — V17

Tanggal: 2026-11-25

## Versi terakhir build yang 100% bekerja: V17

- CI run: **45514371957** (`Build V17 VXTux Kernel`, branch `test1` dgn fix typec) = **SUCCESS**
- CI run modul: **45514396925** (`Build modules for verify`, `test1`+fix) = SUCCESS, 232 .ko, `sc27xx_typec.ko` md5 `d45475bb`
- Rantai bukti tertutup: source → CI 45514396925 → binary md5 `d45475bb` → guest LKVM LOAD OK (2× deterministik, `total=232 loaded=229 skipped=3 failed=0 VXTUX-MODTEST-PASS`)

## Artifact CI (run 45514371957)

- `v17-Image` = `arch/arm64/boot/Image`, 49,922,560 byte, sha256 `fdb82e9869efcb2e0223c7fb295e12423584803ddf5cbb2a2bf7cceb40cb1703`, version string `6.18.52-VXTux-UMS512-artix`
  - Isi: kernel (~31.7MB) + initramfs diagnostik embedded (`070701` @ 31,730,752) → `/vxtux/init` (VXTUX-STEP) yang mount root ext4 & chroot ke Artix
- `v17-dtb` = `ums512-1h10-vxtux.dtb`, 58,656 byte, md5 `4dbc8835c74f91ff3e43cfe80f1a1bab`
- `v17-config`, `v17-built-kos` (232 ko, typec md5 `d45475bb`)

## Resep boot.img (ci_qemu/mk_boot_artix.py)

```
python ci_qemu/mk_boot_artix.py _v17_img/arch/arm64/boot/Image out/boot_v17_artix.img --ramdisk out/artix_ramdisk
```

- Header **v4** disintesis dari nol (header_size=1584), kernel @ page 4096
- Relay stub pass-through 3 instruksi di 0x80080000: `b +0x10` → `movz/movk x2,#0x80200000` → `br x2`
- Image asli di blob offset **0x180000** → mendarat di **0x80200000** (2MB-aligned, text_offset=0)
- Ramdisk **WAJIB** (kalau kosong LK panic parse init_boot): `out/artix_ramdisk/ramdisk` (4,758,421 byte, gzip newc, diagnostic init `/vxtux/init`)
- cmdline: `earlycon console=tty0 console=ttyS1,115200n8 ... rdinit=/vxtux/init panic=15 ... androidboot.slot_suffix=_a`

## Hasil & verifikasi invariant

- **boot_v17_artix.img**: 56,262,656 byte, md5 `399c5811e7035f623edfc3331f481ff2`, sha256 `35d65982c3bea507deb51b53fb8c324cfc833b5a7b7fa14438b846891b53ea68`
- Image asli di blob **identik byte-for-byte** dgn source (True)
- magic ARM64 `0x644D5241` di offset 0x38 Image asli (source & blob) ✓
- relay word0 = `0x14000010` (`b +0x10`), stub `020080d2 0204b0f2 40001fd6` = movz/movk x2=0x80200000 + br x2 ✓
- kernel_size header = 51,495,424 = 0x180000 + len(orig Image) ✓
- Lokasi device (TIDAK di-flash): `/data/local/tmp/boot_v17_artix.img`

## Catatan perilaku kernel artix di LKVM

Boot `Image_v17_artix` + `initramfs_typecfix2.cpio.gz` di LKVM: kernel boot sampai initcalls, lalu **diagnostic init (VXTUX-STEP)** yang menang (embedded initramfs) mencari ext4 rootfs → di LKVM tidak ada rootfs → "no ext4 rootfs with /sbin/init found" / "Root was not found". Ini perilaku BENAR — artix butuh device nyata dgn rootfs Artix. Verifikasi load 232 modul (229/232 PASS) dilakukan pada kernel modtest (`Image_modtest_embedded`), source tree sama (test1 + fix typec).

## Cara flash (HANYA saat ada trigger eksplisit dari Nonamey — jangan flash sekarang)

```
adb push out/boot_v17_artix.img /data/local/tmp/boot_v17_artix.img
# lalu salah satu:
fastboot flash boot_a /data/local/tmp/boot_v17_artix.img
# atau di device root:
dd if=/data/local/tmp/boot_v17_artix.img of=/dev/block/by-name/boot_a bs=4M
```
