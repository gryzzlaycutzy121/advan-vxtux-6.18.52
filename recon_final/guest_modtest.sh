#!/system/bin/sh
export HOME=/data/local/tmp
ulimit -l unlimited 2>/dev/null
LOG=/data/local/tmp/kvm/lkvm_modtest.log
echo "[launcher] HOME=$HOME ulimit-l=$(ulimit -l) uid=$(id -u)" > $LOG
echo "[launcher] starting lkvm: $(date)" >> $LOG
exec /data/local/tmp/kvm/lkvm run \
  --kernel /data/local/tmp/kvm/Image_modtest_embedded \
  --initrd /data/local/tmp/kvm/initramfs_padded.cpio.gz \
  -m 4096 -c 4 \
  --console serial \
  --name vxtux-modtest \
  -p 'console=ttyAMA0 rdinit=/init panic=1 loglevel=7' \
  >> $LOG 2>&1
