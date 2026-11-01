#!/bin/bash
# ci_build.sh -- Bangun 3 modul BFKRT dengan toolchain PERSIS kernel stok
# UMS512 5.4.210: Android clang r416183b (CFI v1) + pohon kernel vendor.
#
# PENTING: memakai pohon kernel VENDOR (marohinmark/kernel_ums512_5.4),
# bukan vanilla. Alasannya (semua dibuktikan dari device):
#   1. struct module vendor punya field tambahan: cfi_check (+8B) dan
#      ANDROID_KABI_RESERVE(1..4) (+32B) -> offset field init = 0x178.
#      Di vanilla offset init = 0x150. Modul yang dibangun thd header
#      vanilla menaruh .init di offset salah -> kernel baca mod->init NULL
#      -> init tak pernah jalan (insmod rc=0, initstate=live, tanpa proc).
#   2. scripts/Makefile.modpost vendor punya dukungan LTO (modpost-ext=.lto
#      + cmd_cc_lto_link_modules). Vanilla tidak punya -> modpost gagal
#      baca bitcode.
#   3. scripts/Makefile.ubsan vendor memakai CONFIG_UBSAN_TRAP (trap mode,
#      tanpa handler runtime). Vanilla memakai handler -> modul import
#      __ubsan_handle_* yang tidak di-export kernel stok.
#   4. arch/arm64/Makefile vendor menambah KBUILD_LDS_MODULE += module.lds
#      -> section .plt/.init.plt/.text.ftrace_trampoline otomatis ada.
#
# Semua flag (CFI, LTO, -fvisibility=default, -mstack-protector-guard=sysreg,
# UBSan trap) come dari Makefile vendor -- TIDAK ada flag manual, agar
# identik dengan build kernel.
set -x
set -e

REPO=$(cd "$(dirname "$0")" && pwd)
WORK=/tmp/bfk54
KSRC=$WORK/linux
CLANG=/opt/clang-r416183b
MOD="$REPO/modules54"
OUT=$WORK/out

mkdir -p "$OUT"

export PATH="$CLANG/bin:$PATH"
CROSS=aarch64-linux-gnu-
CC=$CLANG/bin/clang
LD=$CLANG/bin/ld.lld
AR=$CLANG/bin/llvm-ar
NM=$CLANG/bin/llvm-nm
OBJCOPY=$CLANG/bin/llvm-objcopy
STRIP=$CLANG/bin/llvm-strip

common_make() {
  make -C "$KSRC" ARCH=arm64 \
    CC="$CC --target=aarch64-linux-gnu" \
    CROSS_COMPILE="$CROSS" \
    LD="$LD" AR="$AR" NM="$NM" OBJCOPY="$OBJCOPY" STRIP="$STRIP" \
    HOSTCC=gcc \
    "$@"
}

echo "===== [1/5] config (stock) + modules_prepare ====="
cp "$MOD/stock.config" "$KSRC/.config"
# CONFIG_MODULE_SIG_ALL=y butuh certs/signing_key.pem; nonaktifkan supaya
# modul tidak wajib_signed (kernel tetap men-taint, itu normal & aman).
sed -i 's/^CONFIG_MODULE_SIG_ALL=y/# CONFIG_MODULE_SIG_ALL is not set/' "$KSRC/.config"
sed -i 's/^CONFIG_MODULE_SIG_FORCE=y/# CONFIG_MODULE_SIG_FORCE is not set/' "$KSRC/.config"
common_make olddefconfig
common_make modules_prepare

echo "===== [2/5] siapkan source modul ====="
rm -rf "$KSRC/bfkrt_ko"
mkdir -p "$KSRC/bfkrt_ko"
cp "$MOD"/berryfetcher_kernel.c "$MOD"/berry_trig_all.c \
   "$MOD"/berry_m_virtkrn_obs.c "$MOD"/bfkrt_ko.h "$KSRC/bfkrt_ko/"
cat > "$KSRC/bfkrt_ko/Makefile" <<'EOF'
obj-m := berryfetcher_kernel.o berry_trig_all.o berry_m_virtkrn_obs.o
EOF

echo "===== [3/5] build modul via kbuild NATIVE vendor (LTO + modpost + final link) ====="
# Alur vendor: .c -> .o(bitcode) -> [LTO] .lto.o -> modpost -> .mod.c
#               -> .mod.o(bitcode) -> [LTO final] .ko
# modpost membaca .lto.o (ELF), final link ld.lld -r bitcode+bitcode (LTO native).
cd "$KSRC"
common_make M=bfkrt_ko modules 2>&1 | tee "$OUT/build.log" | tail -40

echo "===== [4/5] verifikasi ====="
ok=1
for m in berryfetcher_kernel berry_trig_all berry_m_virtkrn_obs; do
  ko="$KSRC/bfkrt_ko/$m.ko"
  if [ ! -f "$ko" ]; then
    echo "GAGAL: $m.ko tidak ada"; ok=0; continue
  fi
  cp -f "$ko" "$OUT/$m.ko"
  vm=$(strings "$ko" | grep -m1 '^vermagic=' || echo "(tanpa vermagic)")
  echo "OK  $m.ko  $(stat -c%s "$ko")B"
  echo "    $vm"
  # offset field init/exit di __this_module HARUS 0x178 / 0x338 (vendor)
  python3 "$REPO/modules54/check_layout.py" "$ko" || ok=0
done
[ "$ok" = 1 ] || { echo "=== VERIFIKASI GAGAL ==="; exit 1; }

echo "===== [5/5] artefak ====="
ls -la "$OUT"/*.ko
