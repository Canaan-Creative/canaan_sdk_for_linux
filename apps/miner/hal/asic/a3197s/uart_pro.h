/* SPDX-License-Identifier: MIT */
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

#ifndef UART_PRO_H
#define UART_PRO_H

#include <stdint.h>

#include "toast_port.h"

#define UART_TX_BUFFER_SIZE 256 /* transmit frame buffer, bytes */
#define UART_RX_BUFFER_SIZE 256 /* receive frame buffer, bytes */
/* chip link baud rate before the baud command, bit/s */
#define UART_DEFAULT_BAUD_RATE 115200
/* chip link baud rate after the baud command, bit/s */
#define UART_HIGH_BAUD_RATE 4800000
/* repeats of one chip link warning logged before the rest are suppressed, count */
#define UART_SILENT_LOG_MAX 8

enum uart_mode {
    UART_READ_MODE  = 0,
    UART_WRITE_MODE = 1,
    UART_BAUD_MODE  = 2, /* baud rate divider set */
    UART_INIT_MODE  = 3, /* chain init: chip address assignment, chain-last flag */
};

void     uart_init(uint8_t channel, uint32_t baud);
void     uart_config_baud(uint8_t channel, uint32_t baud);
int      uart_read(uint8_t channel, uint8_t *buf, uint16_t len);
int      uart_write(uint8_t channel, uint8_t *buf, uint16_t len);
void     uart_clear_rxfifo(uint8_t channal);
int      uart_tx_empty(uint8_t channel);
void     uart_write_baud_cmd(uint8_t channel, uint32_t baud);
void     uart_chip_select(uint8_t channel, uint16_t chip_addr);
void     uart_set_chiplast(uint8_t channel, uint8_t uart_chiplast);
uint16_t uart_get_chip_select(uint8_t channel);
int      uart_get_nonce(uint8_t channel, struct toast_nonce *nonce, uint8_t *nonce_count,
             uint32_t *pvmask);
int      uart_read_fifo(uint8_t channel, uint32_t reg_addr, uint32_t *data_recv, uint32_t len);
int      uart_write_fifo(uint8_t channel, uint32_t reg_addr, uint32_t *data_send, uint32_t len,
             enum uart_mode mode);
void     uart_soft_reset(uint8_t channel);
uint32_t uart_get_errcnt(uint8_t channel);
void     uart_clear_errcnt(uint8_t channel);

#endif /* UART_PRO_H */
