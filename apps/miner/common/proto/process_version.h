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

#ifndef PROCESS_VERSION_H
#define PROCESS_VERSION_H

#include <stdint.h>
#include <string.h>

/* version string buffer, bytes, including NUL */
#define MINER_VERSION_CAP     64
#define MINER_VERSION_UNKNOWN "00000000_0000_unknown"
#ifndef CORE_MINER_VERSION
#error "CORE_MINER_VERSION must be defined by the build"
#endif
#ifndef CORE_MINER_SEMVER
#error "CORE_MINER_SEMVER must be defined by the build"
#endif
#ifndef MM_MINER_VERSION
#error "MM_MINER_VERSION must be defined by the build"
#endif
#ifndef ASIC_MINER_VERSION
#error "ASIC_MINER_VERSION must be defined by the build"
#endif

typedef struct {
    char     value[MINER_VERSION_CAP]; /* version string, NUL-terminated; empty if none received */
    uint64_t received_at;              /* receive time, s */
} miner_version_state;

static inline int miner_version_commit_valid(const char *commit, size_t len)
{
    if (len == 7 && !memcmp(commit, "unknown", 7))
        return 1;
    if (len && commit[len - 1] == 't')
        --len;
    if (!len)
        return 0;
    for (size_t index = 0; index < len; ++index) {
        char byte = commit[index];
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
            return 0;
    }
    return 1;
}

static inline int miner_version_receive(miner_version_state *state, const void *data, int len,
        uint64_t now)
{
    if (!data || len < 16 || len > MINER_VERSION_CAP)
        return 0;
    const char *value = data;
    if (value[8] != '_' || value[13] != '_' || value[len - 1] != '\0')
        return 0;
    if (strlen(value) != (size_t)(len - 1))
        return 0;
    for (int index = 0; index < 13; ++index)
        if (index != 8 && (value[index] < '0' || value[index] > '9'))
            return 0;
    for (const char *commit = value + 14;;) {
        const char *end        = strchr(commit, '_');
        size_t      commit_len = end ? (size_t)(end - commit) : strlen(commit);
        if (!miner_version_commit_valid(commit, commit_len))
            return 0;
        if (!end)
            break;
        commit = end + 1;
    }
    memcpy(state->value, value, (size_t)len);
    state->received_at = now;
    return 1;
}

static inline const char *miner_version_get(const miner_version_state *state, uint64_t now)
{
    return state->value[0] && now >= state->received_at && now - state->received_at <= 10 ?
            state->value :
            MINER_VERSION_UNKNOWN;
}

#endif /* PROCESS_VERSION_H */
