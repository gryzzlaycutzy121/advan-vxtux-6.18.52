#!/usr/bin/env bash
# Bangun Image P1 (panic=0, tanpa initcall_debug) untuk 6.18.52-VXTux-UMS512.
#
# Kenapa harus build ulang: CONFIG_CMDLINE_FORCE=y -> kernel MEMBUANG bootargs
# dari LK/DT dan memakai CONFIG_CMDLINE yang ditanam saat build
# (drivers/of/fdt.c). Jadi mengubah cmdline di boot.img saja TIDAK berpengaruh.
#
# Pakai (Linux/CI, BUKAN Windows: kbuild butuh host compiler POSIX):
#   bash p1/build_p1.sh
# Hasil: p1/out/Image  +  p1/out/p1-cmdline-efektif.txt
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TREE="$REPO/6.18.52-VXTux-UMS512"
OUT="$REPO/p1/out"
CROSS="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"

mkdir -p "$OUT"
cd "$TREE"

# 0. Bersihkan sisa build Windows (.exe host tools) supaya tidak dipakai ulang.
rm -f scripts/basic/fixdep scripts/basic/fixdep.exe \
      scripts/kconfig/conf scripts/kconfig/conf.exe

# 1. Simpan config asli, pasang cmdline P1 + pastikan pstore hidup.
if [ ! -f "$REPO/p1/.config.pre-p1" ]; then
	cp .config "$REPO/p1/.config.pre-p1"
fi
CPPFLAGS_CMD="$(tr -d '\r\n' < "$REPO/p1/p1-cmdline.txt")"
./scripts/config \
	--set-str CONFIG_CMDLINE "$CPPFLAGS_CMD" \
	-e CONFIG_CMDLINE_FORCE -d CONFIG_CMDLINE_FROM_BOOTLOADER -d CONFIG_CMDLINE_EXTEND \
	-e CONFIG_PSTORE -e CONFIG_PSTORE_RAM -e CONFIG_PSTORE_CONSOLE -e CONFIG_PSTORE_PMSG
make ARCH=arm64 CROSS_COMPILE="$CROSS" olddefconfig

# 2. Bukti cmdline benar-benar masuk config (jangan percaya ingatan).
grep -E '^CONFIG_CMDLINE' .config | tee "$OUT/p1-cmdline-efektif.txt"
grep -q "panic=15" .config || { echo "FATAL: panic=15 tidak ada di CONFIG_CMDLINE"; exit 1; }
# initcall_debug boleh muncul di komentar .config, jadi periksa nilai efektifnya.
if sed -n 's/^CONFIG_CMDLINE="\(.*\)"$/\1/p' .config | grep -q 'initcall_debug'; then
	echo "FATAL: initcall_debug masih ada di CONFIG_CMDLINE"; exit 1
fi

# 3. Build.
make ARCH=arm64 CROSS_COMPILE="$CROSS" -j"$JOBS" Image
cp arch/arm64/boot/Image "$OUT/Image"
cp arch/arm64/boot/dts/sprd/ums512-1h10-vxtux.dtb "$OUT/" 2>/dev/null || true
ls -l "$OUT/Image"

# 4. Kemas boot.img dari boot image yang SUDAH terbukti load.
#    Header + ramdisk diambil apa adanya dari template; hanya kernel diganti.
#    Tidak memakai mk_boot_artix.py: skrip itu mensintesis header dari nol,
#    menolak jalan tanpa --ramdisk, dan --base-nya sudah tidak dipakai.
TEMPLATE="${TEMPLATE:-$REPO/out/6.18.52-VXTux-UMS512/boot_artix53_panic.img}"
if [ ! -f "$TEMPLATE" ]; then
	TEMPLATE="$(ls -S "$REPO"/out/6.18.52-VXTux-UMS512/boot_artix*.img 2>/dev/null | head -1)"
fi
[ -f "$TEMPLATE" ] || { echo "FATAL: tidak ada boot image template di out/6.18.52-VXTux-UMS512/"; exit 1; }
node "$REPO/p1/pack_p1.mjs" "$TEMPLATE" "$OUT/Image" "$OUT/boot_p1.img" \
	--cmdline-file "$REPO/p1/p1-cmdline.txt"
echo "boot image: $OUT/boot_p1.img"

# 5. Kembalikan config semula supaya build normal tidak ikut berubah.
cp "$REPO/p1/.config.pre-p1" .config
echo "OK. config asli sudah dipulihkan."
