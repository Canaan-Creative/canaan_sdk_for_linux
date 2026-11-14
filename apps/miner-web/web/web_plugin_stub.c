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

#include "web_plugin.h"

int web_plugin_file(const char *name, char *path, size_t cap, int *concat)
{
    (void)name;
    (void)path;
    (void)cap;
    (void)concat;
    return -1;
}

const web_rest_resource *web_plugin_rest(size_t *n)
{
    *n = 0;
    return NULL;
}
