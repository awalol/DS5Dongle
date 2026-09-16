# libopus as an ESP-IDF component.
#
# Reuses the lib/opus submodule that the Pico build compiles via
# add_subdirectory, rather than vendoring a second copy. The upstream opus
# CMakeLists builds a target named `opus`; this component wraps it.
#
# Float mode, matching the Pico build (OPUS_FIXED_POINT is commented out in the
# top-level CMakeLists.txt). ESP32-S31 is dual-core RISC-V at 240MHz with an
# FPU, so float remains the right choice; opus falls back to its generic C paths
# with no RISC-V intrinsics.

set(OPUS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../lib/opus")

idf_component_register(INCLUDE_DIRS "${OPUS_DIR}/include")

# Keep the upstream build to just the library.
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

add_subdirectory("${OPUS_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/opus_build" EXCLUDE_FROM_ALL)

target_link_libraries(${COMPONENT_LIB} INTERFACE opus)
