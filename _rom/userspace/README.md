# unisoc userspace — port software ROM ke aplikasi userspace Linux

Bongkaran (extraction) software dari partisi `super` device ADVAN Tab VX Lite
(Unisoc SC9863A / UMS512, Android 13), lalu implementasi ulang sebagai
program userspace Linux generik ber-nama **`unisoc-*`** (penamaan mengikuti
konvensi vendor Unisoc/Spreadtrum di Android, bukan nama Android-proprietary).

## Isi

| binary | menggantikan (ROM Android) | fungsi |
|---|---|---|
| `usr/sbin/unisoc-tee-ipc` | `/vendor/bin/tee_rpc` + `/vendor/lib64/libtrusty.so` | klien Trusty TIPC: buka `/dev/trusty-ipc-dev0`, `ioctl(TIPC_IOC_CONNECT, service)`, kirim/baca pesan |
| `usr/sbin/unisoc-ims-bridge` | `/system_ext/bin/ims_bridged` + `/system_ext/lib64/libimsbrd.so` | kontrol IMS bridge via generic netlink family `imsbr` |

## ABI (diverifikasi dari dua sisi)

### Trusty TIPC
Sumber kernel: `drivers/trusty/trusty-ipc.c`
- `TIPC_IOC_MAGIC = 'r'`
- `TIPC_IOC_CONNECT = _IOW('r', 0x80, char *) = 0x40087280`
- device node: `device_create(tipc_class, ..., "trusty-ipc-%s", name)`
  → `/dev/trusty-ipc-dev0`
- service name contoh: `com.android.trusty.ion`

Konfirmasi dari binary ROM (`libtrusty.so::tipc_connect`, r2):
`open(path, O_RDWR)` lalu `ioctl(fd, 0x40087280, service)` — **sama persis**.

### IMS bridge
Sumber kernel: `net/ims_bridge/imsbr_netlink.c`, uapi
`include/uapi/linux/ims_bridge/ims_bridge.h`
- family name `imsbr`, version `1`
- commands `IMSBR_C_CALL_STATE..IMSBR_C_DEL_SPI` (1..9)
- attributes `IMSBR_A_CALL_STATE..IMSBR_A_ESP_SPI` (1..8)
- modul params: `/sys/module/ims_bridge/parameters/{tcpdump_enable,vowifi_in_mark}`

Konfirmasi dari binary ROM (`ims_bridged` strings): `genl_connect`,
`genl_ctrl_resolve`, tag `imsbr`, nama cmd `addtuple/deltuple/resettuple/
addspi/delspi/senddstmac`, serta path param di atas — **sama persis**.

## Build

```sh
# host (Windows): arm-gnu-toolchain 13.2 Rel1 aarch64-none-linux-gnu
export CROSS=/e/opt/arm-gnu-toolchain/bin/aarch64-none-linux-gnu-
make
# atau manual:
$CROSS gcc -O2 -Wall -static -o usr/sbin/unisoc-tee-ipc    src/unisoc-tee-ipc.c
$CROSS gcc -O2 -Wall -static -o usr/sbin/unisoc-ims-bridge src/unisoc-ims-bridge.c
```

Static-linked (`-static`): jalan tanpa libc dynamic di rootfs minimal /
LKVM guest, hanya butuh kernel + `CONFIG_IMS_BRIDGE` / `CONFIG_TRUSTY`.

## Bukti uji (device asli, kernel stock 5.4.210)

```
$ unisoc-ims-bridge params
/sys/module/ims_bridge/parameters/tcpdump_enable : 1
/sys/module/ims_bridge/parameters/vowifi_in_mark : 1073741824

$ unisoc-ims-bridge list
imsbr generic netlink family: imsbr (v1), id=27

$ unisoc-ims-bridge spi-add 0 0x1234     # via CAP_NET_ADMIN (su)
OK: command 8 accepted
$ dmesg | grep spi
[81224.899038] sprd-imsbr: add esp spi 1234
[81225.009471] sprd-imsbr: del esp spi 1234

$ unisoc-tee-ipc probe
probe: OK - connected to service "com.android.trusty.ion" on /dev/trusty-ipc-dev0
$ unisoc-tee-ipc send com.android.trusty.ion 00000000
sent 4 bytes
recv 18 bytes: 69 6f 6e 20 63 6f 6e 6e 65 63 74 20 64 6f 6e 65 21 00
           = "ion connect done!"
```

## Catatan penting (temuan)
- `CTRL_CMD_GETFAMILY` **wajib** membungkus nama family dalam atribut
  `CTRL_ATTR_FAMILY_NAME`; mengirim string mentah ditolak kernel dengan
  `-EINVAL` (-22). Bug ini ditemukan saat uji pertama.
- `GENL_ADMIN_PERM` → butuh `CAP_NET_ADMIN`; tanpa root semua perintah
  IMS bridge dikembalikan `EPERM`.
