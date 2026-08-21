# Cockos WDL resampler as an ESP-IDF component.
#
# One translation unit. -fsigned-char matches the Pico build; WDL's resampler
# assumes signed char, and RISC-V (like ARM) defaults to unsigned.

set(WDL_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../lib/WDL/WDL")

idf_component_register(
    SRCS "${WDL_DIR}/resample.cpp"
    INCLUDE_DIRS "${WDL_DIR}"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -fsigned-char)
target_compile_definitions(${COMPONENT_LIB} PUBLIC WDL_RESAMPLE_TYPE=float)
