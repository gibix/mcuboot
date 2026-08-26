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
zephyr_library_link_libraries(MCUBOOT_BOOTUTIL)
