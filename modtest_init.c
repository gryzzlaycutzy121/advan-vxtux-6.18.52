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
// TICK15 (2026-11-25): modul yang sebelumnya di-SKIP by-nama (ion_ipc_trusty,
// ims_bridge, ims_bridge_t) sekarang DICOBA dimuat -- tidak lagi dilewati
// buta. Analisis source port (docs/PORT_RUNTIME_VALIDATION.md):
//   - ion_ipc_trusty -> ion_tipc_init() -> tipc_create_channel(NULL,...) ->
//     default_vdev == NULL (belum ada device Trusty virtio) -> ERR_PTR(-ENOENT)
//     => load gagal DETERMINISTIK errno -2, tidak hang.
//   - ims_bridge      -> imsbr_init() -> imsbr_sipc_init() hanya schedule_work
//     (async; retry 120x msleep lalu menyerah) => init return 0, aman.
//   - ims_bridge_t    -> imsbr_transit_init() -> imsbr_sipc_create() (sblock).
// Kandidat ENV dicoba DI AKHIR (Phase B) supaya kalau ternyata ada yang hang,
// bukti Phase A (229 LOAD OK) tetap tercetak di serial log.
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

/* Kandidat "environment-only": module whose probe/IO path needs hardware that
 * is absent in QEMU virt / LKVM. Dulu dilewati tanpa dicoba (SKIP by-nama).
 * TICK15: sekarang tetap DICoba; hanya bila errno menunjukkan ketiadaan
 * device (ENOENT/ENODEV/ENXIO) dan file .ko-nya memang ada, hasilnya
 * dicatat sebagai ENV-SKIP (attempted) -- bukan gap porting.
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
static const char *env_candidate(const char *base) {
    /* butuh secure world Trusty (TIPC endpoint) -- tak ada di QEMU */
    static const char *trusty[] = {
        "ion_ipc_trusty",
    };
    /* butuh modem/CP via SIPC (jadwal kerja async; probe CP bisa error) */
    static const char *modem[] = {
        "ims_bridge", "ims_bridge_t",
    };
    for (unsigned i = 0; i < sizeof trusty / sizeof trusty[0]; i++)
        if (strcmp(base, trusty[i]) == 0) return "butuh secure world Trusty";
    for (unsigned i = 0; i < sizeof modem / sizeof modem[0]; i++)
        if (strcmp(base, modem[i]) == 0) return "butuh modem/CP via SIPC";
    return NULL;
}

/* errno yang menandakan "device/environment tidak ada", BUKAN defect modul */
static int is_env_errno(int e) {
    return e == -ENOENT || e == -ENODEV || e == -ENXIO;
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
    /* 0=pending, 1=loaded, 2=env-skip(attempted), 3=hard-fail */
    int *state = calloc(total, sizeof(int));
    int *lasterr = calloc(total, sizeof(int));
    const char **envwhy = calloc(total, sizeof(char *));
    int *isenv = calloc(total, sizeof(int));
    int progress = 1;

    /* pisahkan kandidat ENV (dicoba di Phase B) dari modul biasa */
    for (int i = 0; i < total; i++) {
        char name[300]; strcpy(name, lines[i]);
        char *base = strrchr(name, '/');
        char *dot = strrchr(base ? base : name, '.');
        if (dot) *dot = 0;
        char *b2 = strrchr(name, '/'); b2 = b2 ? b2 + 1 : name;
        const char *why = env_candidate(b2);
        if (why) { isenv[i] = 1; envwhy[i] = why; }
    }

    /* ---------------- Phase A: modul biasa (retry multi-pass) -------------- */
    for (int pass = 0; pass < passes && progress; pass++) {
        progress = 0;
        for (int i = 0; i < total; i++) {
            if (state[i]) continue;
            if (isenv[i]) continue;          /* defer ke Phase B */
            /* already loaded (built-in or earlier module)? */
            char name[300]; strcpy(name, lines[i]);
            char *base = strrchr(name, '/');
            char *dot = strrchr(base ? base : name, '.');
            if (dot) *dot = 0;
            char *b2 = strrchr(name, '/'); b2 = b2 ? b2 + 1 : name;
            if (is_already(b2)) { state[i] = 1; loaded++; progress++; continue; }

            char path[384];
            snprintf(path, sizeof path, "/modules/%s", lines[i]);

            /* VXTux test harness: before the stub Trusty virtio device is
             * registered, force trusty-ipc.ko to auto-online so that
             * ion_ipc_trusty.ko (tried in Phase B) can bind to it. The stub
             * device registration triggers tipc_virtio_probe(), which is when
             * the parameter must already be 1. */
            if (strstr(lines[i], "trusty_vdev_stub")) {
                if (is_already("trusty_ipc")) {
                    int pfd = open("/sys/module/trusty_ipc/parameters/test_autoonline",
                                   O_WRONLY);
                    if (pfd >= 0) {
                        if (write(pfd, "1\n", 2) < 0)
                            serf("WARN: write test_autoonline errno=%d\n", errno);
                        close(pfd);
                        ser("set trusty_ipc test_autoonline=1 (stub harness)\n");
                    } else {
                        serf("WARN: no test_autoonline param (errno=%d)\n", errno);
                    }
                } else {
                    /* trusty-ipc not loaded yet: defer to the next pass. */
                    progress++;
                    continue;
                }
            }

            int e = load_one(path);
            if (e == 0) {
                state[i] = 1; loaded++; progress++;
                serf("LOAD OK   %s\n", lines[i]);
            } else {
                if (lasterr[i] == 0)   /* errno pertama kali gagal -- TICK16 */
                    serf("LOAD FAIL-1st %s (errno=%d)\n", lines[i], e);
                lasterr[i] = e;
                if (pass == passes - 1) {
                    state[i] = 3;
                    serf("LOAD FAIL %s (errno=%d)\n", lines[i], e);
                }
            }
        }
        if (!progress) break; /* tidak ada lagi yang bisa dimuat */
    }
    /* TICK16: modul yang tersisa di state 0 (selalu gagal, loop break lebih
     * awal dari pass terakhir) sebelumnya HILANG senyap dari FAILED LIST.
     * Sekarang dipromosikan ke hard-fail agar tidak ada bukti yang hilang. */
    for (int i = 0; i < total; i++) {
        if (state[i] == 0 && lasterr[i] != 0) {
            state[i] = 3;
            serf("LOAD FAIL(stuck) %s (errno=%d)\n", lines[i], lasterr[i]);
        }
    }
    serf("=== PHASE-A SUMMARY: total=%d loaded=%d (env candidates ditunda) ===\n",
         total, loaded);

    /* ---------------- Phase B: kandidat ENV, dicoba satu kali --------------- */
    for (int i = 0; i < total; i++) {
        if (!isenv[i]) continue;
        char name[300]; strcpy(name, lines[i]);
        char *base = strrchr(name, '/');
        char *dot = strrchr(base ? base : name, '.');
        if (dot) *dot = 0;
        char *b2 = strrchr(name, '/'); b2 = b2 ? b2 + 1 : name;

        serf("ENV-CANDIDATE try %s (env=%s)\n", lines[i], envwhy[i]);
        if (is_already(b2)) {
            state[i] = 1; loaded++;
            serf("LOAD OK   %s (sudah termuat/builtin)\n", lines[i]);
            continue;
        }
        char path[384];
        snprintf(path, sizeof path, "/modules/%s", lines[i]);
        if (access(path, F_OK) != 0) {
            state[i] = 3; lasterr[i] = -errno;
            serf("LOAD FAIL %s (file hilang, errno=%d)\n", lines[i], -errno);
            continue;
        }
        int e = load_one(path);
        if (e == 0) {
            state[i] = 1; loaded++;
            serf("LOAD OK   %s\n", lines[i]);
        } else {
            lasterr[i] = e;
            if (is_env_errno(e)) {
                state[i] = 2; skipped++;
                serf("ENV-SKIP (attempted, errno=%d: %s) %s\n", e, envwhy[i], lines[i]);
            } else {
                state[i] = 3;
                serf("LOAD FAIL %s (errno=%d)\n", lines[i], e);
            }
        }
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
            if (state[i] == 3)
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
