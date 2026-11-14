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
project(miner_behavior_harness C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror=format -Wno-error=format-truncation -O2)

enable_testing()
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/MinerProtoVer.cmake)
add_compile_definitions(FW_TYPE="Debug" MINER_PROTO_VER="${MINER_PROTO_VER}"
    HARNESS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_compile_definitions(CORE_MINER_VERSION="00000000_0000_unknown"
    CORE_MINER_SEMVER="0.0.0"
    MM_MINER_VERSION="00000000_0000_unknown"
    ASIC_MINER_VERSION="00000000_0000_unknown")

function(harness_case exe)
    foreach(id ${ARGN})
        add_test(NAME ${id} COMMAND ${exe} ${id})
    endforeach()
endfunction()

set(COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common)
include_directories(${COMMON_DIR}/util)

set(BOARDCONF_INI
    ${COMMON_DIR}/../../../product/k230/miner/avalon-home/nano3s/conf/product/boardconf.ini)
set(BOARDCONF_H ${CMAKE_CURRENT_BINARY_DIR}/gen/boardconf.h)
set(BOARDCONF_T ${COMMON_DIR}/../hal/board/board_tables.c)
include_directories(${COMMON_DIR}/../hal/board)
add_custom_command(OUTPUT ${BOARDCONF_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${COMMON_DIR}/../tools/gen_boardconf.py ${BOARDCONF_INI} ${BOARDCONF_H}
    DEPENDS ${BOARDCONF_INI} ${COMMON_DIR}/../tools/gen_boardconf.py
    COMMENT "gen boardconf.h from ${BOARDCONF_INI}")
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)

add_executable(test_alg host/test_alg.c host/sha256_ref.c ${COMMON_DIR}/sha/sha256.c)
target_include_directories(test_alg PRIVATE ${COMMON_DIR}/sha)
add_executable(test_mining host/test_mining.c host/sha256_ref.c
    ${COMMON_DIR}/../miner_core/work.c
    ${COMMON_DIR}/../miner_core/share.c
    ${COMMON_DIR}/sha/sha256.c)
target_include_directories(test_mining PRIVATE
    ${COMMON_DIR}/../miner_core ${COMMON_DIR}/sha)
target_link_libraries(test_mining PRIVATE m)
add_executable(test_pool host/test_pool.c host/sha256_ref.c
    ${COMMON_DIR}/../miner_core/stratum/sv1.c
    ${COMMON_DIR}/json/jread.c
    ${COMMON_DIR}/pool/pool.c)
target_include_directories(test_pool PRIVATE ${COMMON_DIR}/../miner_core/stratum ${COMMON_DIR}/pool
    ${COMMON_DIR}/json)
target_link_libraries(test_pool PRIVATE pthread)
add_executable(test_api host/test_api.c
    ${COMMON_DIR}/../miner_mgmt/api/status.c
    ${COMMON_DIR}/../miner_mgmt/api/webauth.c
    ${COMMON_DIR}/../miner_core/api/mc_api_server.c
    ${COMMON_DIR}/../miner_core/api/mc_api_qrlogin.c
    ${COMMON_DIR}/../miner_core/api/mc_api_cmds.c
    ${COMMON_DIR}/../miner_core/api/mc_api_cmds_json.c
    ${COMMON_DIR}/../miner_core/api/mc_api_cmds_text.c
    ${COMMON_DIR}/../miner_core/api/mc_api_estats.c
    ${COMMON_DIR}/../miner_core/api/mc_api_lcd.c
    ${COMMON_DIR}/../miner_core/api/mc_api_command.c
    ${COMMON_DIR}/../miner_core/api/mc_api_setter.c ${COMMON_DIR}/json/jread.c
    ${COMMON_DIR}/../osif/linux/osif_log_linux.c
    ${COMMON_DIR}/json/jwrite.c
    ${COMMON_DIR}/ini/ini.c
    ${BOARDCONF_H} ${BOARDCONF_T})
target_include_directories(test_api PRIVATE
    ${COMMON_DIR}/../miner_mgmt/api ${COMMON_DIR}/../miner_core/api
    ${COMMON_DIR}/../osif
    ${COMMON_DIR}/json ${COMMON_DIR}/ini ${COMMON_DIR}/proto ${GEN_DIR})
target_link_libraries(test_api PRIVATE pthread) # mc_api_server_start's thread
add_executable(test_config host/test_config.c)

harness_case(test_alg sha256d)
harness_case(test_mining mining_notify_header mining_share_validate mining_share_submit)
harness_case(test_pool pool_list_max3 pool_failover pool_sv1_session)
harness_case(test_api api_ascset_workmode_err api_pool_commands api_ascset_unknown_option
    api_ascset_ip_absent api_time_set api_dashboard_telemetry api_web_auth)
harness_case(test_config config_pools_conf_json config_pool_list)
