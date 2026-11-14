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

#include "process_version.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    miner_version_state state    = { 0 };
    const char          first[]  = "26091101_2138_1111111";
    const char          second[] = "26091607_2138_2222222";
    assert(!strcmp(miner_version_get(&state, 1), MINER_VERSION_UNKNOWN));
    assert(miner_version_receive(&state, first, sizeof first, 10));
    assert(!strcmp(miner_version_get(&state, 15), first));
    assert(!strcmp(miner_version_get(&state, 21), MINER_VERSION_UNKNOWN));
    assert(!miner_version_receive(&state, first, sizeof first - 1, 22));
    assert(!miner_version_receive(&state, NULL, 1, 22));
    assert(!miner_version_receive(&state, first, -1, 22));
    char oversized[MINER_VERSION_CAP + 1];
    memset(oversized, 'a', sizeof oversized);
    assert(!miner_version_receive(&state, oversized, sizeof oversized, 22));
    char maximum[MINER_VERSION_CAP];
    memset(maximum, 'a', sizeof maximum);
    memcpy(maximum, "26091601_2138_", 14);
    maximum[sizeof maximum - 1] = '\0';
    assert(miner_version_receive(&state, maximum, sizeof maximum, 22));
    assert(!strcmp(miner_version_get(&state, 22), maximum));
    assert(!miner_version_receive(&state, "bad]", 5, 22));
    assert(!miner_version_receive(&state, "0.3.57", 7, 22));
    assert(!miner_version_receive(&state, "26091601_2138_a\0b", 18, 22));
    assert(!miner_version_receive(&state, "2609160a_2138_0.3.57", 21, 22));
    assert(!miner_version_receive(&state, "2609160_2138_0.3.57", 20, 22));
    assert(!miner_version_receive(&state, "260916010_2138_0.3.57", 22, 22));
    assert(miner_version_receive(&state, second, sizeof second, 23));
    assert(!strcmp(miner_version_get(&state, 23), second));
    const char clean[] = "26091598_2138_63395d2";
    assert(miner_version_receive(&state, clean, sizeof clean, 24));
    assert(!strcmp(miner_version_get(&state, 24), clean));
    const char dirty[] = "26091598_2138_63395d2t";
    assert(miner_version_receive(&state, dirty, sizeof dirty, 25));
    assert(!strcmp(miner_version_get(&state, 25), dirty));
    const char tarball[] = "26091598_2138_unknown";
    assert(miner_version_receive(&state, tarball, sizeof tarball, 26));
    assert(!strcmp(miner_version_get(&state, 26), tarball));
    const char shortest[] = "26091598_2138_0";
    assert(miner_version_receive(&state, shortest, sizeof shortest, 27));
    assert(!strcmp(miner_version_get(&state, 27), shortest));
    const char shortest_dirty[] = "26091598_2138_0t";
    assert(miner_version_receive(&state, shortest_dirty, sizeof shortest_dirty, 28));
    assert(!strcmp(miner_version_get(&state, 28), shortest_dirty));
    const char two[] = "26091598_2138_0123abcd_89abcde";
    assert(miner_version_receive(&state, two, sizeof two, 28));
    assert(!strcmp(miner_version_get(&state, 28), two));
    const char two_dirty[] = "26091598_2138_0123abcdt_89abcdet";
    assert(miner_version_receive(&state, two_dirty, sizeof two_dirty, 28));
    assert(!strcmp(miner_version_get(&state, 28), two_dirty));
    const char second_dirty[] = "26091598_2138_0123abcd_89abcdet";
    assert(miner_version_receive(&state, second_dirty, sizeof second_dirty, 28));
    const char first_dirty[] = "26091598_2138_0123abcdt_89abcde";
    assert(miner_version_receive(&state, first_dirty, sizeof first_dirty, 28));
    const char second_unknown[] = "26091598_2138_0123abcd_unknown";
    assert(miner_version_receive(&state, second_unknown, sizeof second_unknown, 28));
    const char first_unknown[] = "26091598_2138_unknown_89abcdet";
    assert(miner_version_receive(&state, first_unknown, sizeof first_unknown, 28));
    const char three[] = "26091598_2138_63395d2_89abcdef_1t";
    assert(miner_version_receive(&state, three, sizeof three, 28));
    assert(!strcmp(miner_version_get(&state, 28), three));
    assert(miner_version_receive(&state, shortest_dirty, sizeof shortest_dirty, 28));
    assert(!miner_version_receive(&state, "26091601_2138__89abcdef", 24, 29));
    assert(!miner_version_receive(&state, "26091601_2138_t_89abcdef", 25, 29));
    assert(!miner_version_receive(&state, "26091601_2138_63395d2_t", 24, 29));
    assert(!miner_version_receive(&state, "26091601_2138_63395d2_89abcdett", 32, 29));
    assert(!miner_version_receive(&state, "26091601_2138_63395d2_89abcdO3", 31, 29));
    assert(!miner_version_receive(&state, "26091601_2138_63395d2_89abcdef_", 32, 29));
    assert(!miner_version_receive(&state, "26091601_2138_63395d2_89abcdef_1g", 34, 29));
    assert(!miner_version_receive(&state, "26091601_2138_63395d2_89abcdef__1", 34, 29));
    assert(!miner_version_receive(&state, "26091601_2138_63395d2t_89abtcdef", 33, 29));
    assert(!miner_version_receive(&state, "26091601_2138_63395d2__89abcdef", 32, 29));
    assert(!miner_version_receive(&state, "26091601_2138_63395d2t-89abcdef", 32, 29));
    assert(!miner_version_receive(&state, "26091601_2138_0.3.58", 21, 29));
    assert(!miner_version_receive(&state, "26091601_63395d2", 17, 29));
    assert(!miner_version_receive(&state, "26091601_21x8_abcdef0", 22, 29));
    assert(!miner_version_receive(&state, "25010100_202609102037_1111111_9.9.8", 36, 29));
    assert(!miner_version_receive(&state, "25010100_abcdef0_0.3.57", 24, 29));
    assert(!miner_version_receive(&state, "26091601_2138_63395D2", 22, 29));
    assert(!miner_version_receive(&state, "26091601_2138_63395g2", 22, 29));
    assert(!miner_version_receive(&state, "26091601_2138_63395d2-", 23, 29));
    assert(!miner_version_receive(&state, "26091601_2138_6t395d2", 22, 29));
    assert(!miner_version_receive(&state, "26091601_2138_t", 16, 29));
    assert(!miner_version_receive(&state, "26091601_2138_tt", 17, 29));
    assert(!miner_version_receive(&state, "26091601_2138_unknownt", 23, 29));
    assert(!miner_version_receive(&state, "26091601_2138_UNKNOWN", 22, 29));
    assert(!miner_version_receive(&state, "26091601_2138_unknown\0x", 24, 29));
    assert(!strcmp(miner_version_get(&state, 28), shortest_dirty));
    return 0;
}
