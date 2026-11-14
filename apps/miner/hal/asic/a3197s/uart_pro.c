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

#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <asm/termbits.h>
#include "osif.h"
#include "hex.h"
#include <byteswap.h>

#include "toast_port.h"

#include "uart_pro.h"
#include "a3197s_reg.h"

#include "board/board_tables.h"
#include "boardconf.h"

#ifndef DEBUG_UART
#define DEBUG_UART 0 /* 1 = hex-dump chip link frames to the log */
#endif

#define UART_FRAME_LEN   5 /* data word frame: 4 data bytes + CRC-8, bytes */
#define UART_HDR_LEN     5 /* frame preamble, bytes */
#define UART_CMD_LEN     5 /* command: 4 command bytes + CRC-8, bytes */
#define UART_HDR_CMD_LEN (UART_HDR_LEN + UART_CMD_LEN) /* preamble + command, bytes */
#define MAX_OVERTIME     2000                          /* reply wait per select() call, us */
/* select() timeouts in a row before a read is abandoned, count */
#define MAX_RETRY   3
#define MAX_READ_MS 10 /* deadline for one complete reply, ms */

_Static_assert(HBOARD_COUNT >= 1,
        "lay1 descriptor declares no hashboard: board_hboards[] would be empty and "
        "the ASIC UART transport would have no channel to address");
_Static_assert((TOAST_NONCEBUF_FIFO_LEN * 4 - 16) / 10 <= TOAST_NONCE_COUNT,
        "a full TOAST_NONCEBUF_FIFO_LEN reply carries more nonce records than "
        "TOAST_NONCE_COUNT: uart_get_nonce() would overrun the caller's nonce array");

typedef struct uart_handle {
    int fd; /* tty fd; 0 before the first uart_init(), -1 after a failed open */
} uart_handle_t;

uart_handle_t        uart_hdl[HBOARD_COUNT];
static const uint8_t uart_hdr_def[UART_HDR_LEN] = { 0x5a, 0x5a, 0x5a, 0x5a, 0x5a };
/* selected chip address per board, A3197S_BROADCAST = all chips */
static volatile uint32_t g_uart_select_chip[HBOARD_COUNT] = { 0 };
static uint32_t          g_uart_errcnt[HBOARD_COUNT]; /* failed chip reads per board, count */
/* reads with no reply in a row per board, count */
static uint32_t g_uart_silent_run[HBOARD_COUNT];
/* framing resyncs per board since the last in-sync reply, count */
static uint32_t g_uart_resync_run[HBOARD_COUNT];
static uint32_t g_uart_soft_reset_n[HBOARD_COUNT]; /* chip link soft resets per board, count */
/* nonce version mask mismatches per board since the last all-matched read, count */
static uint32_t g_uart_vmask_run[HBOARD_COUNT];

/* clang-format off */
static const uint8_t uart_crc8_table[256] = /* CRC-8 lookup table, polynomial 0xd5 */
{
    0x00, 0xd5, 0x7f, 0xaa, 0xfe, 0x2b, 0x81, 0x54,
    0x29, 0xfc, 0x56, 0x83, 0xd7, 0x02, 0xa8, 0x7d,
    0x52, 0x87, 0x2d, 0xf8, 0xac, 0x79, 0xd3, 0x06,
    0x7b, 0xae, 0x04, 0xd1, 0x85, 0x50, 0xfa, 0x2f,
    0xa4, 0x71, 0xdb, 0x0e, 0x5a, 0x8f, 0x25, 0xf0,
    0x8d, 0x58, 0xf2, 0x27, 0x73, 0xa6, 0x0c, 0xd9,
    0xf6, 0x23, 0x89, 0x5c, 0x08, 0xdd, 0x77, 0xa2,
    0xdf, 0x0a, 0xa0, 0x75, 0x21, 0xf4, 0x5e, 0x8b,
    0x9d, 0x48, 0xe2, 0x37, 0x63, 0xb6, 0x1c, 0xc9,
    0xb4, 0x61, 0xcb, 0x1e, 0x4a, 0x9f, 0x35, 0xe0,
    0xcf, 0x1a, 0xb0, 0x65, 0x31, 0xe4, 0x4e, 0x9b,
    0xe6, 0x33, 0x99, 0x4c, 0x18, 0xcd, 0x67, 0xb2,
    0x39, 0xec, 0x46, 0x93, 0xc7, 0x12, 0xb8, 0x6d,
    0x10, 0xc5, 0x6f, 0xba, 0xee, 0x3b, 0x91, 0x44,
    0x6b, 0xbe, 0x14, 0xc1, 0x95, 0x40, 0xea, 0x3f,
    0x42, 0x97, 0x3d, 0xe8, 0xbc, 0x69, 0xc3, 0x16,
    0xef, 0x3a, 0x90, 0x45, 0x11, 0xc4, 0x6e, 0xbb,
    0xc6, 0x13, 0xb9, 0x6c, 0x38, 0xed, 0x47, 0x92,
    0xbd, 0x68, 0xc2, 0x17, 0x43, 0x96, 0x3c, 0xe9,
    0x94, 0x41, 0xeb, 0x3e, 0x6a, 0xbf, 0x15, 0xc0,
    0x4b, 0x9e, 0x34, 0xe1, 0xb5, 0x60, 0xca, 0x1f,
    0x62, 0xb7, 0x1d, 0xc8, 0x9c, 0x49, 0xe3, 0x36,
    0x19, 0xcc, 0x66, 0xb3, 0xe7, 0x32, 0x98, 0x4d,
    0x30, 0xe5, 0x4f, 0x9a, 0xce, 0x1b, 0xb1, 0x64,
    0x72, 0xa7, 0x0d, 0xd8, 0x8c, 0x59, 0xf3, 0x26,
    0x5b, 0x8e, 0x24, 0xf1, 0xa5, 0x70, 0xda, 0x0f,
    0x20, 0xf5, 0x5f, 0x8a, 0xde, 0x0b, 0xa1, 0x74,
    0x09, 0xdc, 0x76, 0xa3, 0xf7, 0x22, 0x88, 0x5d,
    0xd6, 0x03, 0xa9, 0x7c, 0x28, 0xfd, 0x57, 0x82,
    0xff, 0x2a, 0x80, 0x55, 0x01, 0xd4, 0x7e, 0xab,
    0x84, 0x51, 0xfb, 0x2e, 0x7a, 0xaf, 0x05, 0xd0,
    0xad, 0x78, 0xd2, 0x07, 0x53, 0x86, 0x2c, 0xf9,
};
/* clang-format on */

static void uart_dbg_hex(const char *tag, int n, const void *buf, size_t len)
{
    char hex[UART_RX_BUFFER_SIZE * 2 + 1];

    if (!DEBUG_UART)
        return;
    if (len > UART_RX_BUFFER_SIZE)
        len = UART_RX_BUFFER_SIZE;
    hex_encode(hex, buf, len);
    osif_log(OSIF_LOG_WARN, "%s(%d):%s", tag, n, hex);
}

static int uart_apply_baud(int fd, uint32_t baud)
{
    struct termios2 t;
    if (ioctl(fd, TCGETS2, &t) < 0)
        return -1;
    t.c_cflag &= ~(CBAUD | CSIZE | PARENB | CSTOPB | CRTSCTS);
    t.c_cflag |= BOTHER | CS8 | CLOCAL | CREAD;
    t.c_ispeed    = baud;
    t.c_ospeed    = baud;
    t.c_iflag     = 0;
    t.c_oflag     = 0;
    t.c_lflag     = 0;
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    return ioctl(fd, TCSETS2, &t);
}

static const char *uart_node(uint8_t channel)
{
    return board_hboards[channel % HBOARD_COUNT].uart_dev;
}

void uart_init(uint8_t channel, uint32_t baud)
{
    uart_handle_t *uh   = &uart_hdl[channel % HBOARD_COUNT];
    const char    *node = uart_node(channel);
    if (uh->fd > 0)
        close(uh->fd);
    uh->fd = open(node, O_RDWR | O_NOCTTY);

    if (uh->fd < 0) {
        osif_log(OSIF_LOG_ERR, "board %u: cannot open chip link %s: %s", channel, node,
                strerror(errno));
        return;
    }
    osif_log(OSIF_LOG_INFO, "board %u: chip link %s opened at %u baud", channel, node, baud);

    if (uart_apply_baud(uh->fd, baud)) {
        osif_log(OSIF_LOG_ERR, "board %u: cannot set chip link to %u baud", channel, baud);
    }

    uart_clear_rxfifo(channel);
}

int uart_read(uint8_t channel, uint8_t *buf, uint16_t len)
{
    uart_handle_t *uh = &uart_hdl[channel % HBOARD_COUNT];
    if (uh->fd <= 0)
        return -1;
    return (read(uh->fd, buf, len));
}

int uart_tx_empty(uint8_t channel)
{
    uart_handle_t *uh   = &uart_hdl[channel % HBOARD_COUNT];
    int            outq = 0;
    if (uh->fd <= 0)
        return 0;
    if (ioctl(uh->fd, TIOCOUTQ, &outq) < 0)
        return 1;
    return outq == 0;
}

int uart_write(uint8_t channel, uint8_t *buf, uint16_t len)
{
    uart_handle_t *uh = &uart_hdl[channel % HBOARD_COUNT];
    if (uh->fd <= 0) {
        osif_log(OSIF_LOG_ERR,
                "board %u: chip link not open, %u bytes not "
                "sent",
                channel, len);
        return -1;
    }
    uint16_t off = 0;
    while (off < len) {
        ssize_t n = write(uh->fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            osif_log(OSIF_LOG_ERR,
                    "board %u: chip link write failed after "
                    "%u of %u bytes: %s",
                    channel, off, len, strerror(errno));
            return -1;
        }
        off += (uint16_t)n;
    }
    return len;
}

void uart_config_baud(uint8_t channel, uint32_t baud)
{
    uart_handle_t *uh = &uart_hdl[channel % HBOARD_COUNT];

    ioctl(uh->fd, TCSBRK, 1);
    if (uart_apply_baud(uh->fd, baud)) {
        osif_log(OSIF_LOG_WARN, "board %u: cannot set chip link to %u baud", channel, baud);
    }
}

void uart_clear_rxfifo(uint8_t channel)
{
    uart_handle_t *uh = &uart_hdl[channel % HBOARD_COUNT];
    uint8_t        buf[256];
    int            ret = 0;

    if (uh->fd <= 0)
        return;

    ioctl(uh->fd, TCSBRK, 1);

    while (1) {
        ret = uart_read(channel, buf, sizeof(buf));
        if (ret <= 0) {
            break;
        }
        uart_dbg_hex("RX to clear", ret, buf, ret);
    }
}

static uint32_t uart_sel_chip(uint8_t channel)
{
    return g_uart_select_chip[channel % HBOARD_COUNT];
}

uint32_t uart_get_errcnt(uint8_t channel)
{
    return g_uart_errcnt[channel % HBOARD_COUNT];
}
void uart_clear_errcnt(uint8_t channel)
{
    g_uart_errcnt[channel % HBOARD_COUNT] = 0;
}

static int uart_silent_loud(uint8_t channel)
{
    return g_uart_silent_run[channel % HBOARD_COUNT] <= (uint32_t)UART_SILENT_LOG_MAX;
}

static int uart_silent_hit(uint8_t channel)
{
    g_uart_silent_run[channel % HBOARD_COUNT]++;
    return uart_silent_loud(channel);
}

static void uart_silent_clear(uint8_t channel)
{
    uint32_t *run = &g_uart_silent_run[channel % HBOARD_COUNT];
    if (*run > (uint32_t)UART_SILENT_LOG_MAX)
        osif_log(OSIF_LOG_INFO,
                "board %d: chips answering again after %u "
                "silent reads (%u error lines suppressed)",
                channel, *run, *run - (uint32_t)UART_SILENT_LOG_MAX);
    *run = 0;
}

static int uart_resync_hit(uint8_t channel)
{
    uint32_t *run = &g_uart_resync_run[channel % HBOARD_COUNT];
    return ++(*run) <= (uint32_t)UART_SILENT_LOG_MAX;
}

static void uart_resync_clear(uint8_t channel)
{
    uint32_t *run = &g_uart_resync_run[channel % HBOARD_COUNT];
    if (*run > (uint32_t)UART_SILENT_LOG_MAX)
        osif_log(OSIF_LOG_WARN,
                "board %d: chip link framing in sync again "
                "after %u resyncs (%u warning lines suppressed)",
                channel, *run, *run - (uint32_t)UART_SILENT_LOG_MAX);
    *run = 0;
}

static uint8_t uart_get_crc8(uint8_t *data, uint16_t len)
{
    uint8_t tmp8_crc = 0;

    while (len--) {
        tmp8_crc ^= *data--;
        tmp8_crc = uart_crc8_table[tmp8_crc];
    }

    return tmp8_crc;
}

static void uart_put_cmd(uint8_t *buf, uint8_t b5, uint8_t b6, uint8_t b7, uint8_t b8)
{
    memcpy(buf, uart_hdr_def, UART_HDR_LEN);
    buf[5] = b5;
    buf[6] = b6;
    buf[7] = b7;
    buf[8] = b8;
    buf[9] = uart_get_crc8(&buf[8], 4);
}

void uart_chip_select(uint8_t channel, uint16_t chip_addr)
{
    g_uart_select_chip[channel % HBOARD_COUNT] = chip_addr;
}

uint16_t uart_get_chip_select(uint8_t channel)
{
    return (uint16_t)uart_sel_chip(channel);
}

int uart_write_fifo(uint8_t channel, uint32_t reg_addr, uint32_t *data_send, uint32_t len,
        enum uart_mode mode)
{
    uint8_t  tmp8_tx_buf[UART_TX_BUFFER_SIZE];
    uint32_t i, wr_len = 0;
    int      werr = 0;

    if (len == 0)
        return 0;
    if (len > (UART_TX_BUFFER_SIZE - UART_HDR_CMD_LEN) / UART_FRAME_LEN) {
        osif_log(OSIF_LOG_ERR,
                "chip link write of %u words refused: more than "
                "the transmit buffer holds (%u)",
                len, (unsigned)((UART_TX_BUFFER_SIZE - UART_HDR_CMD_LEN) / UART_FRAME_LEN));
        return 1;
    }

    if (mode == UART_INIT_MODE) {
        uart_put_cmd(tmp8_tx_buf, 0xff, 0xff, 0xff, 0xff);
    } else {
        uint32_t sel = uart_sel_chip(channel);
        uart_put_cmd(tmp8_tx_buf, sel & 0xff,
                ((reg_addr & 0x3c) << 2) | (mode << 2) | ((sel >> 8) & 0x3), reg_addr >> 6, len);
    }

    if (data_send) {
        for (i = 0; i < len; i++) {
            tmp8_tx_buf[10 + 5 * i] = data_send[i] >> 0;
            tmp8_tx_buf[11 + 5 * i] = data_send[i] >> 8;
            tmp8_tx_buf[12 + 5 * i] = data_send[i] >> 16;
            tmp8_tx_buf[13 + 5 * i] = data_send[i] >> 24;
            tmp8_tx_buf[14 + 5 * i] = uart_get_crc8(&tmp8_tx_buf[13 + 5 * i], 4);
        }

        if (mode == UART_INIT_MODE) {
            werr |= uart_write(channel, &tmp8_tx_buf[0], 10) < 0;
            osif_sleep_ms(1);
            werr |= uart_write(channel, &tmp8_tx_buf[10], len * 5) < 0;
        } else {
            werr |= uart_write(channel, &tmp8_tx_buf[0], len * 5 + 10) < 0;
        }
        wr_len = len * 5 + 10;
    } else {
        werr |= uart_write(channel, &tmp8_tx_buf[0], 10) < 0;
        wr_len = 10;
    }

    uart_dbg_hex("TX", wr_len, tmp8_tx_buf, wr_len);
    return werr;
}

int uart_read_fifo_with_ret(uint8_t channel, uint32_t *chipid, uint32_t *reg_addr,
        uint32_t *data_recv, uint32_t *plen, int check_len)
{
    uint32_t       tmp32;
    uint8_t        tmp8_crc, cmd_len_valid = 0;
    uint8_t        tmp8_buf[UART_RX_BUFFER_SIZE];
    int            rd_len = 0, offset = 0;
    int            count = 0, pos = 0, i = 0;
    int            retry = 0, resynced = 0;
    uart_handle_t *uh = &uart_hdl[channel % HBOARD_COUNT];
    fd_set         read_fds;
    int            ret = 0;
    struct timeval timeout;

    rd_len = UART_HDR_CMD_LEN;

    if (data_recv)
        memset(data_recv, 0, (*plen) * sizeof(uint32_t));
    else
        return 1;

    if (uh->fd <= 0)
        return 1;

    uint32_t t_start = (uint32_t)osif_now_ms();

    while (1) {
        if ((uint32_t)osif_now_ms() - t_start > (uint32_t)MAX_READ_MS) {
            osif_log(OSIF_LOG_WARN,
                    "board %d asic %d: register 0x%x read "
                    "timed out after %d ms (%d bytes received)",
                    channel, *chipid, *reg_addr, MAX_READ_MS, pos);
            return 1;
        }
        FD_ZERO(&read_fds);
        FD_SET(uh->fd, &read_fds);
        timeout.tv_sec  = 0;
        timeout.tv_usec = MAX_OVERTIME;
        ret             = select(uh->fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ret <= 0) {
            retry++;
            if (retry < MAX_RETRY) {
                continue;
            }
            if (uart_silent_hit(channel))
                osif_log(OSIF_LOG_WARN,
                        "board %d asic %d: no reply to "
                        "register 0x%x read (%s, %d bytes received)",
                        channel, *chipid, *reg_addr, ret < 0 ? strerror(errno) : "timeout", pos);
            return 1;
        }
        if (retry > 0) {
            osif_log(OSIF_LOG_DBG,
                    "board %d asic %d: register 0x%x reply "
                    "after %d retries",
                    channel, *chipid, *reg_addr, retry);
        }

        count = uart_read(channel, tmp8_buf + pos, rd_len - pos);
        if (count <= 0) {
            osif_log(OSIF_LOG_WARN,
                    "board %d asic %d: chip link read of "
                    "register 0x%x failed (%d)",
                    channel, *chipid, *reg_addr, count);
            return 1;
        }

        uart_silent_clear(channel);
        retry = 0;
        pos += count;
        uart_dbg_hex("RX", pos, tmp8_buf, pos);

        if (pos < UART_HDR_CMD_LEN) {
            continue;
        }

        for (i = 0; i <= (pos - UART_HDR_LEN); i++) {
            if (!memcmp(tmp8_buf + i, uart_hdr_def, UART_HDR_LEN))
                break;
        }
        if (i != 0) {
            resynced = 1;
            if (uart_resync_hit(channel))
                osif_log(OSIF_LOG_WARN,
                        "board %d: chip link out of sync, "
                        "skipped %d stray bytes (first 0x%02x)",
                        channel, i, tmp8_buf[0]);
            memmove(tmp8_buf, tmp8_buf + i, pos - i);
            pos -= i;

            if (pos < UART_HDR_CMD_LEN)
                continue;
        }

        if (!cmd_len_valid) {
            tmp8_crc = uart_get_crc8(&tmp8_buf[UART_HDR_LEN + 3], 4);
            if (tmp8_crc != tmp8_buf[UART_HDR_LEN + 4]) {
                osif_log(OSIF_LOG_WARN,
                        "board %d asic %d: corrupted reply "
                        "header for register 0x%x "
                        "(CRC received 0x%02x, expected 0x%02x)",
                        channel, *chipid, *reg_addr, tmp8_buf[UART_HDR_LEN + 4], tmp8_crc);
                return 1;
            }
            if (tmp8_buf[UART_HDR_LEN + 1] == 0xff) {
                *plen = 1;
            } else if ((tmp8_buf[UART_HDR_LEN + 3]) != *plen) {
                if (check_len) {
                    if (DEBUG_UART)
                        osif_log(OSIF_LOG_ERR,
                                "board %d asic %d: 0x%x read %d word, "
                                "except %d word",
                                channel, *chipid, *reg_addr, tmp8_buf[UART_HDR_LEN + 3], *plen);
                    return 1;
                }
                if ((uint32_t)tmp8_buf[UART_HDR_LEN + 3] > *plen) {
                    osif_log(OSIF_LOG_WARN,
                            "board %d asic %d: reply for "
                            "register 0x%x has %d words, expected at most %d",
                            channel, *chipid, *reg_addr, tmp8_buf[UART_HDR_LEN + 3], *plen);
                    return 1;
                }

                *plen = tmp8_buf[UART_HDR_LEN + 3];
            }
            rd_len = UART_HDR_CMD_LEN + (*plen) * UART_FRAME_LEN;
            if (rd_len > UART_RX_BUFFER_SIZE) {
                osif_log(OSIF_LOG_WARN,
                        "board %d asic %d: reply for register "
                        "0x%x too long (%d bytes, limit %d)",
                        channel, *chipid, *reg_addr, rd_len, UART_RX_BUFFER_SIZE);
                return 1;
            }
            cmd_len_valid = 1;
        }

        if (pos == rd_len) {
            break;
        }
    }

    tmp32 = (tmp8_buf[UART_HDR_LEN + 1] >> 2) & 0x3;
    if (tmp32 == UART_READ_MODE) {
        tmp32 = tmp8_buf[UART_HDR_LEN] + ((tmp8_buf[UART_HDR_LEN + 1] & 0x3) << 8);
        if ((tmp32 != *chipid) && (*chipid != 0x3ff) && (*chipid != 0xffffffff)) {
            osif_log(OSIF_LOG_WARN,
                    "board %d: reply came from asic %u, "
                    "expected asic %u",
                    channel, tmp32, *chipid);
            return 1;
        }
        *chipid = tmp32;

        tmp32 = ((tmp8_buf[UART_HDR_LEN + 2] & 0xff) << 6) |
                ((tmp8_buf[UART_HDR_LEN + 1] & 0xf0) >> 2);
        if ((tmp32 != *reg_addr) && (*reg_addr != 0xffffffff)) {
            osif_log(OSIF_LOG_WARN,
                    "board %d: reply for register 0x%08x, "
                    "expected 0x%08x",
                    channel, tmp32, *reg_addr);
            return 1;
        }

        *reg_addr = tmp32;
    } else if (tmp32 == UART_WRITE_MODE) {
        osif_log(OSIF_LOG_WARN,
                "board %d: reply marked as a write "
                "(0x%02x), expected a read reply",
                channel, tmp8_buf[UART_HDR_LEN + 1]);
        return 1;
    }

    offset = UART_HDR_CMD_LEN;
    for (i = 0; i < (int)*plen; i++) {
        tmp8_crc = uart_get_crc8(&tmp8_buf[offset + 3], 4);
        if (tmp8_crc != tmp8_buf[offset + 4]) {
            osif_log(OSIF_LOG_WARN,
                    "board %d asic %d: corrupted reply data "
                    "for register 0x%x (CRC received 0x%02x, expected 0x%02x)",
                    channel, *chipid, *reg_addr, tmp8_buf[offset + 4], tmp8_crc);
            return 1;
        }

        data_recv[i] = ((tmp8_buf[offset + 0] << 0) | (tmp8_buf[offset + 1] << 8) |
                (tmp8_buf[offset + 2] << 16) | (tmp8_buf[offset + 3] << 24));
        offset += 5;
    }

    if (!resynced)
        uart_resync_clear(channel);

    return 0;
}

void uart_soft_reset(uint8_t channel)
{
    uint32_t v = A3197S_DEFAULT_UART_CFG3;
    uart_write_fifo(channel, A3197S_UART_CFG3_ADDR, &v, 1, UART_WRITE_MODE);
    uart_clear_rxfifo(channel);
    v = 0x80000000u;
    uart_write_fifo(channel, A3197S_NONCE_UPDATE_ADDR, &v, 1, UART_WRITE_MODE);
    uart_write_fifo(channel, A3197S_NONCE_UPDATE_ADDR, &v, 1, UART_WRITE_MODE);
    v = A3197S_DEFAULT_CFG;
    uart_write_fifo(channel, A3197S_CFG_ADDR, &v, 1, UART_WRITE_MODE);
    osif_log(OSIF_LOG_DBG,
            "board %d: chip read failed, chip link soft "
            "reset #%u",
            channel, ++g_uart_soft_reset_n[channel % HBOARD_COUNT]);
}

int uart_read_fifo(uint8_t channel, uint32_t reg_addr, uint32_t *data_recv, uint32_t len)
{
    uint32_t chipid      = uart_sel_chip(channel);
    uint32_t rd_reg_addr = reg_addr;
    uint32_t rd_len      = len;
    int      ret = uart_read_fifo_with_ret(channel, &chipid, &rd_reg_addr, data_recv, &rd_len, 1);
    if (ret) {
        g_uart_errcnt[channel % HBOARD_COUNT]++;
        uart_soft_reset(channel);
    }
    return ret;
}

static int uart_vmask_hit(uint8_t channel)
{
    uint32_t *run = &g_uart_vmask_run[channel % HBOARD_COUNT];
    return ++(*run) <= (uint32_t)UART_SILENT_LOG_MAX;
}

static void uart_vmask_clear(uint8_t channel)
{
    uint32_t *run = &g_uart_vmask_run[channel % HBOARD_COUNT];
    if (*run > (uint32_t)UART_SILENT_LOG_MAX)
        osif_log(OSIF_LOG_WARN,
                "board %d: nonce version masks match again "
                "(%u mismatch lines suppressed)",
                channel, *run - (uint32_t)UART_SILENT_LOG_MAX);
    *run = 0;
}

static int get_vmask_id(uint8_t channel, uint32_t pvmask[], uint32_t vmask, int *missed)
{
    uint8_t i = 0;

    for (i = 0; i < 8; i++) {
        if (pvmask[i] == vmask)
            return i;
    }

    *missed = 1;
    if (uart_vmask_hit(channel))
        osif_log(OSIF_LOG_WARN,
                "board %d: nonce version mask 0x%08x matches "
                "none of the active masks 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x "
                "0x%08x 0x%08x 0x%08x; counted against mask 0",
                channel, vmask, pvmask[0], pvmask[1], pvmask[2], pvmask[3], pvmask[4], pvmask[5],
                pvmask[6], pvmask[7]);

    return 0;
}

int uart_get_nonce(uint8_t channel, struct toast_nonce *nonce, uint8_t *nonce_count,
        uint32_t *pvmask)
{
    uint8_t *p = NULL;
    uint32_t i, tmp32, nonce_num = 0;
    uint32_t nonce_buf[TOAST_NONCEBUF_FIFO_LEN];

    uint32_t chipid   = uart_sel_chip(channel);
    uint32_t tmp_len  = TOAST_NONCEBUF_FIFO_LEN;
    uint32_t reg_addr = TOAST_NONCEBUF_ADDR;
    int      ret      = 0;
    int      missed   = 0;

    uart_write_fifo(channel, reg_addr, NULL, TOAST_NONCEBUF_FIFO_LEN, UART_READ_MODE);
    ret = uart_read_fifo_with_ret(channel, &chipid, &reg_addr, nonce_buf, &tmp_len, 0);
    if (ret) {
        if (uart_silent_loud(channel))
            osif_log(OSIF_LOG_WARN, "board %d asic %u: nonce read failed", channel, chipid);
        goto ERR_RET;
    }

    if (tmp_len < 7) {
        if (tmp_len != 4) {
            osif_log(OSIF_LOG_WARN,
                    "board %d asic %u: nonce reply has an "
                    "unexpected length (%u words)",
                    channel, chipid, tmp_len);
            uart_dbg_hex("nonce noteerr", tmp_len, nonce_buf, tmp_len * 4);
            goto ERR_RET;
        }

        uart_dbg_hex("toast nonce", tmp_len * 4, nonce_buf, tmp_len * 4);

        *nonce_count = 0;
        return 0;
    }

    nonce_num    = (tmp_len * 4 - 16) / 10;
    *nonce_count = 0;

    p = (uint8_t *)&nonce_buf[4];

    for (i = 0; i < nonce_num;) {
        nonce[i].nonce2   = nonce_buf[0];
        nonce[i].job_id   = nonce_buf[1];
        tmp32             = bswap_32(nonce_buf[2]);
        nonce[i].asic_id  = tmp32;
        nonce[i].miner_id = tmp32 >> 16;
        nonce[i].nonce    = ((p[0] << 0) | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
        tmp32             = ((p[4] << 0) | (p[5] << 8) | (p[6] << 16) | (p[7] << 24));
        nonce[i].version  = tmp32;
        nonce[i].mid_id   = get_vmask_id(channel, pvmask, tmp32, &missed);
        nonce[i].ntime    = p[8];
        nonce[i].valid    = 1;

        i++;
        p += 10;
    }

    *nonce_count = nonce_num;
    if (!missed)
        uart_vmask_clear(channel);
    return 0;

ERR_RET:
    g_uart_errcnt[channel % HBOARD_COUNT]++;
    uart_soft_reset(channel);
    return 1;
}

void uart_write_baud_cmd(uint8_t channel, uint32_t baud)
{
    uint16_t m;
    uint8_t  tmp8_tx_buf[UART_HDR_CMD_LEN];
    uint32_t sel = uart_sel_chip(channel);

    m = 24000000 / baud;
    if (24000000 % baud)
        m += 1;

    uart_put_cmd(tmp8_tx_buf, sel & 0xff, (UART_BAUD_MODE << 2) | ((sel >> 8) & 0x3), m, 0);

    uart_write(channel, &tmp8_tx_buf[0], 10);

    uart_dbg_hex("TX", 10, tmp8_tx_buf, 10);
}

void uart_set_chiplast(uint8_t channel, uint8_t uart_chiplast)
{
    uint8_t  tmp8_tx_buf[UART_HDR_CMD_LEN];
    uint32_t sel = uart_sel_chip(channel);

    uart_put_cmd(tmp8_tx_buf, sel & 0xff, 0x0c | ((sel >> 8) & 0x3), uart_chiplast << 7, 0);

    uart_write(channel, tmp8_tx_buf, 10);
    uart_dbg_hex("TX", 10, tmp8_tx_buf, 10);
}
