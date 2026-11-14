# SPDX-License-Identifier: MIT
# Copyright (c) 2026, Canaan Creative Co., Ltd.
#
# For the licensing terms, see the LICENSE file (MIT License).
#
# NO WARRANTY. The Software is provided "AS IS", without warranty of any kind, express or
# implied, including but not limited to the warranties of merchantability, fitness for a
# particular purpose and non-infringement. In no event shall the authors or copyright
# holders be liable for any claim, damages or other liability arising from, out of or in
# connection with the Software or the use or other dealings in the Software.

set(MINER_PROTO_VER "1.5" CACHE STRING "API protocol version, major.minor")
if(NOT MINER_PROTO_VER MATCHES "^[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "MINER_PROTO_VER must be a major.minor version")
endif()
