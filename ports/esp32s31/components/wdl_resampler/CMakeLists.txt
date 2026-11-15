# Cockos WDL resampler as an ESP-IDF component.
#
# One translation unit: the project's src/wdl_resample.cpp, which includes WDL's
# resample.cpp after a float specialization of the stereo sinc tap loop. The
# speaker path selects that sinc mode, and the ESP32-S31's FPU is single
# precision, so WDL's own double accumulators would run in software here.
# -fsigned-char matches the Pico build; WDL's resampler assumes signed char,
# and RISC-V (like ARM) defaults to unsigned.

set(WDL_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../lib/WDL/WDL")
set(DS5_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../src")

idf_component_register(
    SRCS "${DS5_SRC_DIR}/wdl_resample.cpp"
    INCLUDE_DIRS "${WDL_DIR}"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -fsigned-char)
target_compile_definitions(${COMPONENT_LIB} PUBLIC WDL_RESAMPLE_TYPE=float)
