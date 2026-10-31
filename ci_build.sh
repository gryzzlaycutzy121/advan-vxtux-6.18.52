#!/bin/bash
# ci_build.sh -- Bangun 3 modul BFKRT dengan Android clang r416183b (CFI v1)
# yang PERSIS dipakai kernel stok (CONFIG_CC_VERSION_TEXT). Menghasilkan .ko
# dengan CFI hash identik -> insmod sempurna tanpa CFI failure.
#
# Dijalankan di GitHub Actions ubuntu-22.04 (Linux x86-64) karena clang
# r416183b adalah ELF linux-x86 (tidak jalan di Windows).
#
# Alur 2-pass:
#   Pass 1: kbuild compile tiap modul ke bitcode LTO (.o) + -fsanitize=cfi
#           (modpost vanilla GAGAL baca bitcode -> dibiarkan; .o sudah jadi).
#   LTO :  ld.lld -r pada bitcode -> ELF relocatable (.o) dengan .cfi_jt
#          (CFI v1 + cross-DSO jump table + __cfi_check ter-instrumentasi).
#   Pass 2: kbuild lanjut (modpost baca .o ELF -> .mod.c -> compile -> link
#           final .ko). .cmd pass2 == pass1 (KCFLAGS sama) -> .o TIDAK
#           di-recompile; hanya modpost+link final yang jalan.
set -x
set -e

# ---- Lokasi (runner) ----
# ci_build.sh dijalankan dari root repo (checkout) di runner
REPO=$(cd "$(dirname "$0")" && pwd)
WORK=/tmp/bfk54
KSRC=$WORK/linux-5.4.210
CLANG=/opt/clang-r416183b
MOD="$REPO/modules54"
OUT=$WORK/out

mkdir -p "$OUT"

# ---- Toolchain ----
export PATH="$CLANG/bin:$PATH"
CROSS=aarch64-linux-gnu-          # GNU binutils (as/ld fallback) dari apt
CC=$CLANG/bin/clang
LD=$CLANG/bin/ld.lld
AR=$CLANG/bin/llvm-ar
NM=$CLANG/bin/llvm-nm
OBJCOPY=$CLANG/bin/llvm-objcopy
STRIP=$CLANG/bin/llvm-strip

# ---- Flag CFI v1 (persis kernel stok: FullLTO + CFI + cross-DSO) ----
CFI_KCFLAGS="-flto=full -fsanitize=cfi -fsanitize-cfi-cross-dso -fsanitize-cfi-canonical-jump-tables -fvisibility=hidden -fPIC"

common_make() {
  make -C "$KSRC" ARCH=arm64 \
    CC="$CC --target=aarch64-linux-gnu" \
    CROSS_COMPILE="$CROSS" \
    LD="$LD" AR="$AR" NM="$NM" OBJCOPY="$OBJCOPY" STRIP="$STRIP" \
    HOSTCC=gcc \
    KBUILD_MODPOST_WARN=1 \
    "$@"
}

echo "===== [1/6] config + modules_prepare (gcc host tools) ====="
cp "$MOD/stock.config" "$KSRC/.config"
common_make olddefconfig
common_make modules_prepare

echo "===== [2/6] pass 1: compile bitcode LTO+CFI (modpost dibiarkan gagal) ====="
mkdir -p "$KSRC/bfkrt_ko"
cp "$MOD"/berryfetcher_kernel.c "$MOD"/berry_trig_all.c "$MOD"/berry_m_virtkrn_obs.c "$MOD"/bfkrt_ko.h "$KSRC/bfkrt_ko/"
cat > "$KSRC/bfkrt_ko/Makefile" <<'EOF'
obj-m := berryfetcher_kernel.o berry_trig_all.o berry_m_virtkrn_obs.o
EOF
set +e
common_make M=bfkrt_ko KCFLAGS="$CFI_KCFLAGS" V=1 modules > "$OUT/pass1.log" 2>&1
P1=$?
set -e
echo "pass1 exit=$P1 (modpost gagal baca bitcode = NORMAL)"
tail -5 "$OUT/pass1.log"

echo "===== [3/6] LTO link bitcode -> ELF relocatable (.cfi_jt) ====="
for m in berryfetcher_kernel berry_trig_all berry_m_virtkrn_obs; do
  bc="$KSRC/bfkrt_ko/$m.o"
  [ -f "$bc" ] || { echo "GAGAL: $m.o (bitcode) tidak ada"; ls -la "$KSRC/bfkrt_ko/"; exit 1; }
  # LTO link: ld.lld -r menjalankan full LTO pada bitcode -> ELF .o relocatable
  "$CC" --target=aarch64-linux-gnu -flto=full -fuse-ld=lld \
    -Wl,-r -nostdlib -o "$OUT/$m.o" "$bc" 2>>"$OUT/lto.log" || {
      echo "LTO gagal untuk $m:"; cat "$OUT/lto.log"; exit 1; }
  # ganti .o di tree dengan ELF hasil LTO (tetap nama + timestamp baru)
  cp -f "$OUT/$m.o" "$KSRC/bfkrt_ko/$m.o"
  echo "LTO $m: $(file -b "$OUT/$m.o")"
done

echo "===== [4/6] pass 2 via kbuild: modpost baca .o ELF -> .mod.c -> .ko ====="
# .o sudah ELF (hasil LTO) dan lebih baru dari .c -> kbuild TIDAK recompile.
# __modpost (FORCE) dijalankan ulang: modpost baca .o ELF -> sukses -> .mod.c
# -> .mod.o (KCFLAGS CFI juga) -> ld.lld -r (LTO) -> .ko final.
set +e
common_make M=bfkrt_ko KCFLAGS="$CFI_KCFLAGS" V=1 modules > "$OUT/pass2.log" 2>&1
P2=$?
set -e
echo "pass2 exit=$P2"
tail -8 "$OUT/pass2.log"

echo "===== [5/6] salin .ko hasil kbuild ke OUT ====="
for m in berryfetcher_kernel berry_trig_all berry_m_virtkrn_obs; do
  ko="$KSRC/bfkrt_ko/$m.ko"
  if [ -f "$ko" ]; then
    cp -f "$ko" "$OUT/$m.ko"
    echo "OK  $m.ko ($(stat -c%s "$ko")B)"
  else
    echo "GAGAL: $m.ko tidak dihasilkan (lihat pass2.log)"
  fi
done

echo "===== [6/6] verifikasi .ko ====="
for m in berryfetcher_kernel berry_trig_all berry_m_virtkrn_obs; do
  ko="$OUT/$m.ko"
  if [ -f "$ko" ]; then
    vm=$(strings "$ko" | grep -m1 '^vermagic=' || echo "(tanpa vermagic)")
    sz=$(stat -c%s "$ko")
    cfis=$(readelf -S "$ko" 2>/dev/null | grep -c cfi || echo 0)
    has_check=$(nm "$ko" 2>/dev/null | grep -c '__cfi_check' || echo 0)
    echo "OK  $m.ko  size=${sz}B  cfi_sections=$cfis  __cfi_check=$has_check"
    echo "    $vm"
  else
    echo "GAGAL: $m.ko tidak dihasilkan"
  fi
done

echo "===== [7/7] selesai; artefak di $OUT ====="
ls -la "$OUT"/*.ko 2>/dev/null || true
