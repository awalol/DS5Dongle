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

# Upstream's single-precision approximations for celt_exp2/celt_log2, instead
# of the double exp()/log() the float build otherwise calls. Needs IEEE-754
# floats, which the S31's FPU provides.
set(OPUS_FLOAT_APPROX ON CACHE BOOL "" FORCE)

add_subdirectory("${OPUS_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/opus_build" EXCLUDE_FROM_ALL)

# Keep CELT's arithmetic in single precision. The S31's FPU has no double
# support, and the float build promotes CELT's math to double in many places
# (see opus_single_precision.h), which measured 2.7 ms per 10 ms mono decode and
# 4.7 ms per stereo encode on core 1 -- more than core 1 can give once both
# audio streams run. Applied to the CELT sources and the CELT-mode entry points
# only; SILK uses double on purpose and is left as upstream builds it.
get_target_property(_opus_srcs opus SOURCES)
set(_opus_single_srcs "")
foreach(_src ${_opus_srcs})
    if(_src MATCHES "(^|/)celt/[^/]+\\.c$" OR _src MATCHES "(^|/)src/opus(_encoder|_decoder)?\\.c$")
        if(NOT IS_ABSOLUTE "${_src}")
            set(_src "${OPUS_DIR}/${_src}")
        endif()
        list(APPEND _opus_single_srcs "${_src}")
    endif()
endforeach()
set_source_files_properties(${_opus_single_srcs} TARGET_DIRECTORY opus PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/opus_single_precision.h;-fsingle-precision-constant")

target_link_libraries(${COMPONENT_LIB} INTERFACE opus)
