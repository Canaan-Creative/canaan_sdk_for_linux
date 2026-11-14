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

#include "a3197s_decode.h"
#include "miner_ipc.h"

#define A3197S_T_A1 2.28746e-10    /* PVT temperature polynomial, code^4 coefficient */
#define A3197S_T_A2 (-2.84236e-06) /* PVT temperature polynomial, code^3 coefficient */
#define A3197S_T_A3 1.31753e-02    /* PVT temperature polynomial, code^2 coefficient */
#define A3197S_T_A4 (-26.70380)    /* PVT temperature polynomial, code^1 coefficient */
#define A3197S_T_A5 19725.61       /* PVT temperature polynomial, constant term */

#define A3197S_T_MIN (-50.0) /* lowest valid decoded temperature, degC */
#define A3197S_T_MAX (200.0) /* highest valid decoded temperature, degC */

double decode_pvt_temp(uint16_t pvt_code)
{
    double c = (double)pvt_code;
    double t = (((A3197S_T_A1 * c + A3197S_T_A2) * c + A3197S_T_A3) * c + A3197S_T_A4) * c +
            A3197S_T_A5;
    if (t < A3197S_T_MIN || t > A3197S_T_MAX)
        return (double)ASIC_TEMP_NONE;
    return t;
}

uint16_t decode_pvt_volt(uint16_t volt)
{
    double mv = ((double)volt * 1000.0 + 19144.2) / 3434.4847;
    if (mv < 14.0)
        return 0;
    return (uint16_t)(mv + 0.5);
}
