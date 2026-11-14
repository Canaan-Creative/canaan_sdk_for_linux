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
project(common_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror=format -Wno-error=format-truncation -O2)

set(COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(FIXTURES_DIR ${COMMON_DIR}/../tests/harness/fixtures)

enable_testing()

add_executable(test_process_version test_process_version.c)
target_include_directories(test_process_version PRIVATE ${COMMON_DIR}/proto)
target_compile_options(test_process_version PRIVATE -UNDEBUG)
target_compile_definitions(test_process_version PRIVATE
    CORE_MINER_VERSION="00000000_0000_unknown" CORE_MINER_SEMVER="0.0.0"
    MM_MINER_VERSION="00000000_0000_unknown" ASIC_MINER_VERSION="00000000_0000_unknown")
add_test(NAME process_version COMMAND test_process_version)
add_test(NAME build_version COMMAND ${CMAKE_COMMAND}
    -DMINER_SOURCE=${COMMON_DIR}/.. -P ${CMAKE_CURRENT_SOURCE_DIR}/test_build_version.cmake)

add_executable(test_sha256 test_sha256.c ${COMMON_DIR}/sha/sha256.c)
target_include_directories(test_sha256 PRIVATE ${COMMON_DIR}/sha)
add_test(NAME sha256 COMMAND test_sha256)

add_executable(test_ini test_ini.c ${COMMON_DIR}/ini/ini.c)
target_include_directories(test_ini PRIVATE ${COMMON_DIR}/ini)
target_compile_definitions(test_ini PRIVATE COMMON_FIXTURES_DIR="${FIXTURES_DIR}")
add_test(NAME ini COMMAND test_ini)

add_executable(test_ini_file test_ini_file.c ${COMMON_DIR}/ini/ini_file.c ${COMMON_DIR}/ini/ini.c)
target_include_directories(test_ini_file PRIVATE ${COMMON_DIR}/ini ${COMMON_DIR}/util)
target_compile_options(test_ini_file PRIVATE -fno-builtin-malloc)
add_test(NAME ini_file COMMAND test_ini_file)

add_executable(test_ini_cache test_ini_cache.c ${COMMON_DIR}/ini/ini_cache.c
    ${COMMON_DIR}/ini/ini_file.c ${COMMON_DIR}/ini/ini.c)
target_include_directories(test_ini_cache PRIVATE ${COMMON_DIR}/ini ${COMMON_DIR}/util)
target_link_libraries(test_ini_cache PRIVATE pthread)
add_test(NAME ini_cache COMMAND test_ini_cache)

add_executable(test_jwrite test_jwrite.c ${COMMON_DIR}/json/jwrite.c)
target_include_directories(test_jwrite PRIVATE ${COMMON_DIR}/json)
add_test(NAME jwrite COMMAND test_jwrite)

add_executable(test_jread test_jread.c ${COMMON_DIR}/json/jread.c)
target_include_directories(test_jread PRIVATE ${COMMON_DIR}/json)
add_test(NAME jread COMMAND test_jread)

add_executable(test_hex test_hex.c)
target_include_directories(test_hex PRIVATE ${COMMON_DIR}/util)
add_test(NAME hex COMMAND test_hex)

add_executable(test_fsync_dir test_fsync_dir.c)
target_include_directories(test_fsync_dir PRIVATE ${COMMON_DIR}/util)
add_test(NAME fsync_dir COMMAND test_fsync_dir)

add_executable(test_cfg_u32 test_cfg_u32.c ${COMMON_DIR}/util/cfg_u32.c)
target_include_directories(test_cfg_u32 PRIVATE ${COMMON_DIR}/util)
add_test(NAME cfg_u32 COMMAND test_cfg_u32)
