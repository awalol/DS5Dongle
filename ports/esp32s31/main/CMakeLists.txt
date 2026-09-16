# Shared firmware sources plus the ESP32-S31 backend. The file list mirrors the
# ds5-bridge target in the top-level CMakeLists.txt; keep the two in step.

set(SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src")

# Reported to the config UI over HID report 0xf8. Overridable the same way as
# the RP2350 build's -DVERSION, so release builds of the two targets can agree.
set(VERSION "dev" CACHE STRING "Program version string")

idf_component_register(
    LDFRAGMENTS "ds5_hot_iram.lf"
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/app_main.cpp"
        "${SRC_DIR}/usb_descriptors.cpp"
        "${SRC_DIR}/main.cpp"
        "${SRC_DIR}/usb.cpp"
        "${SRC_DIR}/audio.cpp"
        "${SRC_DIR}/bt.cpp"
        "${SRC_DIR}/button_functions.cpp"
        "${SRC_DIR}/dse.cpp"
        "${SRC_DIR}/config.cpp"
        "${SRC_DIR}/status_gpio.cpp"
        "${SRC_DIR}/cmd.cpp"
        "${SRC_DIR}/ram_mem.c"
        "${SRC_DIR}/battery_led.cpp"
        "${SRC_DIR}/wake.cpp"
        "${SRC_DIR}/ps_shortcut.cpp"
        "${SRC_DIR}/port/esp32s31/port_esp32s31.cpp"
        "${SRC_DIR}/port/esp32s31/bt_transport_esp32s31.cpp"
    INCLUDE_DIRS
        "${SRC_DIR}"
        "${SRC_DIR}/port/esp32s31"
    REQUIRES
        driver
        esp_timer
        esp_partition
        esp_system  # also provides esp_task_wdt.h
        esp_hw_support  # esp_private/usb_phy.h
        bt
        espressif__led_strip
        espressif__tinyusb
        btstack
        libopus
        wdl_resampler
)

# ram_mem.c overrides memcpy/memset/memmove to keep them out of flash. On
# ESP32-S31 the equivalent win comes from IRAM placement via PORT_FAST_FUNC.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ENABLE_CLASSIC=1
    # Uncomment to dump every HCI packet from ACL connect onwards (the hook is
    # in bt.cpp, guarded on this symbol). Invaluable for connection-setup bugs,
    # but do NOT leave it on once HID reports start flowing: each report becomes
    # a blocking hexdump on a 115200 console, which stalls the main loop long
    # enough to trip the task watchdog.
    # DS5_HCI_DUMP=1
    # Uncomment for per-stage speaker-path counters (usb -> audio_fifo -> opus ->
    # Bluetooth), one line/sec while audio is moving. The hook is in audio.cpp.
    # This is what localised the silent speaker to the USB stage rather than the
    # codec or the link, so it is worth keeping around.
    # DS5_AUDIO_STATS=1
    # Traces feature-report requests and replies, and when the lightbar/state
    # output report goes out. A few lines per connect. Here to establish whether
    # the DualSense has answered 0x05 (calibration) before we try to drive its
    # lights -- it ignores full output reports over Bluetooth until it has.
    # DS5_FEATURE_TRACE=1
    ENABLE_BATT_LED=1
    ENABLE_SERIAL=0
    ENABLE_DEBUG=0
    ENABLE_VERBOSE=0
    ENABLE_WAKE_HID
    DISABLE_SPEAKER_PROC=0
    WDL_RESAMPLE_TYPE=float
    DS5_FIRMWARE_VERSION="${VERSION}"
    # CFG_TUSB_MCU / CFG_TUSB_OS are set globally in ../CMakeLists.txt so the
    # application and the TinyUSB library cannot disagree about them.
)

set_source_files_properties("${SRC_DIR}/ram_mem.c" PROPERTIES
    COMPILE_FLAGS "-fno-builtin")

# IDF builds with a stricter warning set than the Pico toolchain, and these two
# fire inside TinyUSB's headers and the existing descriptor tables rather than
# in new code. Downgraded so the port does not churn shared sources.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=missing-field-initializers
    -Wno-error=volatile
    -Wno-error=comment
    # WDL's wdltypes.h static-asserts that char is signed.
    -fsigned-char
)

# TinyUSB calls back into this component (tud_descriptor_*_cb,
# tud_hid_descriptor_report_cb), but nothing in the dependency graph points that
# way, so the linker discards those objects before TinyUSB's archive is scanned.
idf_component_set_property(main WHOLE_ARCHIVE ON)
