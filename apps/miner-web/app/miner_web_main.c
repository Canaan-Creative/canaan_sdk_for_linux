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

#include "web_server.h"
#include "web_logfmt.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    const char *port = argc > 1 ? argv[1] : "80";
    const char *cfg  = argc > 2 ? argv[2] : "/data/userconfig/systemcfg.ini";

    web_logf(stderr, "INFO", "web server starting\n");
    if (web_server_run(port, cfg) != 0) {
        web_logf(stderr, "ERR", "web server stopped\n");
        return 1;
    }
    return 0;
}
