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

file(STRINGS "${M}/VERSION" MINER_FILE_VERSION LIMIT_COUNT 1)
if(MINER_FILE_VERSION STREQUAL "")
    file(STRINGS "${M}/../../VERSION" MINER_FILE_VERSION LIMIT_COUNT 1)
endif()
set(FW_VERSION "${MINER_FILE_VERSION}" CACHE STRING "miner software semver")
if(NOT FW_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "FW_VERSION must be a major.minor.patch semver")
endif()
string(LENGTH "${FW_VERSION}" FW_VERSION_LENGTH)
if(FW_VERSION_LENGTH GREATER_EQUAL 64)
    message(FATAL_ERROR "FW_VERSION exceeds the wire capacity")
endif()
if(NOT DEFINED MINER_GIT_SHA)
    execute_process(COMMAND git rev-parse --short=8 HEAD
        WORKING_DIRECTORY "${M}"
        OUTPUT_VARIABLE MINER_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE MINER_GIT_RESULT)
    if(NOT MINER_GIT_RESULT EQUAL 0 OR MINER_GIT_SHA STREQUAL "")
        set(MINER_GIT_SHA "unknown")
    else()
        execute_process(COMMAND git status --porcelain --untracked-files=no
            --ignore-submodules=all
            WORKING_DIRECTORY "${M}"
            OUTPUT_VARIABLE MINER_GIT_STATUS ERROR_QUIET)
        if(NOT MINER_GIT_STATUS STREQUAL "")
            string(APPEND MINER_GIT_SHA "t")
        endif()
    endif()
endif()
string(REPEAT "[0-9a-f]" 7 MINER_HEX7)
set(MINER_SHA_REGEX "^(${MINER_HEX7})([0-9a-f])[0-9a-f]*$|^(${MINER_HEX7})[0-9a-f]*(t)$")
if(MINER_GIT_SHA MATCHES "${MINER_SHA_REGEX}")
    set(MINER_GIT_SHA "${CMAKE_MATCH_1}${CMAKE_MATCH_2}${CMAKE_MATCH_3}${CMAKE_MATCH_4}")
elseif(NOT MINER_GIT_SHA STREQUAL "unknown")
    message(FATAL_ERROR "MINER_GIT_SHA must be unknown or a source commit of at least eight "
        "hexadecimal digits, or seven suffixed t when the tree is dirty")
endif()
if(NOT DEFINED MINER_BUILD_TIME)
    string(TIMESTAMP MINER_BUILD_TIME "%Y%m%d%H%M")
endif()
set(MINER_TIME_REGEX
    "^[0-9][0-9][0-9][0-9](0[1-9]|1[0-2])(0[1-9]|[12][0-9]|3[01])([01][0-9]|2[0-3])[0-5][0-9]$")
if(NOT MINER_BUILD_TIME MATCHES "${MINER_TIME_REGEX}")
    message(FATAL_ERROR "MINER_BUILD_TIME must use local YYYYMMDDHHmm (24-hour clock)")
endif()
if(NOT DEFINED MINER_BUILD_REV)
    set(MINER_BUILD_REV "01")
endif()
if(NOT MINER_BUILD_REV MATCHES "^[0-9][0-9]$")
    message(FATAL_ERROR "MINER_BUILD_REV must be two decimal digits")
endif()
string(SUBSTRING "${MINER_BUILD_TIME}" 2 6 MINER_BUILD_DATE)
string(SUBSTRING "${MINER_BUILD_TIME}" 8 4 MINER_BUILD_HM)
if(NOT DEFINED MINER_BUILD_VERSION)
    set(MINER_BUILD_VERSION
        "${MINER_BUILD_DATE}${MINER_BUILD_REV}_${MINER_BUILD_HM}_${MINER_GIT_SHA}")
endif()
string(REPEAT "[0-9]" 8 MINER_DIGIT8)
string(REPEAT "[0-9]" 4 MINER_DIGIT4)
set(MINER_COMMIT_REGEX "([0-9a-f]+t?|unknown)")
set(MINER_VERSION_REGEX
    "^${MINER_DIGIT8}_${MINER_DIGIT4}_${MINER_COMMIT_REGEX}(_${MINER_COMMIT_REGEX})*$")
if(NOT MINER_BUILD_VERSION MATCHES "${MINER_VERSION_REGEX}")
    message(FATAL_ERROR "MINER_BUILD_VERSION must be <yymmdd><rev>_<hhmm>_ followed by one or "
        "more commits separated by _, each unknown or hexadecimal digits with an optional "
        "trailing t")
endif()
string(LENGTH "${MINER_BUILD_VERSION}" MINER_BUILD_VERSION_LENGTH)
if(MINER_BUILD_VERSION_LENGTH GREATER_EQUAL 64)
    message(FATAL_ERROR "MINER_BUILD_VERSION exceeds the wire capacity")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${M}/VERSION")
foreach(GIT_META HEAD packed-refs index)
    execute_process(COMMAND git rev-parse --git-path ${GIT_META} WORKING_DIRECTORY "${M}"
        OUTPUT_VARIABLE MINER_GIT_PATH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(MINER_GIT_PATH)
        get_filename_component(MINER_GIT_PATH "${MINER_GIT_PATH}" ABSOLUTE BASE_DIR "${M}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MINER_GIT_PATH}")
    endif()
endforeach()
execute_process(COMMAND git symbolic-ref -q HEAD WORKING_DIRECTORY "${M}"
    OUTPUT_VARIABLE MINER_GIT_REF OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(MINER_GIT_REF)
    execute_process(COMMAND git rev-parse --git-path "${MINER_GIT_REF}" WORKING_DIRECTORY "${M}"
        OUTPUT_VARIABLE MINER_GIT_REF_PATH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    get_filename_component(MINER_GIT_REF_PATH "${MINER_GIT_REF_PATH}" ABSOLUTE BASE_DIR "${M}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MINER_GIT_REF_PATH}")
endif()
