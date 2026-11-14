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
project(hal_tests C)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../../common/proto)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../../common/util)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror=format -Wno-error=format-truncation -O2)
add_compile_options(-UNDEBUG)
add_compile_definitions(CORE_MINER_VERSION="00000000_0000_unknown"
    CORE_MINER_SEMVER="0.0.0"
    MM_MINER_VERSION="00000000_0000_unknown"
    ASIC_MINER_VERSION="00000000_0000_unknown")

set(HAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(OSIF_DIR ${HAL_DIR}/../osif)

enable_testing()

add_executable(test_dcdc_protection test_dcdc_protection.c
    ${HAL_DIR}/platform/linux/psu_dcdc_pd.c)
target_include_directories(test_dcdc_protection PRIVATE ${HAL_DIR}
    ${HAL_DIR}/platform/linux ${OSIF_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gen)

set(BOARDCONF_INI
    ${HAL_DIR}/../../../product/k230/miner/avalon-home/nano3s/conf/product/boardconf.ini)
set(GEN_BOARDCONF ${HAL_DIR}/../tools/gen_boardconf.py)
set(BOARDCONF_H ${CMAKE_CURRENT_BINARY_DIR}/gen/boardconf.h)
set(BOARDCONF_T ${HAL_DIR}/board/board_tables.c)
add_dependencies(test_dcdc_protection gen_boardconf_nano3s)
add_test(NAME dcdc_protection COMMAND test_dcdc_protection)
include_directories(${HAL_DIR}/board)
add_custom_command(OUTPUT ${BOARDCONF_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${GEN_BOARDCONF} ${BOARDCONF_INI} ${BOARDCONF_H}
    DEPENDS ${BOARDCONF_INI} ${GEN_BOARDCONF}
    COMMENT "gen boardconf from ${BOARDCONF_INI}")
add_custom_target(gen_boardconf_nano3s DEPENDS ${BOARDCONF_H})

add_executable(test_a3197s_pll
    test_a3197s_pll.c
    ${HAL_DIR}/asic/a3197s/a3197s_pll.c
    ${HAL_DIR}/asic/a3197s/a3197s_cpm_table.c)
add_dependencies(test_a3197s_pll gen_boardconf_nano3s)
target_include_directories(test_a3197s_pll PRIVATE
    ${HAL_DIR}/asic/a3197s ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_test(NAME a3197s_pll COMMAND test_a3197s_pll)

add_executable(test_a3197s_temp test_a3197s_temp.c ${HAL_DIR}/asic/a3197s/a3197s_pvt.c)
add_dependencies(test_a3197s_temp gen_boardconf_nano3s)
target_include_directories(test_a3197s_temp PRIVATE
    ${HAL_DIR}/asic/a3197s ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(test_a3197s_temp PRIVATE m)
add_test(NAME a3197s_temp COMMAND test_a3197s_temp)

add_executable(test_ntc test_ntc.c)
target_include_directories(test_ntc PRIVATE ${HAL_DIR})
target_link_libraries(test_ntc PRIVATE m)
add_test(NAME ntc COMMAND test_ntc)

add_executable(test_adc_iio test_adc_iio.c)
target_include_directories(test_adc_iio PRIVATE ${HAL_DIR}/platform/linux)
target_compile_definitions(test_adc_iio PRIVATE
    TST_IIO_DEVICE="${CMAKE_CURRENT_BINARY_DIR}/fx-iio")
add_test(NAME adc_iio COMMAND test_adc_iio)

add_executable(test_board_volt test_board_volt.c ${BOARDCONF_T}
    ${HAL_DIR}/platform/linux/board_sensors.c
    ${HAL_DIR}/platform/linux/psu_dcdc_pd.c ${HAL_DIR}/platform/linux/temp_adc_ntc.c
    ${HAL_DIR}/platform/linux/gpio_dev.c
    ${OSIF_DIR}/linux/osif_log_linux.c)
add_dependencies(test_board_volt gen_boardconf_nano3s)
target_include_directories(test_board_volt PRIVATE ${HAL_DIR} ${HAL_DIR}/board ${HAL_DIR}/board
    ${HAL_DIR}/platform/linux ${OSIF_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(test_board_volt PRIVATE m)
add_test(NAME board_volt COMMAND test_board_volt)

add_executable(test_gpio_dev test_gpio_dev.c)
add_dependencies(test_gpio_dev gen_boardconf_nano3s)
target_include_directories(test_gpio_dev PRIVATE
    ${HAL_DIR}/platform/linux ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_compile_definitions(test_gpio_dev PRIVATE
    TST_GPIO_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-gpio")
add_test(NAME gpio_dev COMMAND test_gpio_dev)

add_executable(test_i2c_dev test_i2c_dev.c)
add_dependencies(test_i2c_dev gen_boardconf_nano3s)
target_include_directories(test_i2c_dev PRIVATE
    ${HAL_DIR}/platform/linux ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_compile_definitions(test_i2c_dev PRIVATE
    TST_I2C_NODE="${CMAKE_CURRENT_BINARY_DIR}/fx-i2c")
target_link_libraries(test_i2c_dev PRIVATE pthread)
add_test(NAME i2c_dev COMMAND test_i2c_dev)

add_executable(test_pwm_sys test_pwm_sys.c)
target_include_directories(test_pwm_sys PRIVATE ${HAL_DIR}/platform/linux)
target_compile_definitions(test_pwm_sys PRIVATE
    TST_PWM_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-pwm")
add_test(NAME pwm_sys COMMAND test_pwm_sys)

set(FAN_DEV_SRC test_fan_dev.c
    ${HAL_DIR}/platform/linux/fan_dev.c
    ${OSIF_DIR}/linux/osif_log_linux.c)

add_executable(test_fan_dev ${FAN_DEV_SRC} ${BOARDCONF_T})
add_dependencies(test_fan_dev gen_boardconf_nano3s)
target_include_directories(test_fan_dev PRIVATE ${HAL_DIR} ${HAL_DIR}/board ${HAL_DIR}/board
    ${HAL_DIR}/platform/linux ${OSIF_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_compile_definitions(test_fan_dev PRIVATE
    TST_PWM_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-pwmfan"
    TST_TACH_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-tachfan")
target_link_libraries(test_fan_dev PRIVATE pthread)
add_test(NAME fan_dev COMMAND test_fan_dev)

set(BOARDCONF_X3_INI ${HAL_DIR}/../tests/fixtures/boardconf_x3.ini)
set(BOARDCONF_X3_H ${CMAKE_CURRENT_BINARY_DIR}/gen_x3/boardconf.h)
set(BOARDCONF_X3_T ${HAL_DIR}/board/board_tables.c)
add_custom_command(OUTPUT ${BOARDCONF_X3_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen_x3
    COMMAND python3 ${GEN_BOARDCONF} ${BOARDCONF_X3_INI} ${BOARDCONF_X3_H}
    DEPENDS ${BOARDCONF_X3_INI} ${GEN_BOARDCONF}
    COMMENT "gen x3 boardconf from ${BOARDCONF_X3_INI}")
add_custom_target(gen_boardconf_x3 DEPENDS ${BOARDCONF_X3_H})
set(X3_INC ${HAL_DIR}/../tests/fixtures ${CMAKE_CURRENT_BINARY_DIR}/gen_x3)

add_executable(test_a3197s_pll_seq
    test_a3197s_pll_seq.c
    ${HAL_DIR}/asic/a3197s/a3197s_pll_seq.c
    ${HAL_DIR}/asic/a3197s/a3197s_pll.c
    ${HAL_DIR}/asic/a3197s/a3197s_cpm_table.c
    ${OSIF_DIR}/linux/osif_log_linux.c)
add_dependencies(test_a3197s_pll_seq gen_boardconf_nano3s)
target_include_directories(test_a3197s_pll_seq PRIVATE
    ${HAL_DIR}/asic/a3197s ${OSIF_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(test_a3197s_pll_seq PRIVATE m)
add_test(NAME a3197s_pll_seq COMMAND test_a3197s_pll_seq)

add_executable(test_a3197s_pll_seq_board
    test_a3197s_pll_seq_board.c
    ${HAL_DIR}/asic/a3197s/a3197s_pll_seq.c
    ${HAL_DIR}/asic/a3197s/a3197s_pll.c
    ${HAL_DIR}/asic/a3197s/a3197s_cpm_table.c
    ${OSIF_DIR}/linux/osif_log_linux.c)
add_dependencies(test_a3197s_pll_seq_board gen_boardconf_nano3s)
target_include_directories(test_a3197s_pll_seq_board PRIVATE
    ${HAL_DIR}/asic/a3197s ${OSIF_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(test_a3197s_pll_seq_board PRIVATE m)
add_test(NAME a3197s_pll_seq_board COMMAND test_a3197s_pll_seq_board)

add_executable(test_a3197s_pll_seq_board_x3
    test_a3197s_pll_seq_board.c
    ${HAL_DIR}/asic/a3197s/a3197s_pll_seq.c
    ${HAL_DIR}/asic/a3197s/a3197s_pll.c
    ${HAL_DIR}/asic/a3197s/a3197s_cpm_table.c
    ${OSIF_DIR}/linux/osif_log_linux.c)
add_dependencies(test_a3197s_pll_seq_board_x3 gen_boardconf_x3)
target_compile_definitions(test_a3197s_pll_seq_board_x3 PRIVATE TEST_BOARD_X3=1)
target_include_directories(test_a3197s_pll_seq_board_x3 PRIVATE
    ${HAL_DIR}/asic/a3197s ${OSIF_DIR} ${X3_INC})
target_link_libraries(test_a3197s_pll_seq_board_x3 PRIVATE m)
add_test(NAME a3197s_pll_seq_board_x3 COMMAND test_a3197s_pll_seq_board_x3)

add_executable(test_a3197s_pll_cs
    test_a3197s_pll_cs.c
    ${HAL_DIR}/asic/a3197s/a3197s_pll_seq.c
    ${HAL_DIR}/asic/a3197s/a3197s_pll.c
    ${HAL_DIR}/asic/a3197s/a3197s_cpm_table.c
    ${OSIF_DIR}/linux/osif_log_linux.c)
add_dependencies(test_a3197s_pll_cs gen_boardconf_nano3s)
target_include_directories(test_a3197s_pll_cs PRIVATE ${HAL_DIR} ${HAL_DIR}/board ${HAL_DIR}/board
    ${HAL_DIR}/asic/a3197s ${OSIF_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(test_a3197s_pll_cs PRIVATE m)
add_test(NAME a3197s_pll_cs COMMAND test_a3197s_pll_cs)

set(UART_CS_SRC test_uart_chip_select.c
    ${HAL_DIR}/asic/a3197s/uart_pro.c
    ${OSIF_DIR}/linux/osif_log_linux.c
    ${OSIF_DIR}/linux/osif_timer_linux.c)

add_executable(test_uart_chip_select ${UART_CS_SRC} ${BOARDCONF_T})
add_dependencies(test_uart_chip_select gen_boardconf_nano3s)
target_include_directories(test_uart_chip_select PRIVATE ${HAL_DIR} ${HAL_DIR}/board
    ${HAL_DIR}/board ${HAL_DIR}/asic/a3197s ${OSIF_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_test(NAME uart_chip_select COMMAND test_uart_chip_select)

add_executable(test_uart_chip_select_x3 ${UART_CS_SRC} ${BOARDCONF_X3_T})
add_dependencies(test_uart_chip_select_x3 gen_boardconf_x3)
target_compile_definitions(test_uart_chip_select_x3 PRIVATE TEST_BOARD_X3=1)
target_include_directories(test_uart_chip_select_x3 PRIVATE ${HAL_DIR} ${HAL_DIR}/board
    ${HAL_DIR}/asic/a3197s ${OSIF_DIR} ${X3_INC})
add_test(NAME uart_chip_select_x3 COMMAND test_uart_chip_select_x3)

set(UART_SILENT_SRC test_uart_silent.c
    ${OSIF_DIR}/linux/osif_timer_linux.c)

add_executable(test_uart_silent ${UART_SILENT_SRC} ${BOARDCONF_T})
add_dependencies(test_uart_silent gen_boardconf_nano3s)
target_compile_definitions(test_uart_silent PRIVATE
    TST_UART_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-uart")
target_include_directories(test_uart_silent PRIVATE ${HAL_DIR} ${HAL_DIR}/board ${HAL_DIR}/board
    ${HAL_DIR}/asic/a3197s ${OSIF_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_test(NAME uart_silent COMMAND test_uart_silent)

add_executable(test_uart_silent_x3 ${UART_SILENT_SRC} ${BOARDCONF_X3_T})
add_dependencies(test_uart_silent_x3 gen_boardconf_x3)
target_compile_definitions(test_uart_silent_x3 PRIVATE TEST_BOARD_X3=1
    TST_UART_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-uartx3")
target_include_directories(test_uart_silent_x3 PRIVATE ${HAL_DIR} ${HAL_DIR}/board
    ${HAL_DIR}/asic/a3197s ${OSIF_DIR} ${X3_INC})
add_test(NAME uart_silent_x3 COMMAND test_uart_silent_x3)

set(UART_RECOVER_SRC test_uart_recover.c ${OSIF_DIR}/linux/osif_timer_linux.c)

add_executable(test_uart_recover ${UART_RECOVER_SRC} ${BOARDCONF_T})
add_dependencies(test_uart_recover gen_boardconf_nano3s)
target_compile_definitions(test_uart_recover PRIVATE
    TST_UART_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-uartrec")
target_include_directories(test_uart_recover PRIVATE ${HAL_DIR} ${HAL_DIR}/board
    ${HAL_DIR}/asic/a3197s ${OSIF_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_test(NAME uart_recover COMMAND test_uart_recover)

add_executable(test_uart_recover_x3 ${UART_RECOVER_SRC} ${BOARDCONF_X3_T})
add_dependencies(test_uart_recover_x3 gen_boardconf_x3)
target_compile_definitions(test_uart_recover_x3 PRIVATE TEST_BOARD_X3=1
    TST_UART_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-uartrecx3")
target_include_directories(test_uart_recover_x3 PRIVATE ${HAL_DIR} ${HAL_DIR}/board
    ${HAL_DIR}/asic/a3197s ${OSIF_DIR} ${X3_INC})
add_test(NAME uart_recover_x3 COMMAND test_uart_recover_x3)

add_executable(test_asic_limits test_asic_limits.c)
add_dependencies(test_asic_limits gen_boardconf_nano3s)
target_include_directories(test_asic_limits PRIVATE
    ${HAL_DIR}/../common/proto ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_test(NAME asic_limits COMMAND test_asic_limits)

add_executable(test_asic_limits_x3 test_asic_limits.c)
add_dependencies(test_asic_limits_x3 gen_boardconf_x3)
target_compile_definitions(test_asic_limits_x3 PRIVATE TEST_BOARD_X3=1)
target_include_directories(test_asic_limits_x3 PRIVATE
    ${HAL_DIR}/../common/proto ${X3_INC})
add_test(NAME asic_limits_x3 COMMAND test_asic_limits_x3)

add_executable(test_board_power test_board_power.c ${BOARDCONF_T}
    ${HAL_DIR}/platform/linux/board_sensors.c
    ${HAL_DIR}/platform/linux/psu_dcdc_pd.c ${HAL_DIR}/platform/linux/temp_adc_ntc.c
    ${HAL_DIR}/platform/linux/gpio_dev.c
    ${OSIF_DIR}/linux/osif_log_linux.c)
add_dependencies(test_board_power gen_boardconf_nano3s)
target_include_directories(test_board_power PRIVATE ${HAL_DIR} ${HAL_DIR}/board ${HAL_DIR}/board
    ${HAL_DIR}/platform/linux ${OSIF_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(test_board_power PRIVATE m)
add_test(NAME board_power COMMAND test_board_power)

add_executable(test_fan_dev_x3 ${FAN_DEV_SRC} ${BOARDCONF_X3_T})
add_dependencies(test_fan_dev_x3 gen_boardconf_x3)
target_include_directories(test_fan_dev_x3 PRIVATE ${HAL_DIR} ${HAL_DIR}/board
    ${HAL_DIR}/platform/linux ${OSIF_DIR} ${X3_INC})
target_compile_definitions(test_fan_dev_x3 PRIVATE
    TST_PWM_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-pwmfanx3"
    TST_TACH_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-tachfanx3")
target_link_libraries(test_fan_dev_x3 PRIVATE pthread)
add_test(NAME fan_dev_x3 COMMAND test_fan_dev_x3)

add_executable(test_fan_dev_x3_active_low ${FAN_DEV_SRC} ${BOARDCONF_X3_T})
add_dependencies(test_fan_dev_x3_active_low gen_boardconf_x3)
target_include_directories(test_fan_dev_x3_active_low PRIVATE ${HAL_DIR} ${HAL_DIR}/board
    ${HAL_DIR}/platform/linux ${OSIF_DIR} ${X3_INC})
target_compile_definitions(test_fan_dev_x3_active_low PRIVATE FAN_PWM_ACTIVE_LOW=1
    TST_PWM_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-pwmfanx3al"
    TST_TACH_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-tachfanx3al")
target_link_libraries(test_fan_dev_x3_active_low PRIVATE pthread)
add_test(NAME fan_dev_x3_active_low COMMAND test_fan_dev_x3_active_low)

set(TIMER_SRC test_timer.c
    ${OSIF_DIR}/linux/osif_log_linux.c)

add_executable(test_timer ${TIMER_SRC} ${BOARDCONF_T})
add_dependencies(test_timer gen_boardconf_nano3s)
target_compile_definitions(test_timer PRIVATE
    TST_TACH_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-tach")
target_include_directories(test_timer PRIVATE ${HAL_DIR} ${HAL_DIR}/board ${HAL_DIR}/board
    ${HAL_DIR}/platform/linux ${OSIF_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_test(NAME timer COMMAND test_timer)

add_executable(test_timer_x3 ${TIMER_SRC} ${BOARDCONF_X3_T})
add_dependencies(test_timer_x3 gen_boardconf_x3)
target_compile_definitions(test_timer_x3 PRIVATE TEST_BOARD_X3=1
    TST_TACH_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-tachx3")
target_include_directories(test_timer_x3 PRIVATE ${HAL_DIR} ${HAL_DIR}/board
    ${HAL_DIR}/platform/linux ${OSIF_DIR} ${X3_INC})
add_test(NAME timer_x3 COMMAND test_timer_x3)

add_executable(test_board_power_x3 test_board_power.c ${BOARDCONF_X3_T}
    ${HAL_DIR}/platform/linux/board_sensors.c
    ${HAL_DIR}/platform/linux/psu_dcdc_pd.c ${HAL_DIR}/platform/linux/temp_adc_ntc.c
    ${HAL_DIR}/platform/linux/gpio_dev.c
    ${OSIF_DIR}/linux/osif_log_linux.c)
add_dependencies(test_board_power_x3 gen_boardconf_x3)
target_compile_definitions(test_board_power_x3 PRIVATE TEST_BOARD_X3=1)
target_include_directories(test_board_power_x3 PRIVATE ${HAL_DIR} ${HAL_DIR}/board
    ${HAL_DIR}/platform/linux ${OSIF_DIR} ${X3_INC})
target_link_libraries(test_board_power_x3 PRIVATE m)
add_test(NAME board_power_x3 COMMAND test_board_power_x3)

set(MM_HB_SRC test_mm_hashboard.c ${HAL_DIR}/platform/linux/hashmcu.c
    ${HAL_DIR}/platform/linux/i2c_dev.c
    ${OSIF_DIR}/linux/osif_timer_linux.c
    ${OSIF_DIR}/linux/osif_log_linux.c)

add_executable(test_mm_hashboard ${MM_HB_SRC} ${BOARDCONF_T})
add_dependencies(test_mm_hashboard gen_boardconf_nano3s)
target_compile_definitions(test_mm_hashboard PRIVATE
    TST_GPIO_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-gpiohb")
target_include_directories(test_mm_hashboard PRIVATE ${HAL_DIR} ${HAL_DIR}/board ${HAL_DIR}/board
    ${HAL_DIR}/platform/linux ${OSIF_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(test_mm_hashboard PRIVATE pthread)
add_test(NAME mm_hashboard COMMAND test_mm_hashboard)

add_executable(test_mm_hashboard_x3 ${MM_HB_SRC} ${BOARDCONF_X3_T})
add_dependencies(test_mm_hashboard_x3 gen_boardconf_x3)
target_compile_definitions(test_mm_hashboard_x3 PRIVATE
    TST_GPIO_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fx-gpiohbx3")
target_include_directories(test_mm_hashboard_x3 PRIVATE ${HAL_DIR} ${HAL_DIR}/board
    ${HAL_DIR}/platform/linux ${OSIF_DIR}
    ${HAL_DIR}/../tests/fixtures ${CMAKE_CURRENT_BINARY_DIR}/gen_x3)
target_link_libraries(test_mm_hashboard_x3 PRIVATE pthread)
add_test(NAME mm_hashboard_x3 COMMAND test_mm_hashboard_x3)

set(HASHMCU_SRC test_hashmcu.c ${HAL_DIR}/platform/linux/hashmcu.c
    ${HAL_DIR}/platform/linux/gpio_dev.c
    ${OSIF_DIR}/linux/osif_timer_linux.c
    ${OSIF_DIR}/linux/osif_log_linux.c)

add_executable(test_hashmcu ${HASHMCU_SRC} ${BOARDCONF_T})
add_dependencies(test_hashmcu gen_boardconf_nano3s)
target_include_directories(test_hashmcu PRIVATE ${HAL_DIR} ${HAL_DIR}/board ${HAL_DIR}/board
    ${HAL_DIR}/platform/linux ${OSIF_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_test(NAME hashmcu COMMAND test_hashmcu)

add_executable(test_hashmcu_x3 ${HASHMCU_SRC} ${BOARDCONF_X3_T})
add_dependencies(test_hashmcu_x3 gen_boardconf_x3)
target_include_directories(test_hashmcu_x3 PRIVATE ${HAL_DIR} ${HAL_DIR}/board
    ${HAL_DIR}/platform/linux ${OSIF_DIR} ${X3_INC})
add_test(NAME hashmcu_x3 COMMAND test_hashmcu_x3)

add_executable(test_eeprom_dev test_eeprom_dev.c
    ${HAL_DIR}/platform/linux/eeprom_dev.c)
add_dependencies(test_eeprom_dev gen_boardconf_nano3s)
target_include_directories(test_eeprom_dev PRIVATE
    ${HAL_DIR}/platform/linux ${OSIF_DIR} ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_test(NAME eeprom_dev COMMAND test_eeprom_dev)

add_executable(test_mtd_dev test_mtd_dev.c
    ${HAL_DIR}/platform/linux/mtd_dev.c)
target_include_directories(test_mtd_dev PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fake ${HAL_DIR}/platform/linux ${OSIF_DIR})
add_test(NAME mtd_dev COMMAND test_mtd_dev)

add_executable(test_ubootenv test_ubootenv.c
    ${HAL_DIR}/platform/linux/ubootenv.c)
target_include_directories(test_ubootenv PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fake ${HAL_DIR}/platform/linux ${OSIF_DIR})
add_test(NAME ubootenv COMMAND test_ubootenv)

add_executable(test_wdt_dev test_wdt_dev.c)
target_compile_definitions(test_wdt_dev PRIVATE
    TST_WDT_DEV="${CMAKE_CURRENT_BINARY_DIR}/fx-wdt")
target_include_directories(test_wdt_dev PRIVATE
    ${HAL_DIR}/platform/linux ${OSIF_DIR})
add_test(NAME wdt_dev COMMAND test_wdt_dev)

add_executable(test_net_linux_common test_net_linux_common.c)
add_dependencies(test_net_linux_common gen_boardconf_nano3s)
target_include_directories(test_net_linux_common PRIVATE
    ${HAL_DIR} ${HAL_DIR}/platform/linux ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_test(NAME net_linux_common COMMAND test_net_linux_common)
