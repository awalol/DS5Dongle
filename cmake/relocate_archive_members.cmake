# Relocate the .text of SELECTED static-archive members into RAM by renaming it
# to .time_critical.opus_text, which the Pico SDK linker script places in RAM
# (LMA flash, copied to VMA RAM at boot). Used to move ONLY the Opus CELT codec
# hot path into RAM (not the whole 241 KB libopus.a), so per-frame execute-in-place
# stalls disappear from core1's audio loop while the heap stays large enough that
# BT pairing never runs out of memory.
#
# Why per-member archive surgery (not relocate_to_ram.cmake): libopus.a is built
# by add_subdirectory(lib/opus) as its own target, so its objects never land under
# ds5-bridge.dir where relocate_to_ram() looks. And Opus is compiled WITHOUT
# -ffunction-sections, so each TU has a single ".text" section -- renaming that
# one section moves the whole translation unit.
#
# IMPORTANT: use the RAW binutils ar (arm-none-eabi-ar), NOT gcc-ar -- gcc-ar
# loads the LTO plugin and would choke / behave differently here.
#
# MEMBERS are given as EXTENSIONLESS base names (e.g. "celt_encoder.c"). The actual
# object extension differs by platform/generator -- ".c.o" on Linux/GNU, ".c.obj"
# on Windows/Ninja -- so the real archive member name is resolved at run time from
# "ar t" output rather than hardcoded. (Hardcoding ".c.obj" broke the Linux CI.)
#
# Args (via -D):
#   AR       path to arm-none-eabi-ar
#   OBJCOPY  path to arm-none-eabi-objcopy
#   ARCHIVE  absolute path to libopus.a ($<TARGET_FILE:opus>)
#   MEMBERS  |-separated list of extensionless member base names (e.g. celt_encoder.c)
#   WORKDIR  scratch dir for extraction
#
# Idempotent: objcopy silently no-ops a member whose .text was already renamed, so
# re-running across incremental relinks is safe. A CLEAN build is still required
# when the MEMBER LIST changes (the archive is mutated in place).

foreach(_v AR OBJCOPY ARCHIVE MEMBERS WORKDIR)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "relocate_archive_members: missing required -D${_v}")
    endif()
endforeach()

if(NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "relocate_archive_members: archive not found: ${ARCHIVE}")
endif()

# List the archive's actual members so we can resolve each base name to its real
# object file regardless of the .o/.obj extension.
execute_process(
    COMMAND "${AR}" t "${ARCHIVE}"
    OUTPUT_VARIABLE _ar_list
    RESULT_VARIABLE _rt)
if(NOT _rt EQUAL 0)
    message(FATAL_ERROR "relocate_archive_members: 'ar t' failed (rc=${_rt}) on ${ARCHIVE}")
endif()
string(REPLACE "\n" ";" _ar_members "${_ar_list}")
list(TRANSFORM _ar_members STRIP)

string(REPLACE "|" ";" _members "${MEMBERS}")
file(MAKE_DIRECTORY "${WORKDIR}")

foreach(_base ${_members})
    # Resolve the real member name: prefer <base>.o, then <base>.obj.
    # (list(FIND) instead of IN_LIST -- IN_LIST needs CMP0057 NEW, which is not
    #  set in a standalone -P script context.)
    set(_member "")
    foreach(_cand "${_base}.o" "${_base}.obj")
        list(FIND _ar_members "${_cand}" _idx)
        if(NOT _idx EQUAL -1)
            set(_member "${_cand}")
            break()
        endif()
    endforeach()
    if(NOT _member)
        message(FATAL_ERROR "relocate_archive_members: no member '${_base}.o' or '${_base}.obj' in ${ARCHIVE}")
    endif()

    # 1) extract the member into WORKDIR (ar writes to the working dir, by basename)
    execute_process(
        COMMAND "${AR}" x "${ARCHIVE}" "${_member}"
        WORKING_DIRECTORY "${WORKDIR}"
        RESULT_VARIABLE _rx)
    if(NOT _rx EQUAL 0)
        message(FATAL_ERROR "relocate_archive_members: 'ar x ${_member}' failed (rc=${_rx}) on ${ARCHIVE}")
    endif()
    if(NOT EXISTS "${WORKDIR}/${_member}")
        message(FATAL_ERROR "relocate_archive_members: member '${_member}' not extracted from ${ARCHIVE}")
    endif()
    # 2) move the whole TU's code into RAM (rename its single .text section)
    execute_process(
        COMMAND "${OBJCOPY}" --rename-section .text=.time_critical.opus_text "${WORKDIR}/${_member}"
        RESULT_VARIABLE _ro)
    if(NOT _ro EQUAL 0)
        message(FATAL_ERROR "relocate_archive_members: objcopy failed (rc=${_ro}) on ${_member}")
    endif()
    # 3) write the modified member back into the archive
    execute_process(
        COMMAND "${AR}" r "${ARCHIVE}" "${_member}"
        WORKING_DIRECTORY "${WORKDIR}"
        RESULT_VARIABLE _rr)
    if(NOT _rr EQUAL 0)
        message(FATAL_ERROR "relocate_archive_members: 'ar r ${_member}' failed (rc=${_rr}) on ${ARCHIVE}")
    endif()
endforeach()

# 4) regenerate the archive symbol index so the linker resolves the rewritten members
execute_process(COMMAND "${AR}" s "${ARCHIVE}" RESULT_VARIABLE _rs)
if(NOT _rs EQUAL 0)
    message(FATAL_ERROR "relocate_archive_members: 'ar s' (reindex) failed (rc=${_rs}) on ${ARCHIVE}")
endif()

message(STATUS "relocate_archive_members: relocated .text of {${MEMBERS}} into RAM (.time_critical.opus_text)")
