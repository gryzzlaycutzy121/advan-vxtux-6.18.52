#!/usr/bin/env bash
# Dump semua kanal log saat perangkat HIDUP dan adb tersedia.
# (Kalau perangkat mati / layar beku tanpa adb, pakai dump_all.bat.)
#
# Pakai:  bash p1/dump_all_adb.sh
# Hasil:  p1/logs/adb-<tanggal-jam>/ + 01-RINGKAS.txt
set -uo pipefail

ADB="${ADB:-/g/UMS512/tools/platform-tools/adb.exe}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/logs/adb-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"

sh_() { # sh_ <namafile> <perintah shell> [fallback su]
	local name="$1" cmd="$2"
	"$ADB" shell "$cmd" > "$OUT/$name" 2>"$OUT/$name.err"
	if [ ! -s "$OUT/$name" ] && [ $# -ge 3 ]; then
		"$ADB" shell "su -c '$3'" > "$OUT/$name" 2>>"$OUT/$name.err"
	fi
	rm -f "$OUT/$name.err" 2>/dev/null
}

echo "== menunggu adb =="
"$ADB" wait-for-device || { echo "adb tidak terdeteksi"; exit 1; }
"$ADB" root >/dev/null 2>&1 || true
sleep 2

sh_ a1_dmesg.txt          'dmesg'                                   'dmesg'
sh_ a2_cmdline.txt        'cat /proc/cmdline'
sh_ a3_uname.txt          'uname -a'
sh_ a4_pstore_console.txt 'cat /sys/fs/pstore/console-ramoops-0'
sh_ a5_pstore_dmesg.txt   'cat /sys/fs/pstore/dmesg-ramoops-0'
sh_ a6_vxtuxlog.txt       'cat /proc/vxtuxlog'
sh_ a7_deferred.txt       'cat /sys/kernel/debug/devices_deferred'
sh_ a8_cache_list.txt     'ls -l /cache/vxtux-logs'
sh_ a9_last_kmsg.txt      'cat /proc/last_kmsg'                     'cat /proc/last_kmsg'
sh_ b1_props.txt          'getprop | grep -iE "slot|serialno|hardware|verity|vbmeta"'

echo "== menarik /cache/vxtux-logs =="
"$ADB" pull /cache/vxtux-logs "$OUT/cache-logs" >/dev/null 2>&1 \
	|| echo "  (tidak bisa pull; butuh root / partisi belum ter-mount)"

echo "== mengubah .bin (bila ada) =="
for f in "$OUT"/*.bin; do
	[ -e "$f" ] || continue
	node "$HERE/dump_extract.mjs" "$f" >/dev/null 2>&1
done

echo "== laporan =="
node "$HERE/log_report.mjs" "$OUT"
echo
echo "Buka: $OUT/01-RINGKAS.txt"
