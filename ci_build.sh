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
# Flag CFI PERSIS vendor UNISOC (Makefile:924-943): -fno-sanitize-cfi-canonical-jump-tables
# wajib (tanpa ini loader stok tolak: canonical jt memecah .text jadi section .cfi.jumptable).
# -ftrap-function=__ubsan_handle_cfi_check_fail_abort (CONFIG_CFI_PERMISSIVE off).
# -fPIC HARAM: kernel non-PIC; -fPIC -> relokasi GOT 311/312 -> loader menolak
# ("unsupported RELA relocation: 312"). Section .plt/.init.plt/.text.ftrace_trampoline
# di-inject via llvm-objcopy (persis arch/arm64/kernel/module.lds vendor).
CFI_KCFLAGS="-flto=full -fsanitize=cfi -fno-sanitize-cfi-canonical-jump-tables -fno-sanitize-blacklist -fsanitize-cfi-cross-dso -ftrap-function=__ubsan_handle_cfi_check_fail_abort -fvisibility=hidden"

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

echo "===== [4/6] modpost manual: .o ELF -> .mod.c ====="
# kbuild __modpost membaca .o via modules.order; tapi pass 2 kbuild akan
# re-LTO .mod.c (KCFLAGS -flto) -> __cfi_check GANDA. Jadi:
#   a) modpost manual: sed 's/ko$$/o/' modules.order | scripts/mod/modpost
#   b) compile .mod.c TANPA LTO, flags diekstrak PER-MODUL dari pass1.log
#   c) ld.lld -r TANPA LTO: .o (ELF) + .mod.o (ELF) -> .ko
cd "$KSRC"
# modules.order dari kbuild sering kosong (pass1 gagal sebelum MODORDER ditulis).
# Tulis MANUAL: deterministik, modul yang memang kita bangun.
{
  echo "bfkrt_ko/berryfetcher_kernel.ko"
  echo "bfkrt_ko/berry_trig_all.ko"
  echo "bfkrt_ko/berry_m_virtkrn_obs.ko"
} > bfkrt_ko/modules.order
echo "modules.order manual:"
cat bfkrt_ko/modules.order
[ -f Module.symvers ] || touch Module.symvers
[ -f bfkrt_ko/Module.symvers ] || touch bfkrt_ko/Module.symvers
sed 's/ko$/o/' bfkrt_ko/modules.order | scripts/mod/modpost \
    -m -i Module.symvers -o bfkrt_ko/Module.symvers -w -T - \
    > "$OUT/modpost.log" 2>&1 || {
  echo "modpost gagal:"; tail -15 "$OUT/modpost.log"; exit 1; }
echo "modpost OK; cari .mod.c:"
ls -la bfkrt_ko/*.mod.c 2>/dev/null || echo "(tidak di bfkrt_ko/)"
ls -la ./*.mod.c 2>/dev/null || echo "(tidak di root)"
# lokasi .mod.c bisa di bfkrt_ko/ ATAU root tergantung cara modpost ditulis
MODC_DIR=""
for d in bfkrt_ko .; do
  if ls "$d"/*.mod.c >/dev/null 2>&1; then MODC_DIR="$d"; break; fi
done
[ -n "$MODC_DIR" ] || { echo "GAGAL: .mod.c tidak ditemukan (lihat modpost.log)"; tail -15 "$OUT/modpost.log"; exit 1; }
echo "MODC_DIR=$MODC_DIR"

# flags tetap untuk compile .mod.c (tanpa LTO/CFI; terbukti valid di uji lokal)
# Nama modul (KBUILD_MODNAME) diambil per-modul dari pass1.log.
MODC_TEMPLATE='--target=aarch64-linux-gnu -nostdinc -isystem /opt/clang-r416183b/lib64/clang/12.0.5/include -I./arch/arm64/include -I./arch/arm64/include/generated -I./include -I./arch/arm64/include/uapi -I./arch/arm64/include/generated/uapi -I./include/uapi -I./include/generated/uapi -include ./include/linux/kconfig.h -include ./include/linux/compiler_types.h -D__KERNEL__ -mlittle-endian -Qunused-arguments -Wall -Wundef -Wno-trigraphs -fno-strict-aliasing -fno-common -fshort-wchar -fno-PIE -std=gnu89 -no-integrated-as -mgeneral-regs-only -fno-asynchronous-unwind-tables -Wno-psabi -mbranch-protection=none -fno-delete-null-pointer-checks -O2 -fstack-protector-strong -fno-omit-frame-pointer -fno-optimize-sibling-calls -g -gdwarf-4 -fno-strict-overflow -fno-merge-all-constants -fno-stack-check -fmacro-prefix-map=./= -DMODULE'

echo "===== [5/6] compile .mod.c (tanpa LTO) + link .ko (tanpa LTO) ====="
for m in berryfetcher_kernel berry_trig_all berry_m_virtkrn_obs; do
  modc="$MODC_DIR/$m.mod.c"
  [ -f "$modc" ] || { echo "GAGAL: $modc tidak ada"; continue; }
  "$CLANG/bin/clang" $MODC_TEMPLATE \
      -DKBUILD_BASENAME=\"$m\" -DKBUILD_MODNAME=\"$m\" \
      -c "$modc" -o "$OUT/$m.mod.o" >> "$OUT/link.log" 2>&1 \
      || { echo "compile $m.mod.o gagal:"; tail -6 "$OUT/link.log"; continue; }
  "$CLANG/bin/ld.lld" -r "$OUT/$m.o" "$OUT/$m.mod.o" \
      -o "$OUT/$m.ko" >> "$OUT/link.log" 2>&1 \
      || { echo "link $m.ko gagal:"; tail -6 "$OUT/link.log"; continue; }
  # Inject .plt/.init.plt/.text.ftrace_trampoline (loader arm64 WAJIB punya
  # section .plt/.init.plt; link manual ld.lld -r tidak menjalankan
  # arch/arm64/kernel/module.lds). Kernel menulis ulang section ini sendiri.
  # Tanpa 3 section ini: "module PLT section(s) missing".
  printf '\0' > "$OUT/pltbyte.bin"
  "$CLANG/bin/llvm-objcopy" \
      --add-section=.plt="$OUT/pltbyte.bin" \
      --add-section=.init.plt="$OUT/pltbyte.bin" \
      --add-section=.text.ftrace_trampoline="$OUT/pltbyte.bin" \
      "$OUT/$m.ko" 2>>"$OUT/link.log" \
      || { echo "objcopy $m.ko gagal:"; tail -4 "$OUT/link.log"; continue; }
  echo "OK  $m.ko ($(stat -c%s "$OUT/$m.ko")B)"
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
