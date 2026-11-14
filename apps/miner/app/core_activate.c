// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026, Canaan Creative Co., Ltd.
 *
 * For the licensing terms, see the LICENSE file (MIT License).
 *
 * NO WARRANTY. The Software is provided "AS IS", without warranty of any kind, express or
 * implied, including but not limited to the warranties of merchantability, fitness for a
 * particular purpose and non-infringement. In no event shall the authors or copyright
 * holders be liable for any claim, damages or other liability arising from, out of or in
 * connection with the Software or the use or other dealings in the Software.
 */

#include "core_activate.h"
#include "osif.h"

#include "core_sigverify.h"
#include "hex.h"
#include "sha256.h"
#include <stdint.h>
#include <string.h>

#define ACT_SIGN_LEN 128 /* activation signature length, bytes */
#define ACT_DNA_LEN  16  /* chip DNA length, hex chars */
#define ACT_REF_HEX  64  /* field before the signature, hex chars */

int core_activate_verify(const char *dna16, const char *args)
{
    if (!dna16 || strlen(dna16) < ACT_DNA_LEN)
        return 0;
    if (!args || strlen(args) < ACT_REF_HEX + ACT_SIGN_LEN * 2)
        return 0;

    uint8_t sig[ACT_SIGN_LEN];
    if (hex_decode(sig, args + ACT_REF_HEX, ACT_SIGN_LEN) != 0)
        return 0;

    uint8_t hash[SHA256_DIGEST_SIZE];
    sha256d(dna16, ACT_DNA_LEN, hash);

    int ok = core_sig_verify("activation", core_pic_public_pem, 1, sig, hash);
    if (!ok)
        osif_log(OSIF_LOG_WARN, "activation refused: signature check failed");
    return ok;
}
