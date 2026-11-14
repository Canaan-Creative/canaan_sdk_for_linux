# Changelog

## 1.1.2 — 2026-11-14 (alpha)

First public preview (alpha) of Canaan SDK (canaan_sdk_for_linux).

### Added
- `./build.sh` builds the Avalon Nano3s image with Yocto in a container
  ([docs/BUILD.md](docs/BUILD.md)).
- The board boots SPL → U-Boot 2025.07 → OpenSBI 1.7 → Linux 6.6.36.
- The miner runs as four processes with an HTTPS web UI and OTA upload
  ([docs/miner/MINER.md](docs/miner/MINER.md)).
- `test/scripts/host_tests.sh` runs the host-side tests ([docs/TESTING.md](docs/TESTING.md)).
