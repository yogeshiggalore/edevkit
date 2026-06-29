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
find_program(PIOASM_EXECUTABLE
    NAMES pioasm
    HINTS
        $ENV{PIOASM}
        $ENV{HOME}/.pico-sdk/tools/2.2.0/pioasm
        $ENV{HOME}/.pico-sdk/tools/2.1.1/pioasm
)

if(NOT PIOASM_EXECUTABLE)
    message(FATAL_ERROR "pioasm not found")
endif()

set(PIO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/zephyr/include/generated)

foreach(pio_name toggle probe_swd)
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/pio/${pio_name}.pio)
    set(hdr ${PIO_GEN_DIR}/${pio_name}.pio.h)
    add_custom_command(
        OUTPUT ${hdr}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PIO_GEN_DIR}
        COMMAND ${PIOASM_EXECUTABLE} -o c-sdk ${src} ${hdr}
        DEPENDS ${src}
        VERBATIM
    )
    add_custom_target(${pio_name}_pio_header DEPENDS ${hdr})
    add_dependencies(app ${pio_name}_pio_header)
endforeach()

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/zephyr/include/generated)
