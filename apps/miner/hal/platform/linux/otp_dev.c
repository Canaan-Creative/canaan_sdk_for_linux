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

#include "otp_dev.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define OTP_NVMEM_PATH "/sys/bus/nvmem/devices/kendryte_otp0/nvmem"
#define DNA_LOC_OFST   0x60 /* chip DNA offset in the OTP, bytes */

int mm_dna_read(uint8_t *dna, size_t len)
{
    int rc = 0;
    int fd = open(OTP_NVMEM_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -ENOENT;
    if (pread(fd, dna, len, DNA_LOC_OFST) != (ssize_t)len)
        rc = -EIO;
    close(fd);
    return rc;
}
