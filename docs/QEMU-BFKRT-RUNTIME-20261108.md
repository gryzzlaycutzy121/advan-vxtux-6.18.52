# Validasi Runtime BFKRT (QEMU) — 6.18.52-VXTux-UMS512

**Run CI hijau:** https://github.com/gryzzlaycutzy121/advan-vxtux-6.18.52/actions/runs/42457102028
(`qemu-bfkrt-diff.yml`, `test1` @ `75d98f151`, 2026-11-08, 33m41s)

**Gate:** `marker boot OK` + `3× INSMOD rc=0` + `VXTUX-BFKRT-DONE` → lolos.

Bukti serial: [`evidence-20261108/serial-bfkrt-ci42457102028.log`](evidence-20261108/serial-bfkrt-ci42457102028.log)

## Hasil per langkah

| Langkah | Hasil |
|---|---|
| Build DTB QEMU | ✓ |
| Build Image (`6.18.52-VXTux-UMS512-artix`) | ✓ |
| `make modules` (234 .ko) + `M=bfkrt_ko` (3 modul) | ✓ |
| Initramfs (init C statis, insmod + rekam) | ✓ |
| Boot QEMU (`-M virt`, gicv3, cortex-a72, 8 vCPU, 2G) | ✓ `VXTUX-QEMU-BOOT-OK` |

## Load & stabilitas driver

```
INSMOD berryfetcher_kernel.ko  rc=0 OK
INSMOD berry_trig_all.ko       rc=0 OK
INSMOD berry_m_virtkrn_obs.ko  rc=0 OK
rmmod berry_m_virtkrn_obs rc=0
rmmod berry_trig_all      rc=0
rmmod berryfetcher_kernel rc=0
```

- **TRIG** (kprobe ring 128): `driver_probe_device` terpicu, snapshot register terekam.
- **OBS** (10 detik): 26 catatan; **reentran = 0 kali** (indikasi deadlock),
  **kedalaman kunci > 8 = 0 kali**, **deferred = 0 kali**.
- **Panic / Oops / BUG / Call trace: 0** di seluruh log serial.

## Catatan (bukan kegagalan)

1. `resolver: tidak tersedia` di `/proc/bfkrt_sym` — `kallsyms_lookup_name`
   tidak terjangkau via kprobe pada 6.18 (liminasi yang sudah terdokumentasi
   di sumber modul untuk Android 5.7+). Modul tetap load, TRIG/OBS tetap jalan.
2. `platform gpio-keys: deferred probe pending` — tanpa HW gpio di QEMU (wajar).
3. `sprd_wdf: get public api error in sprd_wdt -19` (ENODEV) — PMIC WDT tidak
   ada di virt machine (wajar, tanpa crash).
4. Satu `hrtimer: interrupt took 17967104 ns` — lonjakan sekali saat insmod
   pertama (QEMU, wajar; bukan warning berulang).
