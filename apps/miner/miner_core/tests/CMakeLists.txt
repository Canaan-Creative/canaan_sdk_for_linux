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

cmake_minimum_required(VERSION 3.16)
project(miner_core_tests C)
set(CTRL_CMDS_C ${CMAKE_CURRENT_LIST_DIR}/../../common/proto/ctrl_cmds.c)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../../common/proto)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../../common/log)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../../common/util)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror=format -Wno-error=format-truncation -O2)
add_compile_options(-UNDEBUG)
set(TST_SKIP 77)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/MinerProtoVer.cmake)
add_compile_definitions(TST_SKIP=${TST_SKIP} MINER_PROTO_VER="${MINER_PROTO_VER}"
    FW_TYPE="$<IF:$<BOOL:$<TARGET_PROPERTY:FW_TYPE>>,$<TARGET_PROPERTY:FW_TYPE>,Debug>")
add_compile_definitions(CORE_MINER_VERSION="00000000_0000_unknown"
    CORE_MINER_SEMVER="0.0.0"
    MM_MINER_VERSION="00000000_0000_unknown"
    ASIC_MINER_VERSION="00000000_0000_unknown")

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(JSON_DIR ${CORE_DIR}/../common/json)

enable_testing()
find_package(OpenSSL REQUIRED)
link_libraries(OpenSSL::Crypto)
include(${CORE_DIR}/../cmake/AupPubkey.cmake)

set(PROD_DIR ${CORE_DIR}/../../../product/k230/miner)
set(GEN_BOARDCONF ${CORE_DIR}/../tools/gen_boardconf.py)
set(BOARDCONF_H ${CMAKE_CURRENT_BINARY_DIR}/gen/boardconf.h)
set(BOARDCONF_T ${CORE_DIR}/../hal/board/board_tables.c)
include_directories(${CORE_DIR}/../hal/board)
include_directories(${CORE_DIR}/../tests/fixtures)
include_directories(${CORE_DIR}/../hal/platform/linux)
add_custom_command(OUTPUT ${BOARDCONF_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${GEN_BOARDCONF}
        ${PROD_DIR}/avalon-home/nano3s/conf/product/boardconf.ini ${BOARDCONF_H}
    DEPENDS ${PROD_DIR}/avalon-home/nano3s/conf/product/boardconf.ini ${GEN_BOARDCONF}
    COMMENT "gen nano3s boardconf.h")
set(BOARDCONF_X3_H ${CMAKE_CURRENT_BINARY_DIR}/gen_x3/boardconf.h)
set(BOARDCONF_X3_T ${CORE_DIR}/../hal/board/board_tables.c)
add_custom_command(OUTPUT ${BOARDCONF_X3_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen_x3
    COMMAND python3 ${GEN_BOARDCONF}
        ${CORE_DIR}/../tests/fixtures/boardconf_x3.ini ${BOARDCONF_X3_H}
    DEPENDS ${CORE_DIR}/../tests/fixtures/boardconf_x3.ini ${GEN_BOARDCONF}
    COMMENT "gen x3 boardconf.h")
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(GEN_X3_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen_x3)

include(${CORE_DIR}/../cmake/MinerTestBoards.cmake)
miner_board_register(nano3s ${BOARDCONF_H} ${BOARDCONF_T} ${GEN_DIR})
miner_board_register(x3 ${BOARDCONF_X3_H} ${BOARDCONF_X3_T} ${GEN_X3_DIR})

set(MINER_TEST_BOARDS nano3s x3)

add_executable(test_nonce_ring test_nonce_ring.c ${CORE_DIR}/nonce_ring.c)
target_include_directories(test_nonce_ring PRIVATE ${CORE_DIR})
add_test(NAME nonce_ring COMMAND test_nonce_ring)

add_executable(test_work test_work.c ${CORE_DIR}/work.c ${CORE_DIR}/../common/sha/sha256.c)
target_include_directories(test_work PRIVATE ${CORE_DIR} ${CORE_DIR}/../common/sha)
target_link_libraries(test_work PRIVATE m)
target_compile_definitions(test_work PRIVATE
    STRATUM_FIXTURES_DIR="${CORE_DIR}/../tests/harness/fixtures")
add_test(NAME work COMMAND test_work)

add_executable(test_share test_share.c ${CORE_DIR}/share.c)
target_include_directories(test_share PRIVATE ${CORE_DIR})
add_test(NAME share COMMAND test_share)

add_executable(test_pipeline test_pipeline.c
    ${CORE_DIR}/pipeline.c ${CORE_DIR}/share.c
    ${CORE_DIR}/stratum/sv1.c ${JSON_DIR}/jread.c)
target_include_directories(test_pipeline PRIVATE
    ${CORE_DIR} ${CORE_DIR}/stratum ${JSON_DIR} ${CORE_DIR}/../common/sha
    ${CORE_DIR}/../common/proto)
target_compile_definitions(test_pipeline PRIVATE
    STRATUM_FIXTURES_DIR="${CORE_DIR}/../tests/harness/fixtures")
target_link_libraries(test_pipeline PRIVATE pthread)
add_test(NAME pipeline COMMAND test_pipeline)

set(STRATUM_DIR ${CORE_DIR}/stratum)
set(POOL_DIR ${CORE_DIR}/../common/pool)
set(CFGWR_DIR ${CORE_DIR}/../common/cfgwr)
include_directories(${POOL_DIR} ${CFGWR_DIR})
set(FIXTURES_DIR ${CORE_DIR}/../tests/harness/fixtures)


add_executable(test_sv1 test_sv1.c ${STRATUM_DIR}/sv1.c ${JSON_DIR}/jread.c)
target_include_directories(test_sv1 PRIVATE ${STRATUM_DIR} ${JSON_DIR})
target_compile_definitions(test_sv1 PRIVATE STRATUM_FIXTURES_DIR="${FIXTURES_DIR}")
target_compile_options(test_sv1 PRIVATE -fno-builtin-malloc -fno-builtin-free -fno-builtin-strdup)
target_link_libraries(test_sv1 PRIVATE pthread)
add_test(NAME sv1 COMMAND test_sv1)

add_executable(test_pool_mgr test_pool_mgr.c ${POOL_DIR}/pool.c)
target_include_directories(test_pool_mgr PRIVATE ${STRATUM_DIR})
add_test(NAME pool_mgr COMMAND test_pool_mgr)

add_executable(test_poolid test_poolid.c ${CORE_DIR}/api/mc_api_poolid.c)
target_include_directories(test_poolid PRIVATE ${CORE_DIR}/api)
add_test(NAME poolid COMMAND test_poolid)

add_executable(test_addpool test_addpool.c ${CORE_DIR}/api/mc_api_addpool.c)
target_include_directories(test_addpool PRIVATE ${CORE_DIR}/api ${STRATUM_DIR})
add_test(NAME addpool COMMAND test_addpool)

add_executable(test_poollist test_poollist.c ${CORE_DIR}/api/mc_api_poollist.c)
target_include_directories(test_poollist PRIVATE ${CORE_DIR}/api ${STRATUM_DIR})
add_test(NAME poollist COMMAND test_poollist)

add_executable(test_pool_config test_pool_config.c
    ${POOL_DIR}/pool_config.c ${POOL_DIR}/pool.c
    ${JSON_DIR}/jread.c ${JSON_DIR}/jwrite.c)
target_include_directories(test_pool_config PRIVATE ${STRATUM_DIR} ${JSON_DIR})
target_compile_definitions(test_pool_config PRIVATE CONFIG_FIXTURES_DIR="${FIXTURES_DIR}/config")
add_test(NAME pool_config COMMAND test_pool_config)

add_executable(test_stratum_net test_stratum_net.c
    ${STRATUM_DIR}/stratum_net.c ${STRATUM_DIR}/sv1.c ${POOL_DIR}/pool.c
    ${JSON_DIR}/jread.c)
target_include_directories(test_stratum_net PRIVATE ${STRATUM_DIR} ${JSON_DIR} ${CORE_DIR}/../osif)
target_compile_definitions(test_stratum_net PRIVATE STRATUM_FIXTURES_DIR="${FIXTURES_DIR}")
target_link_libraries(test_stratum_net PRIVATE pthread)
add_test(NAME stratum_net COMMAND test_stratum_net)

add_executable(test_driver_avalon test_driver_avalon.c ${CORE_DIR}/driver_avalon.c
    ${CORE_DIR}/avalon_gen.c ${CORE_DIR}/work.c ${CORE_DIR}/../common/sha/sha256.c)
target_include_directories(test_driver_avalon PRIVATE
    ${CORE_DIR} ${CORE_DIR}/../common/proto ${CORE_DIR}/../hal ${CORE_DIR}/../hal/asic/a3197s
    ${CORE_DIR}/../common/sha ${GEN_DIR})
add_dependencies(test_driver_avalon gen_boardconf_nano3s)
target_link_libraries(test_driver_avalon PRIVATE m)
add_test(NAME driver_avalon COMMAND test_driver_avalon)

set(API_DIR ${CORE_DIR}/api)
set(OSIF_LOG_C ${CORE_DIR}/../osif/linux/osif_log_linux.c)
set(API_CMDS_SRC test_api_cmds.c
    ${API_DIR}/mc_api_cmds.c ${API_DIR}/mc_api_cmds_json.c ${API_DIR}/mc_api_cmds_text.c
    ${API_DIR}/mc_api_estats.c ${API_DIR}/mc_api_lcd.c ${API_DIR}/mc_api_command.c
    ${API_DIR}/mc_api_identity.c ${API_DIR}/mc_api_setter.c ${JSON_DIR}/jread.c
    ${API_DIR}/mc_api_qrlogin.c ${CORE_DIR}/../hal/platform/linux/net_hwaddr.c
    ${API_DIR}/mc_api_time.c
    ${CORE_DIR}/../common/json/jwrite.c ${OSIF_LOG_C})
set(API_SERVER_SRC test_api_server.c
    ${API_DIR}/mc_api_server.c ${API_DIR}/mc_api_cmds.c ${API_DIR}/mc_api_cmds_json.c
    ${API_DIR}/mc_api_cmds_text.c ${API_DIR}/mc_api_estats.c ${API_DIR}/mc_api_lcd.c
    ${API_DIR}/mc_api_command.c
    ${API_DIR}/mc_api_setter.c ${JSON_DIR}/jread.c ${API_DIR}/mc_api_qrlogin.c
    ${API_DIR}/mc_api_time.c
    ${CORE_DIR}/../common/json/jwrite.c ${OSIF_LOG_C})
set(API_INC ${API_DIR} ${CORE_DIR}/../common/json ${CORE_DIR}/../common/proto ${CORE_DIR}/../osif)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    foreach(TGT api_cmds_${BOARD} api_cmds_${BOARD}_lcdoff)
        add_executable(test_${TGT} ${API_CMDS_SRC}
            ${CTRL_CMDS_C} ${BCT})
        add_dependencies(test_${TGT} gen_boardconf_${BOARD})
        target_include_directories(test_${TGT} PRIVATE ${API_INC} ${BCDIR}
            ${CORE_DIR}/../hal/board)
        add_test(NAME ${TGT} COMMAND test_${TGT})
    endforeach()
    target_include_directories(test_api_cmds_${BOARD}_lcdoff BEFORE PRIVATE
        ${CORE_DIR}/../tests/fixtures/cap_off)
    target_compile_definitions(test_api_cmds_${BOARD}_lcdoff PRIVATE TST_CAP_LCD_OFF)
endforeach()

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_api_server_${BOARD} ${API_SERVER_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_api_server_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_api_server_${BOARD} PRIVATE ${API_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    add_test(NAME api_server_${BOARD} COMMAND test_api_server_${BOARD})
endforeach()

add_executable(test_api_accept test_api_accept.c
    ${API_DIR}/mc_api_cmds.c ${API_DIR}/mc_api_cmds_json.c ${API_DIR}/mc_api_cmds_text.c
    ${API_DIR}/mc_api_estats.c ${API_DIR}/mc_api_lcd.c ${API_DIR}/mc_api_command.c
    ${API_DIR}/mc_api_setter.c ${JSON_DIR}/jread.c ${API_DIR}/mc_api_qrlogin.c
    ${API_DIR}/mc_api_time.c
    ${CORE_DIR}/../app/core_ctrl.c
    ${CORE_DIR}/../common/json/jwrite.c ${OSIF_LOG_C}
    ${CTRL_CMDS_C} ${BOARDCONF_T})
add_dependencies(test_api_accept gen_boardconf_nano3s)
target_include_directories(test_api_accept PRIVATE ${API_INC} ${GEN_DIR}
    ${CORE_DIR}/../app ${CORE_DIR}/../hal/board)
target_link_libraries(test_api_accept PRIVATE pthread)
add_test(NAME api_accept COMMAND test_api_accept)

add_executable(test_api_read test_api_read.c
    ${API_DIR}/mc_api_cmds.c ${API_DIR}/mc_api_cmds_json.c ${API_DIR}/mc_api_cmds_text.c
    ${API_DIR}/mc_api_estats.c ${API_DIR}/mc_api_lcd.c ${API_DIR}/mc_api_command.c
    ${API_DIR}/mc_api_setter.c ${JSON_DIR}/jread.c ${API_DIR}/mc_api_qrlogin.c
    ${API_DIR}/mc_api_time.c
    ${CORE_DIR}/../common/json/jwrite.c ${OSIF_LOG_C}
    ${CTRL_CMDS_C} ${BOARDCONF_T})
add_dependencies(test_api_read gen_boardconf_nano3s)
target_include_directories(test_api_read PRIVATE ${API_INC} ${GEN_DIR}
    ${CORE_DIR}/../hal/board)
target_link_libraries(test_api_read PRIVATE pthread)
add_test(NAME api_read COMMAND test_api_read)

add_executable(test_api_command_log test_api_command_log.c ${API_DIR}/mc_api_command.c)
add_dependencies(test_api_command_log gen_boardconf_nano3s)
target_include_directories(test_api_command_log PRIVATE ${API_INC} ${GEN_DIR})
add_test(NAME api_command_log COMMAND test_api_command_log)

add_executable(test_setpool test_setpool.c
    ${API_DIR}/mc_api_setpool.c
    ${POOL_DIR}/pool_config.c ${POOL_DIR}/pool.c
    ${JSON_DIR}/jread.c ${JSON_DIR}/jwrite.c)
target_include_directories(test_setpool PRIVATE ${API_DIR} ${STRATUM_DIR} ${JSON_DIR}
    ${CORE_DIR}/../common/proto)
add_test(NAME setpool COMMAND test_setpool)

add_executable(test_setter test_setter.c
    ${API_DIR}/mc_api_server.c ${API_DIR}/mc_api_cmds.c ${API_DIR}/mc_api_cmds_json.c
    ${API_DIR}/mc_api_cmds_text.c ${API_DIR}/mc_api_estats.c ${API_DIR}/mc_api_lcd.c
    ${API_DIR}/mc_api_command.c
    ${API_DIR}/mc_api_setter.c ${JSON_DIR}/jread.c ${API_DIR}/mc_api_qrlogin.c
    ${API_DIR}/mc_api_time.c
    ${CORE_DIR}/../common/json/jwrite.c ${OSIF_LOG_C}
    ${CTRL_CMDS_C} ${BOARDCONF_T})
add_dependencies(test_setter gen_boardconf_nano3s)
target_include_directories(test_setter PRIVATE ${API_DIR} ${CORE_DIR}/../common/json
    ${CORE_DIR}/../common/proto ${CORE_DIR}/../osif ${GEN_DIR}
    ${CORE_DIR}/../hal/board)
add_test(NAME setter COMMAND test_setter)

set(CORE_WALLPAPER_SRC test_core_wallpaper.c
    ${CORE_DIR}/../app/core_sigverify.c
    ${API_DIR}/mc_api_setter.c ${JSON_DIR}/jread.c
    ${CORE_DIR}/../common/sha/sha256.c
    ${CORE_DIR}/../common/proto/ctrl_cmds.c
    ${CORE_DIR}/../app/ctrl_args.c
    ${BOARDCONF_T})
set(CORE_WALLPAPER_INC ${API_DIR} ${CORE_DIR}/../app ${CORE_DIR}/../osif
    ${CORE_DIR}/../common/sha ${CORE_DIR}/../common/proto ${JSON_DIR}
    ${CORE_DIR}/stratum
    ${CORE_DIR}/../hal/board ${CORE_DIR}/../miner_mgmt/cali
    ${CORE_DIR}/../miner_mgmt/ctrl ${GEN_DIR})

add_executable(test_core_wallpaper ${CORE_WALLPAPER_SRC})
add_dependencies(test_core_wallpaper gen_boardconf_nano3s)
target_include_directories(test_core_wallpaper PRIVATE ${CORE_WALLPAPER_INC})
pic_pubkey_header(test_core_wallpaper ${CORE_DIR}/../miner_mgmt/tests/fixtures/aup-test-key.pub.pem)
target_compile_definitions(test_core_wallpaper PRIVATE
    PIC_SIGN_KEY="${CORE_DIR}/../miner_mgmt/tests/fixtures/aup-test-key.pem")
add_test(NAME core_wallpaper COMMAND test_core_wallpaper)

add_executable(test_time test_time.c
    ${API_DIR}/mc_api_server.c ${API_DIR}/mc_api_cmds.c ${API_DIR}/mc_api_cmds_json.c
    ${API_DIR}/mc_api_cmds_text.c ${API_DIR}/mc_api_estats.c ${API_DIR}/mc_api_lcd.c
    ${API_DIR}/mc_api_command.c
    ${API_DIR}/mc_api_setter.c ${JSON_DIR}/jread.c ${API_DIR}/mc_api_qrlogin.c
    ${CORE_DIR}/../common/json/jwrite.c ${OSIF_LOG_C}
    ${CTRL_CMDS_C} ${BOARDCONF_T})
add_dependencies(test_time gen_boardconf_nano3s)
target_include_directories(test_time PRIVATE ${API_DIR} ${CORE_DIR}/../common/json
    ${CORE_DIR}/../common/proto ${CORE_DIR}/../osif ${GEN_DIR}
    ${CORE_DIR}/../hal/board)
add_test(NAME time COMMAND test_time)

set(API_ESTATS_SRC test_api_estats_width.c
    ${API_DIR}/mc_api_cmds.c ${API_DIR}/mc_api_cmds_json.c ${API_DIR}/mc_api_cmds_text.c
    ${API_DIR}/mc_api_estats.c ${API_DIR}/mc_api_lcd.c ${API_DIR}/mc_api_command.c
    ${API_DIR}/mc_api_identity.c ${API_DIR}/mc_api_setter.c ${JSON_DIR}/jread.c
    ${API_DIR}/mc_api_qrlogin.c ${CORE_DIR}/../hal/platform/linux/net_hwaddr.c
    ${API_DIR}/mc_api_time.c
    ${CORE_DIR}/../common/json/jwrite.c ${OSIF_LOG_C})

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_api_estats_width_${BOARD} ${API_ESTATS_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_api_estats_width_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_api_estats_width_${BOARD} PRIVATE
        ${API_DIR} ${CORE_DIR}/../common/json ${CORE_DIR}/../common/proto ${CORE_DIR}/../osif
        ${BCDIR} ${CORE_DIR}/../hal/board)
    add_test(NAME api_estats_width_${BOARD} COMMAND test_api_estats_width_${BOARD})
endforeach()

set(API_IDENT_SRC test_api_identity.c
    ${API_DIR}/mc_api_cmds.c ${API_DIR}/mc_api_cmds_json.c ${API_DIR}/mc_api_cmds_text.c
    ${API_DIR}/mc_api_estats.c ${API_DIR}/mc_api_lcd.c ${API_DIR}/mc_api_command.c
    ${API_DIR}/mc_api_identity.c ${API_DIR}/mc_api_setter.c ${JSON_DIR}/jread.c
    ${API_DIR}/mc_api_qrlogin.c ${CORE_DIR}/../hal/platform/linux/net_hwaddr.c
    ${API_DIR}/mc_api_time.c
    ${CORE_DIR}/../common/json/jwrite.c ${OSIF_LOG_C})

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_api_identity_${BOARD} ${API_IDENT_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_api_identity_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_api_identity_${BOARD} PRIVATE
        ${API_DIR} ${CORE_DIR}/../common/json ${CORE_DIR}/../common/proto ${CORE_DIR}/../osif
        ${BCDIR} ${CORE_DIR}/../hal/board)
    add_test(NAME api_identity_${BOARD} COMMAND test_api_identity_${BOARD})
endforeach()

foreach(TYPE Release Customized)
    foreach(BOARD ${MINER_TEST_BOARDS})
        miner_board_select(${BOARD} IDENT_BT IDENT_GEN)
        set(IDENT_TARGET test_api_identity_${BOARD}_${TYPE})
        add_executable(${IDENT_TARGET} ${API_IDENT_SRC} ${IDENT_BT})
        add_dependencies(${IDENT_TARGET} gen_boardconf_${BOARD})
        target_include_directories(${IDENT_TARGET} PRIVATE ${API_INC} ${IDENT_GEN}
            ${CORE_DIR}/../hal/board)
        set_target_properties(${IDENT_TARGET} PROPERTIES FW_TYPE ${TYPE})
        target_compile_definitions(${IDENT_TARGET} PRIVATE FWVERSION="26090890_deadbee")
        add_test(NAME api_identity_${BOARD}_${TYPE} COMMAND ${IDENT_TARGET})
    endforeach()
endforeach()

# Linked beside core_miner_tu.h, which compiles the core_miner sources
# itself: none of those may be listed here. stratum_net.c is added per
# target because test_core_reconnect stubs it.
set(CORE_MINER_TU_LINK_SRC
    ${CORE_DIR}/work.c ${CORE_DIR}/share.c ${CORE_DIR}/pipeline.c ${CORE_DIR}/avalon_gen.c
    ${STRATUM_DIR}/sv1.c ${POOL_DIR}/pool.c ${POOL_DIR}/pool_config.c
    ${API_DIR}/mc_api_server.c ${API_DIR}/mc_api_cmds.c ${API_DIR}/mc_api_cmds_json.c
    ${API_DIR}/mc_api_cmds_text.c ${API_DIR}/mc_api_estats.c ${API_DIR}/mc_api_lcd.c
    ${API_DIR}/mc_api_identity.c ${CORE_DIR}/../hal/platform/linux/net_hwaddr.c
    ${API_DIR}/mc_api_setter.c ${API_DIR}/mc_api_qrlogin.c ${API_DIR}/mc_api_time.c
    ${CORE_DIR}/../app/core_ascset.c
    ${CORE_DIR}/../app/core_activate.c
    ${CORE_DIR}/../app/core_sigverify.c
    ${CORE_DIR}/../app/ctrl_args.c
    ${CORE_DIR}/../app/core_ident.c
    ${API_DIR}/mc_api_bootby.c
    ${CORE_DIR}/../app/core_pool.c ${CFGWR_DIR}/cfgwr_client.c
    ${CFGWR_DIR}/cfgwr_io.c
    ${CORE_DIR}/../app/core_softonoff.c ${CORE_DIR}/../app/core_ctrl.c
    ${API_DIR}/mc_api_command.c ${API_DIR}/mc_api_setpool.c
    ${API_DIR}/mc_api_poolid.c ${API_DIR}/mc_api_addpool.c ${API_DIR}/mc_api_poollist.c
    ${CORE_DIR}/../common/ini/ini_file.c
    ${CORE_DIR}/../miner_mgmt/cali/cali.c ${CORE_DIR}/../miner_mgmt/api/webauth.c
    ${CORE_DIR}/../common/util/cfg_u32.c
    ${CORE_DIR}/../miner_mgmt/ctrl/pout_reg.c ${CORE_DIR}/../miner_mgmt/ctrl/aging.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stub_board_sensors.c ${CMAKE_CURRENT_SOURCE_DIR}/stub_eeprom.c
    ${CORE_DIR}/../app/core_wallpaper.c
    ${JSON_DIR}/jread.c ${JSON_DIR}/jwrite.c
    ${CORE_DIR}/../common/sha/sha256.c ${CORE_DIR}/../common/ini/ini.c)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_core_session_${BOARD} test_core_session.c ${CORE_MINER_TU_LINK_SRC}
        ${STRATUM_DIR}/stratum_net.c
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_core_session_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_core_session_${BOARD} PRIVATE
        ${CORE_DIR} ${CORE_DIR}/../app ${STRATUM_DIR} ${API_DIR} ${JSON_DIR}
        ${CORE_DIR}/../common/sha ${CORE_DIR}/../common/ini ${CORE_DIR}/../common/proto
        ${CORE_DIR}/../hal ${CORE_DIR}/../hal/asic/a3197s ${CORE_DIR}/../hal/platform/linux
        ${CORE_DIR}/../osif ${CORE_DIR}/../miner_mgmt/cali ${CORE_DIR}/../miner_mgmt/ctrl
        ${CORE_DIR}/../miner_mgmt/api ${BCDIR}
        ${CORE_DIR}/../hal/board)
    target_compile_definitions(test_core_session_${BOARD} PRIVATE
        STRATUM_FIXTURES_DIR="${CORE_DIR}/../tests/harness/fixtures")
    target_link_libraries(test_core_session_${BOARD} PRIVATE pthread m)
    target_link_options(test_core_session_${BOARD} PRIVATE -Wl,--wrap=sha256d)
    add_test(NAME core_session_${BOARD} COMMAND test_core_session_${BOARD})
endforeach()

add_executable(test_core_reject test_core_reject.c ${CORE_MINER_TU_LINK_SRC}
    ${STRATUM_DIR}/stratum_net.c
    ${CTRL_CMDS_C} ${BOARDCONF_T})
add_dependencies(test_core_reject gen_boardconf_nano3s)
target_include_directories(test_core_reject PRIVATE
    ${CORE_DIR} ${CORE_DIR}/../app ${STRATUM_DIR} ${API_DIR} ${JSON_DIR}
    ${CORE_DIR}/../common/sha ${CORE_DIR}/../common/ini ${CORE_DIR}/../common/proto
    ${CORE_DIR}/../hal ${CORE_DIR}/../hal/asic/a3197s ${CORE_DIR}/../hal/platform/linux
    ${CORE_DIR}/../osif ${CORE_DIR}/../miner_mgmt/cali ${CORE_DIR}/../miner_mgmt/ctrl
    ${CORE_DIR}/../miner_mgmt/api ${GEN_DIR}
    ${CORE_DIR}/../hal/board)
target_compile_definitions(test_core_reject PRIVATE
    STRATUM_FIXTURES_DIR="${CORE_DIR}/../tests/harness/fixtures")
target_link_libraries(test_core_reject PRIVATE pthread m)
add_test(NAME core_reject COMMAND test_core_reject)

add_executable(test_core_reconnect test_core_reconnect.c ${CORE_MINER_TU_LINK_SRC}
    ${CTRL_CMDS_C} ${BOARDCONF_T})
add_dependencies(test_core_reconnect gen_boardconf_nano3s)
target_include_directories(test_core_reconnect PRIVATE
    ${CORE_DIR} ${CORE_DIR}/../app ${STRATUM_DIR} ${API_DIR} ${JSON_DIR}
    ${CORE_DIR}/../common/sha ${CORE_DIR}/../common/ini ${CORE_DIR}/../common/proto
    ${CORE_DIR}/../hal ${CORE_DIR}/../hal/asic/a3197s ${CORE_DIR}/../hal/platform/linux
    ${CORE_DIR}/../osif ${CORE_DIR}/../miner_mgmt/cali ${CORE_DIR}/../miner_mgmt/ctrl
    ${CORE_DIR}/../miner_mgmt/api ${GEN_DIR}
    ${CORE_DIR}/../hal/board)
target_link_libraries(test_core_reconnect PRIVATE pthread m)
add_test(NAME core_reconnect COMMAND test_core_reconnect)

set(CORE_HASHRATE_SRC test_core_hashrate.c ${CORE_MINER_TU_LINK_SRC}
    ${STRATUM_DIR}/stratum_net.c)
set(CORE_HASHRATE_INC
    ${CORE_DIR} ${CORE_DIR}/../app ${STRATUM_DIR} ${API_DIR} ${JSON_DIR}
    ${CORE_DIR}/../common/sha ${CORE_DIR}/../common/ini ${CORE_DIR}/../common/proto
    ${CORE_DIR}/../hal ${CORE_DIR}/../hal/asic/a3197s ${CORE_DIR}/../hal/platform/linux
    ${CORE_DIR}/../osif ${CORE_DIR}/../miner_mgmt/cali ${CORE_DIR}/../miner_mgmt/ctrl
    ${CORE_DIR}/../miner_mgmt/api)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_core_hashrate_${BOARD} ${CORE_HASHRATE_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_core_hashrate_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_core_hashrate_${BOARD} PRIVATE ${CORE_HASHRATE_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    target_compile_definitions(test_core_hashrate_${BOARD} PRIVATE
        STRATUM_FIXTURES_DIR="${CORE_DIR}/../tests/harness/fixtures")
    target_link_libraries(test_core_hashrate_${BOARD} PRIVATE pthread m)
    add_test(NAME core_hashrate_${BOARD} COMMAND test_core_hashrate_${BOARD})
endforeach()

set(CORE_TELEM_SRC test_core_telem.c ${CORE_MINER_TU_LINK_SRC}
    ${STRATUM_DIR}/stratum_net.c)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_core_telem_${BOARD} ${CORE_TELEM_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_core_telem_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_core_telem_${BOARD} PRIVATE ${CORE_HASHRATE_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    target_link_libraries(test_core_telem_${BOARD} PRIVATE pthread m)
    add_test(NAME core_telem_${BOARD} COMMAND test_core_telem_${BOARD})
endforeach()

set(CORE_TEMP_SRC ${CORE_TELEM_SRC})
list(REMOVE_ITEM CORE_TEMP_SRC test_core_telem.c)
list(INSERT CORE_TEMP_SRC 0 test_core_temp.c)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_core_temp_${BOARD} ${CORE_TEMP_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_core_temp_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_core_temp_${BOARD} PRIVATE ${CORE_HASHRATE_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    target_link_libraries(test_core_temp_${BOARD} PRIVATE pthread m)
    add_test(NAME core_temp_${BOARD} COMMAND test_core_temp_${BOARD})
endforeach()

set(ASIC_TELEM_SRC test_asic_telem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stub_asic_hw.c
    ${CORE_DIR}/nonce_ring.c)
set(ASIC_TELEM_INC
    ${CORE_DIR} ${CORE_DIR}/../app ${CORE_DIR}/../common/proto
    ${CORE_DIR}/../hal ${CORE_DIR}/../hal/asic/a3197s
    ${CORE_DIR}/../hal/platform/linux ${CORE_DIR}/../osif)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_asic_telem_${BOARD} ${ASIC_TELEM_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_asic_telem_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_asic_telem_${BOARD} PRIVATE ${ASIC_TELEM_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    add_test(NAME asic_telem_${BOARD} COMMAND test_asic_telem_${BOARD})
endforeach()

set(ASIC_FREQ_SRC ${ASIC_TELEM_SRC})
list(REMOVE_ITEM ASIC_FREQ_SRC test_asic_telem.c)
list(INSERT ASIC_FREQ_SRC 0 test_asic_freq.c)

set(ASIC_STOP_SRC ${ASIC_TELEM_SRC})
list(REMOVE_ITEM ASIC_STOP_SRC test_asic_telem.c)
list(INSERT ASIC_STOP_SRC 0 test_asic_stop.c)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_asic_stop_${BOARD} ${ASIC_STOP_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_asic_stop_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_asic_stop_${BOARD} PRIVATE ${ASIC_TELEM_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    add_test(NAME asic_stop_${BOARD} COMMAND test_asic_stop_${BOARD})
endforeach()

set(ASIC_RAMP_SRC ${ASIC_TELEM_SRC})
list(REMOVE_ITEM ASIC_RAMP_SRC test_asic_telem.c)
list(INSERT ASIC_RAMP_SRC 0 test_asic_ramp.c)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_asic_ramp_${BOARD} ${ASIC_RAMP_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_asic_ramp_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_asic_ramp_${BOARD} PRIVATE ${ASIC_TELEM_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    add_test(NAME asic_ramp_${BOARD} COMMAND test_asic_ramp_${BOARD})
endforeach()

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_asic_freq_${BOARD} ${ASIC_FREQ_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_asic_freq_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_asic_freq_${BOARD} PRIVATE ${ASIC_TELEM_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    add_test(NAME asic_freq_${BOARD} COMMAND test_asic_freq_${BOARD})
endforeach()

set(ASIC_ENUM_SRC ${ASIC_TELEM_SRC})
list(REMOVE_ITEM ASIC_ENUM_SRC test_asic_telem.c)
list(INSERT ASIC_ENUM_SRC 0 test_asic_enum.c)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_asic_enum_${BOARD} ${ASIC_ENUM_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_asic_enum_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_asic_enum_${BOARD} PRIVATE ${ASIC_TELEM_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    add_test(NAME asic_enum_${BOARD} COMMAND test_asic_enum_${BOARD})
endforeach()

set(ASIC_IPC_SRC ${ASIC_TELEM_SRC})
list(REMOVE_ITEM ASIC_IPC_SRC test_asic_telem.c)
list(INSERT ASIC_IPC_SRC 0 test_asic_ipc.c)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_asic_ipc_${BOARD} ${ASIC_IPC_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_asic_ipc_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_asic_ipc_${BOARD} PRIVATE ${ASIC_TELEM_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    add_test(NAME asic_ipc_${BOARD} COMMAND test_asic_ipc_${BOARD})
endforeach()

set(ASIC_NONCE_SRC ${ASIC_TELEM_SRC})
list(REMOVE_ITEM ASIC_NONCE_SRC test_asic_telem.c)
list(INSERT ASIC_NONCE_SRC 0 test_asic_nonce.c)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_asic_nonce_${BOARD} ${ASIC_NONCE_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_asic_nonce_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_asic_nonce_${BOARD} PRIVATE ${ASIC_TELEM_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    add_test(NAME asic_nonce_${BOARD} COMMAND test_asic_nonce_${BOARD})
endforeach()

set(ASIC_STATIC_SRC ${ASIC_TELEM_SRC})
list(REMOVE_ITEM ASIC_STATIC_SRC test_asic_telem.c)
list(INSERT ASIC_STATIC_SRC 0 test_asic_static.c)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_asic_static_${BOARD} ${ASIC_STATIC_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_asic_static_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_asic_static_${BOARD} PRIVATE ${ASIC_TELEM_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    add_test(NAME asic_static_${BOARD} COMMAND test_asic_static_${BOARD})
endforeach()

set(ASIC_TXGATE_SRC ${ASIC_TELEM_SRC})
list(REMOVE_ITEM ASIC_TXGATE_SRC test_asic_telem.c)
list(INSERT ASIC_TXGATE_SRC 0 test_asic_txgate.c)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_asic_txgate_${BOARD} ${ASIC_TXGATE_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_asic_txgate_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_asic_txgate_${BOARD} PRIVATE ${ASIC_TELEM_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    add_test(NAME asic_txgate_${BOARD} COMMAND test_asic_txgate_${BOARD})
endforeach()

set(ASIC_EARN_SRC ${ASIC_TELEM_SRC})
list(REMOVE_ITEM ASIC_EARN_SRC test_asic_telem.c)
list(INSERT ASIC_EARN_SRC 0 test_asic_earn.c)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_asic_earn_${BOARD} ${ASIC_EARN_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_asic_earn_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_asic_earn_${BOARD} PRIVATE ${ASIC_TELEM_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    add_test(NAME asic_earn_${BOARD} COMMAND test_asic_earn_${BOARD})
endforeach()

add_executable(test_api_dispatch test_api_dispatch.c
    ${API_DIR}/mc_api_server.c ${API_DIR}/mc_api_cmds.c ${API_DIR}/mc_api_cmds_json.c
    ${API_DIR}/mc_api_cmds_text.c ${API_DIR}/mc_api_estats.c ${API_DIR}/mc_api_lcd.c
    ${API_DIR}/mc_api_command.c
    ${API_DIR}/mc_api_setter.c ${JSON_DIR}/jread.c ${API_DIR}/mc_api_qrlogin.c
    ${API_DIR}/mc_api_time.c
    ${CORE_DIR}/../common/json/jwrite.c ${OSIF_LOG_C}
    ${CTRL_CMDS_C} ${BOARDCONF_T})
add_dependencies(test_api_dispatch gen_boardconf_nano3s)
target_include_directories(test_api_dispatch PRIVATE ${API_INC} ${GEN_DIR}
    ${CORE_DIR}/../hal/board)
add_test(NAME api_dispatch COMMAND test_api_dispatch)

set(CORE_REGISTRY_SRC ${CORE_TELEM_SRC})
list(REMOVE_ITEM CORE_REGISTRY_SRC test_core_telem.c)
list(INSERT CORE_REGISTRY_SRC 0 test_core_registry.c)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} REG_BCT REG_GEN)
    add_executable(test_core_registry_${BOARD} ${CORE_REGISTRY_SRC}
        ${CTRL_CMDS_C} ${REG_BCT})
    add_dependencies(test_core_registry_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_core_registry_${BOARD} PRIVATE
        ${CORE_HASHRATE_INC} ${REG_GEN} ${CORE_DIR}/../hal/board)
    target_link_libraries(test_core_registry_${BOARD} PRIVATE pthread m)
    add_test(NAME core_registry_${BOARD} COMMAND test_core_registry_${BOARD})
endforeach()

set(API_PLUGIN_STUB_DIR ${CORE_DIR}/../tests/api_plugin_stub)
set(API_PLUGIN_SRC ${CORE_REGISTRY_SRC} ${API_PLUGIN_STUB_DIR}/api_plugin_stub.c)
list(REMOVE_ITEM API_PLUGIN_SRC test_core_registry.c)
list(INSERT API_PLUGIN_SRC 0 test_api_ext.c)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} PLUGIN_BCT PLUGIN_GEN)
    add_executable(test_api_ext_${BOARD} ${API_PLUGIN_SRC}
        ${CTRL_CMDS_C} ${PLUGIN_BCT})
    add_dependencies(test_api_ext_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_api_ext_${BOARD} PRIVATE
        ${API_PLUGIN_STUB_DIR} ${CORE_HASHRATE_INC} ${PLUGIN_GEN} ${CORE_DIR}/../hal/board)
    target_compile_definitions(test_api_ext_${BOARD} PRIVATE
        API_PLUGIN=1 "API_PLUGIN_HEADER=\"api_plugin_cmds.h\"")
    target_link_libraries(test_api_ext_${BOARD} PRIVATE pthread m)
    add_test(NAME api_ext_${BOARD} COMMAND test_api_ext_${BOARD})
endforeach()

set(CORE_POLICY_SRC ${CORE_TELEM_SRC})
list(REMOVE_ITEM CORE_POLICY_SRC test_core_telem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stub_board_sensors.c)
list(INSERT CORE_POLICY_SRC 0 test_core_policy.c)

foreach(BOARD ${MINER_TEST_BOARDS})
    miner_board_select(${BOARD} BCT BCDIR)
    add_executable(test_core_policy_${BOARD} ${CORE_POLICY_SRC}
        ${CTRL_CMDS_C} ${BCT})
    add_dependencies(test_core_policy_${BOARD} gen_boardconf_${BOARD})
    target_include_directories(test_core_policy_${BOARD} PRIVATE ${CORE_HASHRATE_INC} ${BCDIR}
        ${CORE_DIR}/../hal/board)
    target_link_libraries(test_core_policy_${BOARD} PRIVATE pthread m)
    add_test(NAME core_policy_${BOARD} COMMAND test_core_policy_${BOARD})
endforeach()

set(CORE_X3_SMOKE_SRC ${CORE_TELEM_SRC})
list(REMOVE_ITEM CORE_X3_SMOKE_SRC test_core_telem.c)
add_executable(test_core_x3_smoke test_core_x3_smoke.c ${CORE_X3_SMOKE_SRC}
    ${CTRL_CMDS_C} ${BOARDCONF_X3_T})
add_dependencies(test_core_x3_smoke gen_boardconf_x3)
target_include_directories(test_core_x3_smoke PRIVATE ${CORE_HASHRATE_INC}
    ${GEN_X3_DIR} ${CORE_DIR}/../hal/board ${CORE_DIR}/../tests/fixtures)
target_link_libraries(test_core_x3_smoke PRIVATE pthread m)
add_test(NAME core_x3_smoke COMMAND test_core_x3_smoke)

set(CORE_NETFAIL_SRC ${CORE_TELEM_SRC})
list(REMOVE_ITEM CORE_NETFAIL_SRC test_core_telem.c)
add_executable(test_core_netfail test_core_netfail.c ${CORE_NETFAIL_SRC}
    ${CTRL_CMDS_C} ${BOARDCONF_T})
add_dependencies(test_core_netfail gen_boardconf_nano3s)
target_include_directories(test_core_netfail PRIVATE ${CORE_HASHRATE_INC} ${GEN_DIR}
    ${CORE_DIR}/../hal/board)
target_link_libraries(test_core_netfail PRIVATE pthread m)
add_test(NAME core_netfail COMMAND test_core_netfail)

add_executable(test_api_bootby test_api_bootby.c ${API_DIR}/mc_api_bootby.c)
target_include_directories(test_api_bootby PRIVATE ${API_DIR})
add_test(NAME api_bootby COMMAND test_api_bootby)

add_executable(test_qr_sessions test_qr_sessions.c)
add_test(NAME qr_sessions COMMAND test_qr_sessions)

get_property(ALL_TESTS DIRECTORY PROPERTY TESTS)
set_tests_properties(${ALL_TESTS} PROPERTIES SKIP_RETURN_CODE ${TST_SKIP})
