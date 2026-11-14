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

#include "hex.h"
#include <stdio.h>
#include <string.h>

static int fails;

static void expect(const char *name, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "  %-32s FAIL: got %d want %d\n", name, got, want);
        fails++;
    } else {
        printf("  %-32s ok (%d)\n", name, got);
    }
}

static void test_nibble(void)
{
    const char *lower = "0123456789abcdef";
    const char *upper = "0123456789ABCDEF";
    int         bad   = 0;

    for (int i = 0; i < 16; i++) {
        if (hex_nibble(lower[i]) != i || hex_nibble(upper[i]) != i)
            bad++;
    }
    expect("nibble all digits", bad, 0);

    bad = 0;
    for (int c = 0; c < 256; c++) {
        int digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!digit && hex_nibble(c) != -1)
            bad++;
    }
    expect("nibble non-digits", bad, 0);

    expect("nibble '/'", hex_nibble('/'), -1);
    expect("nibble ':'", hex_nibble(':'), -1);
    expect("nibble '@'", hex_nibble('@'), -1);
    expect("nibble 'G'", hex_nibble('G'), -1);
    expect("nibble '`'", hex_nibble('`'), -1);
    expect("nibble 'g'", hex_nibble('g'), -1);
    expect("nibble NUL", hex_nibble('\0'), -1);
}

static void test_valid(void)
{
    expect("valid 0 chars", hex_valid("", 0), 1);
    expect("valid 0 chars of junk", hex_valid("zz", 0), 1);
    expect("valid aF09", hex_valid("aF09", 4), 1);
    expect("valid prefix only", hex_valid("aFxx", 2), 1);
    expect("valid last bad", hex_valid("aF0g", 4), 0);
    expect("valid NUL inside", hex_valid("ab\0cd", 5), 0);
    expect("valid past end", hex_valid("ab", 3), 0);
}

static void test_roundtrip(void)
{
    uint8_t all[256], back[256];
    char    lo[513], up[513];

    for (int i = 0; i < 256; i++)
        all[i] = (uint8_t)i;
    hex_encode(lo, all, sizeof all);
    hex_encode_upper(up, all, sizeof all);

    expect("encode len", (int)strlen(lo), 512);
    expect("encode_upper len", (int)strlen(up), 512);
    expect("encode 0x00..0x01", strncmp(lo, "0001", 4), 0);
    expect("encode 0xab", strncmp(lo + 2 * 0xab, "ab", 2), 0);
    expect("encode_upper 0xab", strncmp(up + 2 * 0xab, "AB", 2), 0);
    expect("encode 0xff", strcmp(lo + 510, "ff"), 0);
    expect("encode_upper 0xff", strcmp(up + 510, "FF"), 0);

    memset(back, 0, sizeof back);
    expect("decode lower", hex_decode(back, lo, sizeof back), 0);
    expect("roundtrip lower", memcmp(back, all, sizeof all), 0);
    memset(back, 0, sizeof back);
    expect("decode upper", hex_decode(back, up, sizeof back), 0);
    expect("roundtrip upper", memcmp(back, all, sizeof all), 0);

    char z[1] = { 'x' };
    hex_encode(z, all, 0);
    expect("encode 0 bytes writes NUL", z[0], '\0');
    z[0] = 'x';
    hex_encode_upper(z, all, 0);
    expect("encode_upper 0 bytes writes NUL", z[0], '\0');

    char    mixed[] = "aBcD";
    uint8_t m[2];
    expect("decode mixed case", hex_decode(m, mixed, 2), 0);
    expect("decode mixed value", m[0] == 0xab && m[1] == 0xcd, 1);
}

static void test_decode_stops(void)
{
    char    buf[4] = { 'a', 'b', '\0', 'Z' };
    uint8_t out[2] = { 0x55, 0x55 };

    expect("decode NUL hi", hex_decode(out, buf, 2), -1);
    expect("decode NUL hi first byte", out[0], 0xab);
    expect("decode NUL hi second untouched", out[1], 0x55);

    char odd[3] = { 'a', '\0', 'b' };
    out[0]      = 0x55;
    expect("decode NUL lo", hex_decode(out, odd, 1), -1);
    expect("decode NUL lo untouched", out[0], 0x55);

    expect("decode bad char", hex_decode(out, "0g", 1), -1);
    expect("decode 0 bytes", hex_decode(out, "", 0), 0);
}

static void test_decode_str(void)
{
    uint8_t d[4];
    int     untouched;

    expect("str NULL", hex_decode_str(d, sizeof d, NULL), 0);
    expect("str empty", hex_decode_str(d, sizeof d, ""), 0);
    expect("str empty cap 0", hex_decode_str(NULL, 0, ""), 0);

    memset(d, 0x5a, sizeof d);
    expect("str odd", hex_decode_str(d, sizeof d, "abc"), -1);
    expect("str \" f\"", hex_decode_str(d, sizeof d, " f"), -1);
    expect("str \"+f\"", hex_decode_str(d, sizeof d, "+f"), -1);
    expect("str \"-f\"", hex_decode_str(d, sizeof d, "-f"), -1);
    expect("str \"0x\"", hex_decode_str(d, sizeof d, "0x"), -1);
    expect("str bad tail", hex_decode_str(d, sizeof d, "abcdef0g"), -1);
    expect("str cap+1", hex_decode_str(d, sizeof d, "0102030405"), -1);
    untouched = 1;
    for (size_t i = 0; i < sizeof d; i++)
        if (d[i] != 0x5a)
            untouched = 0;
    expect("str failures leave dst", untouched, 1);

    expect("str exactly cap", hex_decode_str(d, sizeof d, "DeadBeef"), 4);
    expect("str exactly cap value", d[0] == 0xde && d[1] == 0xad && d[2] == 0xbe && d[3] == 0xef,
            1);

    memset(d, 0x5a, sizeof d);
    expect("str short", hex_decode_str(d, sizeof d, "0a"), 1);
    expect("str short value", d[0] == 0x0a && d[1] == 0x5a, 1);
}

int main(void)
{
    test_nibble();
    test_valid();
    test_roundtrip();
    test_decode_stops();
    test_decode_str();

    if (fails) {
        fprintf(stderr, "test_hex: %d FAIL\n", fails);
        return 1;
    }
    printf("test_hex: all pass\n");
    return 0;
}
