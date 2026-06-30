# SPDX-License-Identifier: Apache-2.0
#
# edev_dapv2 — Zephyr RTOS port.
#
# CMSIS-DAP v2 debug probe firmware for Raspberry Pi Pico 2 (RP2350).
# Ports the working pico-sdk implementation at firmware/projects/edev_dapv2/ to
# Zephyr 4.4.x. The SWD bit-banger is PIO-driven (probe_swd.pio); the rest of
# the stack uses Zephyr USB-Device-Next + subsys/dap.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(edev_dapv2_zephyr LANGUAGES C)

# Pull in the sample USBD helper (sample_usbd_setup_device + Kconfig symbols).
# common.cmake adds sample_usbd_init.c when CONFIG_USB_DEVICE_STACK_NEXT=y and
# wires the include directory.
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

# --------------------------------------------------------------------------
# pioasm — autogenerate probe_swd.pio.h from the .pio source at configure
# time. The pico-sdk vendors pioasm; we use whichever we can find in this
# order: (1) PIOASM env var, (2) system pioasm on PATH, (3) the pico-sdk
# install at ~/.pico-sdk/tools/2.2.0/pioasm/pioasm. If none works, fall back
# to the header that is checked into pio/ so a build is still possible on a
# machine without pioasm.
# --------------------------------------------------------------------------
set(PIO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/pio/probe_swd.pio)
set(PIO_HDR ${CMAKE_CURRENT_BINARY_DIR}/zephyr/include/generated/probe_swd.pio.h)

find_program(PIOASM_EXECUTABLE
    NAMES pioasm
    HINTS
        $ENV{PIOASM}
        $ENV{PICO_SDK_PATH}/tools/pioasm
        $ENV{HOME}/.pico-sdk/tools/2.2.0/pioasm
        $ENV{HOME}/.pico-sdk/tools/2.1.1/pioasm
    DOC "Path to the pioasm executable from pico-sdk"
)

if(PIOASM_EXECUTABLE)
    message(STATUS "pioasm: ${PIOASM_EXECUTABLE} (regenerate ${PIO_HDR} from .pio source)")
    add_custom_command(
        OUTPUT ${PIO_HDR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/zephyr/include/generated
        COMMAND ${PIOASM_EXECUTABLE} -o c-sdk ${PIO_SRC} ${PIO_HDR}
        DEPENDS ${PIO_SRC}
        COMMENT "pioasm: ${PIO_SRC} -> probe_swd.pio.h"
        VERBATIM
    )
    add_custom_target(probe_swd_pio_header DEPENDS ${PIO_HDR})
    add_dependencies(app probe_swd_pio_header)
    set(PIO_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/zephyr/include/generated)
else()
    message(WARNING
        "pioasm not found — falling back to checked-in pio/probe_swd.pio.h. "
        "Edits to probe_swd.pio will NOT be picked up automatically. "
        "Set PIOASM env var or install pico-sdk to enable regeneration.")
    set(PIO_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/pio)
endif()

target_sources(app PRIVATE
    src/main.c
    src/swclk_keepalive.c
    drivers/dp/swdp_pio_rpi_pico.c
)

# nRF5340 ops — recover/erase/write/read primitives. Off by default;
# enable with CONFIG_EDEV_NRF53_OPS=y. Algorithm reference:
# docs/NRF5340_ALGORITHMS.md.
target_sources(app PRIVATE
    $<$<BOOL:${CONFIG_EDEV_NRF53_OPS}>:src/nrf53/nrf53_dp.c>
    $<$<BOOL:${CONFIG_EDEV_NRF53_OPS}>:src/nrf53/nrf53_ap.c>
    $<$<BOOL:${CONFIG_EDEV_NRF53_OPS}>:src/nrf53/nrf53_ctrl_ap.c>
    $<$<BOOL:${CONFIG_EDEV_NRF53_OPS}>:src/nrf53/nrf53_nvmc.c>
    $<$<BOOL:${CONFIG_EDEV_NRF53_OPS}>:src/nrf53/nrf53_uicr.c>
    $<$<BOOL:${CONFIG_EDEV_NRF53_OPS}>:src/nrf53/nrf53_stubs.c>
    $<$<BOOL:${CONFIG_EDEV_NRF53_OPS}>:src/nrf53/nrf53_recover.c>
    $<$<BOOL:${CONFIG_EDEV_NRF53_OPS}>:src/nrf53/nrf53_read_mem.c>
    $<$<BOOL:${CONFIG_EDEV_NRF53_OPS}>:src/nrf53/nrf53_flash_write.c>
    $<$<BOOL:${CONFIG_EDEV_NRF53_OPS}>:src/nrf53/nrf53_target_info.c>
    $<$<BOOL:${CONFIG_EDEV_NRF53_OPS}>:src/nrf53/nrf53_vendor.c>
)

target_include_directories(app PRIVATE
    src
    src/nrf53
    ${PIO_INCLUDE_DIR}
)
