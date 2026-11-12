// SPDX-License-Identifier: GPL-2.0
// /vxtux/init -- module load-test init for QEMU/lkvm VXTux.
//
// Purpose: prove "N modul driver berhasil dimuat" on a kernel boot.
// Usage:
//   - initramfs berisi /init (binari ini), /modules/<relpath>.ko (hasil
//     ekstraksi built_kos.tar.gz), dan /module_load_manifest.txt.
//   - init memuat modul sesuai urutan manifest, dengan beberapa pass retry
//     (untuk dependensi), lalu menulis ringkasan ke serial (/dev/hvc0 atau
//     /dev/console) dan /dev/kmsg.
//
// Exit: RB_POWER_OFF setelah selesai (untuk QEMU exit-code gate).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifndef SYS_init_module
#define SYS_init_module 175
#endif
#ifndef SYS_finit_module
#define SYS_finit_module 273
#endif

static int tfd = -1;

static void ser(const char *s) {
    if (tfd >= 0) { write(tfd, s, strlen(s)); }
    int k = open("/dev/kmsg", O_WRONLY | O_NOCTTY);
    if (k >= 0) { write(k, s, strlen(s)); close(k); }
}
static void serf(const char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap); va_end(ap); ser(b);
}

static int load_one(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    long rc = syscall(SYS_finit_module, fd, "", 0);
    int e = (int)rc;
    if (rc < 0) e = -errno;
    close(fd);
    return e;
}

static int is_already(const char *name) {
    /* crude: check /sys/module/<name> exists */
    char p[256];
    snprintf(p, sizeof p, "/sys/module/%s", name);
    struct stat st;
    return stat(p, &st) == 0;
}

/* Modul yang tidak bisa divalidasi di QEMU virt (tidak ada hardware/secure
 * world-nya). finit_module-nya akan mengeksekusi smc atau menyentuh perangkat
 * yang tak ada, menggantung/menabrak CPU. Sengaja dilewati (SKIP), bukan
 * dihitung gagal.
 *
 * TICK13: modul EL3/SiP (sprd_sip_svc, sprd-cpufreq-v2, sprd_debugstat,
 * sprd_pdbg) dan trusty-pm TIDAK lagi di-skip: QEMU kini boot lewat TF-A BL31
 * (EL3 nyata) yang membalas SMC_UNK, sehingga init SMC tidak menggantung dan
 * semua modul itu load. WCN core/integ/platform/sipc/wlan kini built-in (=y)
 * jadi tidak lagi muncul di daftar modul.
 *
 * TICK14: sprdbt_tty & sprd_fm TIDAK lagi di-skip. Keduanya platform_driver
 * (of_match "sprd,mtty" / "sprd,marlin3_fm") yang tak match DT QEMU virt,
 * sehingga probe tidak jalan dan init hanya mendaftarkan driver -- aman
 * walau tanpa chip WCN nyata. */
static const char *qemu_unsupported(const char *base) {
    /* butuh secure world Trusty (TIPC endpoint) -- tak ada di QEMU */
    static const char *trusty[] = {
        "ion_ipc_trusty",
    };
    /* butuh modem/CP via SIPC (jadwal kerja async menabrak tanpa CP) */
    static const char *modem[] = {
        "ims_bridge", "ims_bridge_t",
    };
    for (unsigned i = 0; i < sizeof trusty / sizeof trusty[0]; i++)
        if (strcmp(base, trusty[i]) == 0) return "butuh secure world Trusty";
    for (unsigned i = 0; i < sizeof modem / sizeof modem[0]; i++)
        if (strcmp(base, modem[i]) == 0) return "butuh modem/CP via SIPC";
    return NULL;
}

int main(void) {
    tfd = open("/dev/hvc0", O_WRONLY | O_NOCTTY);
    if (tfd < 0) tfd = open("/dev/console", O_WRONLY | O_NOCTTY);

    ser("=== VXTUX-MODTEST-INIT ===\n");
    struct utsname u; uname(&u);
    serf("kernel: %s %s\n", u.release, u.version);

    mkdir("/proc", 0755); mkdir("/sys", 0755); mkdir("/dev", 0755);
    mount("proc", "/proc", "proc", 0, 0);
    mount("sysfs", "/sys", "sysfs", 0, 0);
    mount("devtmpfs", "/dev", "devtmpfs", 0, 0);

    FILE *mf = fopen("/module_load_manifest.txt", "r");
    if (!mf) {
        ser("FATAL: no /module_load_manifest.txt\n");
        sync(); reboot(RB_POWER_OFF); return 1;
    }
    char lines[512][300];
    int n = 0;
    while (n < 512 && fgets(lines[n], 300, mf)) {
        size_t l = strlen(lines[n]);
        while (l && (lines[n][l-1] == '\n' || lines[n][l-1] == '\r')) lines[n][--l] = 0;
        if (l) n++;
    }
    fclose(mf);

    int loaded = 0, failed = 0, skipped = 0;
    int total = n;
    int passes = 12;
    int *state = calloc(total, sizeof(int)); /* 0=pending,1=ok,2=skip */
    int *lasterr = calloc(total, sizeof(int));
    int progress = 1;

    for (int pass = 0; pass < passes && progress; pass++) {
        progress = 0;
        for (int i = 0; i < total; i++) {
            if (state[i]) continue;
            /* already loaded (built-in or earlier module)? */
            char name[300]; strcpy(name, lines[i]);
            char *base = strrchr(name, '/');
            char *dot = strrchr(base ? base : name, '.');
            if (dot) *dot = 0;
            char *b2 = strrchr(name, '/'); b2 = b2 ? b2 + 1 : name;
            const char *why = qemu_unsupported(b2);
            if (why) {
                state[i] = 2; skipped++; progress++;
                serf("SKIP (%s) %s\n", why, lines[i]);
                continue;
            }
            if (is_already(b2)) { state[i] = 1; loaded++; progress++; continue; }

            char path[384];
            snprintf(path, sizeof path, "/modules/%s", lines[i]);
            int e = load_one(path);
            if (e == 0) {
                state[i] = 1; loaded++; progress++;
                serf("LOAD OK   %s\n", lines[i]);
            } else {
                lasterr[i] = e;
                if (pass == passes - 1)
                    serf("LOAD FAIL %s (errno=%d)\n", lines[i], e);
            }
        }
        if (!progress) break; /* tidak ada lagi yang bisa dimuat */
    }

    failed = 0; skipped = 0;
    for (int i = 0; i < total; i++) {
        if (state[i] == 1) continue;
        if (state[i] == 2) { skipped++; continue; }
        failed++;
    }
    if (failed) {
        ser("=== FAILED LIST ===\n");
        for (int i = 0; i < total; i++)
            if (state[i] == 0)
                serf("FAILED %s (errno=%d)\n", lines[i], lasterr[i]);
    }
    serf("=== SUMMARY: total=%d loaded=%d skipped=%d failed=%d ===\n",
         total, loaded, skipped, failed);
    if (failed == 0)
        ser("=== VXTUX-MODTEST-PASS ===\n");
    else
        ser("=== VXTUX-MODTEST-FAIL ===\n");
    ser("=== VXTUX-MODTEST-DONE ===\n");
    sync();
    reboot(RB_POWER_OFF);
    return failed == 0 ? 0 : 1;
}
