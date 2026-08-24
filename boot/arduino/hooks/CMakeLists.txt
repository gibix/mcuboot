# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

# Board-specific headers (usb_soft_disconnect.h, pfswap.h) live in the
# ArduinoCore variant directory. Pass -DARDUINO_VARIANT_DIR=<path> from the
# build command so the hooks module stays board-agnostic.
if(NOT DEFINED ARDUINO_VARIANT_DIR)
  message(FATAL_ERROR "ARDUINO_VARIANT_DIR must be set (path to the variant directory)")
endif()
zephyr_library_include_directories(${ARDUINO_VARIANT_DIR})

zephyr_library_sources(hooks.c)
zephyr_library_sources(status_led.c)
# Must be part of the bootloader on this SoC, not optional: see pfswap.c.
zephyr_library_sources(pfswap.c)
zephyr_library_sources_ifdef(CONFIG_USBD_DFU dfu_images.c)
zephyr_library_sources_ifdef(CONFIG_USB_DEVICE_STACK_NEXT usb_setup.c)
zephyr_library_link_libraries(MCUBOOT_BOOTUTIL)

# BFM flash/boot sources for direct DFU write to the spare BFM Dual-Boot slot.
# PIC32CK-specific; other SoCs don't have BFM Dual-Boot.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/bfm/bfm_flash.c")
  zephyr_library_include_directories(${CMAKE_CURRENT_LIST_DIR}/bfm)
  zephyr_library_sources(${CMAKE_CURRENT_LIST_DIR}/bfm/bfm_flash.c
                         ${CMAKE_CURRENT_LIST_DIR}/bfm/bfm_boot.c)
  zephyr_library_compile_definitions(HAS_BFM_FLASH=1)
endif()
