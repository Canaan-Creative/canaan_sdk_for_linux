# Build

## Build flow

```sh
./build.sh setup                 # first time: fetch submodules and Yocto layers, apply patches
./build.sh <product> [<image>]   # build and pack the product's default image, or <image>
./build.sh                       # list products; --help lists the command-line options
```

Each product is a `product/.../<product>/build.conf`; `nano3s` builds `nano3s-image-full`.
`setup` downloads `xuantie-yocto` v1.3.2 from GitHub (a proxy must allow `codeload.github.com`
too) and applies `build/yocto/patches/`. The first run builds the container image, which downloads
the XuanTie toolchains (about 3 GB) from `yocbook.oss-cn-hangzhou.aliyuncs.com`. The
`xuantie-yocto` archive, the toolchains, ShellCheck, Node.js and the host tests' LVGL archive are
checked against a pinned sha256.

## Docker

`./build.sh setup` and `./build.sh <product>` run only in the pinned Ubuntu 22.04 container.
When Docker is missing or your user is not in the `docker` group, both first run
`docker/install-docker.sh` on Ubuntu, which uses `sudo`, and continue without a new login; on
other distributions they stop and ask you to install Docker. Running `./build.sh setup` again is
harmless. `./docker/build.sh --run <command>` runs any command in the container, as your user,
with the repository mounted. A build from scratch needs about 50 GB of free disk.

## Image contents

The image has root login `root`/`root` (`local.conf`), runs an SSH server and a serial getty,
carries the `canaan-debug` diagnostic tools (`packagegroup-canaan-debug`) and lets U-Boot autoboot
be stopped. For example, `/etc/issue` shows `1.1.2` and the `:4028` reply carries `FW[Debug]`.
`/etc/release_version` holds the rootfs build identity.

## Output images

`build/images/` receives the flash image, its `.sha256` and the OTA package; flashing and upgrading
are in the [README](../README.md#flash).

```
<Line>_<model>_<hw>_<sw>_s<SDKVER>_m<MINERVER>_<bld>_<YYYYMMDD>_<HHMM>_<rev>.kdimg.gz
<the same basename>.aup
```

- `<Line>_<model>`: product-line and product folder (`AvalonHome_nano3s`); `<hw>`/`<sw>`:
  `HW_TYPE`/`SW_TYPE` from the product's `boardconf.ini`, lowercased with underscores removed, and
  left out when the product has no `boardconf.ini`.
- `SDKVER`: `VERSION`; `MINERVER`: `apps/miner/VERSION`, else `SDKVER`.
- `<bld>`: `dbg`.
- `<rev>`: this repository's commit; `t` marks a modified work tree.

The nano3s pack script also writes the signed OTA package from `scripts/aup-ota.json`; a build that
cannot produce it fails. The miner binaries carry the build version `<yymmdd><NN>_<hhmm>_<sha>`
(`apps/miner/cmake/MinerVersion.cmake`), where `NN` comes from `./build.sh <product> -v=NN` and
defaults to `01`.

Signing keys: [KEYS.md](miner/KEYS.md).

## Tech stack

| Component | Version | Defined in |
|---|---|---|
| Yocto layers | `xuantie-yocto` v1.3.2 on oe-core langdale | `build.sh` |
| Linux kernel | 6.6.36 | `product/k230/common/recipes-kernel/linux/linux-k230_6.6.36.bb` |
| U-Boot | 2025.07 + K230 patches | `build/yocto/meta-canaan/recipes-bsp/u-boot/u-boot-k230_2025.07.bb`; patches in `product/k230/common/recipes-bsp/u-boot/` |
| OpenSBI | 1.7 | `build/yocto/meta-canaan/conf/distro/canaan-k230.conf`; patches in `product/k230/common/recipes-bsp/opensbi/` |
| GCC / Clang | 14.3.0 / 18.1.8 (XuanTie V3.4.1 / V2.4.2) | `build.sh` (toolchain archives), `build/yocto/xuantie-build/xuantie/conf/local.conf` (Clang version) |
| OpenSSL | 3.5.9 | `build/yocto/meta-canaan/recipes-connectivity/openssl/` |
| OpenSSH | 9.3p2 + CVE-2024-6387 fix | meta-xuantie (version); `build/yocto/meta-canaan/recipes-connectivity/openssh/` (CVE fix) |

The kernel built is the `SRCREV` of the kernel recipe; the `os/linux` submodule is a fetch mirror.
