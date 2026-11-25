#!/usr/bin/env bash
# Rekonstruksi end-to-end: verifikasi runtime modul sc27xx_typec versi FIX
# di LKVM guest pada device Android (kernel 6.18.52-VXTux port).
#
# Prasyarat di host:
#   - E:/vxtux_build_repo/initramfs_padded.cpio.gz   (archive 232 modul, baseline 229/232)
#   - E:/vxtux_build_repo/_typec_verify/drivers/usb/typec/sprd/sc27xx_typec.ko  (ko FIX, md5 d45475bb)
#   - E:/vxtux_build_repo/recon_final/splice_typec_fix.py
#   - adb (scrcpy-win64-v4.1), device online, /dev/kvm ada
#
# Prasyarat di device:
#   - /data/local/tmp/kvm/Image_modtest_embedded   (CI-built kernel + built-ins)
#   - /data/local/tmp/kvm/lkvm                     (binary lkvm)
#
# Hasil: /data/local/tmp/kvm/lkvm_typecfix.log  ->  SUMMARY loaded=229 failed=0 + MODTEST-PASS
set -euo pipefail

REPO="E:/vxtux_build_repo"
ADB="${ADB:-C:/Users/Administrator/Downloads/scrcpy-win64-v4.1/scrcpy-win64-v4.1/adb.exe}"
PY="$(ls -d /c/Users/Administrator/AppData/Local/hermes/tools/python-3.14.9*/python.exe | head -1)"
D=/data/local/tmp/kvm

SRC_ARCHIVE="$REPO/initramfs_padded.cpio.gz"
FIX_KO="$REPO/_typec_verify/drivers/usb/typec/sprd/sc27xx_typec.ko"
OUT_ARCHIVE="$REPO/_padded_typecfix.cpio.gz"

echo "[1/5] sisip modul FIX ke initramfs (byte-level, panjang sama)"
"$PY" "$REPO/recon_final/splice_typec_fix.py" "$SRC_ARCHIVE" "$FIX_KO" "$OUT_ARCHIVE"

echo "[2/5] push initramfs ke device"
"$ADB" push "$OUT_ARCHIVE" "$D/initramfs_typecfix2.cpio.gz"
"$ADB" shell "md5sum $D/initramfs_typecfix2.cpio.gz"

echo "[3/5] push launcher"
cat > "$REPO/_guest_typecfix2.sh" <<'EOF'
#!/system/bin/sh
export HOME=/data/local/tmp
ulimit -l unlimited 2>/dev/null
LOG=/data/local/tmp/kvm/lkvm_typecfix2.log
echo "[launcher] HOME=$HOME ulimit-l=$(ulimit -l) uid=$(id -u)" > $LOG
echo "[launcher] starting lkvm: $(date)" >> $LOG
exec /data/local/tmp/kvm/lkvm run \
  --kernel /data/local/tmp/kvm/Image_modtest_embedded \
  --initrd /data/local/tmp/kvm/initramfs_typecfix2.cpio.gz \
  -m 4096 -c 4 \
  --console serial \
  --name vxtux-typecfix2 \
  -p 'console=ttyAMA0 rdinit=/init panic=1 loglevel=7' \
  >> $LOG 2>&1
EOF
"$ADB" push "$REPO/_guest_typecfix2.sh" "$D/guest_typecfix2.sh"
"$ADB" shell "chmod 755 $D/guest_typecfix2.sh; chown 2000:2000 $D/guest_typecfix2.sh"

echo "[4/5] boot guest (root: /dev/kvm ber-mode 0600 system)"
"$ADB" shell "su -c 'chmod 666 /dev/kvm; nohup $D/guest_typecfix2.sh > /dev/null 2>&1 &'"
sleep 32

echo "[5/5] cek hasil"
"$ADB" shell "grep -aE 'LOAD OK   drivers/usb/typec/sprd/sc27xx_typec|SUMMARY|MODTEST-PASS|MODTEST-FAIL' $D/lkvm_typecfix2.log"
"$ADB" shell "grep -aoE 'FAILED [^ ]+\.ko \(errno=-[0-9]+\)' $D/lkvm_typecfix2.log | sort -u" || true
"$ADB" pull "$D/lkvm_typecfix2.log" "$REPO/recon_final/lkvm_typecfix_final.log"

echo
echo "PASS criteria: 'LOAD OK drivers/usb/typec/sprd/sc27xx_typec.ko'"
echo "               'SUMMARY: total=232 loaded=229 skipped=3 failed=0'"
echo "               'VXTUX-MODTEST-PASS'  dan tidak ada baris FAILED"
echo "Jalankan 2x untuk bukti determinisme."
