# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if (WIN32)
    set(USERHOME $ENV{USERPROFILE})
else ()
    set(USERHOME $ENV{HOME})
endif ()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif ()
# ====================================================================================

option(PICO_W_BUILD "Build for Rasberry Pi Pico W, audio is missing" OFF)
option(WAVESHARE_RP2350B_PLUS_W_BUILD "Build for Waveshare RP2350B-Plus-W (USB-C, 16MB flash, RM2 wireless)" OFF)

if (PICO_W_BUILD AND WAVESHARE_RP2350B_PLUS_W_BUILD)
        message(FATAL_ERROR "PICO_W_BUILD and WAVESHARE_RP2350B_PLUS_W_BUILD are mutually exclusive")
endif ()

if (PICO_W_BUILD)
        set(PICO_BOARD pico_w CACHE STRING "Board type")
        set(PICO_RP2350 OFF CACHE BOOL "Not using RP2350")
elseif (WAVESHARE_RP2350B_PLUS_W_BUILD)
        set(PICO_BOARD waveshare_rp2350b_plus_w CACHE STRING "Board type")
        # Waveshare board header is not in upstream pico-sdk; ship it here and
        # tell the SDK to search this directory when resolving PICO_BOARD.
        list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/boards/headers")
else ()
        set(PICO_BOARD pico2_w CACHE STRING "Board type")
endif ()

# Diagnostic: confirm PICO_BOARD resolution actually applied. Build log will
# show which board path was taken. Remove these once verified on hardware.
message(STATUS "DS5Dongle build target -- PICO_BOARD=${PICO_BOARD}")
message(STATUS "DS5Dongle build target -- PICO_W_BUILD=${PICO_W_BUILD}")
message(STATUS "DS5Dongle build target -- WAVESHARE_RP2350B_PLUS_W_BUILD=${WAVESHARE_RP2350B_PLUS_W_BUILD}")
message(STATUS "DS5Dongle build target -- PICO_BOARD_HEADER_DIRS=${PICO_BOARD_HEADER_DIRS}")
#set(PICO_CYW43_SUPPORTED 1)
#set(PICO_CYW43_ARCH_THREADSAFE_BACKGROUND 1)

option(ENABLE_SERIAL "Enable USB CDC serial printf output" OFF)
if (ENABLE_SERIAL)
        set(ENABLE_SERIAL_VALUE 1)
else ()
        set(ENABLE_SERIAL_VALUE 0)
endif ()
option(ENABLE_VERBOSE "Enable Log Verbose" OFF)
if(ENABLE_VERBOSE)
    set(ENABLE_VERBOSE_VALUE 1)
else()
    set(ENABLE_VERBOSE_VALUE 0)
endif ()
option(ENABLE_DEBUG "Enable debug probes" OFF)
if(ENABLE_DEBUG)
    set(ENABLE_DEBUG_VALUE 1)
else()
    set(ENABLE_DEBUG_VALUE 0)
endif ()
option(ENABLE_BATT_LED "Blink Pico LED when DualSense battery is low (<=10%)" ON)
if (ENABLE_BATT_LED)
        set(ENABLE_BATT_LED_VALUE 1)
else ()
        set(ENABLE_BATT_LED_VALUE 0)
endif ()
option(DISABLE_SPEAKER_PROC "Disable Speaker in controller" OFF)
if (DISABLE_SPEAKER_PROC)
        set(DISABLE_SPEAKER_PROC_VALUE 1)
else ()
        set(DISABLE_SPEAKER_PROC_VALUE 0)
endif ()
option(PICO_W_BUILD "Build for Rasberry Pi Pico W, audio is missing" OFF)
if (PICO_W_BUILD)
        set(CYW43_PIO_CLOCK_DIV_VALUE 3)
        set(SYS_CLOCK_KHZ_VALUE 200000)
        set(DISABLE_SPEAKER_PROC_VALUE 1)
else ()
        if(DISABLE_SPEAKER_PROC_VALUE)
            set(CYW43_PIO_CLOCK_DIV_VALUE 2)
            set(SYS_CLOCK_KHZ_VALUE 150000)
        else ()
            set(CYW43_PIO_CLOCK_DIV_VALUE 2)
            set(SYS_CLOCK_KHZ_VALUE 150000)
        endif ()
endif ()



# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(ds5-bridge C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(ds5-bridge
        src/usb_descriptors.cpp
        src/main.cpp
        src/usb.cpp
        src/audio.cpp
        src/bt.cpp
        src/button_functions.cpp
        src/dse.cpp
        src/config.cpp
        src/cmd.cpp
        src/ram_mem.c
)

if (ENABLE_DEBUG)
        target_sources(ds5-bridge PRIVATE src/debug.cpp)
endif ()

if (ENABLE_BATT_LED)
        target_sources(ds5-bridge PRIVATE src/battery_led.cpp)
endif ()

# Cockos WDLResampler
add_library(wdl_resampler STATIC
        lib/WDL/WDL/resample.cpp
)
target_include_directories(wdl_resampler PUBLIC
        lib/WDL/WDL
)

# Opus
#set(OPUS_FIXED_POINT ON)
add_subdirectory(lib/opus)

# Phase A.3: Move libopus to RAM via .time_critical sections.
# libopus is built by add_subdirectory(lib/opus); the target `opus` is defined in
# the subdirectory's CMakeLists.txt, so we can't use POST_BUILD here. Instead we
# use a stamp-file custom command + custom target that runs after opus is built
# and before ds5-bridge links. Renames .text and .rodata of libopus.a to
# .time_critical.* which the Pico SDK linker places in RAM (loaded from flash
# at boot, executed from RAM at runtime). ~220 KB total relocated, ~5-6 ms boot
# time impact for the flash-to-RAM copy.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/opus_ram_relocate.stamp
    COMMAND ${CMAKE_OBJCOPY} --rename-section .text=.time_critical.opus_text --rename-section .rodata=.time_critical.opus_rodata --rename-section .rodata.str1.4=.time_critical.opus_strings $<TARGET_FILE:opus>
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/opus_ram_relocate.stamp
    DEPENDS $<TARGET_FILE:opus>
    COMMENT "Phase A.3: relocating libopus to .time_critical.* sections"
    VERBATIM
)
add_custom_target(opus_ram_relocate ALL DEPENDS ${CMAKE_BINARY_DIR}/opus_ram_relocate.stamp)
add_dependencies(ds5-bridge opus_ram_relocate)

target_compile_options(wdl_resampler PRIVATE -fsigned-char)

# Phase A.2: Move WDL_Resampler hot-path methods to RAM via .time_critical sections.
# -ffunction-sections gives each function its own .text.<mangled_symbol> section.
# We then rename just the two hot-path methods (called every audio frame) to
# .time_critical.WDL_<name>, which the Pico SDK linker script places in RAM
# (loaded from flash at boot, executed from RAM at runtime). Other WDL methods
# stay in flash. No submodule modification required.
target_compile_options(wdl_resampler PRIVATE -ffunction-sections)
add_custom_command(TARGET wdl_resampler POST_BUILD
    COMMAND ${CMAKE_OBJCOPY}
        --rename-section .text._ZN13WDL_Resampler11ResampleOutEPfiii=.time_critical.WDL_ResampleOut
        --rename-section .text._ZN13WDL_Resampler15ResamplePrepareEiiPPf=.time_critical.WDL_ResamplePrepare
        $<TARGET_FILE:wdl_resampler>
    COMMENT "Phase A.2: relocating WDL_Resampler hot-path methods to .time_critical.*"
)

# Phases B.2-B.4: Move per-packet BT/USB hot-path functions that live in pristine
# SDK/submodule sources (btstack, cyw43-driver, tinyusb, pico_util) into RAM. They
# compile directly into ds5-bridge and can't take __not_in_flash_func at the source.
# Since -ffunction-sections is on, each function has its own .text.<name> section;
# relocate_to_ram() renames the hot ones to .time_critical.<name> on the compiled
# objects in a PRE_LINK step (after compile, before link), and the Pico SDK linker
# places .time_critical.* in RAM (LMA flash, VMA RAM). The helper locates each
# object by its stable SDK-relative path SUFFIX, because the object's path under the
# target's .dir differs by environment (in-tree pico-sdk in CI vs out-of-tree
# locally). RENAMES are |-separated old=new pairs; objcopy skips an absent section,
# so this is idempotent across incremental relinks.
function(relocate_to_ram _suffix _renames _comment)
    add_custom_command(TARGET ds5-bridge PRE_LINK
        COMMAND ${CMAKE_COMMAND}
            "-DOBJROOT=${CMAKE_BINARY_DIR}/CMakeFiles/ds5-bridge.dir"
            "-DOBJCOPY=${CMAKE_OBJCOPY}"
            "-DSUFFIX=${_suffix}"
            "-DRENAMES=${_renames}"
            -P "${CMAKE_SOURCE_DIR}/cmake/relocate_to_ram.cmake"
        COMMENT "${_comment}"
        VERBATIM)
endfunction()

# --- Phase B.2: Tier-1 -- per-packet BT receive + USB FIFO data path ---
relocate_to_ram(lib/btstack/src/hci.c.o
    ".text.packet_handler=.time_critical.bt_packet_handler"
    "Phase B.2: hci.c packet_handler -> RAM")
relocate_to_ram(lib/btstack/src/l2cap.c.o
    ".text.l2cap_acl_handler=.time_critical.l2cap_acl_handler"
    "Phase B.2: l2cap.c l2cap_acl_handler -> RAM")
relocate_to_ram(lib/tinyusb/src/common/tusb_fifo.c.o
    ".text.tu_fifo_read=.time_critical.tu_fifo_read|.text.tu_fifo_read_n_access_mode=.time_critical.tu_fifo_read_n_access_mode|.text.tu_fifo_write=.time_critical.tu_fifo_write|.text.tu_fifo_write_n_access_mode=.time_critical.tu_fifo_write_n_access_mode"
    "Phase B.2: tusb_fifo.c data path -> RAM")
relocate_to_ram(src/rp2_common/pico_cyw43_driver/cyw43_bus_pio_spi.c.o
    ".text.cyw43_spi_transfer=.time_critical.cyw43_spi_transfer|.text.cyw43_read_bytes=.time_critical.cyw43_read_bytes|.text.cyw43_write_bytes=.time_critical.cyw43_write_bytes|.text.cyw43_read_reg_u32=.time_critical.cyw43_read_reg_u32|.text.cyw43_read_reg_u16=.time_critical.cyw43_read_reg_u16|.text.cyw43_read_reg_u8=.time_critical.cyw43_read_reg_u8|.text.cyw43_write_reg_u32=.time_critical.cyw43_write_reg_u32|.text.cyw43_write_reg_u16=.time_critical.cyw43_write_reg_u16|.text.cyw43_write_reg_u8=.time_critical.cyw43_write_reg_u8|.text.read_reg_u32_swap=.time_critical.read_reg_u32_swap|.text.write_reg_u32_swap=.time_critical.write_reg_u32_swap|.text.ns_delay.constprop.0=.time_critical.cyw43_ns_delay"
    "Phase B.2: cyw43 PIO-SPI transport -> RAM")
relocate_to_ram(lib/cyw43-driver/src/cyw43_ll.c.o
    ".text.cyw43_ll_sdpcm_poll_device=.time_critical.cyw43_ll_sdpcm_poll_device|.text.cyw43_ll_process_packets=.time_critical.cyw43_ll_process_packets|.text.cyw43_sdpcm_send_common=.time_critical.cyw43_sdpcm_send_common|.text.cyw43_do_ioctl.part.0=.time_critical.cyw43_do_ioctl|.text.cyw43_ll_bt_has_work=.time_critical.cyw43_ll_bt_has_work|.text.cyw43_ll_parse_async_event=.time_critical.cyw43_ll_parse_async_event"
    "Phase B.2: cyw43 SDPCM pump -> RAM")
relocate_to_ram(lib/cyw43-driver/src/cyw43_ctrl.c.o
    ".text.cyw43_poll_func=.time_critical.cyw43_poll_func|.text.cyw43_bluetooth_hci_read=.time_critical.cyw43_bluetooth_hci_read|.text.cyw43_bluetooth_hci_write=.time_critical.cyw43_bluetooth_hci_write"
    "Phase B.2: cyw43 poll + BT HCI bus -> RAM")

# --- Phase B.3: Tier-2 -- run-loop TX pumps + per-transfer submit paths ---
# state_update is intentionally NOT moved: it runs on the host->controller HID-OUT
# (rumble/LED) path, not the audio frame path, so it stays in flash for headroom.
relocate_to_ram(lib/btstack/src/hci.c.o
    ".text.hci_run=.time_critical.hci_run|.text.hci_send_acl_packet_fragments=.time_critical.hci_send_acl_packet_fragments|.text.hci_send_acl_packet_buffer=.time_critical.hci_send_acl_packet_buffer"
    "Phase B.3: hci.c TX pump + ACL out -> RAM")
relocate_to_ram(lib/btstack/src/l2cap.c.o
    ".text.l2cap_run=.time_critical.l2cap_run|.text.l2cap_send=.time_critical.l2cap_send|.text.l2cap_send_prepared=.time_critical.l2cap_send_prepared|.text.l2cap_notify_channel_can_send=.time_critical.l2cap_notify_channel_can_send|.text.l2cap_request_can_send_now_event=.time_critical.l2cap_request_can_send_now_event"
    "Phase B.3: l2cap.c TX pump + sends -> RAM")
relocate_to_ram(lib/btstack/src/btstack_linked_list.c.o
    ".text.btstack_linked_list_iterator_init=.time_critical.btstack_linked_list_iterator_init|.text.btstack_linked_list_iterator_has_next=.time_critical.btstack_linked_list_iterator_has_next|.text.btstack_linked_list_iterator_next=.time_critical.btstack_linked_list_iterator_next"
    "Phase B.3: btstack linked-list iterators -> RAM")
relocate_to_ram(lib/cyw43-driver/src/cyw43_ll.c.o
    ".text.cyw43_ll_write_backplane_mem=.time_critical.cyw43_ll_write_backplane_mem|.text.cyw43_ll_read_backplane_mem=.time_critical.cyw43_ll_read_backplane_mem|.text.cyw43_write_backplane=.time_critical.cyw43_write_backplane|.text.cyw43_read_backplane.constprop.0=.time_critical.cyw43_read_backplane"
    "Phase B.3: cyw43 backplane accessors -> RAM")
relocate_to_ram(lib/tinyusb/src/device/usbd.c.o
    ".text.tud_task_ext=.time_critical.tud_task_ext|.text.usbd_edpt_xfer=.time_critical.usbd_edpt_xfer|.text.usbd_edpt_xfer_fifo=.time_critical.usbd_edpt_xfer_fifo"
    "Phase B.3: usbd.c task pump + xfer -> RAM")
relocate_to_ram(lib/tinyusb/src/portable/raspberrypi/rp2040/dcd_rp2040.c.o
    ".text.dcd_edpt_xfer=.time_critical.dcd_edpt_xfer|.text.dcd_edpt_iso_activate=.time_critical.dcd_edpt_iso_activate"
    "Phase B.3: dcd_rp2040.c submit + iso -> RAM")
relocate_to_ram(lib/tinyusb/src/class/audio/audio_device.c.o
    ".text.tud_audio_n_read=.time_critical.tud_audio_n_read|.text.tud_audio_n_write=.time_critical.tud_audio_n_write|.text.audiod_calc_tx_packet_sz.isra.0=.time_critical.audiod_calc_tx_packet_sz|.text.audiod_xfer_isr=.time_critical.audiod_xfer_isr"
    "Phase B.3: audio_device.c data path -> RAM")

# --- Phase B.4: input-report path tail (controller -> host) ---
# queue_try_* also back the audio inter-core FIFOs, so this helps both paths.
relocate_to_ram(lib/tinyusb/src/class/hid/hid_device.c.o
    ".text.tud_hid_n_report=.time_critical.tud_hid_n_report|.text.tud_hid_n_ready=.time_critical.tud_hid_n_ready|.text.hidd_xfer_cb=.time_critical.hidd_xfer_cb"
    "Phase B.4: hid_device.c report path -> RAM")
relocate_to_ram(src/common/pico_util/queue.c.o
    ".text.queue_try_add=.time_critical.queue_try_add|.text.queue_try_remove=.time_critical.queue_try_remove"
    "Phase B.4: pico_util queue add/remove -> RAM")
relocate_to_ram(src/rp2_common/pico_cyw43_arch/cyw43_arch.c.o
    ".text.cyw43_arch_poll=.time_critical.cyw43_arch_poll"
    "Phase B.4: cyw43_arch_poll -> RAM")
relocate_to_ram(lib/btstack/src/btstack_run_loop.c.o
    ".text.btstack_run_loop_base_poll_data_sources=.time_critical.btstack_run_loop_base_poll_data_sources|.text.btstack_run_loop_poll_data_sources_from_irq=.time_critical.btstack_run_loop_poll_data_sources_from_irq"
    "Phase B.4: btstack run-loop poll -> RAM")

# Phase B.5: RAM-resident mem ops. core1's audio loop calls memcpy/memset every
# frame (directly and inside libopus), and on RP2350 the Pico SDK uses newlib's
# memcpy/memset, which live in flash -- so core1 still executes flash code during
# steady-state audio. The pico/ROM mem-ops variant does not link on RP2350 (the
# bootrom no longer exports ROM_FUNC_MEMCPY/MEMSET), so src/ram_mem.c provides
# RAM-resident (.time_critical) overrides instead. Built with -fno-builtin and
# -fno-tree-loop-distribute-patterns so the compiler does not turn the copy loops
# back into calls to memcpy/memset (which would self-recurse). This completes the
# move of core1's audio loop into RAM (no per-frame XIP fetches left on core1).
# NOTE: re-validate audio at 150 MHz, since these replace newlib's mem ops.
set_source_files_properties(src/ram_mem.c PROPERTIES COMPILE_OPTIONS
        "-fno-builtin;-fno-tree-loop-distribute-patterns")

target_compile_options(ds5-bridge PRIVATE -fsigned-char)
target_compile_definitions(wdl_resampler PRIVATE
        WDL_RESAMPLE_TYPE=float
)
if(${SYS_CLOCK_KHZ_VALUE} GREATER 266000)
    set(PICO_FLASH_SPI_CLKDIV 4)
else()
    set(PICO_FLASH_SPI_CLKDIV 2)
endif()
target_compile_definitions(bs2_default PRIVATE PICO_FLASH_SPI_CLKDIV=${PICO_FLASH_SPI_CLKDIV})
target_compile_definitions(ds5-bridge PRIVATE PICO_EMBED_XIP_SETUP=1)
target_compile_definitions(ds5-bridge PRIVATE
        CYW43_LWIP=0
        ENABLE_SERIAL=${ENABLE_SERIAL_VALUE}
        # 0 (not 1): flash_safe_execute() must really park core1 while flash is
        # accessed - both config_save() and the BOOTSEL CSn float poll. core1_entry
        # registers via flash_safe_execute_core_init().
        PICO_FLASH_ASSUME_CORE1_SAFE=0
        WDL_RESAMPLE_TYPE=float
        ENABLE_DEBUG=${ENABLE_DEBUG_VALUE}
        CYW43_PIO_CLOCK_DIV_INT=${CYW43_PIO_CLOCK_DIV_VALUE}
        PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
        PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0
        PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=0
        ENABLE_VERBOSE=${ENABLE_VERBOSE_VALUE}
        ENABLE_BATT_LED=${ENABLE_BATT_LED_VALUE}
        SYS_CLOCK_KHZ=${SYS_CLOCK_KHZ_VALUE}
        DISABLE_SPEAKER_PROC=${DISABLE_SPEAKER_PROC_VALUE}
)

# Wake-on-PS + Xbox Game Bar are now in the base firmware, gated at runtime by the
# enable_wake / ps_shortcut_enabled config fields. ENABLE_WAKE_HID stays defined so
# the wake/keyboard descriptors and TinyUSB capacity compile in; the interface and
# behavior are only presented/run when enabled at runtime (see usb_descriptors.cpp,
# wake.cpp).
target_compile_definitions(ds5-bridge PRIVATE ENABLE_WAKE_HID)
target_sources(ds5-bridge PRIVATE
        src/wake.cpp
        src/ps_shortcut.cpp
)

option(WAKE_DEBUG "Print wake-FSM transitions to UART (developer use)" OFF)
if(WAKE_DEBUG)
    target_compile_definitions(ds5-bridge PRIVATE
            WAKE_DEBUG
    )
endif()

set(VERSION "dev" CACHE STRING "Program version string")

pico_set_program_name(ds5-bridge "ds5-bridge")
pico_set_program_version(ds5-bridge "${VERSION}")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(ds5-bridge 1)
pico_enable_stdio_usb(ds5-bridge ${ENABLE_SERIAL_VALUE})

# Add the standard include files to the build
#target_include_directories(ds5-bridge PRIVATE
#        ${CMAKE_CURRENT_LIST_DIR}
#)

target_include_directories(${PROJECT_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Add any user requested libraries
target_link_libraries(ds5-bridge
        pico_stdlib
        pico_multicore
        hardware_watchdog
        hardware_timer
        hardware_flash
        pico_btstack_classic
#        pico_cyw43_arch_threadsafe_background
        pico_cyw43_arch_poll
        pico_btstack_cyw43
        tinyusb_device
        tinyusb_board
        wdl_resampler
        opus
)

pico_add_extra_outputs(ds5-bridge)
