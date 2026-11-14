# Signing keys

| Key | Embedded in | Verifies | Built-in key |
|---|---|---|---|
| OTA key | `mm_miner` (`apps/miner/miner_mgmt/ota/aup_verify.c`) | `.aup` OTA packages | public development key |
| API key | `core_miner` (`apps/miner/app/core_sigverify.c`) | wallpapers from the Avalon app | public key |

Both are PEM public RSA-1024 keys. A CMake build of `apps/miner` alone takes them as
`-DAUP_VERIFY_PUBKEY_PEM=<path>` and `-DPIC_VERIFY_PUBKEY_PEM=<path>`.

## OTA key

A device accepts only `.aup` packages signed with the private half of the key its `mm_miner` embeds;
signature format: [CROSS-LAYER-RULES.md](../CROSS-LAYER-RULES.md#miner-aup-ota). The development key
`apps/miner/tools/aup-dev-signing-key.pem`: its private key is published in this repository, so
firmware that embeds it accepts packages signed by anyone. An image build embeds another key given
as `-DAUP_VERIFY_PUBKEY_PEM=<path>` in the miner recipe's `EXTRA_OECMAKE`, and signs `.aup` packages
with the program named by the `AUP_SIGN_CMD` environment variable, for the public key named by
`AUP_VERIFY_KEY`; the product's `scripts/gen-spinand.sh` reads both and passes them to
`apps/miner/tools/gen_aup.py` as `--sign-cmd` and `--verify-key`.

## API key

The Avalon app signs wallpapers with the API private key. Firmware that embeds the
built-in API key accepts them; with another key the app cannot set the wallpaper. Signatures are
RSA-1024 PSS with MGF1-SHA256 and any salt length.
