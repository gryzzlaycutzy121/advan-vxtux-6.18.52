@echo off
rem ============================================================================
rem  DUMP SEMUA KANAL LOG  (spd_dump, mode download)
rem
rem  Target: tablet MATI -> tahan VOL DOWN -> colok USB -> jalankan skrip ini.
rem
rem  Pemakaian:
rem     dump_all.bat           RAM + partisi log (~31 MB, ~1-2 menit)
rem     dump_all.bat full      + partisi cache 128 MB (~7-8 menit)
rem
rem  Yang diambil (RAM, selamat melewati warm reset):
rem     0xfff80000 256 KiB   ramoops (pstore record + console + pmsg)
rem     0xAFC00000   4 MiB   vxtuxlog (snapshot kmsg_dump, 2 slot)
rem     0x9de80000 512 KiB   uboot_log (log LK: boot reason, cmdline, panic LK)
rem     0x80001000   4 KiB   sysdumpinfo (struct sysdump stok)
rem     0x9f000000  16 MiB   sysdump-uboot (area dump vendor/LK)
rem  Partisi: uboot_log, sysdumpdb, misc  (+ cache bila "full")
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
if /i "%~1"=="full" set FULL=1

if not exist "%OUT%" mkdir "%OUT%"
echo ==== DUMP SEMUA LOG P1 ====
echo keluaran: %OUT%
echo mode    : %FULL% (1 = ikut cache 128 MB)
echo.

rem --- rantai perintah spd_dump -------------------------------------------
if "%FULL%"=="1" goto full

spd_dump_new.exe --wait 300 exec_addr 0x3ee8 ^
  fdl fdl1-dl.bin 0x5500 ^
  fdl fdl2-dl.bin 0x9efffe00 ^
  exec ^
  baudrate 4000000 ^
  blk_size 65535 ^
  path "%OUT%" ^
  read_mem 0xfff80000 0x40000 a1_ramoops.bin ^
  read_mem 0xAFC00000 0x400000 a2_vxtuxlog.bin ^
  read_mem 0x9de80000 0x80000 a3_uboot_log_ram.bin ^
  read_mem 0x80001000 0x1000 a4_sysdumpinfo.bin ^
  read_mem 0x9f000000 0x1000000 a5_sysdump_uboot.bin ^
  read_part uboot_log 0 0x400000 b1_uboot_log.bin ^
  read_part sysdumpdb 0 0xA00000 b2_sysdumpdb.bin ^
  read_part misc 0 0x100000 b3_misc.bin ^
  reset
goto extract

:full
spd_dump_new.exe --wait 300 exec_addr 0x3ee8 ^
  fdl fdl1-dl.bin 0x5500 ^
  fdl fdl2-dl.bin 0x9efffe00 ^
  exec ^
  baudrate 4000000 ^
  blk_size 65535 ^
  path "%OUT%" ^
  read_mem 0xfff80000 0x40000 a1_ramoops.bin ^
  read_mem 0xAFC00000 0x400000 a2_vxtuxlog.bin ^
  read_mem 0x9de80000 0x80000 a3_uboot_log_ram.bin ^
  read_mem 0x80001000 0x1000 a4_sysdumpinfo.bin ^
  read_mem 0x9f000000 0x1000000 a5_sysdump_uboot.bin ^
  read_part uboot_log 0 0x400000 b1_uboot_log.bin ^
  read_part sysdumpdb 0 0xA00000 b2_sysdumpdb.bin ^
  read_part misc 0 0x100000 b3_misc.bin ^
  read_part cache 0 0x8000000 b4_cache.bin ^
  reset

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
echo ==== SELESAI ====
echo Buka: %OUT%\01-RINGKAS.txt  (penanda penting)
echo       %OUT%\00-SEMUA.txt     (semua baris)
pause
endlocal
