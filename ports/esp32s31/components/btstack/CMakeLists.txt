# BTstack as an ESP-IDF component, from the lib/btstack submodule. Adapted from
# BTstack's port/esp32 component with two deliberate differences:
#
#  1. src/classic is built unconditionally. Upstream gates it on
#     CONFIG_IDF_TARGET_ESP32; the S31 also has BR/EDR, and without this the
#     DualSense cannot connect at all.
#  2. btstack_config.h resolves to the firmware's own src/btstack_config.h, so
#     both targets share one stack configuration.
#
# BTstack is the host; ESP-IDF supplies only the controller, reached over VHCI.

set(BTSTACK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../lib/btstack")
set(FW_SRC "${CMAKE_CURRENT_LIST_DIR}/../../../../src")
set(ESP32_PORT "${BTSTACK_ROOT}/port/esp32/components/btstack")

file(GLOB btstack_srcs
    "${BTSTACK_ROOT}/src/*.c"
    "${BTSTACK_ROOT}/src/classic/*.c"
    "${BTSTACK_ROOT}/platform/embedded/btstack_run_loop_embedded.c"
    "${BTSTACK_ROOT}/platform/embedded/hci_dump_embedded_stdout.c"
    "${BTSTACK_ROOT}/3rd-party/micro-ecc/uECC.c"
    "${BTSTACK_ROOT}/3rd-party/md5/md5.c"
    "${BTSTACK_ROOT}/3rd-party/yxml/yxml.c"
)

# Only the NVS-backed TLV is taken from BTstack's ESP32 port, for link-key
# storage. btstack_port_esp32.c is deliberately NOT built: it hardwires the
# FreeRTOS run loop, whose execute() never returns and so cannot coexist with
# main.cpp's superloop. src/port/esp32s31/bt_transport_esp32s31.cpp supplies a
# VHCI transport driving the pollable embedded run loop instead.
# btstack_audio_esp32_*.c and es8388.c drive BTstack's own I2S audio path, which
# this firmware does not use -- it owns its audio pipeline in src/audio.cpp.
list(APPEND btstack_srcs
    "${ESP32_PORT}/btstack_tlv_esp32.c"
)

idf_component_register(
    SRCS ${btstack_srcs}
    INCLUDE_DIRS
        # FW_SRC must come FIRST. The bundled port/esp32 btstack_config.h only
        # defines ENABLE_CLASSIC under CONFIG_IDF_TARGET_ESP32 and is BLE-only
        # everywhere else -- on ESP32-S31 that silently drops BR/EDR and the
        # Classic sources then fail to find their memory pools. Every TU must
        # also agree on one config, or struct layouts diverge across the build.
        "${FW_SRC}"
        "${BTSTACK_ROOT}/src"
        "${BTSTACK_ROOT}/src/classic"
        "${BTSTACK_ROOT}/platform/embedded"
        "${BTSTACK_ROOT}/3rd-party/md5"
        "${BTSTACK_ROOT}/3rd-party/yxml"
        # btstack_lc3_google.c is swept up by the src/*.c glob. LC3 is LE Audio,
        # which this Classic-only firmware never calls, so the header is enough
        # -- the archive member is simply never pulled in at link time.
        "${BTSTACK_ROOT}/3rd-party/lc3-google/include"
        # Same story for SBC: btstack_sbc_bluedroid.c is globbed in, A2DP is
        # unused, so the headers satisfy the compiler and the linker drops it.
        "${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include"
        "${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include"
        "${ESP32_PORT}/include"
    PRIV_INCLUDE_DIRS
        "${BTSTACK_ROOT}/3rd-party/micro-ecc"
    PRIV_REQUIRES
        nvs_flash
        bt
        driver
        vfs
)

# BTstack is vendored third-party code built with the project's warning flags,
# which are stricter than upstream's. Downgrade rather than patch the submodule.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=maybe-uninitialized
    -Wno-maybe-uninitialized
)

