@echo off
rem ============================================================================
rem  Dump semua kanal log P1 lewat spd_dump (device harus masuk mode download:
rem  tablet MATI, tahan VOL DOWN, colok USB).
rem
rem  Yang diambil:
rem   1) ramoops   0xfff80000 256 KiB  -> dmesg/oops record + console pstore
rem   2) vxtuxlog  0xAFC00000   4 MiB  -> snapshot kmsg_dump (semua alasan)
rem   3) partisi cache                 -> /cache/vxtux-logs/*.txt
rem   4) partisi sysdumpdb             -> sisa dump vendor/LK
rem
rem  Hasil: E:\spd_dump_logs
rem ============================================================================
setlocal
cd /d E:\spd_flash
if not exist E:\spd_dump_logs mkdir E:\spd_dump_logs

echo ==== DUMP KANAL LOG P1 ====
spd_dump_new.exe --wait 300 exec_addr 0x3ee8 ^
  fdl fdl1-dl.bin 0x5500 ^
  fdl fdl2-dl.bin 0x9efffe00 ^
  exec ^
  baudrate 4000000 ^
  blk_size 65535 ^
  path E:\spd_dump_logs ^
  read_mem 0xfff80000 0x40000 p1_ramoops.bin ^
  read_mem 0xAFC00000 0x400000 p1_vxtuxlog.bin ^
  read_part cache 0 0x8000000 p1_cache.bin ^
  read_part sysdumpdb 0 0xA00000 p1_sysdumpdb.bin ^
  reset

echo.
echo ==== EKSTRAK TEKS ====
node E:\vxtux_build_repo\p1\dump_extract.mjs E:\spd_dump_logs\p1_ramoops.bin E:\spd_dump_logs\p1_vxtuxlog.bin
echo.
echo ==== SELESAI. Teks: E:\spd_dump_logs\p1_*.txt ====
pause
endlocal
