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

set(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/version-fixture")
file(MAKE_DIRECTORY "${SOURCE}")
file(WRITE "${SOURCE}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.16)\n"
    "project(version_test NONE)\n"
    "set(M \"${MINER_SOURCE}\")\n"
    "string(TIMESTAMP DATE_BEFORE \"%y%m%d\")\n"
    "string(TIMESTAMP HM_BEFORE \"%H%M\")\n"
    "include(\"${MINER_SOURCE}/cmake/MinerVersion.cmake\")\n"
    "string(TIMESTAMP DATE_AFTER \"%y%m%d\")\n"
    "string(TIMESTAMP HM_AFTER \"%H%M\")\n"
    "set(EXPECTED_ALT \"\")\n"
    "if(DEFINED EXPECTED_TODAY)\n"
    "string(REPLACE \"@HM@\" \"\${HM_BEFORE}\" EXPECTED \"\${DATE_BEFORE}\${EXPECTED_TODAY}\")\n"
    "string(REPLACE \"@HM@\" \"\${HM_AFTER}\" EXPECTED_ALT \"\${DATE_AFTER}\${EXPECTED_TODAY}\")\n"
    "endif()\n"
    "if(DEFINED EXPECTED AND NOT MINER_BUILD_VERSION STREQUAL EXPECTED AND NOT MINER_BUILD_VERSION"
    " STREQUAL EXPECTED_ALT)\n"
    "message(FATAL_ERROR \"unexpected version: \${MINER_BUILD_VERSION}\")\n"
    "endif()\n"
    "if(DEFINED EXPECTED_FW AND NOT FW_VERSION STREQUAL EXPECTED_FW)\n"
    "message(FATAL_ERROR \"unexpected semver: \${FW_VERSION}\")\n"
    "endif()\n")
function(check_version NAME EXPECT_SUCCESS)
    file(REMOVE_RECURSE "${SOURCE}/${NAME}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${SOURCE}/${NAME}"
        ${ARGN} RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR)
    if(EXPECT_SUCCESS AND NOT RESULT EQUAL 0)
        message(FATAL_ERROR "${NAME}: ${OUTPUT}\n${ERROR}")
    elseif(NOT EXPECT_SUCCESS AND RESULT EQUAL 0)
        message(FATAL_ERROR "${NAME}: invalid input accepted")
    endif()
endfunction()
file(STRINGS "${MINER_SOURCE}/VERSION" SEMVER LIMIT_COUNT 1)
unset(ENV{SOURCE_DATE_EPOCH})
set(ENV{TZ} "UTC-8")
check_version(default TRUE -DMINER_GIT_SHA=deadbeef -DEXPECTED_TODAY=01_@HM@_deadbeef
    "-DEXPECTED_FW=${SEMVER}")
check_version(probe TRUE -DMINER_BUILD_TIME=202609112138)
check_version(explicit TRUE -DFW_VERSION=0.3.57 -DMINER_GIT_SHA=abcdef01
    -DMINER_BUILD_TIME=202609112138 -DEXPECTED=26091101_2138_abcdef01 -DEXPECTED_FW=0.3.57)
check_version(other_date TRUE -DFW_VERSION=0.3.57 -DMINER_GIT_SHA=abcdef01
    -DMINER_BUILD_TIME=202512312359 -DEXPECTED=25123101_2359_abcdef01)
check_version(explicit_rev TRUE -DFW_VERSION=0.3.57 -DMINER_GIT_SHA=abcdef01
    -DMINER_BUILD_TIME=202609162138 -DMINER_BUILD_REV=07 -DEXPECTED=26091607_2138_abcdef01)
check_version(dirty TRUE -DMINER_GIT_SHA=abcdef0t
    -DMINER_BUILD_TIME=202609112138 -DEXPECTED=26091101_2138_abcdef0t)
check_version(long_sha TRUE -DMINER_GIT_SHA=abcdef0123456t
    -DMINER_BUILD_TIME=202609112138 -DEXPECTED=26091101_2138_abcdef0t)
check_version(no_git TRUE -DMINER_GIT_SHA=unknown
    -DMINER_BUILD_TIME=202609112138 -DEXPECTED=26091101_2138_unknown)
check_version(version_two TRUE -DMINER_BUILD_VERSION=26091101_2138_abcdef01_12345678
    -DEXPECTED=26091101_2138_abcdef01_12345678)
check_version(version_one TRUE -DMINER_BUILD_VERSION=26091101_2138_abcdef01
    -DEXPECTED=26091101_2138_abcdef01)
check_version(version_three TRUE -DMINER_BUILD_VERSION=26091101_2138_abcdef01_12345678_9t
    -DEXPECTED=26091101_2138_abcdef01_12345678_9t)
check_version(version_dirty TRUE -DMINER_BUILD_VERSION=26091101_2138_abcdef0t_1234567t
    -DEXPECTED=26091101_2138_abcdef0t_1234567t)
check_version(version_unknown TRUE -DMINER_BUILD_VERSION=26091101_2138_unknown_unknown
    -DEXPECTED=26091101_2138_unknown_unknown)
check_version(version_precedence TRUE -DMINER_GIT_SHA=deadbeef -DMINER_BUILD_TIME=202512312359
    -DMINER_BUILD_REV=07 -DMINER_BUILD_VERSION=26091101_2138_abcdef01_12345678
    -DEXPECTED=26091101_2138_abcdef01_12345678)
check_version(version_longest TRUE
    -DMINER_BUILD_VERSION=26091101_2138_1234567890123456789012345678901234567890123456789
    -DEXPECTED=26091101_2138_1234567890123456789012345678901234567890123456789)
check_version(version_too_long FALSE
    -DMINER_BUILD_VERSION=26091101_2138_12345678901234567890123456789012345678901234567890)
check_version(version_empty FALSE -DMINER_BUILD_VERSION=)
check_version(version_no_commit FALSE -DMINER_BUILD_VERSION=26091101_2138_)
check_version(version_nogit FALSE -DMINER_BUILD_VERSION=26091101_2138_abcdef01_nogit)
check_version(version_upper FALSE -DMINER_BUILD_VERSION=26091101_2138_abcdef01_ABCDEF01)
check_version(version_dirty_twice FALSE -DMINER_BUILD_VERSION=26091101_2138_abcdef01_abcdef0tt)
check_version(version_dirty_only FALSE -DMINER_BUILD_VERSION=26091101_2138_abcdef01_t)
check_version(version_unknown_dirty FALSE -DMINER_BUILD_VERSION=26091101_2138_unknownt)
check_version(version_joined FALSE -DMINER_BUILD_VERSION=26091101_2138_abcdef01__12345678)
check_version(version_trailing FALSE -DMINER_BUILD_VERSION=26091101_2138_abcdef01_)
check_version(version_short_date FALSE -DMINER_BUILD_VERSION=2609110_2138_abcdef01)
check_version(version_bad_time FALSE -DMINER_BUILD_VERSION=26091101_21x8_abcdef01)
check_version(version_dash FALSE -DMINER_BUILD_VERSION=26091101_2138_abcdef01-12345678)
check_version(longest_sha TRUE -DMINER_GIT_SHA=aaaaaaaaaabbbbbbbbbbccccccccccddddddddddee
    -DMINER_BUILD_TIME=202609112138 -DEXPECTED=26091101_2138_aaaaaaaa)
check_version(short_time FALSE -DMINER_BUILD_TIME=2609112138)
check_version(bad_time FALSE -DMINER_BUILD_TIME=2026091124xx)
check_version(bad_hour FALSE -DMINER_BUILD_TIME=202609112438)
check_version(bad_month FALSE -DMINER_BUILD_TIME=202613112138)
check_version(empty_time FALSE -DMINER_BUILD_TIME=)
check_version(short_rev FALSE -DMINER_BUILD_REV=1)
check_version(long_rev FALSE -DMINER_BUILD_REV=001)
check_version(bad_rev FALSE -DMINER_BUILD_REV=1a)
check_version(empty_rev FALSE -DMINER_BUILD_REV=)
check_version(empty_sha FALSE -DMINER_GIT_SHA=)
check_version(bad_sha FALSE -DMINER_GIT_SHA=zzzzzzzz)
check_version(upper_sha FALSE -DMINER_GIT_SHA=ABCDEF01)
check_version(short_sha FALSE -DMINER_GIT_SHA=abcdef0)
check_version(short_dirty_sha FALSE -DMINER_GIT_SHA=abcdeft)
check_version(dirty_only FALSE -DMINER_GIT_SHA=t)
check_version(dirty_twice FALSE -DMINER_GIT_SHA=abcdef0tt)
check_version(dirty_first FALSE -DMINER_GIT_SHA=tabcdef01)
check_version(bad_semver FALSE -DFW_VERSION=invalid)
check_version(longest_dirty_sha TRUE
    -DMINER_GIT_SHA=aaaaaaaaaabbbbbbbbbbccccccccccddddddddddeeeeeeeeeefffffffffft
    -DMINER_BUILD_TIME=202609112138 -DEXPECTED=26091101_2138_aaaaaaat)
check_version(too_long FALSE
    -DFW_VERSION=111111111122222222223333333333444444444455555555556666666666.0.0)
