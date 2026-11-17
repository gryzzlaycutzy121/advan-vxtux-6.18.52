#!/usr/bin/env node
/**
 * Kemas boot_p1.img dari boot image yang SUDAH TERBUKTI load.
 *
 * Tidak memakai ci_qemu/mk_boot_artix.py karena skrip itu mensintesis header
 * v4 dari nol dan menolak jalan tanpa --ramdisk; --base-nya pun sudah tidak
 * dipakai lagi. Di sini header + ramdisk diambil apa adanya dari template
 * (mis. out/6.18.52-VXTux-UMS512/boot_artix53_panic.img) dan hanya blob
 * kernel yang diganti, plus cmdline di offset 64 (yang dicetak LK).
 *
 * Layout header v4 perangkat ini (dibaca dari template, bukan diasumsikan):
 *   0x00 "ANDROID!"   0x08 kernel_size   0x0c ramdisk_size   0x14 header_size(1584)
 *   0x28 header_version(4)  0x40..0x240 cmdline (LK mencetak dari sini)
 *   offset kernel  = 4096 (page)
 *   offset ramdisk = align(4096 + kernel_size, 4096)
 *
 * Pemakaian:
 *   node pack_p1.mjs <template.img> <Image> <out.img> [--cmdline-file CMD]
 */
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname } from "node:path";

const PAGE = 4096;
const align = (n) => Math.ceil(n / PAGE) * PAGE;

const argv = process.argv.slice(2);
const files = argv.filter((a) => !a.startsWith("--"));
const cIdx = argv.indexOf("--cmdline-file");
const cmdlineFile = cIdx >= 0 ? argv[cIdx + 1] : null;

if (files.length < 3) {
  console.error("pakai: node pack_p1.mjs <template.img> <Image> <out.img> [--cmdline-file CMD]");
  process.exit(2);
}
const [tplPath, kernPath, outPath] = files;

const tpl = readFileSync(tplPath);
const kern = readFileSync(kernPath);

if (tpl.subarray(0, 8).toString("latin1") !== "ANDROID!") {
  console.error(`FATAL: ${tplPath} bukan boot image (magic bukan ANDROID!)`);
  process.exit(1);
}

const oldKernelSize = tpl.readUInt32LE(8);
const ramdiskSize = tpl.readUInt32LE(12);
const headerSize = tpl.readUInt32LE(20);
const headerVersion = tpl.readUInt32LE(40);

if (headerVersion !== 4 || headerSize !== 1584) {
  console.error(
    `FATAL: template bukan header v4/1584 (version=${headerVersion}, header_size=${headerSize}). ` +
    `LK perangkat ini memakai header_size untuk mencari kernel.`
  );
  process.exit(1);
}

const ramdiskOff = align(PAGE + oldKernelSize);
if (ramdiskOff + ramdiskSize > tpl.length) {
  console.error(
    `FATAL: ramdisk template keluar batas (off=${ramdiskOff}, size=${ramdiskSize}, file=${tpl.length})`
  );
  process.exit(1);
}
const ramdisk = tpl.subarray(ramdiskOff, ramdiskOff + ramdiskSize);

// ARM64 Image: magic "ARM\x64" di offset 0x38.
if (kern.length < 0x40 || kern.readUInt32LE(0x38) !== 0x644d5241) {
  console.error("FATAL: berkas kernel bukan ARM64 Image (magic di 0x38 tidak cocok)");
  process.exit(1);
}

const header = Buffer.from(tpl.subarray(0, PAGE));
header.writeUInt32LE(kern.length, 8); // kernel_size baru
header.writeUInt32LE(ramdiskSize, 12); // ramdisk sama dengan template

if (cmdlineFile) {
  const cmd = readFileSync(cmdlineFile, "utf8").replace(/[\r\n]+$/, "");
  if (cmd.length > 511) {
    console.error(`FATAL: cmdline ${cmd.length} char > 511`);
    process.exit(1);
  }
  header.fill(0, 64, 576);
  header.write(cmd, 64, "latin1");
}

const out = Buffer.alloc(align(PAGE + kern.length) + align(ramdiskSize));
header.copy(out, 0);
kern.copy(out, PAGE);
ramdisk.copy(out, align(PAGE + kern.length));

// --- verifikasi hasil, jangan percaya begitu saja ------------------------
const chk = Buffer.from(out);
const problems = [];
if (chk.subarray(0, 8).toString("latin1") !== "ANDROID!") problems.push("magic header");
if (chk.readUInt32LE(20) !== 1584) problems.push("header_size");
if (chk.readUInt32LE(40) !== 4) problems.push("header_version");
if (chk.readUInt32LE(8) !== kern.length) problems.push("kernel_size");
if (chk.readUInt32LE(12) !== ramdiskSize) problems.push("ramdisk_size");
if (chk.length % PAGE !== 0) problems.push("ukuran tidak page-aligned");
if (chk.readUInt32LE(PAGE + 0x38) !== 0x644d5241) problems.push("kernel tidak di offset 4096");
const rOff = align(PAGE + kern.length);
if (!chk.subarray(rOff, rOff + ramdiskSize).equals(ramdisk)) problems.push("ramdisk bergeser");

if (problems.length) {
  console.error("FATAL: verifikasi gagal: " + problems.join(", "));
  process.exit(1);
}

mkdirSync(dirname(outPath) || ".", { recursive: true });
writeFileSync(outPath, out);

console.log(`template      : ${tplPath}`);
console.log(`  header v${headerVersion}, header_size ${headerSize}, kernel lama ${oldKernelSize} B`);
console.log(`kernel baru   : ${kernPath} (${kern.length} B)`);
console.log(`ramdisk       : ${ramdiskSize} B (disalin apa adanya dari template, offset ${rOff})`);
if (cmdlineFile) console.log(`cmdline       : ${readFileSync(cmdlineFile, "utf8").trim()}`);
console.log(`hasil         : ${outPath} (${out.length} B, ${out.length / PAGE} page)`);
console.log("verifikasi    : magic, header v4/1584, kernel di 4096, ramdisk identik  OK");
