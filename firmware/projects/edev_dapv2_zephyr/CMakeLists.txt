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

target_sources(app PRIVATE
    src/main.c
)

target_include_directories(app PRIVATE src)
