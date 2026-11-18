// SPDX-License-Identifier: GPL-2.0
/*
 * vxtux_hook.c - trace hook resource API + regmap untuk deblob compare (VXTux).
 *
 * TUJUAN
 * ------
 * Menangkap kontrak SOFTWARE driver (apa yang diminta, nilai register yang
 * ditulis, urutan probe) agar bisa dibandingkan antara kernel 5.4.210 ORI
 * (golden) dan 6.18.52 (port deblob).  Semua event ditulis ke ring di RAM
 * yang di-reserve (default 0xAF800000, 4 MiB, lewat cmdline vxtuxtrace=)
 * yang TAHAN RESET, lalu dipanen lewat `spd_dump read_mem 0xAF800000`
 * (sama seperti vxtuxlog 0xAFC00000) dan dianalisis oleh vxtr_parse.py.
 *
 * KOREKSI DESAIN (hasil bedah API 2026-11-18)
 * -------------------------------------------
 *   - readl/writel = macro static inline di arch/arm64/include/asm/io.h
 *     -> TIDAK ada symbol -> TIDAK bisa kprobe (di 5.4 dan 6.18).
 *   - CONFIG_MMIOTRACE TIDAK ADA di arm64 -> TIDAK bisa dipakai sama sekali.
 *   - Mayoritas driver stock pakai regmap (clk/sprd 100% regmap,
 *     regulator 357:9, mfd 319:178) -> regmap_write/read/update_bits_base
 *     adalah symbol ter-export dengan signature IDENTIK 5.4<->6.18 ->
 *     KPROBEABLE dengan nilai register lengkap.  Ini jalur utama capture.
 *   - platform_get_irq_optional ADA di 5.4.210 (backport vendor, bukan
 *     upstream 5.5) -> bukan perbedaan API; hook kedua-duanya.
 *   - Raw readl/writel minoritas (pinctrl/sprd) ditangani header vxtux_io.h
 *     pada sisi 6.18, dan golden statis (peta BFKRT + urutan dari source)
 *     pada sisi 5.4.
 *
 * PEMAKAIAN
 * ---------
 *   Golden 5.4.210 ORI:
 *     build hook ini di tree 5.4, tambah vxtuxtrace=0xAF800000 ke cmdline,
 *     boot normal -> probe driver stock -> `spd_dump read_mem 0xAF800000`
 *     -> vxtr_parse.py -> golden.txt
 *   Replay 6.18.52 (port):
 *     sama, boot -> probe -> read_mem -> vxtr_parse.py -> replay.txt
 *   Bandingkan: vxtr_diff.py golden.txt replay.txt (urutan, argumen, defer).
 *
 * BATAS JUJUR (yang TIDAK tertangkap di sini):
 *   handshake EL3, IRQ nyata dari silikon, respons fisik panel/charger,
 *   dan apakah tegangan rail benar-benar naik.  Itu hanya bisa diverifikasi
 *   di sesi ssh vxtux bare-metal (/sys/kernel/debug/ + BerryUART).
 */
#define pr_fmt(fmt) "vxtux-hook: " fmt

#include <linux/atomic.h>
#include <linux/build_bug.h>
#include <linux/crc32.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/jhash.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/memblock.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>

#include <asm/cacheflush.h>
#include <asm/io.h>
#include <asm/memory.h>

/* ---- region RAM (tahan reset) ---------------------------------------- */
#define VXTR_DEFAULT_PHYS	0xAF800000ULL	/* 4 MiB sebelum vxtuxlog */
#define VXTR_RAM_BYTES		(4U << 20)
#define VXTR_HDR_SIZE		0x1000U	/* header 4 KiB, record setelahnya */
#define VXTR_REC_SIZE		64U
#define VXTR_REC_AREA		(VXTR_RAM_BYTES - VXTR_HDR_SIZE)
#define VXTR_MAX_REC		(VXTR_REC_AREA / VXTR_REC_SIZE)	/* 65472 */

#define VXTR_MAGIC		0x56585452U	/* 'VXTR' */
#define VXTR_VERSION		1U

/* ---- record event (64 byte, semua u64 align) ------------------------- */
struct vxtr_rec {
	u32	magic;		/* VXTR_MAGIC */
	u32	version;	/* VXTR_VERSION */
	u32	crc;		/* crc32 atas [event..res] */
	u32	seq;		/* nomor urut, naik monoton (lintas boot) */
	u16	event;		/* VXTR_E_* */
	u8	flags;		/* bit0: arg0 = hash nama (bukan numerik) */
	u8	pad;
	u32	reserved;
	u64	arg0;
	u64	arg1;
	u64	caller;		/* return address (LR) di titik kprobe */
	u64	t_order;	/* ktime_get_boottime_ns() */
	u64	res;		/* return value (post-handler), signed */
};
static_assert(sizeof(struct vxtr_rec) == VXTR_REC_SIZE);

/* ---- header ring (di awal region, di-flush tiap emit) ---------------- */
struct vxtr_hdr {
	u32	magic;
	u32	version;
	atomic_t count;		/* total event sepanjang hidup (monoton, atomik) */
	u32	overflow;	/* pernah membuang record tertua */
	u32	pad;
};
static_assert(sizeof(struct vxtr_hdr) <= VXTR_HDR_SIZE);

/* ---- jenis event ------------------------------------------------------ */
enum {
	VXTR_E_CLK_GET		= 1,
	VXTR_E_DEVM_CLK_GET,
	VXTR_E_REG_GET,
	VXTR_E_DEVM_REG_GET,
	VXTR_E_DEVM_REG_OPT,
	VXTR_E_RST_GET,
	VXTR_E_DEVM_RST_GET,
	VXTR_E_DEVM_RST_OPT,
	VXTR_E_PINCTRL_GET,
	VXTR_E_PINCTRL_STATE,
	VXTR_E_IRQ_GET,
	VXTR_E_IRQ_OPT,
	VXTR_E_IRQ_BYNAME,
	VXTR_E_DMA_CHAN,
	VXTR_E_IOREMAP,
	VXTR_E_DRV_REG,		/* platform_driver_register */
	VXTR_E_PROBE_ENTER,	/* driver_probe_device, pre */
	VXTR_E_PROBE_RET,	/* driver_probe_device, post (res = ret) */
	VXTR_E_PM_ATTACH,
	VXTR_E_REGMAP_WRITE,
	VXTR_E_REGMAP_READ,
	VXTR_E_REGMAP_UPDATE,	/* regmap_update_bits_base */
};

/* ---- state ------------------------------------------------------------ */
static phys_addr_t vxtr_phys;
static void *vxtr_ring;			/* __va(vxtr_phys) */
static struct vxtr_hdr *vxtr_hdr;
static struct vxtr_rec *vxtr_recs;	/* = ring + VXTR_HDR_SIZE */
static bool vxtr_ring_ok;
static bool vxtr_enabled;		/* master switch (boot param) */

/* ---- util ------------------------------------------------------------- */

/*
 * Hash nama secara aman (tidak pernah fault): kalau pointer nama tidak valid
 * (userspace/rusak) hash = 0, bukan crash.  Hash dipakai pembanding, bukan
 * konten; nama asli bisa dipetakan lewat DTS/clk tree kalau perlu.
 */
static u64 vxtr_hash_name(const char *s)
{
	char tmp[64];
	long n;

	if (!s)
		return 0;
	n = strncpy_from_kernel_nofault(tmp, s, sizeof(tmp));
	if (n <= 0)
		return 0;
	return (u64)jhash(tmp, (u32)n, 0);
}

/*
 * Tulis satu record + header.  Dipanggil dari handler kprobe (konteks atomik
 * kadang IRQ-off): tidak boleh sleep, tidak boleh lock.  Setiap record dan
 * header di-flush ke PoC supaya isinya ada di DRAM walau CPU hang/reset.
 */
static void vxtr_emit(u16 event, u8 has_name, u64 arg0, u64 arg1,
		      u64 caller, u64 res)
{
	struct vxtr_rec *r;
	u32 n, idx;

	if (!vxtr_ring_ok)
		return;

	/*
	 * Alokasi slot secara ATOMik: kprobe bisa fire di banyak core
	 * sekaligus (SMP).  count = seq monoton -> slot unik per event,
	 * tidak ada dua core menulis slot yang sama.
	 */
	n = (u32)atomic_add_return(1, &vxtr_hdr->count);
	idx = (n - 1) % VXTR_MAX_REC;
	r = &vxtr_recs[idx];

	memset(r, 0, sizeof(*r));
	r->magic = VXTR_MAGIC;
	r->version = VXTR_VERSION;
	r->seq = n;
	r->event = event;
	r->flags = has_name;
	r->arg0 = arg0;
	r->arg1 = arg1;
	r->caller = caller;
	r->t_order = ktime_get_boottime_ns();
	r->res = res;
	r->crc = crc32(0, (const u8 *)r + offsetof(struct vxtr_rec, event),
		       VXTR_REC_SIZE - offsetof(struct vxtr_rec, event));

	dcache_clean_poc((unsigned long)r,
			 (unsigned long)r + VXTR_REC_SIZE);

	if (n > VXTR_MAX_REC)
		vxtr_hdr->overflow = 1;

	/* flush header tiap emit: count adalah jangkar pembaca */
	dcache_clean_poc((unsigned long)vxtr_hdr,
			 (unsigned long)vxtr_hdr + sizeof(*vxtr_hdr));
}

/* Ambil return address (caller) dari pt_regs arm64. */
static u64 vxtr_lr(struct pt_regs *regs)
{
#ifdef CONFIG_ARM64
	return regs->regs[30];
#else
	return regs->ip;
#endif
}

/* ---- kprobe pre/post handler ----------------------------------------- */

static int vxtr_pre_clk_get(struct kprobe *p, struct pt_regs *regs)
{
	/* clk_get(dev, const char *id) / devm_clk_get(dev, id) */
	const char *id = (const char *)regs->regs[1];
	u16 ev = (p->symbol_name[0] == 'd') ? VXTR_E_DEVM_CLK_GET
					    : VXTR_E_CLK_GET;

	vxtr_emit(ev, 1, vxtr_hash_name(id), 0, vxtr_lr(regs), 0);
	return 0;
}

static int vxtr_pre_reg_get(struct kprobe *p, struct pt_regs *regs)
{
	/* regulator_get(dev, id) / devm_regulator_get(dev, id) */
	const char *id = (const char *)regs->regs[1];
	u16 ev = VXTR_E_REG_GET;

	if (p->symbol_name[0] == 'd')
		ev = VXTR_E_DEVM_REG_GET;
	else if (strstr(p->symbol_name, "optional"))
		ev = VXTR_E_DEVM_REG_OPT;
	vxtr_emit(ev, 1, vxtr_hash_name(id), 0, vxtr_lr(regs), 0);
	return 0;
}

static int vxtr_pre_rst_get(struct kprobe *p, struct pt_regs *regs)
{
	/* reset_control_get(dev, id) / devm_reset_control_get[_optional] */
	const char *id = (const char *)regs->regs[1];
	u16 ev = VXTR_E_RST_GET;

	if (p->symbol_name[0] == 'd')
		ev = VXTR_E_DEVM_RST_GET;
	if (strstr(p->symbol_name, "optional"))
		ev = VXTR_E_DEVM_RST_OPT;
	vxtr_emit(ev, 1, vxtr_hash_name(id), 0, vxtr_lr(regs), 0);
	return 0;
}

static int vxtr_pre_pinctrl_get(struct kprobe *p, struct pt_regs *regs)
{
	/* devm_pinctrl_get(dev) */
	vxtr_emit(VXTR_E_PINCTRL_GET, 0, regs->regs[0], 0,
		  vxtr_lr(regs), 0);
	return 0;
}

static int vxtr_pre_pinctrl_state(struct kprobe *p, struct pt_regs *regs)
{
	/* pinctrl_lookup_state(dev, const char *name) */
	const char *name = (const char *)regs->regs[1];

	vxtr_emit(VXTR_E_PINCTRL_STATE, 1, vxtr_hash_name(name), 0,
		  vxtr_lr(regs), 0);
	return 0;
}

static int vxtr_pre_irq_get(struct kprobe *p, struct pt_regs *regs)
{
	/* platform_get_irq(pdev, num) / _optional(pdev, num) / _byname(pdev, name) */
	u16 ev = VXTR_E_IRQ_GET;
	u64 a0, a1 = 0;
	u8 has = 0;

	if (strstr(p->symbol_name, "byname")) {
		ev = VXTR_E_IRQ_BYNAME;
		a0 = vxtr_hash_name((const char *)regs->regs[1]);
		has = 1;
	} else {
		if (strstr(p->symbol_name, "optional"))
			ev = VXTR_E_IRQ_OPT;
		a0 = regs->regs[1];	/* nomor IRQ */
	}
	vxtr_emit(ev, has, a0, a1, vxtr_lr(regs), 0);
	return 0;
}

static int vxtr_pre_dma_chan(struct kprobe *p, struct pt_regs *regs)
{
	/* dma_request_chan(dev, const char *name) */
	const char *name = (const char *)regs->regs[1];

	vxtr_emit(VXTR_E_DMA_CHAN, 1, vxtr_hash_name(name), 0,
		  vxtr_lr(regs), 0);
	return 0;
}

static int vxtr_pre_ioremap(struct kprobe *p, struct pt_regs *regs)
{
	/* ioremap(phys_addr_t offset, size_t size) */
	vxtr_emit(VXTR_E_IOREMAP, 0, regs->regs[0], regs->regs[1],
		  vxtr_lr(regs), 0);
	return 0;
}

static int vxtr_pre_drv_reg(struct kprobe *p, struct pt_regs *regs)
{
	/* platform_driver_register(struct platform_driver *drv) */
	struct platform_driver *drv =
		(struct platform_driver *)regs->regs[0];
	u64 name = 0;

	if (drv && drv->driver.name)
		name = vxtr_hash_name(drv->driver.name);
	vxtr_emit(VXTR_E_DRV_REG, 1, name, 0, vxtr_lr(regs), 0);
	return 0;
}

static int vxtr_pre_probe(struct kprobe *p, struct pt_regs *regs)
{
	/*
	 * driver_probe_device(const struct device_driver *drv,
	 *                     struct device *dev)  <- 6.18 (dan 5.4): STATIC,
	 * urutan (drv, dev), BUKAN (dev, drv).  regs[0]=drv, regs[1]=dev.
	 */
	struct device_driver *drv = (struct device_driver *)regs->regs[0];
	struct device *dev = (struct device *)regs->regs[1];
	u64 dn = 0, rn = 0;

	if (dev && dev->kobj.name)
		dn = vxtr_hash_name(dev->kobj.name);
	if (drv && drv->name)
		rn = vxtr_hash_name(drv->name);
	vxtr_emit(VXTR_E_PROBE_ENTER, 0, dn, rn, vxtr_lr(regs), 0);
	return 0;
}

static int vxtr_post_probe(struct kprobe *p, struct pt_regs *regs,
			   struct kprobe *pp)
{
	/* ret = regs->regs[0] pada arm64 (x0 membawa nilai pulang) */
	vxtr_emit(VXTR_E_PROBE_RET, 0, 0, 0, vxtr_lr(regs),
		  (s64)regs->regs[0]);
	return 0;
}

static int vxtr_pre_pm_attach(struct kprobe *p, struct pt_regs *regs)
{
	/* dev_pm_domain_attach(struct device *dev, bool power_on) */
	vxtr_emit(VXTR_E_PM_ATTACH, 0, regs->regs[1], 0, vxtr_lr(regs), 0);
	return 0;
}

static int vxtr_pre_regmap_write(struct kprobe *p, struct pt_regs *regs)
{
	/* regmap_write(map, reg, val) */
	vxtr_emit(VXTR_E_REGMAP_WRITE, 0, regs->regs[1], regs->regs[2],
		  vxtr_lr(regs), 0);
	return 0;
}

static int vxtr_pre_regmap_read(struct kprobe *p, struct pt_regs *regs)
{
	/* regmap_read(map, reg, *val): simpan pointer val utk post-handler */
	vxtr_emit(VXTR_E_REGMAP_READ, 0, regs->regs[1],
		  (u64)regs->regs[2], vxtr_lr(regs), 0);
	return 0;
}

static int vxtr_post_regmap_read(struct kprobe *p, struct pt_regs *regs,
				 struct kprobe *pp)
{
	/*
	 * Tidak ada state antar pre/post selain dari record yang baru saja
	 * ditulis: baca ulang record terakhir (head-1) dan isi res + nilai.
	 * Ini hanya benar bila tidak ada emit lain di antara pre dan post
	 * (regmap_read tidak memanggil probe point lain di dalamnya).
	 */
	struct vxtr_rec *r;
	u32 idx;
	u32 *valp = (u32 *)0;

	/* pulihkan pointer val dari record terakhir (seq = count) */
	idx = ((u32)atomic_read(&vxtr_hdr->count) - 1) % VXTR_MAX_REC;
	r = &vxtr_recs[idx];
	if (r->event == VXTR_E_REGMAP_READ)
		valp = (u32 *)(uintptr_t)r->arg1;
	r->res = (s64)regs->regs[0];
	if (valp)
		r->arg1 = *valp;
	dcache_clean_poc((unsigned long)r,
			 (unsigned long)r + VXTR_REC_SIZE);
	return 0;
}

static int vxtr_pre_regmap_update(struct kprobe *p, struct pt_regs *regs)
{
	/* regmap_update_bits_base(map, reg, mask, val, ...) */
	u64 mask = regs->regs[2];
	u64 val = regs->regs[3];

	/* pack mask+val jadi satu arg1: (mask << 32) | val */
	vxtr_emit(VXTR_E_REGMAP_UPDATE, 0, regs->regs[1],
		  (mask << 32) | (val & 0xffffffffULL), vxtr_lr(regs), 0);
	return 0;
}

/* ---- tabel kprobe ----------------------------------------------------- */
static struct kprobe vxtr_kps[] = {
	{ .symbol_name = "clk_get",			.pre_handler = vxtr_pre_clk_get },
	{ .symbol_name = "devm_clk_get",			.pre_handler = vxtr_pre_clk_get },
	{ .symbol_name = "regulator_get",		.pre_handler = vxtr_pre_reg_get },
	{ .symbol_name = "devm_regulator_get",		.pre_handler = vxtr_pre_reg_get },
	{ .symbol_name = "devm_regulator_get_optional",	.pre_handler = vxtr_pre_reg_get },
	{ .symbol_name = "reset_control_get",		.pre_handler = vxtr_pre_rst_get },
	{ .symbol_name = "devm_reset_control_get",	.pre_handler = vxtr_pre_rst_get },
	{ .symbol_name = "devm_reset_control_get_optional", .pre_handler = vxtr_pre_rst_get },
	{ .symbol_name = "devm_pinctrl_get",		.pre_handler = vxtr_pre_pinctrl_get },
	{ .symbol_name = "pinctrl_lookup_state",		.pre_handler = vxtr_pre_pinctrl_state },
	{ .symbol_name = "platform_get_irq",		.pre_handler = vxtr_pre_irq_get },
	{ .symbol_name = "platform_get_irq_optional",	.pre_handler = vxtr_pre_irq_get },
	{ .symbol_name = "platform_get_irq_byname",	.pre_handler = vxtr_pre_irq_get },
	{ .symbol_name = "dma_request_chan",		.pre_handler = vxtr_pre_dma_chan },
	{ .symbol_name = "ioremap",			.pre_handler = vxtr_pre_ioremap },
	{ .symbol_name = "platform_driver_register",	.pre_handler = vxtr_pre_drv_reg },
	{ .symbol_name = "driver_probe_device",		.pre_handler = vxtr_pre_probe,
							  .post_handler = vxtr_post_probe },
	{ .symbol_name = "dev_pm_domain_attach",		.pre_handler = vxtr_pre_pm_attach },
	{ .symbol_name = "regmap_write",			.pre_handler = vxtr_pre_regmap_write },
	{ .symbol_name = "regmap_read",			.pre_handler = vxtr_pre_regmap_read,
							  .post_handler = vxtr_post_regmap_read },
	{ .symbol_name = "regmap_update_bits_base",	.pre_handler = vxtr_pre_regmap_update },
};

/* ---- /proc/vxtuxtrace ------------------------------------------------- */
static int vxtr_proc_show(struct seq_file *m, void *v)
{
	u32 i, n, count;

	count = vxtr_hdr ? (u32)atomic_read(&vxtr_hdr->count) : 0;
	seq_printf(m, "phys=%pa count=%u overflow=%u "
		      "rec_size=%u max_rec=%u\n",
		   &vxtr_phys, count,
		   vxtr_hdr ? vxtr_hdr->overflow : 0,
		   VXTR_REC_SIZE, VXTR_MAX_REC);
	if (!vxtr_ring_ok || !vxtr_hdr)
		return 0;

	n = min(count, (u32)VXTR_MAX_REC);
	/* tampilkan dari yang tertua: seq count-n+1 .. count */
	for (i = 0; i < n; i++) {
		u32 s = count - n + 1 + i;	/* seq record ini */
		struct vxtr_rec *r = &vxtr_recs[(s - 1) % VXTR_MAX_REC];
		char sym[KSYM_SYMBOL_LEN];

		if (r->magic != VXTR_MAGIC)
			continue;
		sprint_symbol(sym, r->caller);
		seq_printf(m, "%llu ev=%u a0=%#llx a1=%#llx res=%lld %s\n",
			   (unsigned long long)r->t_order, r->event,
			   (unsigned long long)r->arg0,
			   (unsigned long long)r->arg1,
			   (long long)r->res, sym);
	}
	return 0;
}

/* ---- boot param + init ------------------------------------------------ */
static int __init vxtr_setup(char *str)
{
	unsigned long long addr = VXTR_DEFAULT_PHYS;

	if (str && *str) {
		char *tok = strsep(&str, ",");

		if (!tok || kstrtoull(tok, 0, &addr) || !addr) {
			pr_err("invalid address, using %#llx\n",
			       VXTR_DEFAULT_PHYS);
			addr = VXTR_DEFAULT_PHYS;
		}
	}

	if (memblock_reserve(addr, VXTR_RAM_BYTES)) {
		pr_err("cannot reserve %#llx (%u MiB) - RAM ring disabled; "
		       "hanya capture non-crash via /proc\n",
		       addr, VXTR_RAM_BYTES >> 20);
		vxtr_phys = 0;
		return 0;
	}
	vxtr_phys = addr;
	pr_info("reserved trace ring %#llx..%#llx (%u MiB, %u records)\n",
		addr, addr + VXTR_RAM_BYTES - 1, VXTR_RAM_BYTES >> 20,
		VXTR_MAX_REC);
	return 0;
}
early_param("vxtuxtrace", vxtr_setup);

static int __init vxtr_init(void)
{
	int i, rc;

	if (!IS_ENABLED(CONFIG_KPROBES) || !IS_ENABLED(CONFIG_KALLSYMS))
		return -ENODEV;

	if (vxtr_phys) {
		vxtr_ring = __va(vxtr_phys);
		vxtr_hdr = (struct vxtr_hdr *)vxtr_ring;
		vxtr_recs = (struct vxtr_rec *)((char *)vxtr_ring +
						VXTR_HDR_SIZE);

		/* resume lintas boot: header valid di RAM sisa boot lalu. */
		if (vxtr_hdr->magic == VXTR_MAGIC &&
		    vxtr_hdr->version == VXTR_VERSION) {
			pr_info("resumed: count=%u overflow=%u "
				"(dari boot sebelumnya)\n",
				atomic_read(&vxtr_hdr->count),
				vxtr_hdr->overflow);
		} else {
			memset(vxtr_hdr, 0, sizeof(*vxtr_hdr));
			vxtr_hdr->magic = VXTR_MAGIC;
			vxtr_hdr->version = VXTR_VERSION;
			atomic_set(&vxtr_hdr->count, 0);
			dcache_clean_poc((unsigned long)vxtr_hdr,
					 (unsigned long)vxtr_hdr +
					 sizeof(*vxtr_hdr));
		}
		vxtr_ring_ok = true;
	} else {
		pr_warn("RAM ring disabled (add vxtuxtrace=<phys> to cmdline)\n");
	}

	/* Daftar kprobe; yang gagal dilewati (non-fatal). */
	for (i = 0; i < ARRAY_SIZE(vxtr_kps); i++) {
		rc = register_kprobe(&vxtr_kps[i]);
		if (rc)
			pr_warn("kprobe %s: %d (skip)\n",
				vxtr_kps[i].symbol_name, rc);
	}

	proc_create_single("vxtuxtrace", 0400, NULL, vxtr_proc_show);
	pr_info("active: %d kprobes, ring=%pa, mode=%s\n",
		ARRAY_SIZE(vxtr_kps), &vxtr_phys,
		vxtr_ring_ok ? "RAM (tahan reset)" : "proc-only");
	return 0;
}
/* core_initcall: sebelum driver perangkat probe (device_initcall+). */
core_initcall(vxtr_init);
