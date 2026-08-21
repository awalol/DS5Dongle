# ESP-IDF project for the ESP32-S31-Function-CoreBoard-1 target.
#
# Build with:
#   idf.py -C ports/esp32s31 set-target esp32s31
#   idf.py -C ports/esp32s31 build
#
# The Pico2W / Waveshare builds continue to use the top-level CMakeLists.txt and
# are unaffected by anything here.

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# TinyUSB includes "tusb_config.h" from its own sources, and BTstack includes
# "btstack_config.h" from its own -- both expect the application's copy to be
# globally reachable, which is how the pico-sdk build provides them. Put the
# firmware's src/ on every component's include path to match.
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_LIST_DIR}/../../src" APPEND)

# CFG_TUSB_MCU/CFG_TUSB_OS must agree between the application and the TinyUSB
# library or their shared types diverge (tu_fifo_t carries an osal_mutex_t).
# Setting them per-component let the library fall through to tusb_config.h's
# default while the app compiled with something else.
#
# OPT_OS_NONE, emphatically NOT OPT_OS_FREERTOS: tud_task() is
# tud_task_ext(UINT32_MAX, false), and under the FreeRTOS OSAL that timeout
# blocks the caller until a USB event arrives. This firmware calls it from a
# superloop that must also service BTstack and feed the watchdog, so FreeRTOS
# osal deadlocks on the first idle iteration.
#
# Safe here because the USB ISR and tud_task() are both on core 0, and
# osal_none guards its FIFO by masking interrupts; nothing on core 1 touches
# TinyUSB.
idf_build_set_property(COMPILE_OPTIONS "-DCFG_TUSB_MCU=OPT_MCU_ESP32S31" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DCFG_TUSB_OS=OPT_OS_NONE" APPEND)

# Enumerate as a HIGH-speed device, matching the real DualSense. The S31's only
# PHY is high-speed, so the PHY the core selects and the one the SoC bonds out
# agree. bInterval is an exponent at high speed; ep_interval() in
# src/usb_descriptors.cpp encodes it for whichever speed is built.
idf_build_set_property(COMPILE_OPTIONS "-DBOARD_TUD_MAX_SPEED=OPT_MODE_HIGH_SPEED" APPEND)

# dwc2 fixes for this part, applied to dcd_dwc2.c at configure time:
#
#   - clear_stall sets EPCTL_SD0PID_SEVNFRM unconditionally; on an iso endpoint
#     that is SetEvenFrame, retargeting an already-armed transfer.
#   - The armed (micro)frame is one interval away, not the next one -- the two
#     coincide only at interval 1, so this is invisible at full speed.
#   - GINTSTS_PXFR_INCOMPISOOUT was declared and never handled.
#   - The USB interrupt is allocated at LEVEL3, not LOWMED, for its 1 ms
#     deadline.
#
# managed_components/ is generated and git-ignored, so these live here as a
# patch rather than an edit a re-fetch would silently discard. Drop once
# upstream carries them.
project(ds5-bridge-esp32s31)

set(TINYUSB_SRC "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__tinyusb")
set(DWC2_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/tinyusb-dwc2-esp32s31.patch")
# MUST come after project(): the component manager materialises
# managed_components/ as part of project(), so a guard placed before it tests a
# directory that does not exist yet on a fresh tree, silently skips the patch and
# ships a build with a broken isochronous OUT path. Compiling cleanly hides it.
if(EXISTS "${TINYUSB_SRC}/src/portable/synopsys/dwc2/dcd_dwc2.c")
    # --dry-run first so an already-patched tree is a no-op rather than an error;
    # the component manager re-materialises this directory without warning.
    execute_process(
        COMMAND patch -p1 --forward --dry-run --input "${DWC2_PATCH}"
        WORKING_DIRECTORY "${TINYUSB_SRC}"
        RESULT_VARIABLE dwc2_patch_needed
        OUTPUT_QUIET ERROR_QUIET)
    if(dwc2_patch_needed EQUAL 0)
        message(STATUS "Applying TinyUSB dwc2 isochronous-OUT patch")
        execute_process(
            COMMAND patch -p1 --forward --input "${DWC2_PATCH}"
            WORKING_DIRECTORY "${TINYUSB_SRC}"
            RESULT_VARIABLE dwc2_patch_result)
        if(NOT dwc2_patch_result EQUAL 0)
            message(FATAL_ERROR "TinyUSB dwc2 patch failed to apply")
        endif()
    endif()
else()
    message(FATAL_ERROR
        "TinyUSB sources not found at ${TINYUSB_SRC}; the dwc2 patch cannot be "
        "applied. Failing loudly rather than shipping an unpatched build.")
endif()
