@echo off
rem ============================================================================
rem  DUMP SEMUA PARTISI LOG  (spd_dump, mode download) - v3
rem
rem  Target: tablet MATI -> tahan VOL DOWN -> colok USB -> jalankan skrip ini.
rem
rem  PENTING (diukur 2026-11-18): `read_mem` TIDAK DIDUKUNG FDL2 perangkat ini
rem  (balasan type 0xfe -> "unexpected response 0x00fe", read: 0x0). Jadi region
rem  RAM (0xAFC00000 vxtuxlog, 0xfff80000 ramoops, dst) TIDAK bisa dibaca via
rem  spd_dump.  Satu-satunya kanal log kernel 6.18.52 yang BISA dibaca = RING
rem  VXTUXLOG1 di ekor partisi cache (read_part cache 0x5C00000 0x800000).
rem
rem  Pemakaian:
rem     dump_all.bat          semua partisi log + ring cache (~40 MB, ~2 menit)
rem     dump_all.bat full     seperti default + SELURUH cache 100 MB (~8 menit)
rem     dump_all.bat keep     tanpa reset (device tetap di mode download)
rem
rem  KANAL PARTISI (read_part - berfungsi):
rem     cache ring   8 MiB  RING VXTUXLOG1 (4 slot x 2 MiB), offset 0x5C00000
rem                         (ekor partisi cache 100 MiB)  <- LOG KERNEL 6.18
rem     cache full  100 MiB  seluruh partisi (mode "full": ylog stock + ring)
rem     uboot_log    4 MiB  log LK/u-boot (boot reason, cmdline, panic LK)
rem     sysdumpdb   10 MiB  minidump vendor/kernel
rem     misc         1 MiB  BCB (boot control block)
rem     miscdata     1 MiB  data slot / boot_completed
rem     metadata    16 MiB  A/B slot + boot metrics
rem
rem  KANAL RAM (read_mem - TIDAK berfungsi di FDL2 ini; lihat dump_ram.bat):
rem     vxtuxlog 0xAFC00000 | ramoops 0xfff80000 | rebootescrow 0xfff70000 |
rem     uboot_log_ram 0x9de80000 | sysdumpinfo 0x80001000 | sysdump_uboot 0x9f000000
rem
rem  Catatan ring: lr_base = (cache_size - 8 MiB) & ~1 MiB.  Untuk cache 100 MiB
rem  (0x6400000): 0x6400000 - 0x800000 = 0x5C00000.  Driver menulis ke ring pada
rem  boot 6.18 berikutnya (harvest_prev) atau tiap 3 s sekali (kthread).
rem
rem  Hasil: E:\spd_dump_logs\p1-<tanggal-jam>\
rem           *.bin          dump mentah
rem           *.text.txt     teks hasil ekstraksi
rem           *.rec-*.txt    record vxtuxlog (crc diperiksa)
rem           00-SEMUA.txt   gabungan semua
rem           01-RINGKAS.txt penanda penting (panic/oops/deferred/...)
rem ============================================================================
setlocal EnableDelayedExpansion
cd /d E:\spd_flash

set STAMP=%DATE:/=-%_%TIME::=-%
set STAMP=%STAMP: =0%
set OUT=E:\spd_dump_logs\p1-%STAMP%
set FULL=0
set KEEP=0
if /i "%~1"=="full" set FULL=1
if /i "%~1"=="keep" set KEEP=1

if not exist "%OUT%" mkdir "%OUT%"
echo ==== DUMP PARTISI LOG P1 (v3, read_mem mati) ====
echo keluaran: %OUT%
if "%FULL%"=="1" echo mode    : FULL (ikut seluruh cache 100 MB)
if "%KEEP%"=="1" echo mode    : KEEP (tanpa reset, device tetap di download mode)
if "%FULL%"=="0" if "%KEEP%"=="0" echo mode    : DEFAULT (semua partisi + ring cache)
echo.

if not exist spd_dump_new.exe (
  echo GAGAL: spd_dump_new.exe tidak ada di E:\spd_flash
  pause
  exit /b 1
)
if not exist fdl1-dl.bin (
  echo GAGAL: fdl1-dl.bin / fdl2-dl.bin tidak ada di E:\spd_flash
  pause
  exit /b 1
)

rem --- rantai perintah spd_dump: read_part dulu, reset di akhir -------------
if "%FULL%"=="1" goto full

spd_dump_new.exe --wait 300 exec_addr 0x3ee8 ^
  fdl fdl1-dl.bin 0x5500 ^
  fdl fdl2-dl.bin 0x9efffe00 ^
  exec ^
  path "%OUT%" ^
  read_part cache 0x5C00000 0x800000 b6_cache_ring.bin ^
  read_part uboot_log 0 0x400000 b1_uboot_log.bin ^
  read_part sysdumpdb 0 0xA00000 b2_sysdumpdb.bin ^
  read_part misc 0 0x100000 b3_misc.bin ^
  read_part miscdata 0 0x100000 b4_miscdata.bin ^
  read_part metadata 0 0x1000000 b5_metadata.bin ^
  exit
if "%KEEP%"=="0" spd_dump_new.exe --wait 120 exec_addr 0x3ee8 fdl fdl1-dl.bin 0x5500 fdl fdl2-dl.bin 0x9efffe00 exec reset
goto extract

:full
spd_dump_new.exe --wait 300 exec_addr 0x3ee8 ^
  fdl fdl1-dl.bin 0x5500 ^
  fdl fdl2-dl.bin 0x9efffe00 ^
  exec ^
  path "%OUT%" ^
  read_part cache 0 0x6400000 b7_cache_full.bin ^
  read_part uboot_log 0 0x400000 b1_uboot_log.bin ^
  read_part sysdumpdb 0 0xA00000 b2_sysdumpdb.bin ^
  read_part misc 0 0x100000 b3_misc.bin ^
  read_part miscdata 0 0x100000 b4_miscdata.bin ^
  read_part metadata 0 0x1000000 b5_metadata.bin ^
  exit
if "%KEEP%"=="0" spd_dump_new.exe --wait 120 exec_addr 0x3ee8 fdl fdl1-dl.bin 0x5500 fdl fdl2-dl.bin 0x9efffe00 exec reset

:extract
echo.
echo ==== EKSTRAK TEKS ====
set N=0
for %%f in ("%OUT%\*.bin") do (
  node E:\vxtux_build_repo\p1\dump_extract.mjs "%%f"
  set /a N+=1
)
echo berkas diproses: !N!

echo.
echo ==== LAPORAN ====
node E:\vxtux_build_repo\p1\log_report.mjs "%OUT%"

echo.
echo ==== CEK RING VXTUXLOG1 (eMMC, jalur utama) ====
if exist "%OUT%\b6_cache_ring.rec-*.txt" (
  dir /b "%OUT%\b6_cache_ring.rec-*.txt"
) else (
  echo TIDAK ADA record di ring eMMC - kernel 6.18 belum sempat menulis ke ring
  echo (panic sebelum MMC, atau boot berikutnya bukan 6.18).  Log hanya di RAM
  echo 0xAFC00000, dan read_mem TIDAK didukung FDL2 ini.
)

echo.
echo ==== SELESAI ====
echo Buka: %OUT%\01-RINGKAS.txt  (penanda penting)
echo       %OUT%\00-SEMUA.txt     (semua baris)
pause
endlocal