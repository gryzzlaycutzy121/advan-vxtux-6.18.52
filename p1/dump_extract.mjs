#!/usr/bin/env node
/**
 * Ekstrak log dari hasil `spd_dump read_mem`.
 *
 * Pemakaian:
 *   node dump_extract.mjs p1_ramoops.bin p1_vxtuxlog.bin [...]
 *
 * Untuk setiap berkas dihasilkan:
 *   <nama>.text.txt        -> semua deret teks yang bisa dicetak (>=24 char)
 *   <nama>.rec-<seq>-r<reason>.txt -> record vxtuxlog (header VXTUXRAM/VXTUXLG2)
 *
 * Catatan format: header record vxtux_logrec (dari drivers/vxtux/debug/
 * vxtux_logrec.c) little-endian:
 *   0  char[8] magic   8  u32 version   12 u32 hdr_size   16 u64 seq
 *   24 u64 boot_ns     32 u32 len       36 u32 crc        40 u32 slot
 *   44 u32 reason      hdr_size.. teks
 * ramoops tidak diparse per-header: isinya teks biasa, jadi ekstraksi deret
 * teks berlaku untuk zona record maupun console.
 */
import { readFileSync, writeFileSync } from "node:fs";
import { crc32 } from "node:zlib";

const MIN_RUN = 24;
const MAGICS = ["VXTUXRAM", "VXTUXLG2"];
const REASONS = { 0: "periodic", 1: "panic", 2: "ram", 3: "prevboot", 4: "dump" };

function printableRuns(buf) {
  const out = [];
  let cur = [];
  const push = () => {
    if (cur.length >= MIN_RUN) {
      let s = Buffer.from(cur).toString("latin1").replace(/[ \t]+$/gm, "");
      if (s.trim()) out.push(s);
    }
    cur = [];
  };
  for (const b of buf) {
    const ok = (b >= 0x20 && b <= 0x7e) || b === 0x0a || b === 0x09 || b === 0x0d;
    if (ok) cur.push(b);
    else push();
  }
  push();
  return out.join("\n");
}

function parseRecords(buf, file) {
  const recs = [];
  for (const magic of MAGICS) {
    const m = Buffer.from(magic, "latin1");
    let at = buf.indexOf(m);
    while (at >= 0) {
      if (at + 48 <= buf.length) {
        const version = buf.readUInt32LE(at + 8);
        const hdrSize = buf.readUInt32LE(at + 12);
        const seq = Number(buf.readBigUInt64LE(at + 16));
        const bootNs = Number(buf.readBigUInt64LE(at + 24));
        const len = buf.readUInt32LE(at + 32);
        const crc = buf.readUInt32LE(at + 36);
        const slot = buf.readUInt32LE(at + 40);
        const reason = buf.readUInt32LE(at + 44);
        const textOff = at + (hdrSize || 56);
        if (len > 0 && len <= 8 * 1024 * 1024 && textOff + len <= buf.length) {
          const text = buf.subarray(textOff, textOff + len);
          recs.push({
            magic, version, seq, bootNs, len, slot, reason,
            crcOk: crc32(text) >>> 0 === crc >>> 0,
            text: text.toString("latin1"),
          });
        }
      }
      at = buf.indexOf(m, at + 1);
    }
  }
  recs.sort((a, b) => a.seq - b.seq);
  return recs;
}

const files = process.argv.slice(2);
if (!files.length) {
  console.error("pakai: node dump_extract.mjs <dump.bin> [...]");
  process.exit(2);
}

for (const f of files) {
  let buf;
  try {
    buf = readFileSync(f);
  } catch (e) {
    console.error(`GAGAL baca ${f}: ${e.message}`);
    process.exitCode = 1;
    continue;
  }
  const base = f.replace(/\.bin$/i, "");

  const txt = printableRuns(buf);
  writeFileSync(`${base}.text.txt`, txt);
  console.log(`${f}: ${buf.length} byte -> ${base}.text.txt (${txt.split("\n").length} baris teks)`);

  const recs = parseRecords(buf, f);
  if (!recs.length) {
    console.log("  tidak ada record VXTUXRAM/VXTUXLG2 (normal untuk dump ramoops)");
    continue;
  }
  for (const r of recs) {
    const name = `${base}.rec-${r.seq}-r${r.reason}-${REASONS[r.reason] || "?"}.txt`;
    writeFileSync(name, r.text);
    console.log(
      `  record seq=${r.seq} slot=${r.slot} reason=${REASONS[r.reason] || r.reason} ` +
      `len=${r.len} crc=${r.crcOk ? "OK" : "RUSAK"} -> ${name}`
    );
  }
  console.log(`  total ${recs.length} record`);
}
