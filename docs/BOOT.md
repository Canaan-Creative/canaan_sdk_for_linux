# Boot flow and flash layout (nano3s)

The rules that depend on this layout are in [CROSS-LAYER-RULES.md](CROSS-LAYER-RULES.md).

## Boot sequence

```
ROM → SPL → U-Boot → OpenSBI → Linux → root file system → miner applications
```

1. The boot ROM loads SPL from the start of SPI-NAND.
2. SPL loads U-Boot from a `uboot` slot.
3. U-Boot loads `linux_system.bin` from the `linux` slot, unpacks `fw_payload.bin` (OpenSBI with
   the Linux `Image`) and the device tree, sets the kernel command line and jumps to OpenSBI.
4. OpenSBI starts Linux, which mounts the `rootfs` slot (ubi0) as the root file system.
5. The `avalon-miner` init script mounts the `app` slot (ubi1) on `/mntapp/release` and `data`
   (ubi2) on `/data`, then starts `mm_miner`, which starts `core_miner` and `asic_miner`; the
   `miner-web` init script starts `miner_web`.

`product/k230/miner/avalon-home/nano3s/scripts/gen-spinand.sh` packs the flash images. Component
versions: [BUILD.md](BUILD.md#tech-stack).

## SPI-NAND A/B partition layout

| mtd | Label | Offset | Size | Content |
|---|---|---|---|---|
| 0/1 | `uboot_spl_a`/`_b` | 0x0000000/0x0080000 | 512K | SPL, written by full reflash only |
| 2/3 | `uboot_a`/`_b` | 0x0100000/0x0300000 | 2M | U-Boot |
| 4/5 | `uboot_env_a`/`_b` | 0x0500000/0x0580000 | 512K | U-Boot environment, redundant pair |
| 6/7 | `rtt_a`/`_b` | 0x0600000/0x0a00000 | 4M | reserved, filled with `0xFF` |
| 8/9 | `linux_a`/`_b` | 0x0e00000/0x1600000 | 8M | `linux_system.bin` |
| 10/11 | `rootfs_a`/`_b` | 0x1e00000/0x4e00000 | 48M | root file system, UBI (ubi0) |
| 12/13 | `app_a`/`_b` | 0x7e00000/0x8c00000 | 14M | miner applications, UBI (ubi1) |
| 14 | `data` | 0xc800000 | 56M | persistent user data, UBI (ubi2) |
| 15 | `all_flash` | 0x0000000 | 256M | first 256M, read-only |

0x9a00000-0xc7fffff (46M), between `app_b` and `data`, is not assigned.

- The layout is declared in the kernel device tree `arch/riscv/boot/dts/canaan/k230-nano3s.dts`,
  `genimage-spinand.cfg` and `aup-ota.json` (both in
  `product/k230/miner/avalon-home/nano3s/scripts/`); only the device tree declares `all_flash`.
- U-Boot takes the slot offsets from the nano3s `board.c` (`linux_b`), the shared
  `board/canaan/common/sdk_autoconf.h` (`uboot`, `linux`, `rtt`) and
  `configs/k230_nano3s_defconfig` (`uboot_env`, `uboot_a`), with `CONFIG_SYS_NAND_U_BOOT_OFFS` in
  `include/configs/k230_nano3s_env.h` as fallback.
- The nano3s `board.c` bootargs name the `rootfs` slots as `ubi.mtd=10` and `ubi.mtd=11`.
- `add_firmware_head` in `arch/riscv/cpu/k230/config.mk` writes SPL and U-Boot into `spinand.img`
  at the `uboot_spl` and `uboot` slot offsets.
- `gen-spinand.sh` fails the build when `linux_system.bin` exceeds its 8M slot, and fills both
  `rtt` slots with `0xFF`.

An `.aup` package updates `uboot`, `linux`, `rootfs` and `app`.

## U-Boot A/B slot selection

| Env key | Selects |
|---|---|
| `uboot_partition` | U-Boot slot SPL loads |
| `linux_partition` | `linux` slot U-Boot loads |
| `rootfs_partition` | `ubi.mtd=10` or `ubi.mtd=11` in the kernel command line |
| `app_partition` | slot `avalon-app-mount.sh` attaches as ubi1 |

A value starting with `b` or `B` selects slot B; any other value, or a missing key, selects slot A.
`gen-spinand.sh` seeds the keys with `A`
([CROSS-LAYER-RULES.md](CROSS-LAYER-RULES.md#u-boot-environment-and-image-header)).

## Image header check

SPL and U-Boot load an image only when its header, written by `firmware_gen.py -n`, has:

- magic `K230`;
- a data length that ends within the load buffer;
- crypto type `NONE_SECURITY`;
- a SHA-256 that matches the data.

With bit 0 of the OTP product-misc word (secure boot) set, they refuse every image. When SPL
refuses one U-Boot slot it loads the other; a refused Linux image stops the boot.
