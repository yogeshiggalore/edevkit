# SPDX-License-Identifier: MIT
# edev_dapv2 — CMSIS-DAP v2 firmware for RP2350 (Pico 2 / XIAO RP2350)
#
# Substrate: pico-sdk + TinyUSB. Everything in src/ is our code; we do
# not vendor ARM's CMSIS-DAP reference implementation.

cmake_minimum_required(VERSION 3.13)

# Default to a release build with debug info — we want the optimiser on
# (SWD bit-bang code is timing sensitive) but symbols available for gdb.
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Use the in-tree pico-sdk checkout (symlinked to ../dap_v2/pico-sdk to
# avoid duplicating 382 MB). If you want to point at a different SDK,
# set PICO_SDK_PATH in the environment before configuring.
if (NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-sdk)
endif()

# Default board: Raspberry Pi Pico 2 (RP2350A, ARM-S). Override with
# -DPICO_BOARD=… for XIAO RP2350 (`seeed_xiao_rp2350`) or others.
if (NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2)
endif()

include(pico_sdk_import.cmake)

# ----------------------------------------------------------------------
# Firmware version. Bump on every release. Used as:
#   - the OUTPUT_NAME suffix (edev_dapv2_v0_1.uf2)
#   - the DAP_Info(0x09) "Product Firmware Version" string
#   - the human label in releases/
#
# Convention:
#   *_MAJOR.*_MINOR.*_PATCH      → semver, e.g. "0.1.0"
#   v${MAJOR}_${MINOR}           → filename tag, e.g. "v0_1"
#                                  patch level is NOT in the filename —
#                                  one v0_1 release picks up all
#                                  v0.1.x rebuilds; the semver string
#                                  inside the firmware distinguishes
#                                  them.
# ----------------------------------------------------------------------
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

# ----------------------------------------------------------------------
# Sources
# ----------------------------------------------------------------------

add_executable(edev_dapv2
    src/main.c
    src/util/led.c
    src/hw/chip_id.c

    # M2 — USB layer (descriptors + custom class driver)
    src/usb/usb_descriptors.c
    src/usb/usb_dap_class.c

    # M3 — DAP protocol layer (dispatcher + per-group handlers).
    # In M3 most are spec-compliant skeletons; M4/M5 fill in the actual
    # pin/PIO interaction.
    src/dap/dap.c
    src/dap/dap_info.c
    src/dap/dap_general.c
    src/dap/dap_swj.c
    src/dap/dap_swd.c
    src/dap/dap_jtag.c
    src/dap/dap_swo.c
    src/dap/dap_atomic.c
    src/dap/dap_vendor.c

    # M5 — probe bit-banger shim (SWD via PIO)
    src/hw/probe.c

    # M6 — JTAG bit-banger (SIO; PIO upgrade later)
    src/hw/jtag.c

    # M7 — SWO capture (PIO UART RX + ring buffer)
    src/hw/swo.c
)

target_include_directories(edev_dapv2 PRIVATE
    src
    src/usb
    src/dap
    src/hw
    src/util
)

# PIO programs — assembled to C headers by pico-sdk's pioasm.
pico_generate_pio_header(edev_dapv2 ${CMAKE_CURRENT_LIST_DIR}/src/pio/probe.pio)
pico_generate_pio_header(edev_dapv2 ${CMAKE_CURRENT_LIST_DIR}/src/pio/probe_keepalive.pio)
pico_generate_pio_header(edev_dapv2 ${CMAKE_CURRENT_LIST_DIR}/src/pio/swo_uart.pio)

# ----------------------------------------------------------------------
# Compile-time configuration
# ----------------------------------------------------------------------

target_compile_definitions(edev_dapv2 PRIVATE
    # Fix for a USB controller errata on RP2 silicon — required for
    # reliable enumeration on some hosts. pico-sdk handles the workaround
    # when this is set.
    PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1

    # DAP-protocol version we advertise via DAP_Info(0x04). probe-rs
    # 0.31+ rejects DAP_FW_VER < "2.2.0" at probe-open time. The protocol
    # surface in src/dap/ meets the 2.2.0 spec.
    EDEV_DAPV2_DAP_FW_VER="2.2.0"

    # Firmware version — surfaced via DAP_Info(0x09) "Product Firmware
    # Version" and embedded in the UF2 filename. Owned by the version
    # block at the top of this file; do not hard-code elsewhere.
    EDEV_DAPV2_FW_VERSION="${EDEV_DAPV2_VERSION}"

    # Default packet size and count we advertise via DAP_Info(0xFF, 0xFE).
    # Larger than 64 = host can pipeline multiple bulk transfers into one
    # logical DAP packet, which unlocks bulk-mode throughput.
    EDEV_DAPV2_DAP_PACKET_SIZE=512
    EDEV_DAPV2_DAP_PACKET_COUNT=4
)

target_compile_options(edev_dapv2 PRIVATE
    -Wall
    -Wextra
    -Wshadow
    -Wno-unused-parameter
    -Werror=return-type
    -ffunction-sections
    -fdata-sections
    # -Werror=implicit-function-declaration is C-only (g++ warns about
    # the flag itself for C++ TUs) — apply via $<COMPILE_LANGUAGE:C>.
    $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>
)

# ----------------------------------------------------------------------
# pico-sdk linkage
# ----------------------------------------------------------------------

target_link_libraries(edev_dapv2 PRIVATE
    pico_stdlib            # gpio, uart, time, sleep helpers
    pico_unique_id         # 64-bit chip ID for the serial number string
    hardware_gpio
    hardware_pio           # PIO programming (probe.pio)
    hardware_clocks        # clock_get_hz for PIO divider calc
    tinyusb_device         # USB device stack

    # M7+
#    hardware_dma           # DMA for SWO ring buffer
)

# ----------------------------------------------------------------------
# Output formats
# ----------------------------------------------------------------------

# Build artifacts get the version suffix baked into their filenames —
# edev_dapv2_v0_1.elf, edev_dapv2_v0_1.uf2, etc. pico_add_extra_outputs
# reads the executable's OUTPUT_NAME for its derived files.
set_target_properties(edev_dapv2 PROPERTIES
    OUTPUT_NAME "edev_dapv2_${EDEV_DAPV2_VERSION_TAG}")

pico_add_extra_outputs(edev_dapv2)         # .uf2, .hex, .bin, .map
pico_enable_stdio_usb(edev_dapv2 0)        # no stdio over USB (we own USB)
pico_enable_stdio_uart(edev_dapv2 1)       # stdio over UART0 for printf debug

# Print memory usage at link time — keeps us honest about footprint.
target_link_options(edev_dapv2 PRIVATE
    -Wl,--print-memory-usage
)

# ----------------------------------------------------------------------
# releases/ — every successful build drops a copy of the UF2 + ELF into
# the in-tree releases directory, named with the same version tag. Old
# releases stay there as a permanent record; bump EDEV_DAPV2_VERSION_* at
# the top of this file when you want a new slot.
#
# This is a thin convenience — the canonical artifact is whatever the
# tagged commit produces. We don't gate the build on this copy; if you
# rm -rf releases/ during dev it'll come back next make.
# ----------------------------------------------------------------------
set(EDEV_DAPV2_RELEASE_DIR ${CMAKE_SOURCE_DIR}/releases)
set(EDEV_DAPV2_RELEASE_BASE ${EDEV_DAPV2_RELEASE_DIR}/edev_dapv2_${EDEV_DAPV2_VERSION_TAG})

add_custom_command(TARGET edev_dapv2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${EDEV_DAPV2_RELEASE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE_DIR:edev_dapv2>/edev_dapv2_${EDEV_DAPV2_VERSION_TAG}.uf2
        ${EDEV_DAPV2_RELEASE_BASE}.uf2
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE_DIR:edev_dapv2>/edev_dapv2_${EDEV_DAPV2_VERSION_TAG}.elf
        ${EDEV_DAPV2_RELEASE_BASE}.elf
    COMMENT "→ releases/edev_dapv2_${EDEV_DAPV2_VERSION_TAG}.{uf2,elf}"
    VERBATIM
)
