// SPDX-License-Identifier: GPL-2.0
/*
 * berryfetcher_kernel.ko -- "pintu masuk ruang kernel" milik BFKRT.
 *
 * Tugasnya kecil dan sengaja tidak melakukan apa pun yang berbahaya:
 *   /proc/bfkrt_info   -- release, banner, umur sistem, status resolver simbol
 *   /proc/bfkrt_sym    -- tulis nama simbol, baca alamatnya (kallsyms)
 *
 * Pemecahan masalah klasik Android 5.7+: kallsyms_lookup_name tidak lagi
 * di-export ke modul. Caranya kita pasang kprobe sesaat pada simbol itu,
 * membaca alamatnya dari struct kprobe (p->addr), lalu melepas kprobe lagi.
 * Tidak ada patching permanen, tidak ada yang berubah setelah init selesai.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/utsname.h>
#include <linux/jiffies.h>
#include "bfkrt_ko.h"

static char bf_sym[64];
static bool bf_ok;

/*
 * Resolve alamat simbol TANPA memanggil kallsyms_lookup_name lewat function
 * pointer. Kernel stok dibangun dgn CFI v1 (clang OEM); modul dibangun dgn
 * clang r416183b -> type hash CFI beda -> pemanggilan FP lintas-modul yang
 * di-cast = "CFI failure" -> PANIC. register_kprobe memakai
 * kprobe_lookup_name INTERNAL (tidak lewat FP modul) -> aman.
 */
static unsigned long bf_resolve(const char *name)
{
	struct kprobe kp = { .symbol_name = (char *)name };
	unsigned long addr = 0;

	if (register_kprobe(&kp) == 0) {
		addr = (unsigned long)kp.addr;
		unregister_kprobe(&kp);
	}
	return addr;
}

static int info_show(struct seq_file *m, void *v)
{
	seq_printf(m, "modul   : berryfetcher_kernel\n");
	seq_printf(m, "release : %s\n", init_utsname()->release);
	seq_printf(m, "banner  : %s\n", init_utsname()->version);
	seq_printf(m, "uptime  : %lu detik\n", jiffies / HZ);
	seq_printf(m, "resolver: %s\n", bf_ok ? "siap" : "tidak tersedia");
	seq_printf(m, "cara    : tulis nama simbol ke /proc/bfkrt_sym, lalu baca lagi\n");
	return 0;
}

static ssize_t sym_read(struct file *f, char __user *buf, size_t count, loff_t *ppos)
{
	char kbuf[128];
	int len;

	if (!bf_sym[0]) {
		len = scnprintf(kbuf, sizeof(kbuf),
				"(kosong) tulis nama simbol ke /proc/bfkrt_sym\n");
	} else if (!bf_ok) {
		len = scnprintf(kbuf, sizeof(kbuf),
				"%s = ? (resolver tidak tersedia)\n", bf_sym);
	} else {
		unsigned long a = bf_resolve(bf_sym);
		len = scnprintf(kbuf, sizeof(kbuf), "%s = 0x%lx%s\n",
				bf_sym, a, a ? "" : " (tidak ketemu)");
	}
	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t sym_write(struct file *f, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	char tmp[64];
	size_t n;
	ssize_t r;

	memset(tmp, 0, sizeof(tmp));
	n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
	r = simple_write_to_buffer(tmp, sizeof(tmp), ppos, buf, n);
	if (r < 0)
		return r;
	tmp[sizeof(tmp) - 1] = 0;
	strscpy(bf_sym, tmp, sizeof(bf_sym));
	strim(bf_sym);
	return r;
}

/* Versi 5.4.210: struct proc_ops (6.5+) -> struct file_operations (5.4).
 * Signature read/write identik; hanya nama field berubah. */
static const struct file_operations sym_ops = {
	.read  = sym_read,
	.write = sym_write,
};

static int __init bfkrt_init(void)
{
	struct kprobe kp_self = { .symbol_name = "spi_sync" };
	int ret;

	/* Self-test: resolve satu simbol yang pasti ada. Kalau register_kprobe
	 * + lookup internal berhasil, resolver siap. TIDAK pernah memanggil
	 * kallsyms_lookup_name via function pointer -> CFI failure/panic. */
	ret = register_kprobe(&kp_self);
	if (ret == 0) {
		bf_ok = true;
		unregister_kprobe(&kp_self);
	} else {
		pr_info("berryfetcher: resolver via kprobe gagal (%d)\n", ret);
	}

	if (!proc_create_single(BFKRT_PROC_INFO, 0444, NULL, info_show)) {
		pr_err("berryfetcher: gagal buat /proc/%s\n", BFKRT_PROC_INFO);
		return -ENOMEM;
	}
	if (!proc_create(BFKRT_PROC_SYM, 0644, NULL, &sym_ops)) {
		remove_proc_entry(BFKRT_PROC_INFO, NULL);
		pr_err("berryfetcher: gagal buat /proc/%s\n", BFKRT_PROC_SYM);
		return -ENOMEM;
	}
	pr_info("berryfetcher: siap (resolver %s)\n", bf_kln ? "aktif" : "pasif");
	return 0;
}

static void __exit bfkrt_exit(void)
{
	remove_proc_entry(BFKRT_PROC_SYM, NULL);
	remove_proc_entry(BFKRT_PROC_INFO, NULL);
	pr_info("berryfetcher: dilepas\n");
}

module_init(bfkrt_init);
module_exit(bfkrt_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("BFKRT");
MODULE_DESCRIPTION("Berry Fetching Kernel Runtime -- fetcher ruang kernel");
