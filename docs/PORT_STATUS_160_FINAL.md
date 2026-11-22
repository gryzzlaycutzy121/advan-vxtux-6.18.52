# Status Porting Driver Stok 5.4.210 → VXTux 6.18.52 (final)

Bahan: 160 modul stok ter-load di ADVAN Tab VX Lite (`/proc/modules` + `/vendor*/lib/modules`),
hasil RE `assembly_160.zip` (160/160 modul punya symtab runtime, total 77.791 simbol),
tree port `E:/vxtux_build_repo/6.18.52-VXTux-UMS512` branch `p1-build`,
CI fork `gryzzlaycutzy121/advan-vxtux-6.18.52` (workflow `build-modules-verify.yml`).

**Run verifikasi utama: `45074672882` → `success` → 240 `.ko` ter-build dari branch `p1-build`.**

## 1. Peta kesenjangan (160 modul stok vs port 6.18)

| Kategori | Jumlah | Dasar bukti |
|---|---|---|
| **KO-TERBANGUN** (nama sama, `.ko` ada di artifact CI) | **70** | `built-kos` run 45074672882 |
| **PORT-SOURCE** (source ada, nama/arsitektur berbeda) | **63** | 41 di `drivers/vxtux/` (RE-port), sisanya subtree rename (musb_sprd_vxtux, sprd_dcam, sprd_drm, sprd-iommu, mmdvfs…) |
| **UPSTREAM/GENERIK** (fitur kernel 6.18, nama berbeda) | **14** | arc4, ghash_ce, sha1_ce, kfifo_buf, ledtrig_pattern, mmc_swcq, thermal_generic_adc, twofish_common, twofish_generic, zram, zsmalloc, sprd_charger_manager, sprd_cp_dvfs, unisoc_iommu |
| **BFKRT milik sendiri** (belum di-port ke 6.18) | **3** | berry_symdump, berry_m_virtkrn_obs, berry_lkvm (+ berry_trig_all, berryfetcher_kernel) |
| **SISA BENAR-BENAR BELUM ADA** | **2** | `ion_cma_heap`, `pinctrl` (modul uji) |

## 2. Yang dikerjakan sesi ini (2 port baru, build terverifikasi)

| Modul stok | Donor | Adaptasi 6.18 | Bukti |
|---|---|---|---|
| `bq2560x_charger` | `drivers/power/supply/bq2560x-charger.c` (Veynamer `b9a76872`) | `enum cm_fast_charge_command` + `enum power_supply_wireless_charger_type` (nilai OEM: `0x20/0x21/0x22`), `.of_node`→`.fwnode = dev_fwnode(dev)`, `GPIOF_DIR_IN`→`GPIOD_IN`, `i2c_driver.probe` 2-arg→1-arg, `.remove`→`void`, include `sprd/sprd_battery_info.h` | `bq2560x-charger.ko` ada di artifact CI 45074672882 |
| `i2c_sprd_hw` | `drivers/i2c/busses/i2c-sprd-hw.c` (Veynamer `b9a76872`) | API `i2c_adapter`/`master_xfer`/`pm_runtime` kompatibel 6.18 tanpa perubahan | `i2c-sprd-hw.ko` ada di artifact CI 45074672882 |

Keduanya sebelumnya **hanya ada simbol config** (`CONFIG_CHARGER_BQ2560X=m`, `CONFIG_I2C_SPRD_HW=m`)
tanpa Kconfig/source → tidak pernah ter-build. Sekarang: Kconfig + Makefile + source ada.
CI run `45071545990` (percobaan pertama) gagal dengan 7 error API → semuanya diperbaiki →
run `45074672882` sukses, artifact `built-kos` berisi kedua `.ko` baru itu.

## 3. Ekuivalensi terverifikasi (stok → port, nama berbeda)

| Modul stok | Ekuivalen port 6.18 | Bukti |
|---|---|---|
| `core` | `sprd_autotest_core.c` | `drivers/vxtux/soc/autotest/` |
| `sblock_bridge`, `sbuf_bridge` | `drivers/vxtux/sipc/{sblock,sbuf}.c` + `sprd_actions_queue.c` | `sprd_create_action_queue(name, do_actions, …)` & `sprd_add_action_ex()` = mekanisme `sb_bridge_do_actions`/`sb_sblock_action_calculate` stok; `sblock_register_notifier_ex()` = penerima event `sblock_a_cb`/`sbuf_b_cb` |
| `musb_sprd`, `musb_hdrc` | `drivers/usb/musb/musb_sprd_vxtux.c` | config `USB_MUSB_SPRD_VXTUX=y` |
| `gnss_common_ctl_all`, `gnss_pmnotify_ctl`, `gnss_dbg` | `sprdwcn/boot/wcn_integrate*.c` + `sprdwcn/platform/gnss/gnss_dbg.c` | 6/6 simbol fungsi stok ketemu di source port |
| `sprd_camera` | `drivers/media/platform/sprd/pipeline/sprd_dcam.c` | file ada (nama DCAM) |
| `sprd_drm` | `drivers/gpu/drm/sprd/sprd_drm.c` | file ada |
| `sprd_camsys_pw_domain` | `drivers/pmdomain/sprd/sprd_camsys_pw_domain.c` | file ada |
| `sprd_ddr_dvfs` | `drivers/devfreq/sprd/mm/sprd_mm_dvfs_core.c` | file ada |
| `sc27xx_fuel_gauge` | `drivers/power/supply/sc27xx_fuel_gauge.c` | file ada |
| `snd_soc_sprd_codec_sc2730_power(_dev)` | `drivers/vxtux/sound/asoc/codec/sprd-audio-power.c` | file ada |
| `snd_soc_sprd_vbc_fe` | `drivers/vxtux/sound/asoc/dai_v4/sprd-fe-dai.c` | 6/6 simbol stok ketemu |
| `phy_sprd_sharkl5Pro` | `drivers/phy/sprd/phy-sprd-ums512.c` (RE-port ABI) | 4/6 simbol stok ketemu; Kconfig menyebut "hasil port ABI dari blob vendor phy-sprd-sharkl5Pro.ko" |
| `mali_gondul` | `drivers/vxtux/gpu/mali_kbase/**` | 5/6 simbol stok ketemu |
| `sprd_wlan_combo`, `wcn_bsp` | `drivers/net/wireless/sprd/sprdwcn/**` | 6/6 simbol stok ketemu |
| `sprd_sensor` | `drivers/media/platform/sprd/sensors/sprd_sensor/` | 6/6 |

## 4. Sisa pekerjaan (2 item + tooling)

1. **`ion_cma_heap` — belum ada.** Port punya `drivers/dma-buf/sprd/sprd_ion.c` (mendaftarkan heap
   bertipe `"carveout"` saja) dan `drivers/dma-buf/heaps/cma_heap.c` (dma-buf), tapi **tidak** ada
   ION heap bertipe CMA seperti modul stok (`ion_cma_heap.ko`, `cma_heap_desc`, `cma_heaps`).
   Dampak: userspace Android yang butuh `/dev/ion` dengan heap CMA akan shortfall.
   Rencana: backport `drivers/staging/android/ion` + `ION_HEAP_TYPE_CMA` ke port, atau shim ION
   di atas dma-buf CMA heap.
2. **`pinctrl` — modul uji.** `pinctrl_handler`/`pinctrl_pre_test`/`pinctrl_test` (harness
   diagnostik OEM). Non-esensial; port sudah punya `pinctrl-sprd.c` + `pinctrl-sprd-sharkl5pro.c`.
3. **Modul BFKRT milik sendiri** (`berry_symdump`, `berry_m_virtkrn_obs`, `berry_lkvm`,
   `berry_trig_all`, `berryfetcher_kernel`) belum ada di tree 6.18 — port sudah punya suite
   sendiri `drivers/vxtux/debug/` (vxtux_hook, vxtux_lkfb, vxtux_beacon, vxtux_logrec,
   vxtux_flightrec, vxtux_inittrace). Porting BFKRT ke 6.18 = pekerjaan terpisah (kernel 6.18
   sudah full CFI-Clang + LTO → perlu cek CFI ulang).

## 5. Perbaikan CI (wajib, tanpa ini verifikasi beracak)

`.github/workflows/build-modules-verify.yml` sebelumnya hardcode `ref: test1`, sehingga run "verify"
selalu membangun branch basi dan hasilnya tidak bisa dipakai memverifikasi branch kerja.
Diubah ke `ref: ${{ inputs.ref != '' && inputs.ref || github.ref_name }}` + input `workflow_dispatch`.
Semua angka modul ter-build di dokumen ini berasal dari run yang benar-benar memakai branch `p1-build`.

## 6. Artefak & cara reproduksi

- Bahan RE: `assembly_160.zip` (`dis/` 160 file, `modsyms/` 160 simpanan simbol, `INDEX_160.md`, `SUMMARY_160.md`, `README.md`).
- Data klasifikasi: `scratch/gap_final.json` (160 baris verdict), `scratch/gap_srcverify.json`, `scratch/pat_map.json`.
- Artifact build: run CI `45074672882` → artifact `built-kos` (240 `.ko`).
- Peta modul stok→`.ko` lokal: `scratch/map_ko.txt` (160 baris).
- Donor: `Veynamer/android_kernel_unisoc_ums512` ref `b9a76872` (path sumber ter-embed di `.ko` stok).
