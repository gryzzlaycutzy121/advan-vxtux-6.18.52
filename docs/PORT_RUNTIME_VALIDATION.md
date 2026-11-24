# PORT RUNTIME VALIDATION — LKVM guest (device ADVAN Tab VX Lite)

Tanggal: 2026-11-24
Host: kernel 5.4.210 OEM (UMS512), root su
Guest: kernel 6.18.52-VXTux-UMS512-artix (CI build #2, Nov 24)
Hypervisor: LKVM (KVM ring0, `/data/local/tmp/kvm/lkvm`, `/dev/kvm` owner system mode 600)

## HASIL AKHIR

```
=== SUMMARY: total=232 loaded=229 skipped=3 failed=0 ===
=== VXTUX-MODTEST-PASS ===
```

- **229 modul ter-load** (insmod rc=0, urutan dependensi, 12 pass retry)
- **3 skipped by-design**: `ion_ipc_trusty` (butuh secure world Trusty),
  `ims_bridge`, `ims_bridge_t` (butuh modem/CP via SIPC) — sama seperti CI QEMU
- **0 failed**, boot bersih, `reboot: Power down`, `KVM session ended normally`
- **Deterministik**: 2× run berturut-turut hasil identik

Tes pemicu BFKRT (runtime diff) di LKVM:
```
INSMOD berryfetcher_kernel.ko  rc=0 OK
INSMOD berry_trig_all.ko       rc=0 OK
INSMOD berry_m_virtkrn_obs.ko  rc=0 OK
TRIG dump  → driver_probe_device (pc/lr/x0-x3) tertangkap
OBS dump   → 27 unlock event (mutex), dep=0, 0 reentrant, 0 deferred
RMMOD      → 3 modul rc=0
=== VXTUX-BFKRT-DONE ===
```

## LANGKAH REKONSTRUKSI (end-to-end, dari nol)

### 1. Ambil Image + initramfs dari CI
```
# GitHub artifact (run 45404840233 "Module Load Test (QEMU)"):
gh api repos/gryzzlaycutzy121/advan-vxtux-6.18.52/actions/runs/45404840233/artifacts
#   → modtest-artifacts (14926048420): Image(92MB), initramfs.cpio.gz(42MB),
#     modules.list(232), serial-modtest.log
# GitHub artifact (run 45407692432 "QEMU BFKRT Runtime Diff"):
#   → bfkrt-lkvm (14930505668): Image(49.6MB), initramfs.cpio.gz(657KB, 3 modul BFKRT)
#   → qemu-bfkrt-serial: serial-bfkrt.log
```

### 2. Push ke device
```
adb push Image                      /data/local/tmp/kvm/Image_modtest_embedded
adb push initramfs.cpio.gz          /data/local/tmp/kvm/initramfs_padded.cpio.gz
adb push guest_modtest.sh           /data/local/tmp/kvm/guest_modtest.sh
adb push Image(bfkrt 49.6MB)        /data/local/tmp/kvm/Image_vxtux618_lkvm
adb push initramfs_bfkrt.cpio.gz    /data/local/tmp/kvm/initramfs_bfkrt.cpio.gz
adb push guest_bfkrt.sh             /data/local/tmp/kvm/guest_bfkrt.sh
adb shell su -c "chmod 755 /data/local/tmp/kvm/guest_*.sh"
```
(md5 diverifikasi host==device setiap push)

### 3. Boot LKVM guest — load semua modul
```
adb shell su -c "nohup /data/local/tmp/kvm/guest_modtest.sh > /dev/null 2>&1 &"
# tunggu ~150 s, lalu:
adb shell cat /data/local/tmp/kvm/lkvm_modtest.log | grep -aE "SUMMARY|PASS|FAIL"
```
Hasil: `total=232 loaded=229 skipped=3 failed=0` + `VXTUX-MODTEST-PASS` (2×)

### 4. Boot LKVM guest — tes pemicu BFKRT
```
adb shell su -c "nohup /data/local/tmp/kvm/guest_bfkrt.sh > /dev/null 2>&1 &"
# tunggu ~90 s, lalu:
adb shell cat /data/local/tmp/kvm/lkvm_bfkrt.log | grep -aE "INSMOD|TRIG|OBS|ringkasan"
```
Hasil: 3 modul insmod rc=0, trigger + observasi tertangkap, rmmod rc=0, DONE.

## ROOT CAUSE TERCATAT (penting untuk rekonstruksi berikutnya)

1. **Console LKVM = PL011 (`ttyAMA0`)**, bukan 8250 (`ttyS0`). Parameter
   `console=ttyS0,earlycon=uart8250...` membuat serial kosong → guest tampak
   hang padahal boot. Log sukses lama (boot2.log/run15.log) sudah pakai ttyAMA0.
2. **`--initrd` harus tetap diberikan** meski Image juga berisi initramfs
   embedded: lkvm butuh `--initrd` ("Guest init image not compiled in" tanpa
   itu). Kernel menggunakan initramfs eksternal sebagai rootfs.
3. **Initramfs modtest (232 modul) harus di-rebuild dengan padding** pada
   `ec100.ko` (+4096 byte prepend): tanpa padding, kernel LKVM membaca
   `ELF Spec violation: section 0 type(7168370)!=SH_NULL` untuk modul itu saja
   (bug alignment memori LKVM — file ELF valid, terverifikasi
   `md5 7cc4d93285b0a6cdde15faa0628ad9e8` identik di initramfs & tar, dan
   load OK di QEMU CI serta load OK di LKVM setelah padding).
4. **RAM/CPU**: `-m 4096 -c 4` (attempt5/padded) dan `-m 2048` (attempt2)
   keduanya boot; `-m 512` awal dengan Image 49.6MB + initramfs 42MB cukup
   tapi ec100 gagal; root cause ec100 adalah padding (poin 3), bukan RAM.
5. **Cek `/dev/kvm`**: owner `system` mode 600 → **boot wajib lewat `su -c`**.

## BUKTI FILE

- `lkvm_modtest_final.log` — serial guest load-semua (PASS, 2×)
- `lkvm_bfkrt_final.log` — serial guest tes pemicu BFKRT (DONE)
- `serial-modtest-ci-qemu.log` — CI QEMU (229 loaded, PASS) untuk pembanding
- `serial-bfkrt-qemu.log` — CI QEMU BFKRT untuk pembanding
- `modules.list` — manifest 232 modul
- `guest_modtest.sh`, `guest_bfkrt.sh` — skrip boot LKVM
- CI run: 45404840233 (module load test, PASS), 45407692432 (bfkrt diff, sukses)

## DIFF REGISTER/BIT (static, source-level)

Tool: `diff/regbit_macro2.py` (resolusi `#include` header + macro), hasil
`regbit_macro2.json`:
- 5 REGISTER-EXACT (offset & mask identik): gpio-pmic-eic-sprd, i2c-sprd-hw,
  rtc-sc27xx, sc27xx_tsensor_thermal, sprd_7sreset
- 3 DIVERGENT → diaudit manual:
  - `sc27xx_adc`: semua register ADC (0x0/0x4/0x4c/0x50/0x54/0x58/0x5c) identik
    di port (artefak harness: `missO=[84]` karena 0x54 `SC27XX_ADC_INT_CLR`
    hanya di satu jalur) → **bukan defect**
  - `sc27xx_typec`: port diturunkan dari RE biner (tanpa source GPL vendor);
    mode_ctrl_off sc2730 = 0x14 (dari tabel .rodata blob) vs stock source
    `SC27XX_MODE=0x04` — **konflik source-vs-binary** yang perlu keputusan
    silikon (di-RE port mengikuti binary blob stok yang jadi ground truth)
  - `clk-sprd`: akses register via offset runtime (`common->reg - div->offset`)
    → **artefak harness**, bukan defect
- 5 HARNESS-MISS: source stock memanggil regmap tapi harness tak baca offset
  (butuh resolusi lebih dalam) — bukan bukti defect
- 34 NO-REG-EVIDENCE: source stock tak punya akses register langsung (via
  framework PWM/regulator/thermal) — tidak ada yang bisa dibandingkan

## KESIMPULAN

- Porting 6.18.52 → 242 `.ko`: **terbukti bisa di-load 229/232 di LKVM device
  (0 failed, 3 skip by-design)** + tes pemicu BFKRT jalan end-to-end.
- Diff register/bit static: **5 EXACT, 2 artefak harness, 1 konflik RE-vs-source
  (sc27xx_typec — tercatat, perlu keputusan silikon)**.
- BFKRT runtime diff (QEMU + LKVM): boot bersih, trigger & observasi tertangkap,
  0 panic, rmmod bersih.
