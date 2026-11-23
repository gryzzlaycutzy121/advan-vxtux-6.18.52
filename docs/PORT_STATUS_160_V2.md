# Status Porting Driver Stok 5.4.210 → VXTux 6.18.52 (final v2)

Bahan: 160 modul stok ter-load di ADVAN Tab VX Lite (`/proc/modules` + `/vendor*/lib/modules`),
hasil RE `assembly_160.zip` (160/160 modul punya symtab runtime, total 77.791 simbol),
tree port `E:/vxtux_build_repo/6.18.52-VXTux-UMS512` branch `p1-build`,
CI fork `gryzzlaycutzy121/advan-vxtux-6.18.52` (workflow `build-modules-verify.yml`).

**Run verifikasi terbaru: `45106765053` → `success` → 242 `.ko` ter-build dari branch `p1-build`
(riwayat: `45074672882` 240 `.ko` → `45100242777` 241 `.ko` + `ion_cma_heap.ko` → `45106765053` 242 `.ko`
+ `sprd_autotest_pinctrl.ko`).**

## 1. Peta kesenjangan (160 modul stok vs port 6.18) — setelah sesi backport ION

| Kategori | Jumlah | Dasar bukti |
|---|---|---|
| **KO-TERBANGUN** (nama sama, `.ko` ada di artifact CI) | **72** | `built-kos` run 45106765053 (termasuk `ion_cma_heap.ko`, `sprd_autotest_pinctrl.ko`) |
| **PORT-SOURCE** (source ada, nama/arsitektur berbeda) | **63** | 42 di `drivers/vxtux/` (RE-port), sisanya subtree rename (musb_sprd_vxtux, sprd_dcam, sprd_drm, sprd-iommu, mmdvfs…) |
| **UPSTREAM/GENERIC** (fitur kernel 6.18, nama berbeda) | **14** | arc4, ghash_ce, sha1_ce, kfifo_buf, ledtrig_pattern, mmc_swcq, thermal_generic_adc, twofish_common, twofish_generic, zram, zsmalloc, sprd_charger_manager, sprd_cp_dvfs, unisoc_iommu |
| **BFKRT milik sendiri** (belum di-port ke 6.18) | **3** | berry_symdump, berry_m_virtkrn_obs, berry_lkvm (+ berry_trig_all, berryfetcher_kernel) |
| **SISA FUNGSIONAL** | **0** | `ion_cma_heap` ✅ ter-build, `pinctrl` ✅ ter-port (handler autotest) |

## 2. Port baru (4 driver)

| Modul stok | Donor | Adaptasi 6.18 | Bukti |
|---|---|---|---|
| `bq2560x_charger` | `drivers/power/supply/bq2560x-charger.c` (Veynamer `b9a76872`) | `enum cm_fast_charge_command` + `enum power_supply_wireless_charger_type` (nilai OEM `0x20/0x21/0x22`), `.of_node`→`.fwnode = dev_fwnode(dev)`, `GPIOF_DIR_IN`→`GPIOD_IN`, `i2c_driver.probe` 2-arg→1-arg, `.remove`→`void`, include `sprd/sprd_battery_info.h` | `bq2560x-charger.ko` di artifact run 45074672882 |
| `i2c_sprd_hw` | `drivers/i2c/busses/i2c-sprd-hw.c` (Veynamer `b9a76872`) | API `i2c_adapter`/`master_xfer`/`pm_runtime` kompatibel 6.18 tanpa perubahan | `i2c-sprd-hw.ko` di artifact run 45074672882 |
| `ion_cma_heap` (+ inti ION) | OEM 5.4 `drivers/staging/android/ion/**` | backport inti ION (ABI `/dev/ion`): `shrinker` → `shrinker->private_data`, field `dma_buf_ops` yang dihapus upstream dibuang, `cma->ranges[0]` → `cma_get_base()`/`cma_get_size()` (accessor resmi 6.18), `linux/dma-noncoherent.h` → `linux/dma-map-ops.h`, `dma_buf_attachment.dma_map_attrs` dihapus upstream → attrs 0, header `ion_ipc_trusty.h` lokal → `<linux/ion_ipc_trusty.h>`, `struct ion_message` + `enum ion_command` (TA_LOCK/UNLOCK_DRM_MEM) ditambahkan | `drivers/staging/android/ion/heaps/ion_cma_heap.ko` di artifact run **45100242777** (241 `.ko`); import `cma_get_base/cma_get_size/cma_get_name`, `__ion_device_add_heap`, `ion_tipc_*` terverifikasi |
| `pinctrl` (harness autotest) | RE blob `pinctrl.ko` (`init_module`, `pinctrl_pre_test`, `pinctrl_test`) | handler `.data`: `name="pinctrl"`, `cmd=0xc0044100` (_IOWR('A',0,4)), `init`=`pinctrl_pre_test`, `test`=`pinctrl_test`; `of_find_compatible_node(NULL,NULL,"sprd,autotest")` → phandle `"sprd,pinctrl"` → `of_find_device_by_node()` → `devm_pinctrl_get()` → `pinctrl_lookup_state("gpio_%d")` → `pinctrl_select_state()` | `drivers/vxtux/soc/autotest/sprd_autotest_pinctrl.c` (commit `aea5319d4`), CI `45104148090` |

Keduanya (`bq2560x`, `i2c_sprd_hw`) sebelumnya **hanya ada simbol config** tanpa Kconfig/source
→ tidak pernah ter-build. `ion_cma_heap` & `pinctrl` punya simbol config
(`CONFIG_ION_CMA_HEAP=m`, `CONFIG_SPRD_AUTOTEST_PINCTRL=m`) tetapi **tanpa source** → phantom.

### Riwayat CI (fakta, bukan klaim)

| Run | Hasil | Isu yang diperbaiki |
|---|---|---|
| `45071545990` | gagal | 7 error API 6.18 di `bq2560x-charger.c` (enum charger-manager, fwnode, GPIOD_IN, i2c probe/remove) |
| `45074672882` | sukses, 240 `.ko` | — |
| `45084863441` | gagal | ION: `ion_ipc_trusty.h` lokal tidak ada, `linux/dma-noncoherent.h` dihapus di 6.18, `plist_node_init` implisit |
| `45087305388` | gagal | ION: `struct cma` opaque di 6.18 (`ranges[]`/`count` tak terlihat) |
| `45090405358` | gagal | ION: `struct ion_message`/`TA_LOCK_DRM_MEM` belum ada di header port |
| `45092394909` | gagal | ION: `dma_buf_attachment.dma_map_attrs` dihapus upstream |
| `45097266443` | gagal | modpost: `cma_get_base`/`cma_get_size` belum di-export dari `mm/cma.c` |
| `45100242777` | **sukses, 241 `.ko`** | `ion_cma_heap.ko` terbentuk |
| `45104148090` | gagal | pinctrl: `struct platform_device` tak lengkap (kurang `linux/platform_device.h`) |
| `45106765053` | **sukses, 242 `.ko`** | `sprd_autotest_pinctrl.ko` terbentuk, `depends=sprd_autotest_core` |

## 3. Ekuivalensi terverifikasi (stok → port, nama berbeda)

| Modul stok | Ekuivalen port 6.18 | Bukti |
|---|---|---|
| `core` | `sprd_autotest_core.c` | `drivers/vxtux/soc/autotest/` |
| `pinctrl` | `sprd_autotest_pinctrl.c` (baru, sesi ini) | `drivers/vxtux/soc/autotest/` |
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

## 4. Yang tersisa

1. **Modul BFKRT milik sendiri** (`berry_symdump`, `berry_m_virtkrn_obs`, `berry_lkvm`,
   `berry_trig_all`, `berryfetcher_kernel`) belum ada di tree 6.18 — port sudah punya suite
   sendiri `drivers/vxtux/debug/` (vxtux_hook, vxtux_lkfb, vxtux_beacon, vxtux_logrec,
   vxtux_flightrec, vxtux_inittrace). Porting BFKRT ke 6.18 = pekerjaan terpisah (kernel 6.18
   sudah full CFI-Clang + LTO → perlu audit CFI ulang).
2. **Validasi runtime**: modul baru (`ion_cma_heap.ko`, `sprd_autotest_pinctrl.ko`) baru
   terverifikasi **build** di CI; belum di-`insmod` ke device (device masih jalan kernel OEM 5.4).
3. `CONFIG_ION=y` → inti ION jadi **built-in** di vmlinux 6.18; `CONFIG_ION_CMA_HEAP=m` →
   heap CMA tetap modul (parity dengan stok).

## 5. Perbaikan CI (wajib, tanpa ini verifikasi beracak)

`.github/workflows/build-modules-verify.yml` sebelumnya hardcode `ref: test1`, sehingga run "verify"
selalu membangun branch basi dan hasilnya tidak bisa dipakai memverifikasi branch kerja.
Diubah ke `ref: ${{ inputs.ref != '' && inputs.ref || github.ref_name }}` + input `workflow_dispatch`.
Semua angka modul ter-build di dokumen ini berasal dari run yang benar-benar memakai branch `p1-build`.

## 6. Artefak & cara reproduksi

- Bahan RE: `assembly_160.zip` (`dis/` 160 file, `modsyms/` 160 simpanan simbol, `INDEX_160.md`, `SUMMARY_160.md`, `README.md`).
- Data klasifikasi: `scratch/gap_final.json` (160 baris verdict), `scratch/gap_srcverify.json`, `scratch/pat_map.json`.
- Artifact build terbaru: run CI `45106765053` → artifact `built-kos` (242 `.ko`).
- Peta modul stok→`.ko` lokal: `scratch/map_ko.txt` (160 baris).
- Donor: `Veynamer/android_kernel_unisoc_ums512` ref `b9a76872` (path sumber ter-embed di `.ko` stok)
  + `E:/Advan/out/odm54` (tree OEM 5.4 untuk backport ION).
