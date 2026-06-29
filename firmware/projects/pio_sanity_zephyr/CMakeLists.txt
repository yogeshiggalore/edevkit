# SPDX-License-Identifier: Apache-2.0
#
# pio_sanity_zephyr — minimal Zephyr app that exercises PIO output on RP2350.
# No external instruments needed; the main loop polls the pad input via SIO
# and reports the state Zephyr sees. If PIO is actually driving the pin, the
# poll will see it alternate HIGH/LOW at the toggle rate.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pio_sanity_zephyr LANGUAGES C)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

# pioasm — same auto-generation path as edev_dapv2_zephyr.
set(PIO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/pio/toggle.pio)
set(PIO_HDR ${CMAKE_CURRENT_BINARY_DIR}/zephyr/include/generated/toggle.pio.h)

find_program(PIOASM_EXECUTABLE
    NAMES pioasm
    HINTS
        $ENV{PIOASM}
        $ENV{HOME}/.pico-sdk/tools/2.2.0/pioasm
        $ENV{HOME}/.pico-sdk/tools/2.1.1/pioasm
)

if(PIOASM_EXECUTABLE)
    add_custom_command(
        OUTPUT ${PIO_HDR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/zephyr/include/generated
        COMMAND ${PIOASM_EXECUTABLE} -o c-sdk ${PIO_SRC} ${PIO_HDR}
        DEPENDS ${PIO_SRC}
        VERBATIM
    )
    add_custom_target(toggle_pio_header DEPENDS ${PIO_HDR})
    add_dependencies(app toggle_pio_header)
else()
    message(FATAL_ERROR "pioasm not found")
endif()

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/zephyr/include/generated)
