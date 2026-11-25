#!/usr/bin/env bash
# recon_lkvm_modtest.sh — REKONSTRUKSI end-to-end: boot LKVM guest VXTux 6.18.52
# dan load SEMUA modul port (229/232) + tes pemicu BFKRT.
#
# Prasyarat:
#   - device ADVAN Tab VX Lite terhubung, root su, /dev/kvm ada
#   - gh CLI auth (repo gryzzlaycutzy121/advan-vxtux-6.18.52)
#   - artifact CI run 45404840233 (modtest) & 45407692432 (bfkrt) masih ada
#
# Dipakai 2026-11-24 untuk bukti PORT_RUNTIME_VALIDATION.md.
set -e
REPO=gryzzlaycutzy121/advan-vxtux-6.18.52
GH=/c/Users/Administrator/AppData/Local/Temp/ghx/bin/gh.exe
ADB="C:/Users/Administrator/Downloads/scrcpy-win64-v4.1/scrcpy-win64-v4.1/adb.exe"
WORK=/e/vxtux_build_repo
KVM=/data/local/tmp/kvm

run_modtest() {
  echo "=== [1/2] load semua modul (LKVM) ==="
  $ADB shell su -c "rm -f $KVM/lkvm_modtest.log; nohup $KVM/guest_modtest.sh > /dev/null 2>&1 &"
  sleep 150
  $ADB shell "grep -aE 'SUMMARY|VXTUX-MODTEST-PASS|VXTUX-MODTEST-FAIL' $KVM/lkvm_modtest.log"
  $ADB pull $KVM/lkvm_modtest.log "$WORK/recon_final/lkvm_modtest_final.log"
}

run_bfkrt() {
  echo "=== [2/2] tes pemicu BFKRT (LKVM) ==="
  $ADB shell su -c "rm -f $KVM/lkvm_bfkrt.log; nohup $KVM/guest_bfkrt.sh > /dev/null 2>&1 &"
  sleep 90
  $ADB shell "grep -aE 'INSMOD|VXTUX-BFKRT-DONE|reentran' $KVM/lkvm_bfkrt.log"
  $ADB pull $KVM/lkvm_bfkrt.log "$WORK/recon_final/lkvm_bfkrt_final.log"
}

echo "=== cek prasyarat ==="
$ADB devices
$ADB shell 'su -c "ls -la /dev/kvm"' | grep -q kvm || { echo "FATAL: /dev/kvm tak ada"; exit 1; }

run_modtest
run_bfkrt
echo "=== SELESAI: bukti di $WORK/recon_final/ ==="
ls -la "$WORK/recon_final/"
