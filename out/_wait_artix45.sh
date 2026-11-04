#!/usr/bin/env bash
# _wait_artix45.sh -- tunggu run CI artix45, unduh Image, jalankan pipeline 45.
#
#   bash out/_wait_artix45.sh [run-id]
#
# Menulis kemajuan ke out/wait45.log (dipantau dari sesi utama).
set -u
cd "$(dirname "$0")/.."
R=gryzzlaycutzy121/advan-vxtux-6.18.52
ID=${1:-40853533109}
ART=out/ci_artix45_src
mkdir -p "$ART"
LOG=out/wait45_${ID}.log
: > "$LOG"
ln -sf "wait45_${ID}.log" out/wait45.log 2>/dev/null || true

echo "[wait] run $ID di $R" | tee -a "$LOG"
cc=""
for i in $(seq 1 160); do
    line=$(gh run view "$ID" -R "$R" --json status,conclusion \
        -q '.status+" "+.conclusion' 2>/dev/null || echo "unknown none")
    st=${line%% *}
    cc=${line##* }
    echo "[wait] $(date +%H:%M:%S) status=$st conclusion=$cc" | tee -a "$LOG"
    [ "$st" = "completed" ] && break
    sleep 30
done

if [ "${cc:-}" != "success" ]; then
    echo "CI_GAGAL=$cc" | tee -a "$LOG"
    exit 1
fi

rm -rf "$ART"
mkdir -p "$ART"
echo "[dl] unduh v17-Image" | tee -a "$LOG"
gh run download "$ID" -R "$R" -n v17-Image -D "$ART" >>"$LOG" 2>&1 || {
    echo "DL_GAGAL" | tee -a "$LOG"
    exit 1
}
gh run download "$ID" -R "$R" -n v17-config -D "$ART" >>"$LOG" 2>&1 || \
    echo "[dl] v17-config tidak ada (boleh)" | tee -a "$LOG"
echo "$ID" > "$ART/RUN.txt"
find "$ART" -type f -name Image | tee -a "$LOG"

echo "[pipe] jalankan ci_artix45_pipeline.sh" | tee -a "$LOG"
bash out/ci_artix45_pipeline.sh >>"$LOG" 2>&1
rc=$?
echo "PIPELINE_RC=$rc" | tee -a "$LOG"
grep -E '^(DONE=|FAILED=|boot_artix45|.*md5)' "$LOG" | tail -12
