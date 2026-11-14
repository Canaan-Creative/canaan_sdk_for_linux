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

macro(miner_board_register board conf tables gendir)
    set(MINER_BOARD_CONF_${board} ${conf})
    set(MINER_BOARD_TABLES_${board} ${tables})
    set(MINER_BOARD_GENDIR_${board} ${gendir})
    add_custom_target(gen_boardconf_${board} DEPENDS ${conf})
endmacro()

macro(miner_board_select board out_tables out_gendir)
    if(NOT DEFINED MINER_BOARD_CONF_${board})
        message(FATAL_ERROR
            "board '${board}' has no descriptor registered in this test suite: "
            "call miner_board_register(${board} <boardconf.h> <board_tables.c> <gen dir>) "
            "beside the other boards, and add an add_custom_command generating that boardconf.h")
    endif()
    set(${out_tables} ${MINER_BOARD_TABLES_${board}})
    set(${out_gendir} ${MINER_BOARD_GENDIR_${board}})
endmacro()
