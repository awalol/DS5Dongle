# Post-link check that the .time_critical RAM relocation actually took effect.
#
# The relocation scheme renames sections on compiled objects/archives, and
# objcopy silently no-ops a rename whose section name does not match. Two
# realistic failure modes produce a successfully-linked firmware whose hot
# paths quietly run from flash (XIP) instead of RAM:
#   - a stale in-place libopus.a relocation (build dir reconfigured across
#     strategies without a clean build), and
#   - a non-pinned GCC whose compiler-decorated section names
#     (.constprop.0/.isra.0/.part.0) differ, so those renames no-op.
# Both were invisible before this check: audio glitches with zero build-time
# indication. Here we assert that a handful of sentinel hot-path symbols
# resolved into SRAM (0x200xxxxx) in the final ELF.
#
# Arguments: -DNM=<nm path> -DELF=<elf path> -DSYMBOLS=<;-separated symbol list>

if (NOT NM)
    message(WARNING "verify_ram_relocation: no nm tool available; skipping the RAM-placement check")
    return()
endif ()

execute_process(
    COMMAND "${NM}" "${ELF}"
    OUTPUT_VARIABLE _nm_out
    ERROR_VARIABLE _nm_err
    RESULT_VARIABLE _rc
)
if (NOT _rc EQUAL 0)
    message(FATAL_ERROR "verify_ram_relocation: nm failed on ${ELF}: ${_nm_err}")
endif ()

set(_bad "")
foreach (_sym IN LISTS SYMBOLS)
    # Anchor the symbol name to end-of-line so e.g. tu_fifo_read does not match
    # the tu_fifo_read_n line.
    string(REGEX MATCH "([0-9a-fA-F]+)[ \t]+[TtWw][ \t]+${_sym}(\r?\n|$)" _m "${_nm_out}")
    if (NOT _m)
        list(APPEND _bad "${_sym}: not found in the ELF")
        continue()
    endif ()
    # Save the capture before the next MATCHES resets CMAKE_MATCH_1. Any SRAM
    # address starts with 0x200 (flash/XIP is 0x10xxxxxx); CMake's regex engine
    # has no {n} repetition, so a prefix check is what we can express anyway.
    set(_addr "${CMAKE_MATCH_1}")
    if (NOT _addr MATCHES "^200")
        list(APPEND _bad "${_sym}: at 0x${_addr}, expected SRAM (0x200xxxxx)")
    endif ()
endforeach ()

if (_bad)
    string(REPLACE ";" "\n  " _bad_lines "${_bad}")
    message(FATAL_ERROR
        "RAM relocation did NOT take effect:\n  ${_bad_lines}\n"
        "Likely causes: a stale in-place libopus.a relocation (delete the build "
        "directory and rebuild), or a toolchain other than the pinned ARM GCC "
        "14.2.Rel1 whose compiler-decorated section names made the objcopy "
        "renames no-op.")
endif ()

message(STATUS "RAM relocation verified: hot-path symbols are in SRAM")
