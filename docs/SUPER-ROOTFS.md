# Booting a Linux rootfs that lives in `super`

Two completely different things can sit in the GPT partition named `super`
(`mmcblk0p48`, 5 600 MiB on this tablet). The right way to boot from it
depends on which one is there, and mounting it as ext4 without checking is
the usual reason an initramfs says "root not found".

```
GPT partition "super"  (mmcblk0p48, 5 600 MiB)
│
├── A. ext4 on the partition itself            <- what this tablet has now
│      offset 1080 (1024 + 0x38): 53 EF
│      offset 1024 + 0x78        : volume label, here "super"
│      -> mount("/dev/mmcblk0p48", "/newroot", "ext4") works
│
└── B. Android "dynamic partitions" container (LP metadata)
       offset 4096: magic 67 44 6c 61  (0x616c4467, geometry)
       offset 8192: magic 30 50 4c 41  (0x414c5030, metadata header)
       no filesystem at offset 0
       -> the filesystem is inside one of several logical partitions,
          each of which must first become a device-mapper device
```

## A. ext4 directly in `super` (this device)

Dump `logs/artix44/super.bin` (first 16 MiB) shows:

```
ext4 magic  0xef53
label       "super"
block size  4096, blocks 1433600  -> 5.47 GiB
```

So the Artix rootfs was written to `super` as a plain filesystem. The
initramfs path is a normal mount:

```c
mount("/dev/mmcblk0p48", "/newroot", "ext4", 0, "");
if (access("/newroot/sbin/init", X_OK) == 0) { chroot("/newroot"); execv(...); }
```

`/vxtux/init` already does this: `find_and_mount_root()` walks `mmcblk0p*`,
matches the GPT name `super`, probes the ext4 magic and mounts it. Since
artix45 it also prints which of the two layouts it found
(`VXTUX-SUPER: ... ext4 langsung` / `... LP magic ...`).

Nothing about device-mapper is needed for this layout. Rooting/flashing is
simply `dd` (or `spd_dump w super`) of an ext4 image.

## B. LP container (Android 10+ stock layout)

Android groups `system`, `vendor`, `product`, `system_ext`, `odm` into one
physical partition and describes the slices in its own metadata format.
The device-mapper `linear` target is what turns one slice into a block
device; Android's `liblp` + `fs_mgr` do that at boot.

### B.1 Metadata layout (LP, little-endian)

| offset | size | content |
|---|---|---|
| 0 | 4096 | reserved (`LP_PARTITION_RESERVED_BYTES`) |
| 4096 | 4096 | `LpMetadataGeometry` (magic `0x616c4467`, struct size, checksum, metadata_max_size, metadata_slot_count, logical_block_size) |
| 8192 | 4096 | primary `LpMetadataHeader` (magic `0x414c5030`, major/minor, header size, tables size, descriptor offsets, SHA-256 of body) |
| 12288 | … | primary tables: partitions, extents, groups, block devices |
| 4096+4096*(1+slot) | … | backup copy of geometry+header+tables |

Header fields that matter for a minimal reader:

* `partitions[i]`: name (36 bytes), attributes, first_extent_index,
  num_extents, group_index.
* `extents[i]`: `num_sectors`, `target_type` (0 = linear),
  `target_data` = absolute sector inside `super`, `target_source` = 0.
* `block_devices[0]`: first_logical_sector (the `super` base offset in
  sectors, normally 0).

Sector size is 512 bytes, so a table for one logical partition is:

```
0 <num_sectors> linear /dev/mmcblk0p48 <first_logical_sector + target_data>
```

and, when a partition has several extents, one line per extent with the
running device offset:

```
0 <n0> linear /dev/mmcblk0p48 <s0>
<n0> <n1> linear /dev/mmcblk0p48 <s1>
```

`dmsetup create --concise "$(parse-android-dynparts /dev/block/mmcblk0p48)"`
(host side, see sources) produces exactly these lines and creates
`/dev/mapper/dynpart-system_a` etc.

### B.2 Three ways to get from initramfs to that rootfs

1. **Device-side dm ioctls (what Android does).**
   In the initramfs open `/dev/mapper/control` and issue, per partition:
   `DM_VERSION`, `DM_DEV_CREATE` (name `system_a`), `DM_TABLE_LOAD`
   (target `linear`, params `"<start_sector>"`), `DM_DEV_SUSPEND`, then
   `mount("/dev/dm-0" or "/dev/mapper/system_a", ...)`.
   Needs `CONFIG_BLK_DEV_DM=y` (already set) — `dm-linear` is part of
   `dm-mod`, no extra Kconfig. It is ~150 lines of raw ioctl plus the
   metadata reader from B.1; no libdevmapper, no liblp.

2. **Kernel command line, no userspace code at all.**
   `drivers/md/dm-init.c` accepts
   `dm-mod.create="system_a,,0,ro,0 <sectors> linear /dev/mmcblk0p48 <start>"`
   and creates the device before init runs. It is only compiled when
   `CONFIG_DM_INIT=y`, which is **not** set in `enhanced_config.cfg` today.
   The table has to be known at build time (fine for one fixed device), and
   one `dm-mod.create` string can hold several `;`-separated devices.

3. **Offline on the PC, then flash flat partitions.**
   `lpunpack super.img out/` extracts `system_a.img`, `vendor_a.img`, …;
   flash them to GPT partitions of their own (or keep them as files on an
   ext4 data partition) and mount those. No dm at runtime, at the cost of
   re-partitioning.

For this tablet layout A applies, so option 1/2 are only needed if `super`
is ever restored to the stock Android image.

## What the initramfs prints now (artix45)

```
VXTUX-SUPER: /dev/mmcblk0p48 berisi ext4 langsung, label "super" -> mount(2) biasa
```
or
```
VXTUX-SUPER: /dev/mmcblk0p48 LP magic 0x616c4467 @4096 -> Android dynamic partitions
VXTUX-SUPER: tidak ada filesystem di awal super; butuh dm-linear dari metadata LP
```

The second pair means "the rootfs is inside a logical partition, not at
offset 0 of super" — that is the moment to implement B.2.1 or enable
`CONFIG_DM_INIT` and B.2.2.

## Sources

* AOSP, *Implement dynamic partitions* — super holds dm-linear metadata:
  <https://source.android.com/docs/core/ota/dynamic_partitions/implement>
* `tchebb/parse-android-dynparts` — parses LP metadata and prints a
  `dmsetup --concise` spec (`dm-linear`, `/dev/mapper/dynpart-*`):
  <https://github.com/tchebb/parse-android-dynparts>
* Kaitai Struct, *Android Super* format description (geometry, header,
  partitions, extents):
  <https://formats.kaitai.io/android_super/>
* senyuuri, *Patching Dynamic Partitions in Android Super Image* —
  `lpunpack`/`lpmake` workflow:
  <https://blog.senyuuri.info/posts/2022-04-27-patching-android-super-images/>
* Kernel documentation, `dm-init` (`dm-mod.create=`):
  `Documentation/admin-guide/device-mapper/dm-init.rst` in this tree,
  implementation `drivers/md/dm-init.c`.
