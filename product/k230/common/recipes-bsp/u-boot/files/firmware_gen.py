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

"""Wrap an image in the 528-byte K230 firmware header.

The header is the magic "K230", the payload length and the crypto type (both
u32 little-endian) and 516 bytes of type-specific fields; the payload follows.
The payload is a zero 4-byte version word and the input image, encrypted for
the SM4 and AES types. The input file is rewritten with the version word
prepended.

  -n  type 0: SHA-256 of the payload, zero-padded
  -s  type 1: SM4-CBC payload; ID length, ID, SM2 public key, SM2 signature
  -a  type 2: AES-GCM payload and tag; RSA-2048 modulus, exponent, signature
              of the tag

All modes need pycryptodome; -s also needs gmssl.
"""

import getopt
import hashlib
import struct
import sys

from Crypto.Cipher import AES
from Crypto.Hash import SHA256
from Crypto.PublicKey import RSA
from Crypto.Signature import pkcs1_15

try:
    from gmssl import func, sm2, sm3
    from gmssl.sm4 import SM4_ENCRYPT, CryptSM4
except ImportError:
    sm2 = None

USAGE = 'usage: firmware_gen.py -i <input> -o <output> (-n | -s | -a)'

MAGIC = b'K230'
VERSION = bytes(4)
FIELDS_LEN = 516

TYPE_NONE = 0
TYPE_SM4 = 1
TYPE_AES = 2

AES_KEY = bytes.fromhex('24501ad384e473963d476edcfe08205237acfd49b5b8f33857f8114e863fec7f')
AES_NONCE = bytes.fromhex('9ff18563b978ec281b3f2794')

RSA_N = bytes.fromhex(
    'cea80475324c1dc8347827818da58bac069d3419c614a6ea1ac6a3b510dcd72c'
    'c516954905e9fef908d45e13006adf27d467a7d83c111d1a5df15ef293771aef'
    'b920032a5bb989f8e4f5e1b05093d3f130f984c07a772a3683f4dc6fb28a9681'
    '5b32123ccdd13954f19d5b8b24a103e771a34c328755c65ed64e1924ffd04d30'
    'b2142cc262f6e0048fef6dbc652f21479ea1c4b1d66d28f4d46ef7185e390cbf'
    'a2e02380582f3188bb94ebbf05d31487a09aff01fcbb4cd4bfd1f0a833b38c11'
    '813c84360bb53c7d4481031c40bad8713bb6b835cb08098ed15ba31ee4ba728a'
    '8c8e10f7294e1b4163b7aee57277bfd881a6f9d43e02c6925aa3a043fb7fb78d')
RSA_E = 0x260445
RSA_D = bytes.fromhex(
    '0997634c477c1a039d44c810b2aaa3c7862b0b88d3708272e1e15f66fc938970'
    '9f8a11f3ea6a5af7effa2d01c189c50f0d5bcbe3fa272e56cfc4a4e1d388a9dc'
    'd65df8628902556c8b6bb6a641709b5a35dd2622c73d4640bfa1359d0e76e1f2'
    '19f8e33eb9bd0b59ec198eb2fccaae0346bd8b401e12e3c67cb629569c185a2e'
    '0f35a2f741644c1cca5ebb139d77a89a2953fc5e30048c0e619f07c8d21d1e56'
    'b8af07193d0fdf3f49cd49f2ef3138b5138862f1470bd2d16e34a2b9e7777a6c'
    '8c8d4cb94b4e8b5d616cd5393753e7b0f31cc7da559ba8e98d888914e334773b'
    'af498ad88d9631eb5fe32e53a4145bf0ba548bf2b0a50c63f67b14e398a34b0d')

SM4_KEY = bytes.fromhex('0123456789abcdeffedcba9876543210')
SM4_IV = bytes.fromhex('000102030405060708090a0b0c0d0e0f')

# GB/T 32918.5 recommended curve parameters.
SM2_A = bytes.fromhex('fffffffeffffffffffffffffffffffffffffffff00000000fffffffffffffffc')
SM2_B = bytes.fromhex('28e9fa9e9d9f5e344d5a9e4bcf6509a7f39789f515ab8f92ddbcbd414d940e93')
SM2_GX = bytes.fromhex('32c4ae2c1f1981195f9904466a39c9948fe30bbff2660be1715a4589334c74c7')
SM2_GY = bytes.fromhex('bc3736a2f4f6779c59bdcee36b692153d0a9877cc62a474002df32e52139f0a0')

SM2_D = bytes.fromhex('3945208f7b2144b13f36e38ac6d39f95889393692860b51a42fb81ef4df7c5b8')
SM2_PUB_X = bytes.fromhex('09f9df311e5421a150dd7d161e4bc5c672179fad1833fc076bb08ff356f35020')
SM2_PUB_Y = bytes.fromhex('ccea490ce26775a52dc6ea718cc1aa600aed05fbf35e084a6632f6072da9ad13')
SM2_K = bytes.fromhex('59276e27d506861a16680f3ad9c02dccef3cc1fa3cdbe4ce6d54b80deac1bc21')
SM2_ID = b'1234567812345678'
SM2_ID_FIELD_LEN = 384


def u32(value):
    return struct.pack('<I', value)


def sm3_digest(data):
    return bytes.fromhex(sm3.sm3_hash(func.bytes_to_list(data)))


def sm2_z():
    """Return Z_A of GB/T 32918.2 for SM2_ID and the SM2 public key."""
    entl = (len(SM2_ID) * 8).to_bytes(2, 'big')
    return sm3_digest(entl + SM2_ID + SM2_A + SM2_B + SM2_GX + SM2_GY +
                      SM2_PUB_X + SM2_PUB_Y)


def none_header(data):
    digest = hashlib.sha256(data).digest()
    print('sha256:', digest.hex())
    return TYPE_NONE, data, digest.ljust(FIELDS_LEN, b'\0')


def sm4_sm2_header(data):
    if sm2 is None:
        sys.exit('firmware_gen.py: -s needs the Python package gmssl')
    sm4 = CryptSM4()
    sm4.set_key(SM4_KEY, SM4_ENCRYPT)
    payload = sm4.crypt_cbc(SM4_IV, data)

    signer = sm2.CryptSM2(private_key=SM2_D.hex(),
                          public_key=(SM2_PUB_X + SM2_PUB_Y).hex())
    digest = sm3_digest(sm2_z() + payload)
    signature = bytes.fromhex(signer.sign(digest, SM2_K.hex()))

    pubkey = (u32(len(SM2_ID)) + SM2_ID.ljust(SM2_ID_FIELD_LEN, b'\0') +
              SM2_PUB_X + SM2_PUB_Y)
    print('SM2 public key SM3:', sm3_digest(pubkey).hex())
    return TYPE_SM4, payload, pubkey + signature


def aes_rsa_header(data):
    ciphertext, tag = AES.new(AES_KEY, AES.MODE_GCM, nonce=AES_NONCE).encrypt_and_digest(data)

    key = RSA.construct((int.from_bytes(RSA_N, 'big'), RSA_E,
                         int.from_bytes(RSA_D, 'big')))
    signature = pkcs1_15.new(key).sign(SHA256.new(tag))

    pubkey = RSA_N + u32(RSA_E)
    print('RSA public key SHA-256:', hashlib.sha256(pubkey).hexdigest())
    return TYPE_AES, ciphertext + tag, pubkey + signature


MODES = {
    '-n': none_header, '--non-encryption': none_header,
    '-s': sm4_sm2_header, '--sm4': sm4_sm2_header,
    '-a': aes_rsa_header, '--aes': aes_rsa_header,
}


def main(argv):
    try:
        opts, _ = getopt.getopt(argv, 'hi:o:asn',
                                ['ifile=', 'ofile=', 'aes', 'sm4', 'non-encryption'])
    except getopt.GetoptError as err:
        print(f'firmware_gen.py: {err}\n{USAGE}', file=sys.stderr)
        return 2

    inputfile = outputfile = None
    for opt, arg in opts:
        if opt == '-h':
            print(USAGE)
            return 0
        if opt in ('-i', '--ifile'):
            inputfile = arg
        elif opt in ('-o', '--ofile'):
            outputfile = arg
    if inputfile is None or outputfile is None:
        print(USAGE, file=sys.stderr)
        return 2

    with open(inputfile, 'rb') as f:
        data = VERSION + f.read()
    with open(inputfile, 'wb') as f:
        f.write(data)

    image = bytearray(MAGIC)
    payload = data
    for opt, _ in opts:
        if opt in MODES:
            ftype, payload, fields = MODES[opt](data)
            image += u32(len(payload)) + u32(ftype) + fields

    with open(outputfile, 'wb') as f:
        f.write(image + payload)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
