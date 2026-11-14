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

#include <stdint.h>
#include "i2c_dev.h"
#include "boardconf.h"

#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#if CAP_HB_MCU
#define I2C_MCU_NODE I2C_MCU_DEV
#else /* CAP_HB_MCU */
#define I2C_MCU_NODE NULL
#endif /* CAP_HB_MCU */

struct i2c_dev_t {
    const char     *file; /* /dev/i2c-N node, NULL when the board has none */
    int             fd;   /* open descriptor, -1 when closed */
    pthread_mutex_t mutex;
};

/* bus slots indexed by i2c_id */
static struct i2c_dev_t g_i2c[I2C_MAX] = {
    [I2C_PD]        = { .file = I2C_PD_DEV, .fd = -1, .mutex = PTHREAD_MUTEX_INITIALIZER },
    [I2C_POWER_E2P] = { .file = I2C_POWER_E2P_DEV, .fd = -1, .mutex = PTHREAD_MUTEX_INITIALIZER },
    [I2C_MCU]       = { .file = I2C_MCU_NODE, .fd = -1, .mutex = PTHREAD_MUTEX_INITIALIZER },
};
static uint8_t        g_lock_owner[I2C_MAX]; /* slot whose mutex guards each slot's bus */
static pthread_once_t g_lock_once = PTHREAD_ONCE_INIT;

static void lock_owner_init(void)
{
    for (uint8_t i = 0; i < I2C_MAX; i++) {
        g_lock_owner[i] = i;
        const char *p   = g_i2c[i].file;
        if (!p)
            continue;
        for (uint8_t j = 0; j < i; j++) {
            const char *q = g_i2c[j].file;
            if (q && strcmp(p, q) == 0) {
                g_lock_owner[i] = j;
                break;
            }
        }
    }
}

static pthread_mutex_t *bus_mutex(uint8_t id)
{
    pthread_once(&g_lock_once, lock_owner_init);
    return &g_i2c[g_lock_owner[id]].mutex;
}

int i2c_init(uint8_t id)
{
    int rc = 0;

    if (id >= I2C_MAX)
        return -1;
    const char *node = g_i2c[id].file;
    if (!node)
        return -1;
    pthread_mutex_t *lock = bus_mutex(id);
    pthread_mutex_lock(lock);
    if (g_i2c[id].fd < 0) {
        int fd = open(node, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            ioctl(fd, I2C_TIMEOUT, 20);
            ioctl(fd, I2C_RETRIES, 1);
            g_i2c[id].fd = fd;
        } else {
            rc = -1;
        }
    }
    pthread_mutex_unlock(lock);
    return rc;
}

int i2c_deinit(uint8_t id)
{
    if (id >= I2C_MAX)
        return -1;
    pthread_mutex_t *lock = bus_mutex(id);
    pthread_mutex_lock(lock);
    if (g_i2c[id].fd >= 0) {
        close(g_i2c[id].fd);
        g_i2c[id].fd = -1;
    }
    pthread_mutex_unlock(lock);
    return 0;
}

int i2c_fd(uint8_t id)
{
    if (id >= I2C_MAX)
        return -1;
    pthread_mutex_t *lock = bus_mutex(id);
    pthread_mutex_lock(lock);
    int fd = g_i2c[id].fd;
    pthread_mutex_unlock(lock);
    return fd;
}

int i2c_write(uint8_t id, uint16_t slave_addr, uint8_t reg, uint8_t *val, uint8_t len)
{
    if (id >= I2C_MAX || (len && !val))
        return -1;
    uint8_t buf[1 + 255];
    buf[0] = reg;
    memcpy(buf + 1, val, len);
    struct i2c_msg msg = { .addr = slave_addr, .flags = 0, .len = (uint16_t)(len + 1), .buf = buf };
    struct i2c_rdwr_ioctl_data work_queue = { .msgs = &msg, .nmsgs = 1 };

    pthread_mutex_t *lock = bus_mutex(id);
    pthread_mutex_lock(lock);
    int fd = g_i2c[id].fd;
    int rc = -1;
    if (fd >= 0 && ioctl(fd, I2C_RDWR, (unsigned long)&work_queue) >= 0)
        rc = 0;
    pthread_mutex_unlock(lock);
    return rc;
}

int i2c_read(uint8_t id, uint16_t slave_addr, uint8_t reg, uint8_t *val, uint8_t len)
{
    if (id >= I2C_MAX || !val || !len)
        return -1;

    uint8_t        w_buf   = reg;
    struct i2c_msg msgs[2] = {
        { .addr = slave_addr, .flags = 0, .len = 1, .buf = &w_buf },
        { .addr = slave_addr, .flags = I2C_M_RD, .len = len, .buf = val },
    };
    struct i2c_rdwr_ioctl_data work_queue = { .msgs = msgs, .nmsgs = 2 };

    pthread_mutex_t *lock = bus_mutex(id);
    pthread_mutex_lock(lock);
    int fd = g_i2c[id].fd;
    int rc = -1;
    if (fd >= 0 && ioctl(fd, I2C_RDWR, (unsigned long)&work_queue) >= 0)
        rc = 0;
    pthread_mutex_unlock(lock);
    return rc;
}
