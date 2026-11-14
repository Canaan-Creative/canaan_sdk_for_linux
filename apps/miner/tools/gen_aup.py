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

"""Build an OTA .aup upgrade package from a JSON descriptor.

usage:
  gen_aup.py -f packagefile.json [-i INPUT_DIR] [-o OUTPUT_DIR]
             [-k private.pem | --sign-cmd PROGRAM] [--verify-key public.pem]
             --firmware BINARY [-t TYPE] [-r RELEASE] [-v AUP_VER]
             [-m COMMIT]

The 1192 header bytes are signed in one of two ways:

  local       the default: sign on this host with an RSA-1024 private key,
              given by -k, else the development key committed beside this
              script as aup-dev-signing-key.pem -- so a fresh clone signs with
              no key argument, no network and no setup.
              That development key is public by construction: a production
              build signs with --sign-cmd and names the public key the
              firmware embeds with --verify-key, not -k.
  --sign-cmd  run PROGRAM with the header bytes on stdin.  It must write the
              raw 128-byte RSA-1024 PKCS#1 v1.5 SHA-256 signature to stdout and
              exit 0; any other outcome fails the package.  Naming a key as
              well is refused rather than silently resolved.

Whatever signs must hold the private half of the public key the firmware
embeds -- the one in apps/miner/miner_mgmt/ota/aup_verify.c, or the PEM named
by --verify-key for a firmware built with AUP_VERIFY_PUBKEY_PEM -- or the device
rejects the package with UPGRADE_VERIFY_ERR.  That is not left to the operator:
the signature just produced is verified against that key, and a package that
would be rejected on the device is not written.  --firmware names the mm_miner
binary the package is built for; one of the PEM public keys in it must be
that key, compared as a key rather than as PEM text, so a package and a
firmware built for different keys fail here instead of on the device.

Only "upgrade_type": "OTA" is accepted.
See docs/miner/KEYS.md, "OTA key".
"""

import argparse
import base64
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import time

# C source holding the public key the device verifies against.
EMBEDDED_KEY_SRC = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "..", "miner_mgmt", "ota", "aup_verify.c"))
EMBEDDED_KEY_SYM = "aup_public_pem"
# Development private key used when -k is not given.
KEY_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "aup-dev-signing-key.pem")
# Full package header in bytes; payloads start at this offset.
HEADER_SIZE = 2048
# Number of struct aup_bin slots in the header (AUP_MAX_BINS).
BINFILE_NUM_MAX = 14
# RSA-1024 signature length in bytes.
SIG_SIZE = 128
# Payload alignment in bytes; each bin is zero-padded to a multiple of it.
BIN_PAD = 32
# Fixed header, big-endian: format, manufacturer[8], product[16],
# mainctroler[16], time[20], totalbins, totalsize, version[64], hw[16],
# sw[16], upgrade_type[16].
HDR_FMT = ">i8s16s16s20sii64s16s16s16s"
# struct aup_bin, big-endian: name[16], ofst, size, partofst, partsize,
# partofst2, partsize2, sha256[32].
BIN_FMT = ">16siiiiii32s"
# Fixed header length in bytes (184).
HDR_FIXED = struct.calcsize(HDR_FMT)
# struct aup_bin length in bytes (72).
BIN_SIZE = struct.calcsize(BIN_FMT)
# Signed header length in bytes (1192); the signature follows it.
COMMON_HDR_SIZE = HDR_FIXED + BINFILE_NUM_MAX * BIN_SIZE
# Signed header plus signature, in bytes (1320).
UPGRADE_HDR_SIZE = COMMON_HDR_SIZE + SIG_SIZE


class ConfigError(Exception):
    pass


def _fixed(name, value, cap):
    raw = value.encode("utf-8")
    if len(raw) >= cap:
        raise ConfigError(
            "%s = %r is %d bytes; must be at most %d so it stays NUL-terminated"
            % (name, value, len(raw), cap - 1))
    return raw


def _hexint(name, value):
    if isinstance(value, int):
        return value
    try:
        return int(value, 16)
    except (TypeError, ValueError):
        raise ConfigError("%s = %r is not a hex string" % (name, value))


def _pad32(data):
    rem = len(data) % BIN_PAD
    if rem == 0:
        return data
    return data + b"\x00" * (BIN_PAD - rem)


def _version_string(cfg, args, stamp):
    day = time.strftime("%Y%m%d", time.localtime(stamp))
    return "Avalon_%s_%s_%s_%s%s_%s" % (args.type, args.release,
                                        cfg["upgrade_type"], day,
                                        args.aup_ver, args.commit)


def _sign(header, key_path):
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import padding, rsa
    except ImportError:
        raise ConfigError(
            "signing needs the 'cryptography' module; install python3-cryptography")

    if not os.path.exists(key_path):
        raise ConfigError(
            "no signing key at %s; pass -k" % key_path)
    with open(key_path, "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    if not isinstance(key, rsa.RSAPrivateKey):
        raise ConfigError("%s is not an RSA private key" % key_path)
    if key.key_size != SIG_SIZE * 8:
        raise ConfigError(
            "%s is RSA-%d; the device verifies RSA-%d and will reject this "
            "package" % (key_path, key.key_size, SIG_SIZE * 8))

    return key.sign(header, padding.PKCS1v15(), hashes.SHA256())


def _c_unescape(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            i += 1
            c = {"n": "\n", "t": "\t", "r": "\r", "0": "\0"}.get(s[i], s[i])
        out.append(c)
        i += 1
    return "".join(out)


def _embedded_public_pem(path):
    """Return the PEM the device verifier compiles in, exactly as it sees it.

    The literal is a run of concatenated C strings, so the result has no
    newlines inside the base64 -- the same bytes PEM_read_bio_PUBKEY
    is handed on the device, not the 64-column-wrapped PEM a standard encoder emits.
    """
    try:
        with open(path, "r") as fh:
            src = fh.read()
    except OSError as e:
        raise ConfigError("cannot read the embedded public key from %s (%s)"
                          % (path, e))
    m = re.search(EMBEDDED_KEY_SYM + r"\s*\[\s*\]\s*=(.*?);", src, re.S)
    if m is None:
        raise ConfigError("no %s[] declaration in %s" % (EMBEDDED_KEY_SYM, path))
    pem = "".join(_c_unescape(lit)
                  for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))
    if "-----BEGIN PUBLIC KEY-----" not in pem:
        raise ConfigError("%s[] in %s is not a PEM public key"
                          % (EMBEDDED_KEY_SYM, path))
    return pem


def _device_public_pem(path):
    if path is None:
        return _embedded_public_pem(EMBEDDED_KEY_SRC), EMBEDDED_KEY_SRC
    try:
        with open(path, "rb") as fh:
            pem = fh.read().decode("ascii")
    except (OSError, UnicodeDecodeError) as e:
        raise ConfigError("cannot read the verify key %s (%s)" % (path, e))
    if "-----BEGIN PUBLIC KEY-----" not in pem:
        raise ConfigError("%s is not a PEM public key" % path)
    return pem, path


def _check_embedded(header, sig, pem, origin):
    try:
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import padding, rsa
    except ImportError:
        raise ConfigError(
            "checking the signature against the embedded key needs the "
            "'cryptography' module; install python3-cryptography")

    try:
        key = serialization.load_pem_public_key(pem.encode("utf-8"))
    except (ValueError, TypeError) as e:
        raise ConfigError("the key embedded in %s does not parse (%s)"
                          % (origin, e))
    if not isinstance(key, rsa.RSAPublicKey):
        raise ConfigError("the key embedded in %s is not an RSA public key"
                          % origin)
    try:
        key.verify(sig, header, padding.PKCS1v15(), hashes.SHA256())
    except InvalidSignature:
        raise ConfigError(
            "the signature does not verify against the RSA-%d public key "
            "embedded in %s -- the device would refuse this package with "
            "UPGRADE_VERIFY_ERR.  Sign with the private half of that key, or "
            "embed the public half of the key you signed with."
            % (key.key_size, origin))
    print("verified against the RSA-%d key embedded in %s"
          % (key.key_size, origin))


def _check_firmware(pem, origin, path):
    from cryptography.exceptions import UnsupportedAlgorithm
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa

    try:
        with open(path, "rb") as fh:
            blob = fh.read()
    except OSError as e:
        raise ConfigError("cannot read the firmware %s (%s)" % (path, e))
    want = serialization.load_pem_public_key(pem.encode("ascii")).public_numbers()
    for m in re.finditer(rb"-----BEGIN PUBLIC KEY-----([^-]*)-----END PUBLIC KEY-----",
                         blob):
        try:
            key = serialization.load_der_public_key(
                base64.b64decode(b"".join(m.group(1).split()), validate=True))
        except (ValueError, TypeError, UnsupportedAlgorithm):
            continue
        if isinstance(key, rsa.RSAPublicKey) and key.public_numbers() == want:
            break
    else:
        raise ConfigError(
            "%s does not embed the public key of %s -- the firmware and this "
            "package are built for different keys and the device would refuse "
            "the package with UPGRADE_VERIFY_ERR." % (path, origin))
    print("%s embeds the key of %s" % (path, origin))


def _sign_cmd(header, prog):
    try:
        res = subprocess.run([prog], input=header, stdout=subprocess.PIPE)
    except OSError as e:
        raise ConfigError("cannot run the signer %s (%s)" % (prog, e))
    if res.returncode != 0:
        raise ConfigError("the signer %s exited with status %d"
                          % (prog, res.returncode))
    if len(res.stdout) != SIG_SIZE:
        raise ConfigError("the signer %s wrote %d bytes; expected a %d-byte "
                          "signature" % (prog, len(res.stdout), SIG_SIZE))
    return res.stdout


def build(cfg, args):
    if args.sign_cmd:
        # The program and a key name different signers, so naming both is an
        # error, not a preference.
        if args.key:
            raise ConfigError("-k %s and --sign-cmd %s both name a signer; "
                              "pick one" % (args.key, args.sign_cmd))

    if cfg["upgrade_type"] != "OTA":
        raise ConfigError(
            "upgrade_type = %r; this tool builds OTA packages only"
            % cfg["upgrade_type"])

    try:
        major, minor = cfg["format"].split(".")
        fmt = int(major) * 16 + int(minor)
    except (AttributeError, ValueError):
        raise ConfigError("format = %r; expected MAJOR.MINOR, e.g. \"1.0\""
                          % cfg["format"])

    files = cfg["files"]
    nbins = int(cfg["totalbins"])
    if nbins != len(files):
        raise ConfigError("totalbins = %d but files[] has %d entries"
                          % (nbins, len(files)))
    if nbins <= 0 or nbins > BINFILE_NUM_MAX:
        raise ConfigError("totalbins = %d; must be 1..%d"
                          % (nbins, BINFILE_NUM_MAX))

    bins = []
    payloads = []
    ofst = HEADER_SIZE
    for i, f in enumerate(files):
        path = os.path.join(args.input_dir, f["path"])
        with open(path, "rb") as fh:
            data = _pad32(fh.read())
        partofst = _hexint("files[%d].partofst" % i, f["partofst"])
        partsize = _hexint("files[%d].partsize" % i, f["partsize"])
        if "partofst2" in f:
            partofst2 = _hexint("files[%d].partofst2" % i, f["partofst2"])
            partsize2 = _hexint("files[%d].partsize2" % i, f["partsize2"])
        else:
            partofst2, partsize2 = partofst, partsize
        if len(data) > partsize:
            raise ConfigError(
                "%s is %d bytes padded; partition %r holds %d"
                % (path, len(data), f["name"], partsize))
        sha = hashlib.sha256(data).digest()
        bins.append(struct.pack(BIN_FMT,
                                _fixed("files[%d].name" % i, f["name"], 16),
                                ofst, len(data), partofst, partsize,
                                partofst2, partsize2, sha))
        payloads.append(data)
        print("bin %2d %-16s off=0x%08x size=0x%08x part=0x%08x/0x%08x "
              "part2=0x%08x/0x%08x sha256=%s"
              % (i, f["name"], ofst, len(data), partofst, partsize,
                 partofst2, partsize2, sha.hex()))
        ofst += len(data)

    stamp = time.time()
    version = _version_string(cfg, args, stamp)
    header = struct.pack(HDR_FMT, fmt,
                         _fixed("manufacturer", cfg["manufacturer"], 8),
                         _fixed("product", cfg["product"], 16),
                         _fixed("mainctroler", cfg["mainctroler"], 16),
                         _fixed("time",
                                time.strftime("%Y-%m-%d %H:%M:%S",
                                              time.localtime(stamp)), 20),
                         nbins, ofst,
                         _fixed("version", version, 64),
                         _fixed("hw", cfg["hw"], 16),
                         _fixed("sw", cfg["sw"], 16),
                         _fixed("upgrade_type", cfg["upgrade_type"], 16))
    header += b"".join(bins)
    header += b"\x00" * BIN_SIZE * (BINFILE_NUM_MAX - nbins)
    assert len(header) == COMMON_HDR_SIZE

    if args.sign_cmd:
        sig = _sign_cmd(header, args.sign_cmd)
        print("signed by %s over %d header bytes" % (args.sign_cmd, len(header)))
    else:
        key = args.key or KEY_DEFAULT
        sig = _sign(header, key)
        print("signed with %s over %d header bytes" % (key, len(header)))

    pem, origin = _device_public_pem(args.verify_key)
    _check_embedded(header, sig, pem, origin)
    _check_firmware(pem, origin, args.firmware)

    blob = header + sig
    blob += b"\xff" * (HEADER_SIZE - len(blob))
    out = os.path.join(args.output_dir, version + ".aup")
    tmp = out + ".tmp"
    try:
        with open(tmp, "wb") as fh:
            fh.write(blob)
            for data in payloads:
                fh.write(data)
        os.replace(tmp, out)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("-f", "--config", required=True)
    p.add_argument("-i", "--input-dir", default=".")
    p.add_argument("-o", "--output-dir", default=".")
    p.add_argument("-k", "--key",
                   help="RSA-1024 private key (PEM) to sign with; defaults to "
                        + os.path.basename(KEY_DEFAULT) + " beside this script")
    p.add_argument("--sign-cmd", metavar="PROGRAM",
                   help="sign by running PROGRAM: header on stdin, 128-byte "
                        "signature on stdout")
    p.add_argument("--verify-key", metavar="PEM",
                   help="public key the firmware embeds; defaults to the key "
                        "in " + EMBEDDED_KEY_SRC)
    p.add_argument("--firmware", metavar="BINARY", required=True,
                   help="mm_miner binary that must embed the verify key")
    p.add_argument("-t", "--type", default="")
    p.add_argument("-r", "--release", default="")
    p.add_argument("-v", "--ver", dest="aup_ver", default="01")
    p.add_argument("-m", "--commit", default="000000")
    args = p.parse_args()

    try:
        with open(args.config, "r") as fh:
            cfg = json.load(fh)
        out = build(cfg, args)
    except ValueError as e:
        print("gen_aup: %s: %s" % (args.config, e), file=sys.stderr)
        return 1
    except (ConfigError, KeyError, OSError) as e:
        print("gen_aup: %s" % e, file=sys.stderr)
        return 1
    print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
