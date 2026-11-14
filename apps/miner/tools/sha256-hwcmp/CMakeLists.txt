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
project(sha256_hwcmp C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror=format -Wno-error=format-truncation -O2)

set(MINER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(sha256_hwcmp sha256_hwcmp.c ${MINER_DIR}/common/sha/sha256.c)
target_include_directories(sha256_hwcmp PRIVATE ${MINER_DIR}/common/sha)

# The host kernel has no K230 engine; sha256-generic exercises the AF_ALG side.
enable_testing()
add_test(NAME generic COMMAND sha256_hwcmp -q -d sha256-generic -j 2 -t 1
    verify contend stress soak gap)
set_tests_properties(generic PROPERTIES SKIP_RETURN_CODE 77)
