#!/bin/bash
# ============================================================================
#  lkvm_artix_boot.sh  --  boot Artix Linux VXTux penuh via lkvm + BFKRT
#
#  DIJALANKAN DI TABLET (root, :2222) -- bukan di PC.
#
#  Apa yang dilakukan:
#    1. Salin rootfs Artix native dari /.stage ke /root/artixstage.
#    2. Suntik modul berry (BFKRT) + service dinit `berry-load`.
#    3. Tambah sysprep (mount proc/sys/pts/shm/run) + getty serial autologin.
#    4. No-op chain system/default dinit yang tak relevan di VM virtio
#       (root-ro/fsck-root/cgroups/login.target/... supaya tak kaskade).
#    5. Bangun artix_rootfs.img (ext4 4G) via mke2fs -d.
#    6. Boot lkvm: bfkrt-Image + disk /dev/vda, rekam serial -> bootlog.
#
#  Hasil: /root/lkvm-artix-boot.log (+ salinan di repo out/logs/).
#
#  Catatan: Image kernel bfkrt-Image punya initramfs tertanam, jadi dipakai
#  --initrd empty.cpio.gz (initramfs kosong) supaya root=/dev/vda dipakai.
#  lkvm `--disk` = virtio-blk; kernel VXTux punya VIRTIO_BLK=y.
# ============================================================================
set -euo pipefail

SRC=/.stage
DST=/root/artixstage
BFKRT=/root/bfkrt-initramfs.cpio.gz
IMG=/root/artix_rootfs.img
EMPTY=/root/empty.cpio.gz
LKVMBIN=/root/lkvm
KERNEL=/root/bfkrt-Image
LOG=/root/lkvm-artix-boot.log
MKE2FS=/.l2s/.l2s.mke2fs0001.0004      # biner mke2fs asli (symlink .l2s buntu)
E2FSCK=/.l2s/.l2s.e2fsck0001.0004

echo "[i] 1/6 salin rootfs Artix dari $SRC"
rm -rf "$DST"; mkdir -p "$DST"; cp -a "$SRC/." "$DST/"

echo "[i] 2/6 modul berry (BFKRT)"
rm -rf /tmp/berryx; mkdir -p /tmp/berryx
( cd /tmp/berryx && gzip -dc "$BFKRT" | cpio -idm 'm/berry*' 2>/dev/null ) || true
mkdir -p "$DST/opt/berry"; cp -a /tmp/berryx/m/. "$DST/opt/berry/"

echo "[i] 3/6 service dinit"
mkdir -p "$DST/usr/local/bin"
cat > "$DST/usr/local/bin/autologin" <<'EOS'
#!/bin/sh
exec /usr/bin/login -f root
EOS
chmod 0755 "$DST/usr/local/bin/autologin"

cat > "$DST/usr/local/bin/berry-load" <<'EOS'
#!/bin/sh
C=/dev/console
echo "=== VXTUX-BFKRT-LKVM: mulai ===" | tee -a $C
INS=$(command -v insmod 2>/dev/null || echo /usr/bin/insmod)
[ -x "$INS" ] || INS=/sbin/insmod
for m in berryfetcher_kernel berry_trig_all berry_m_virtkrn_obs; do
    if "$INS" /opt/berry/$m.ko; then
        echo "INSMOD $m.ko rc=0 OK" | tee -a $C
    else
        echo "INSMOD $m.ko rc=$? FAIL" | tee -a $C
    fi
done
echo "=== VXTUX-BFKRT-LKVM-DONE ===" | tee -a $C
while true; do sleep 3600; done
EOS
chmod 0755 "$DST/usr/local/bin/berry-load"

cat > "$DST/usr/local/bin/lkvm-sysprep" <<'EOS'
#!/bin/sh
mkdir -p /proc /sys /dev /dev/pts /dev/shm /run /run/user/0 /tmp
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devpts devpts /dev/pts 2>/dev/null
mount -t tmpfs tmpfs /dev/shm -o size=256m 2>/dev/null
mount -t tmpfs tmpfs /run -o mode=0755 2>/dev/null || true
chmod 1777 /tmp /dev/shm 2>/dev/null
chmod 700 /run/user/0 2>/dev/null
exit 0
EOS
chmod 0755 "$DST/usr/local/bin/lkvm-sysprep"

cat > "$DST/usr/local/bin/getty-serial" <<'EOS'
#!/bin/sh
echo "GETTY-SERIAL: autologin root di ttyS0" > /dev/console
exec /usr/bin/agetty --noreset --noclear --autologin root ttyS0 115200 vt100
EOS
chmod 0755 "$DST/usr/local/bin/getty-serial"

cat > "$DST/etc/dinit.d/berry-load" <<'EOS'
type            = process
command         = /usr/local/bin/berry-load
smooth-recovery = true
restart         = false
EOS
cat > "$DST/etc/dinit.d/lkvm-sysprep" <<'EOS'
type    = scripted
command = /usr/local/bin/lkvm-sysprep
restart = false
EOS
cat > "$DST/etc/dinit.d/serial-getty" <<'EOS'
type            = process
command         = /usr/local/bin/getty-serial
restart         = true
depends-on      = lkvm-sysprep
termsignal      = HUP
smooth-recovery = true
EOS
cat > "$DST/etc/dinit.d/boot" <<'EOS'
# boot minimal untuk lkvm: sysprep (mount) lalu boot.d (berry-load + getty).
type        = internal
depends-on  = lkvm-sysprep
waits-for.d = /etc/dinit.d/boot.d
EOS

# no-op service yang kaskade di VM (system/default chain)
for s in system cgroups early-prepare.target udevd-early early-devices.target \
         random-seed kmod-static-nodes early-console.target pre-local.target \
         local.target login.target early-keyboard.target early-fs-pre.target \
         udev-settle udev-trigger udevd hwclock locale root-ro fsck-root \
         early-root-rw.target; do
  printf 'type = internal\n' > "$DST/etc/dinit.d/$s"
done
rm -f "$DST/etc/dinit.d/getty@ttyS0"        # instance template, bukan service bebas

# boot.d: hanya berry-load + serial-getty
cd "$DST/etc/dinit.d/boot.d"; rm -f *
ln -sf ../berry-load berry-load
ln -sf ../serial-getty serial-getty

cat > "$DST/etc/fstab" <<'EOS'
proc     /proc  proc  defaults                    0 0
sysfs    /sys   sysfs defaults                    0 0
devtmpfs /dev   devtmpfs mode=0755,nosuid          0 0
tmpfs    /tmp   tmpfs rw,nosuid,nodev,size=256M   0 0
EOS
echo artix-vxtux > "$DST/etc/hostname"

echo "[i] 4/6 initramfs kosong (penimpa initramfs tertanam di Image)"
rm -rf /tmp/emptyinit; mkdir -p /tmp/emptyinit
( cd /tmp/emptyinit && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip > "$EMPTY" )

echo "[i] 5/6 bangun $IMG (ext4 4G)"
truncate -s 4G "$IMG"
"$MKE2FS" -F -t ext4 -L super -U bcb89503-886c-49e3-b47f-464af345cfcd -d "$DST" "$IMG" 2>&1 | tail -3
"$E2FSCK" -fn "$IMG" 2>&1 | tail -3

echo "[i] 6/6 boot lkvm -> Artix, rekam ke $LOG"
cd /root
timeout 480 "$LKVMBIN" run \
  --kernel "$KERNEL" --initrd "$EMPTY" --disk "$IMG" \
  --params "root=/dev/vda rw console=ttyS0 loglevel=8 ignore_loglevel panic=15 printk.devkmsg=on" \
  --mem 2048 --cpus 4 --console serial --irqchip gicv3 -n mode=none \
  2>&1 | tee "$LOG" | tail -25

echo "[i] selesai. log: $LOG  ($(wc -l < "$LOG") baris)"
