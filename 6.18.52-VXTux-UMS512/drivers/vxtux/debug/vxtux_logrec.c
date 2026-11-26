// SPDX-License-Identifier: GPL-2.0
/*
 * vxtux_logrec.c - perekam log boot tahan-reset untuk UMS512 (VXTux).
 *
 * MASALAH YANG DIPERBAIKI
 * -----------------------
 * vxtux_flightrec.c (versi lama) menulis snapshot dmesg ke partisi cache
 * lewat kernel_write() lalu MEMBACA BALIK lewat kernel_read() dan menganggap
 * "readback verified" sebagai bukti byte sudah mendarat di eMMC.  Itu salah:
 * kernel_write() pada block device menulis ke PAGE CACHE, dan kernel_read()
 * membaca dari page cache yang sama.  Selama writeback belum jalan (default
 * dirty_expire_centisecs = 30 s) tidak ada satu byte pun di eMMC, dan pada
 * perangkat ini AP mati oleh watchdog sekitar detik ke-30 - persis sebelum
 * writeback.  Itulah sebabnya 28 dump cache (artix6..artix33) semuanya
 * byte-identik walau log kernel menyatakan "write OK": data hanya pernah
 * hidup di RAM.
 *
 * Perbaikan di file ini:
 *   1. setiap tulisan diakhiri vfs_fsync() -> blockdev_fsync: sync_blockdev()
 *      lalu blkdev_issue_flush(), jadi byte benar-benar sampai ke eMMC
 *      (dan FLUSH menembus cache internal eMMC);
 *   2. verifikasi dilakukan SETELAH invalidate_inode_pages2(), sehingga yang
 *      dibaca balik adalah isi disk, bukan page cache;
 *   3. log disimpan di 4 slot berurutan (bukan satu posisi tetap), jadi
 *      riwayat boot - bukan hanya cuplikan terakhir - tetap ada saat dump;
 *   4. snapshot juga ditulis ke region RAM yang direserve, sehingga boot
 *      yang mati SEBELUM driver MMC probe tetap meninggalkan jejak yang
 *      dipanen boot berikutnya dan ikut ditulis ke eMMC.
 *
 * TATA LETAK DI PARTISI cache (mmcblk0p49, 100 MiB, ext4 scratch)
 * --------------------------------------------------------------
 * Slot berada di EKOR partisi, dan base-nya dihitung saat runtime dari
 * ukuran partisi (100 MiB -> base = 92 MiB, area 92..100 MiB).
 *
 * Kenapa bukan offset tetap di awal partisi (versi pertama memakai
 * +0x00800000): partisi cache di perangkat ini adalah ext4 dengan SATU
 * block group (blok 4 KiB, 25600 blok).  Semua metadata (block bitmap,
 * inode bitmap, inode table) occupy blok 0..~1566, jadi area data mulai
 * ~6,25 MiB - dan file log initramfs (/cache/vxtux-logs/*) dialokasikan
 * tepat di sana.  Area +8 MiB milik kita bertabrakan dengan file log kita
 * SENDIRI: slot logrec menimpa isi berkas yang justru menyimpan dmesg
 * boot tersebut.  Ekor partisi adalah area data murni (metadata selalu
 * mendahului data di dalam satu group) dan praktis tidak pernah dipakai
 * initramfs (file log initramfs kecil, di awal partisi).
 *
 *        +0x0000  struct lr_hdr (56 byte, lihat di bawah)
 *        +0x1000  teks dmesg mentah (maks 2 MiB - 4096)
 *
 *   Slot dipakai bergilang: snapshot ke-N ditulis ke slot (N % 4).  Jadi
 *   dump `cache` selalu memuat 4 snapshot terakhir; dengan initcall_debug=1
 *   itu cukup untuk membaca urutan initcall lintas beberapa detik boot.
 *
 *   Ukuran partisi diambil dari block device saat runtime.  Kalau ukuran
 *   tidak bisa dibaca, atau partisi terlalu kecil, kanal eMMC DIMATIKAN
 *   (cetak satu kali) dan rekaman tetap jalan di RAM saja - lebih baik
 *   tidak menulis sama sekali daripada menebak offset dan menimpa data.
 *
 * TATA LETAK REKAMAN RAM (default 0xAFC00000, 4 MiB, lewat cmdline vxtuxlog=)
 * --------------------------------------------------------------------------
 *   dua slot 2 MiB (double buffer): header yang sama + teks dmesg.  Setiap
 *   snapshot di-flush ke PoC (dcache_clean_poc), jadi isinya ada di DRAM
 *   walau CPU menggantung.  Pada boot berikutnya region ini diperiksa:
 *   record valid -> disalin ke /proc dan ditulis ke eMMC (reason=PREVBOOT),
 *   record tidak valid -> dicetak "tidak ada", yang sekaligus menjawab
 *   pertanyaan apakah DRAM bertahan melewati reset watchdog di perangkat ini.
 *
 * CARA MEMBACA HASILNYA
 * ---------------------
 *   spd_dump baca partisi cache -> out/tools/baca_logrec.py cache.bin
 *   Skrip itu mencari magic "VXTUXLG2" di seluruh image, memvalidasi CRC,
 *   mengurutkan record menurut seq, dan menandai initcall terakhir.
 *
 * BIAYA
 * -----
 * Satu snapshot = satu kali kmsg_dump_get_buffer (~3,5 MiB memcpy) plus satu
 * tulisan 2 MiB + fsync tiap 3 detik setelah channel eMMC hidup (500 ms
 * selama boot awal).  Slot dilewati bila isi log tidak berubah, jadi eMMC
 * tidak dipakai menulis data yang sama berulang-ulang.
 */
#define pr_fmt(fmt) "vxtux-log: " fmt

#include <linux/blkdev.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/device/class.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/kthread.h>
#include <linux/memblock.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pagemap.h>
#include <linux/panic_notifier.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/stringify.h>
#include <linux/timekeeping.h>
#include <linux/vmalloc.h>

#include <asm/cacheflush.h>
#include <asm/io.h>
#include <asm/memory.h>

/* ---- partisi tujuan ---------------------------------------------------- */
#define LR_DISK		"mmcblk0"
#define LR_PARTNO	49		/* cache: 100 MiB, scratch */
#define LR_SLOT_SIZE	(2U << 20)	/* 2 MiB per snapshot */
#define LR_SLOTS	4U		/* riwayat 4 snapshot */
#define LR_AREA_SIZE	((u64)LR_SLOTS * LR_SLOT_SIZE)
#define LR_TEXT_OFF	4096U		/* teks selalu mulai di +0x1000 */
#define LR_TEXT_MAX	(LR_SLOT_SIZE - LR_TEXT_OFF)

/* ---- rekaman RAM (tahan reset) ---------------------------------------- */
#define LR_RAM_BYTES	(4U << 20)	/* 2 slot x 2 MiB */
#define LR_RAM_SLOT	(2U << 20)
#define LR_RAM_MAX	(LR_RAM_SLOT - LR_TEXT_OFF)

#define LR_MAGIC	"VXTUXLG2"
#define LR_RAM_MAGIC	"VXTUXRAM"
#define LR_MAGIC_LEN	8
#define LR_VERSION	2U

/* alasan record - dipakai pembaca untuk membedakan konteks */
#define LR_REASON_PERIODIC	0U	/* snapshot berkala */
#define LR_REASON_PANIC		1U	/* ditulis dari notifier panic */
#define LR_REASON_RAM		2U	/* hanya di region RAM */
#define LR_REASON_PREVBOOT	3U	/* panjang dari boot sebelumnya (RAM) */

/*
 * Header 56 byte.  Disimpan little endian apa adanya (arm64 LE), sehingga
 * pembaca Python cukup struct "<8sIIQQIIIIII".
 */
struct lr_hdr {
	char	magic[8];	/* "VXTUXLG2" / "VXTUXRAM" */
	u32	version;	/* LR_VERSION */
	u32	hdr_size;	/* LR_TEXT_OFF */
	u64	seq;		/* nomor snapshot, naik monoton */
	u64	boot_ns;	/* CLOCK_BOOTTIME saat snapshot */
	u32	len;		/* panjang teks */
	u32	crc;		/* crc32 teks */
	u32	slot;		/* slot tempat record ini berada */
	u32	reason;		/* LR_REASON_* */
	u32	boot_count;	/* berapa kali region RAM sudah dipakai */
	u32	text_off;	/* offset teks dari awal record */
};

static_assert(sizeof(struct lr_hdr) == 56);

/* ---- state ------------------------------------------------------------ */
static char *lr_buf;			/* header + teks untuk ditulis */
static char *lr_text;			/* = lr_buf + LR_TEXT_OFF */
static size_t lr_prev_len;
static u32 lr_prev_crc;
static bool lr_prev_valid;		/* ada snapshot yang layak dibandingkan */

static phys_addr_t lr_ram_phys;		/* 0 = tidak direserve */
static void *lr_ram;			/* __va() setelah paging_init */
static bool lr_ram_ok;
static u64 lr_ram_seq;
static u32 lr_boot_count = 1;

static u64 lr_disk_seq;		/* jumlah snapshot yang sampai ke disk */
static u32 lr_next_slot;		/* slot berikutnya yang akan dipakai */
static bool lr_channel_ok;		/* sudah pernah sukses menulis ke eMMC */
static u64 lr_base;			/* offset slot 0 di partisi (ekor) */
static u64 lr_part_bytes;		/* ukuran partisi cache (byte) */
static bool lr_prev_boot_saved;		/* record boot lama sudah didorong */
static char *lr_prev_boot;		/* salinan record dari region RAM */
static size_t lr_prev_boot_len;

static int lr_last_err;			/* errno terakhir yang sudah dicetak */
static unsigned long lr_last_err_jif;
static struct task_struct *lr_task;
static DEFINE_MUTEX(lr_lock);

/* ---- util ------------------------------------------------------------- */

/*
 * Cetak kegagalan tanpa membanjiri layar: errno yang sama hanya dicetak
 * ulang tiap 10 detik, jadi keadaan terkini tetap terlihat di fbcon.
 */
static void lr_report(const char *what, int err)
{
	if (err == lr_last_err &&
	    time_before(jiffies, lr_last_err_jif + 10 * HZ))
		return;
	lr_last_err = err;
	lr_last_err_jif = jiffies;
	pr_err("%s failed: %d (%pe)\n", what, err, ERR_PTR(err));
}

/* Ambil ekor dmesg ke lr_text.  Mengembalikan panjang teks. */
static size_t lr_grab_log(void)
{
	struct kmsg_dump_iter iter = { };
	size_t len = 0;

	kmsg_dump_rewind(&iter);
	if (!kmsg_dump_get_buffer(&iter, false, lr_text, LR_TEXT_MAX, &len))
		return 0;
	return len;
}

/* Isi header record. */
static void lr_fill_hdr(struct lr_hdr *h, const char *magic, u64 seq,
			const char *text, size_t len, u32 slot, u32 reason)
{
	memset(h, 0, LR_TEXT_OFF);
	memcpy(h->magic, magic, LR_MAGIC_LEN);
	h->version = LR_VERSION;
	h->hdr_size = LR_TEXT_OFF;
	h->seq = seq;
	h->boot_ns = ktime_get_boottime_ns();
	h->len = len;
	h->crc = crc32(0, text, len);
	h->slot = slot;
	h->reason = reason;
	h->boot_count = lr_boot_count;
	h->text_off = LR_TEXT_OFF;
}

/* ---- rekaman RAM (selamat melewati reset watchdog) -------------------- */

/*
 * Ditulis bergantian ke dua slot, jadi snapshot yang terpotong reset tidak
 * menghapus snapshot sebelumnya.  Flush ke PoC wajib: tanpa itu teks masih
 * di D-cache CPU dan hilang bersama reset.
 */
static void lr_ram_save(u32 reason)
{
	struct lr_hdr *h;
	size_t len;

	if (!lr_ram_ok || !lr_buf)
		return;
	len = lr_grab_log();
	if (!len)
		return;
	if (len > LR_RAM_MAX)
		len = LR_RAM_MAX;

	h = (struct lr_hdr *)((char *)lr_ram +
			      (size_t)((lr_ram_seq + 1) & 1) * LR_RAM_SLOT);
	lr_fill_hdr(h, LR_RAM_MAGIC, lr_ram_seq + 1, lr_text, len,
		    (u32)((lr_ram_seq + 1) & 1), reason);
	memcpy((char *)h + LR_TEXT_OFF, lr_text, len);
	dcache_clean_poc((unsigned long)h,
			 (unsigned long)h + LR_TEXT_OFF + len);
	lr_ram_seq++;
}

/*
 * Cari record valid dari boot sebelumnya di region RAM.  Dipanggil sekali
 * saat init, SEBELUM region dipakai lagi.
 */
static void lr_harvest_prev(void)
{
	struct lr_hdr *best = NULL;
	int i;

	for (i = 0; i < 2; i++) {
		struct lr_hdr *h = (struct lr_hdr *)((char *)lr_ram +
						    (size_t)i * LR_RAM_SLOT);
		const char *text = (const char *)h + LR_TEXT_OFF;

		if (memcmp(h->magic, LR_RAM_MAGIC, LR_MAGIC_LEN))
			continue;
		if (h->version != LR_VERSION || h->text_off != LR_TEXT_OFF)
			continue;
		if (!h->len || h->len > LR_RAM_MAX)
			continue;
		if (crc32(0, text, h->len) != h->crc)
			continue;
		if (!best || (s64)(h->seq - best->seq) > 0)
			best = h;
	}

	if (!best) {
		pr_info("RAM record: none - DRAM was cleared by the bootloader (or this is the first boot)\n");
		return;
	}

	lr_boot_count = best->boot_count + 1;
	lr_prev_boot = vmalloc(best->len);
	if (!lr_prev_boot) {
		pr_warn("RAM record: out of memory copying %u bytes\n",
			best->len);
		return;
	}
	memcpy(lr_prev_boot, (const char *)best + LR_TEXT_OFF, best->len);
	lr_prev_boot_len = best->len;
	pr_info("RAM record: HARVESTED %u bytes from boot #%u (seq %llu, uptime %llu ms)\n",
		best->len, best->boot_count, best->seq, best->boot_ns / 1000000);
}

/* ---- channel eMMC ----------------------------------------------------- */

/* Buka partisi cache: lewat dev_t gendisk (tanpa devtmpfs), lalu lewat node. */
static struct file *lr_open_part(void)
{
	struct device *dev;
	struct gendisk *disk;
	dev_t devt = 0;
	struct file *f;

	dev = class_find_device_by_name(&block_class, LR_DISK);
	if (dev) {
		disk = dev_to_disk(dev);
		if (disk)
			devt = part_devt(disk, LR_PARTNO);
		put_device(dev);
	}

	if (devt) {
		f = bdev_file_open_by_dev(devt, BLK_OPEN_WRITE, NULL, NULL);
		if (!IS_ERR(f))
			return f;
		lr_report("open " LR_DISK "p" __stringify(LR_PARTNO) " by devt",
			  PTR_ERR(f));
	}

	f = bdev_file_open_by_path("/dev/" LR_DISK "p" __stringify(LR_PARTNO),
				   BLK_OPEN_WRITE, NULL, NULL);
	if (IS_ERR(f))
		lr_report("open /dev/" LR_DISK "p" __stringify(LR_PARTNO),
			  PTR_ERR(f));
	return f;
}

/*
 * Hitung base slot dari UKURAN SEBENARNYA partisi (bukan offset tebakan).
 * Partisi cache perangkat ini 100 MiB -> area slot 92..100 MiB (ekor).
 * Kalau ukuran tidak bisa dibaca atau partisi terlalu kecil, kanal eMMC
 * dimatikan: rekaman RAM tetap jalan, dan kita tidak menimpa data apa pun.
 */
static int lr_setup_base(struct file *f)
{
	struct block_device *bdev;
	u64 bytes;

	if (lr_base)
		return 0;

	bdev = file_bdev(f);
	if (!bdev) {
		lr_report("partition size", -ENODEV);
		return -ENODEV;
	}
	bytes = (u64)bdev_nr_bytes(bdev);
	if (bytes < LR_AREA_SIZE + (16ULL << 20)) {
		lr_report("partition too small", -EINVAL);
		return -EINVAL;
	}
	lr_part_bytes = bytes;
	lr_base = (bytes - LR_AREA_SIZE) & ~((u64)(1 << 20) - 1);
	pr_info("cache p%d: %llu bytes, slots at %#llx..%#llx (%u x %u MiB)\n",
		LR_PARTNO, bytes, lr_base, lr_base + LR_AREA_SIZE - 1,
		LR_SLOTS, LR_SLOT_SIZE >> 20);
	return 0;
}

/*
 * Tulis satu record ke slot eMMC lalu PASTIKAN byte-nya ada di disk.
 *
 * Urutan ini yang membuat kanal ini benar-benar persisten:
 *   kernel_write -> vfs_fsync (sync_blockdev + blkdev_issue_flush)
 *                -> invalidate_inode_pages2 (buang page cache)
 *                -> kernel_read (baca dari disk, bukan dari cache)
 */
static int lr_disk_put(const char *magic, const char *text, size_t len,
		       u32 reason)
{
	struct lr_hdr *h = (struct lr_hdr *)lr_buf;
	struct file *f;
	loff_t pos;
	size_t total;
	u32 slot;
	u64 seq;
	u8 back[LR_MAGIC_LEN];
	ssize_t n;
	int ret;

	slot = lr_next_slot;
	seq = lr_disk_seq + 1;
	lr_fill_hdr(h, magic, seq, text, len, slot, reason);
	if (text != lr_text)
		memcpy(lr_buf + LR_TEXT_OFF, text, len);
	total = ALIGN((size_t)LR_TEXT_OFF + len, 512);

	f = lr_open_part();
	if (IS_ERR(f))
		return PTR_ERR(f);

	if (lr_setup_base(f)) {
		fput(f);
		return -EIO;
	}

	pos = lr_base + (u64)slot * LR_SLOT_SIZE;
	ret = kernel_write(f, lr_buf, total, &pos);
	if (ret != (int)total) {
		int err = ret < 0 ? ret : -EIO;

		fput(f);
		lr_report("write", err);
		return err;
	}

	/* fsync: tanpa ini data hanya ada di page cache (bug versi lama). */
	ret = vfs_fsync(f, 0);
	if (ret) {
		fput(f);
		lr_report("fsync", ret);
		return ret;
	}

	/* Buang page cache, lalu baca balik dari disk. */
	invalidate_inode_pages2(f->f_mapping);
	pos = lr_base + (u64)slot * LR_SLOT_SIZE;
	n = kernel_read(f, back, sizeof(back), &pos);
	fput(f);
	if (n != (ssize_t)sizeof(back) || memcmp(back, magic, LR_MAGIC_LEN)) {
		lr_report("verify readback after fsync", -EIO);
		return -EIO;
	}

	lr_disk_seq++;
	lr_next_slot = (slot + 1) % LR_SLOTS;
	return 0;
}

/* Snapshot berkala: lewati bila isi log belum berubah sejak tulisan terakhir. */
static void lr_disk_tick(void)
{
	size_t len;
	u32 crc;

	/* record boot sebelumnya didahulukan: isinya tidak bisa dibuat ulang */
	if (lr_prev_boot && !lr_prev_boot_saved) {
		if (!lr_disk_put(LR_MAGIC, lr_prev_boot, lr_prev_boot_len,
				 LR_REASON_PREVBOOT))
			lr_prev_boot_saved = true;
		else
			return;
	}

	len = lr_grab_log();
	if (!len)
		return;
	crc = crc32(0, lr_text, len);
	if (lr_prev_valid && len == lr_prev_len && crc == lr_prev_crc)
		return;

	if (lr_disk_put(LR_MAGIC, lr_text, len, LR_REASON_PERIODIC))
		return;

	lr_prev_len = len;
	lr_prev_crc = crc;
	lr_prev_valid = true;
	if (!lr_channel_ok) {
		lr_channel_ok = true;
		pr_info("eMMC channel up: %zu bytes of dmesg -> cache p%d + %#llx slot %u (fsync + disk verify)\n",
			len, LR_PARTNO, (unsigned long long)lr_base,
			(lr_next_slot + LR_SLOTS - 1) % LR_SLOTS);
	}
}

/* ---- kthread ---------------------------------------------------------- */

static int lr_thread(void *unused)
{
	while (!kthread_should_stop()) {
		mutex_lock(&lr_lock);
		lr_ram_save(LR_REASON_RAM);
		lr_disk_tick();
		mutex_unlock(&lr_lock);

		msleep(lr_channel_ok ? 3000 : 500);
	}
	return 0;
}

/* ---- panic ------------------------------------------------------------ */

/*
 * Jangan pernah memblokir jalur panic: CPU lain bisa memegang lock, dan
 * watchdog akan mematikan semuanya sebelum tulisan selesai.  Yang penting
 * teks sudah masuk region RAM (flush PoC), karena itu yang dipanen boot
 * berikutnya.
 */
static int lr_panic_notify(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	/*
	 * Hanya region RAM: lr_disk_tick() memakai fsync/invalidate yang boleh
	 * tidur dan bisa mengambil lock blok - tidak boleh dari jalur panic.
	 * Jejak di RAM (sudah di-flush ke PoC) dipanen boot berikutnya.
	 */
	if (mutex_trylock(&lr_lock)) {
		lr_ram_save(LR_REASON_PANIC);
		mutex_unlock(&lr_lock);
	}
	return NOTIFY_OK;
}

static struct notifier_block lr_panic_nb = {
	.notifier_call	= lr_panic_notify,
};

/* ---- /proc/vxtuxlog (status; teks lengkap ada di partisi cache) ------ */

static int lr_show(struct seq_file *m, void *v)
{
	seq_printf(m, "disk_seq=%llu next_slot=%u slots=%u base=%#llx size=%u\n",
		   lr_disk_seq, lr_next_slot, LR_SLOTS,
		   (unsigned long long)lr_base, LR_SLOT_SIZE);
	seq_printf(m, "channel_ok=%u last_len=%zu last_crc=%08x last_err=%d\n",
		   lr_channel_ok, lr_prev_len, lr_prev_crc, lr_last_err);
	seq_printf(m, "ram_phys=%pa ram_ok=%u ram_seq=%llu boot_count=%u\n",
		   &lr_ram_phys, lr_ram_ok, lr_ram_seq, lr_boot_count);
	seq_printf(m, "prev_boot=%s (%zu bytes, saved=%u)\n",
		   lr_prev_boot ? "present" : "absent", lr_prev_boot_len,
		   lr_prev_boot_saved);
	return 0;
}

/* ---- init ------------------------------------------------------------- */

/*
 * vxtuxlog=<phys>[,<bytes>]
 *
 * Reserve region RAM selagi memblock masih bisa diedit (parse_early_param),
 * jauh sebelum buddy allocator ada.  Tanpa ini, region bisa diberikan ke
 * siapa pun dan record kita ditimpa.
 */
static int __init lr_setup(char *str)
{
	unsigned long long addr = 0, size = LR_RAM_BYTES;
	char *tok;

	if (!str || !*str)
		return 0;
	tok = strsep(&str, ",");
	if (!tok || kstrtoull(tok, 0, &addr) || !addr) {
		pr_err("vxtuxlog: invalid address\n");
		return 0;
	}
	if (str && *str && !kstrtoull(str, 0, &size))
		size = LR_RAM_BYTES;
	if (size != LR_RAM_BYTES)
		pr_warn("vxtuxlog: size %llu ignored, using %u\n",
			size, LR_RAM_BYTES);
	if (memblock_reserve(addr, LR_RAM_BYTES)) {
		pr_err("vxtuxlog: cannot reserve %#llx\n", addr);
		return 0;
	}
	lr_ram_phys = addr;
	pr_info("reserved RAM record %#llx..%#llx (%u KiB)\n",
		addr, addr + LR_RAM_BYTES - 1, LR_RAM_BYTES >> 10);
	return 0;
}
early_param("vxtuxlog", lr_setup);

static int __init vxtux_logrec_init(void)
{
	if (!IS_ENABLED(CONFIG_PRINTK) || !IS_ENABLED(CONFIG_BLOCK))
		return -ENODEV;

	lr_buf = vzalloc((size_t)LR_TEXT_OFF + LR_TEXT_MAX);
	if (!lr_buf) {
		pr_err("out of memory for %u byte buffer\n", LR_TEXT_MAX);
		return -ENOMEM;
	}
	lr_text = lr_buf + LR_TEXT_OFF;

	if (lr_ram_phys) {
		lr_ram = __va(lr_ram_phys);
		lr_ram_ok = true;
		lr_harvest_prev();
	} else {
		pr_warn("RAM region disabled (add vxtuxlog=<phys> to the command line); eMMC channel still active\n");
	}

	atomic_notifier_chain_register(&panic_notifier_list, &lr_panic_nb);

	lr_task = kthread_run(lr_thread, NULL, "vxtux-log");
	if (IS_ERR(lr_task)) {
		int ret = PTR_ERR(lr_task);

		lr_task = NULL;
		atomic_notifier_chain_unregister(&panic_notifier_list,
						 &lr_panic_nb);
		vfree(lr_buf);
		lr_buf = NULL;
		lr_text = NULL;
		return ret;
	}

	proc_create_single("vxtuxlog", 0400, NULL, lr_show);
	pr_info("active: %u slots x %u MiB in " LR_DISK "p%d + %#llx, ram=%pa, boot #%u\n",
		LR_SLOTS, LR_SLOT_SIZE >> 20, LR_PARTNO,
		(unsigned long long)lr_base, &lr_ram_phys, lr_boot_count);
	return 0;
}
/*
 * core_initcall: berjalan sebelum SMP dan sebelum driver perangkat, jadi
 * thread perekam sudah hidup saat kernel menggantung di driver mana pun
 * setelah titik ini.
 */
core_initcall(vxtux_logrec_init);
