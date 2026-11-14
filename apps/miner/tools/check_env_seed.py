#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026, Canaan Creative Co., Ltd.
#
# For the licensing terms, see the LICENSE file (MIT License).
#
# NO WARRANTY. The Software is provided "AS IS", without warranty of any kind, express or
# implied, including but not limited to the warranties of merchantability, fitness for a
# particular purpose and non-infringement. In no event shall the authors or copyright
# holders be liable for any claim, damages or other liability arising from, out of or in
# connection with the Software or the use or other dealings in the Software.

"""Verify the U-Boot environment image a miner product seeds into its .kdimg."""

import argparse
import os
import sys
import zlib

# Variables an OTA-capable board gets only from the seeded env image.
REQUIRED_KEYS = (
    "HW",
    "SW",
    "quick_boot",
    "autoload",
    "uboot_partition",
    "linux_partition",
    "rootfs_partition",
    "app_partition",
)

# A/B slot selectors; U-Boot and mm_ota read only their first character.
SLOT_KEYS = tuple(k for k in REQUIRED_KEYS if k.endswith("_partition"))
# Accepted first characters of a slot selector.
SLOT_CHARS = "aAbB"

# Leading CRC32 field in bytes, little-endian; the redundant-copy flag byte
# follows it.
CRC_SIZE = 4
# Expected image size in bytes (mkenvimage -s).
DEFAULT_SIZE = 0x10000


def fail(product, problems, env_image):
    print("ERROR: %s: seeded U-Boot environment is not usable for OTA (%s)"
          % (product, env_image), file=sys.stderr)
    for p in problems:
        print("ERROR: %s: %s" % (product, p), file=sys.stderr)
    print("       These keys reach the board ONLY through this image -- "
          "U-Boot's compiled-in\n"
          "       default environment has none of them (the board headers' "
          "CONFIG_EXTRA_ENV_SETTINGS\n"
          "       block is dead; docs/CROSS-LAYER-RULES.md).  A board "
          "flashed with this image\n"
          "       refuses every OTA with UPGRADE_SW_ERR and is pinned to slot "
          "A until reflashed.", file=sys.stderr)
    sys.exit(1)


def parse_env(raw, product, env_image, size):
    if raw and raw.count(raw[0:1]) == len(raw) and raw[0:1] in (b"\xff", b"\x00"):
        fail(product, ["env image is a blank 0x%02X fill (%d bytes), not an "
                       "environment -- nothing seeds the required keys"
                       % (raw[0], len(raw))], env_image)

    if len(raw) != size:
        fail(product, ["env image is %d bytes, expected %d -- did the "
                       "mkenvimage -s argument change?" % (len(raw), size)],
             env_image)

    stored = int.from_bytes(raw[:CRC_SIZE], "little")
    data = raw[CRC_SIZE + 1:]
    if zlib.crc32(data) & 0xFFFFFFFF != stored:
        if zlib.crc32(raw[CRC_SIZE:]) & 0xFFFFFFFF == stored:
            fail(product, ["env image has no redundant-copy flag byte -- was "
                           "mkenvimage's -r argument dropped?  U-Boot reads "
                           "this partition as redundant and would reject it."],
                 env_image)
        fail(product, ["env image CRC32 does not match its contents -- U-Boot "
                       "would discard the whole environment"], env_image)

    env = {}
    problems = []
    for chunk in data.split(b"\x00"):
        if not chunk:
            break
        entry = chunk.decode("latin-1")
        if "=" not in entry:
            # An environment ends with an empty record; without it the 0xFF
            # padding reads as one entry the size of the image, so quote only
            # a bounded prefix and let the length identify it.
            shown = entry if len(entry) <= 60 else entry[:60] + "..."
            problems.append("env image holds a malformed entry %r "
                            "(%d bytes, no '=')" % (shown, len(entry)))
            continue
        key, value = entry.split("=", 1)
        if key in env:
            problems.append("env image sets %s twice (%r then %r)"
                            % (key, env[key], value))
        env[key] = value
    if problems:
        fail(product, problems, env_image)
    return env


def check_keys(env, product, env_image):
    problems = []
    for key in REQUIRED_KEYS:
        if key not in env:
            problems.append("missing required key %r" % key)
        elif env[key] == "":
            problems.append("required key %r is empty" % key)
        elif "CONFIG_IMG_" in env[key]:
            problems.append("required key %r still holds the unexpanded "
                            "placeholder %r" % (key, env[key]))
    for key in SLOT_KEYS:
        value = env.get(key, "")
        if value and "CONFIG_IMG_" not in value and value[0] not in SLOT_CHARS:
            problems.append("slot key %r is %r; U-Boot and mm_ota read only its "
                            "first character and accept A or B, either case"
                            % (key, value))
    if problems:
        fail(product, problems, env_image)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--product", required=True,
                    help="product name, for the failure message")
    ap.add_argument("--env-image", required=True,
                    help="the mkenvimage output staged into the .kdimg")
    ap.add_argument("--aup-config",
                    help="the product's aup-ota.json; when it does not exist "
                         "the product ships no OTA path and the check is "
                         "skipped")
    ap.add_argument("--size", type=lambda s: int(s, 0), default=DEFAULT_SIZE,
                    help="expected image size, must match mkenvimage -s "
                         "(default 0x%x)" % DEFAULT_SIZE)
    args = ap.parse_args()

    if args.aup_config and not os.path.exists(args.aup_config):
        print("    NOTE: %s ships no OTA path (%s absent) -- U-Boot env seed "
              "not required" % (args.product, args.aup_config))
        return

    try:
        with open(args.env_image, "rb") as fh:
            raw = fh.read()
    except OSError as e:
        fail(args.product, ["cannot read the env image: %s" % e],
             args.env_image)

    env = parse_env(raw, args.product, args.env_image, args.size)
    check_keys(env, args.product, args.env_image)
    print("    env seed: %d variables, all %d OTA keys present (%s)"
          % (len(env), len(REQUIRED_KEYS), os.path.basename(args.env_image)))


if __name__ == "__main__":
    main()
