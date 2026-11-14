# Cross-layer rules

Rules that span layers: a change on one side needs the matching change on every other side.

## SPI-NAND A/B layout (nano3s)

Table and slot selection: [BOOT.md](BOOT.md#spi-nand-ab-partition-layout).

- **One layout:** the files that [declare the layout](BOOT.md#spi-nand-ab-partition-layout) change
  together; `all_flash` is declared in the device tree only. `mm_ota` and `avalon-app-mount.sh` find
  slots by label only.
- **Slot value:** a `*_partition` value starting with `b`/`B` selects B, else A, in U-Boot, `mm_ota`
  and `avalon-app-mount.sh` alike; a mismatch makes an update erase the running system.

## Miner AUP OTA

- **Signature:** 2048-byte header, RSA-1024 PKCS#1 v1.5 over the SHA-256 of its first 1192 bytes,
  checked against the key compiled into `mm_miner`; the 128-byte field fixes the key size.
- **Keys:** a build embeds and signs with `apps/miner/tools/aup-dev-signing-key.pem` by default,
  or with another key pair as described in [KEYS.md](miner/KEYS.md#ota-key).
- **Signing:** the build fails unless `gen_aup.py` writes a `.aup` the packed `mm_miner` verifies.
- **Refused packages:** `SW`/`HW` differing from the env (`UPGRADE_SW_ERR`/`UPGRADE_HW_ERR`); a bin
  without a readable `<name>_partition`, aimed at a UBI-attached slot, or whose `partsize` differs
  from its slots (`UPGRADE_PARTITION_ERR`), so a new geometry rejects every old `.aup`.
- **Verify before flash:** each bin is staged whole in RAM and written only after its SHA-256
  matches; the env flips only after every bin is written. The largest bin must fit in free RAM.
- **IPC:** `/tmp/sockupctrl`, `SOCK_STREAM`, `mm_miner` server, `miner_web` client, 12-byte status,
  magic `0xdeadbeaf` (`apps/miner/common/proto/ota_proto.h`).
- **`/tmp` is tmpfs:** the success latch `/tmp/up_scceed_file` clears only on reboot; the image
  recipe mounts it and `conf_check.sh` checks the recipe.
- **Slot after an update:** U-Boot boots the slot the env selects; a bad but verified slot stays
  until reflash.

## U-Boot environment and image header

- **Seeded by the packager:** `gen-spinand.sh` writes `HW`, `SW` and the four `*_partition` keys
  (`mkenvimage -r -s 0x10000`, checked by `check_env_seed.py`); without them U-Boot boots slot A and
  every OTA is refused.
- **`CONFIG_EXTRA_ENV_SETTINGS` is dead** in `include/configs/k230_nano3s_env.h` (U-Boot reads only
  `CFG_EXTRA_ENV_SETTINGS`); renaming it compiles its values into the U-Boot and SPL default env
  and needs a hardware boot test.
- **Env defaults stay in U-Boot:** an `/etc/u-boot-initial-env` would persist defaults without
  `HW`/`SW`; `conf_check.sh` fails when a recipe ships one.
- **Image header:** every boot image carries the `firmware_gen.py -n` header, added by the U-Boot
  build to SPL and U-Boot and by `gen-spinand.sh` to `linux_system.bin`; SPL and U-Boot refuse an
  image whose header fails the [check](BOOT.md#image-header-check).

## Miner API

- **Read-only `:4028`:** every write, loopback included, answers `E`/45 `Access denied`; writes
  reach `core_miner` only through the trusted socket.
- **Trusted socket:** `MC_API_SOCK_PATH` (`apps/miner/common/proto/api_sock.h`),
  `SOCK_STREAM`, mode 0600, `core_miner` server, `miner_web` client, one request per connection,
  received and answered within 2 s each; `miner_web` authenticates every write before it
  forwards it, except `activation` and `qr-auth`, whose signature or proof `core_miner` checks and
  whose failures `miner_web` limits per address.
- **Pool passwords:** the `pools` reply carries URL, user and status; the pool passwords reach only
  a logged-in dashboard, read by `miner_web` from `mm_miner` over `/tmp/sockcfgwr`
  (`MM_CFGWR_READ_POOLS`).

## Miner factory defaults

- **Location:** `/mntapp/release/linux/confiles/{usrcon,factory}` on the app volume, installed from
  the product's `files/`; `FACTORY_DEFAULT_DIR` (`mc_paths.h`, `…/confiles/usrcon`),
  `FACTORY_CALI_DIR` (`mc_paths.h`, `…/confiles/factory`) and `DEFAULTS` (`avalon-data-mount.sh`,
  `…/confiles`) name it and move with it.
- **Mount order:** the app volume mounts first; missing `/data` files are created from it.
- **Single source:** the default files hold every key the miner reads; an empty calibration value
  (`cali_param0`) selects the built-in default. Start-up fills keys missing or empty on `/data` in
  `systemcfg.ini`, `hashrate_cali.ini` and `activation.ini`, copies a missing `pools.conf`,
  `interfaces` or `wpa_supplicant.conf` whole, and keeps every readable file and set value.
- **Factory reset:** every `factory_default_files` entry (`mm_factory.c`) exists in `files/usrcon`.

## Miner watchdog

- **One owner:** `mm_miner` holds `/dev/watchdog`, the only enabled `snps,dw-wdt` node
  (`wdt0@91106000` in `k230.dtsi`).
- **Reboot:** a watchdog timeout, a kernel panic, an oops or a soft lockup reboots the board.
- **One gate:** every build arms the dog; only the `mm_wdt` thread feeds it, once at start and
  then for 90 s after each tick on which `SUPERVISOR`, `ASIC` and `CORE` have all reported. If the
  thread cannot start, `mm_miner` hands the dog to the kernel. A freshly spawned `core_miner` is
  left out of the gate for 30 s and `asic_miner` for 120 s, for at most three spawns in a row
  without a live report; a V/F control stall over 60 s withholds the `SUPERVISOR` report.
  Outside that grace a silent process counts as dead.
- **`asic_health`:** any bit set withholds the `ASIC` report.
- **Softoff** (thermal, user, fault, cold-start) exempts only the ASIC leg.
