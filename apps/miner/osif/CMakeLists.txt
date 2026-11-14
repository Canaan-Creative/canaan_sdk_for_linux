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
project(osif C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror=format -Wno-error=format-truncation -O2)

add_library(osif STATIC
    linux/osif_thread_linux.c
    linux/osif_lock_linux.c
    linux/osif_timer_linux.c
    linux/osif_log_linux.c
    linux/osif_ipc_linux.c
)
target_include_directories(osif PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
find_package(Threads REQUIRED)
target_link_libraries(osif PUBLIC Threads::Threads)

option(OSIF_BUILD_TESTS "build osif unit tests" OFF)
if(OSIF_BUILD_TESTS)
    enable_testing()
    foreach(t osif_thread osif_lock_timer osif_log osif_ipc osif_ipc_multi)
        add_executable(test_${t} tests/test_${t}.c)
        target_link_libraries(test_${t} PRIVATE osif)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()
endif()
