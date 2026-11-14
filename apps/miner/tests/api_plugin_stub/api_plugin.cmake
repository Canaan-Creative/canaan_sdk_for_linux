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

target_sources(core_miner PRIVATE ${CMAKE_CURRENT_LIST_DIR}/api_plugin_stub.c)
target_include_directories(core_miner PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_sources(mc_ctrl_args PRIVATE ${CMAKE_CURRENT_LIST_DIR}/ctrl_plugin_stub.c)
