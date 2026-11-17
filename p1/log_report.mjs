#!/usr/bin/env node
/**
 * Gabungkan semua hasil dump jadi satu laporan yang bisa dibaca.
 *
 * Pemakaian:
 *   node log_report.mjs <dir-hasil-dump>
 *
 * Masukan : semua *.txt di <dir> (hasil dump_extract.mjs: *.text.txt, *.rec-*.txt)
 * Keluaran: 00-SEMUA.txt   (gabungan berurutan + header berkas)
 *           01-RINGKAS.txt (hitungan + baris penting per penanda)
 *
 * Penanda dipilih dari pengalaman P1: panic, watchdog, initcall gagal,
 * deferred probe, display "not ready", pstore/ramoops, init userspace.
 */
import { readdirSync, readFileSync, writeFileSync, statSync } from "node:fs";
import { join } from "node:path";

const dir = process.argv[2];
if (!dir) {
  console.error("pakai: node log_report.mjs <dir>");
  process.exit(2);
}

const OUTPUTS = new Set(["00-SEMUA.txt", "01-RINGKAS.txt"]);
let files = [];
try {
  files = readdirSync(dir).filter((f) => f.endsWith(".txt") && !OUTPUTS.has(f));
} catch (e) {
  console.error(`tidak bisa membaca ${dir}: ${e.message}`);
  process.exit(1);
}
files.sort();

const MARKERS = [
  ["PANIC", /Kernel panic|Attempted to kill init|panic - not syncing|Fatal exception/i],
  ["OOPS/WARN", /WARNING: CPU|Unable to handle kernel|BUG: |Call trace|Internal error/i],
  ["WATCHDOG/LOCKUP", /watchdog|soft lockup|BUG: soft lockup|hung task|stall/i],
  ["INITCALL GAGAL", /initcall .* returned -(?!0)\d+/i],
  ["DEFERRED", /EPROBE_DEFER|deferred|is not ready yet/i],
  ["PSTORE/RAMOOPS", /pstore|ramoops|vxtuxlog/i],
  ["INIT USERSPACE", /Run \/init|init first stage|init second stage|can't find an executable|Failed to execute/i],
  ["STORAGE/MMC", /\bmmc\d?\b|gpt|ext4|PARTLABEL|root=/i],
  ["DISPLAY", /\[drm\]|panel|dsi|fbcon|frame buffer|backlight/i],
  ["FIRMWARE", /regulatory\.db|request_firmware|failed with error -2\b/i],
  ["REBOOT", /reboot|reset|Freezing|Restarting system/i],
];

const allTxt = [];
const hits = new Map(MARKERS.map(([name]) => [name, []]));
let totalLines = 0;

for (const f of files) {
  const p = join(dir, f);
  let txt;
  try {
    if (!statSync(p).isFile()) continue;
    txt = readFileSync(p, "utf8");
  } catch {
    continue;
  }
  const lines = txt.split(/\r?\n/);
  totalLines += lines.length;
  allTxt.push(`\n===== ${f} (${lines.length} baris) =====`);
  allTxt.push(txt.trimEnd());
  lines.forEach((line, i) => {
    if (!line.trim()) return;
    for (const [name, re] of MARKERS) {
      if (re.test(line)) {
        const bucket = hits.get(name);
        if (bucket.length < 200) bucket.push(`${f}:${i + 1}: ${line.trim().slice(0, 300)}`);
        break;
      }
    }
  });
}

writeFileSync(join(dir, "00-SEMUA.txt"), allTxt.join("\n") + "\n");

const out = [];
out.push("# RINGKASAN DUMP LOG P1");
out.push(`dir     : ${dir}`);
out.push(`berkas  : ${files.length}`);
out.push(`baris   : ${totalLines}`);
out.push("");
for (const [name] of MARKERS) {
  const h = hits.get(name);
  out.push(`## ${name}: ${h.length === 0 ? "tidak ada" : h.length + " baris"}`);
  for (const line of h.slice(0, 40)) out.push(`   ${line}`);
  if (h.length > 40) out.push(`   ... (${h.length - 40} baris lagi, lihat 00-SEMUA.txt)`);
  out.push("");
}
writeFileSync(join(dir, "01-RINGKAS.txt"), out.join("\n") + "\n");

console.log(`berkas: ${files.length}, baris: ${totalLines}`);
for (const [name] of MARKERS) {
  const n = hits.get(name).length;
  console.log(`  ${name.padEnd(16)} ${n === 0 ? "-" : n}`);
}
console.log(`\n-> ${join(dir, "00-SEMUA.txt")}`);
console.log(`-> ${join(dir, "01-RINGKAS.txt")}`);
