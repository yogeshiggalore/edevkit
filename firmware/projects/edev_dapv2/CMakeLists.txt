# SPDX-License-Identifier: MIT
# edev_dapv2 — CMSIS-DAP v2 firmware for RP2350 (Pico 2 W default).
#
# Substrate: pico-sdk + TinyUSB. Pure CMSIS-DAP v2 transport — no
# vendor commands, no host-side custom tool. Works out-of-the-box with
# probe-rs, pyocd, OpenOCD.

cmake_minimum_required(VERSION 3.13)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if (NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    if (EXISTS ${CMAKE_CURRENT_LIST_DIR}/pico-sdk)
        set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-sdk)
    endif()
endif()

if (NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w)
endif()

include(pico_sdk_import.cmake)

set(EDEV_DAPV2_VERSION_MAJOR 0)
set(EDEV_DAPV2_VERSION_MINOR 1)
set(EDEV_DAPV2_VERSION_PATCH 0)
set(EDEV_DAPV2_VERSION
    "${EDEV_DAPV2_VERSION_MAJOR}.${EDEV_DAPV2_VERSION_MINOR}.${EDEV_DAPV2_VERSION_PATCH}")
set(EDEV_DAPV2_VERSION_TAG
    "v${EDEV_DAPV2_VERSION_MAJOR}_${EDEV_DAPV2_VERSION_MINOR}")

project(edev_dapv2
    VERSION ${EDEV_DAPV2_VERSION}
    DESCRIPTION "CMSIS-DAP v2 debug probe for RP2350"
    LANGUAGES C CXX ASM)

pico_sdk_init()

if (${PICO_SDK_VERSION_MAJOR} LESS 2)
    message(SEND_ERROR "pico-sdk v2 or newer required (RP2350 support)")
endif()

# Generate PIO header(s) — pioasm is bundled with pico-sdk.
# `pico_generate_pio_header` runs pioasm at build time and adds the
# output dir to the target's include path.

# ----------------------------------------------------------------------
# Sources
# ----------------------------------------------------------------------
add_executable(edev_dapv2
    src/main.c

    src/util/led.c
    src/util/log.c
    src/util/safety.c

    src/hw/chip_id.c
    src/hw/probe.c

    src/usb/usb_descriptors.c
    src/usb/usb_dap_class.c

    src/dap/dap.c
    src/dap/dap_info.c
    src/dap/dap_general.c
    src/dap/dap_swj.c
    src/dap/dap_swd.c
    src/dap/dap_jtag.c
    src/dap/dap_swo.c
    src/dap/dap_atomic.c
)

pico_generate_pio_header(edev_dapv2
    ${CMAKE_CURRENT_LIST_DIR}/src/pio/probe_swd.pio
)

target_include_directories(edev_dapv2 PRIVATE
    src
)

target_compile_definitions(edev_dapv2 PRIVATE
    EDEV_DAPV2_VERSION_STR="${EDEV_DAPV2_VERSION}"
    EDEV_DAPV2_VERSION_MAJOR=${EDEV_DAPV2_VERSION_MAJOR}
    EDEV_DAPV2_VERSION_MINOR=${EDEV_DAPV2_VERSION_MINOR}
    EDEV_DAPV2_VERSION_PATCH=${EDEV_DAPV2_VERSION_PATCH}
)

target_compile_options(edev_dapv2 PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# stdio: UART0 (GP0/GP1). USB is owned by our DAP class driver.
pico_enable_stdio_usb(edev_dapv2 0)
pico_enable_stdio_uart(edev_dapv2 1)

target_link_libraries(edev_dapv2
    pico_stdlib
    pico_unique_id
    hardware_gpio
    hardware_pio
    hardware_watchdog
    tinyusb_device
)

# pico_cyw43_arch_none gives us cyw43_arch_gpio_put() for the on-board
# LED (which lives on the CYW43439's GPIO 0, not an RP2350 GPIO) WITHOUT
# pulling in lwIP / WiFi. ~30-50 KB of cyw43 driver code, no networking.
if (PICO_BOARD STREQUAL "pico2_w" OR PICO_BOARD STREQUAL "pico_w")
    target_link_libraries(edev_dapv2 pico_cyw43_arch_none)
endif()

pico_add_extra_outputs(edev_dapv2)

set_target_properties(edev_dapv2 PROPERTIES
    OUTPUT_NAME "edev_dapv2_${EDEV_DAPV2_VERSION_TAG}"
)
