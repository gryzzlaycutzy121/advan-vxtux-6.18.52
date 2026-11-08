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

    int loaded = 0, failed = 0, tainted = 0;
    int total = n;
    int passes = 3;
    int *state = calloc(total, sizeof(int)); /* 0=try,1=ok,2=fail */

    for (int pass = 0; pass < passes; pass++) {
        for (int i = 0; i < total; i++) {
            if (state[i] == 1) continue;
            /* already loaded (built-in or earlier module)? */
            char name[300]; strcpy(name, lines[i]);
            char *base = strrchr(name, '/');
            char *dot = strrchr(base ? base : name, '.');
            if (dot) *dot = 0;
            char *b2 = strrchr(name, '/'); b2 = b2 ? b2 + 1 : name;
            if (is_already(b2)) { state[i] = 1; loaded++; continue; }

            char path[384];
            snprintf(path, sizeof path, "/modules/%s", lines[i]);
            int e = load_one(path);
            if (e == 0) {
                state[i] = 1; loaded++;
                serf("LOAD OK   %s\n", lines[i]);
            } else {
                state[i] = 2; failed++;
                serf("LOAD FAIL %s (errno=%d)\n", lines[i], e);
            }
        }
        if (failed == 0) break;
        /* reset failed to retry next pass */
        failed = 0;
        for (int i = 0; i < total; i++) if (state[i] == 2) state[i] = 0;
    }

    serf("=== SUMMARY: total=%d loaded=%d failed=%d ===\n", total, loaded, failed);
    if (failed == 0)
        ser("=== VXTUX-MODTEST-PASS ===\n");
    else
        ser("=== VXTUX-MODTEST-FAIL ===\n");
    ser("=== VXTUX-MODTEST-DONE ===\n");
    sync();
    reboot(RB_POWER_OFF);
    return failed == 0 ? 0 : 1;
}
