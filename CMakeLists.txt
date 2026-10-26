cmake_minimum_required(VERSION 3.21)
project(TLOTR_TTA_Recomp LANGUAGES C CXX)

# The Lord of the Rings: The Third Age (NTSC-U, SLUS-21027) static recompilation on top of PS2Recomp.
#
# Build the tools first (scripts/build_tools.sh), generate code (scripts/recomp.sh),
# then configure this project to get the `ps2EntryRunner` executable with the
# recompiled game linked in.

set(TTA_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH
    "Directory containing the output of ps2_recomp for the game ELF")

# Only the runtime is needed here; the recompiler/analyzer are built separately.
set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)

add_subdirectory(PS2Recomp)

file(GLOB TTA_GENERATED_SOURCES CONFIGURE_DEPENDS "${TTA_GENERATED_DIR}/*.cpp")
if(NOT TTA_GENERATED_SOURCES)
    message(FATAL_ERROR
        "No generated sources in ${TTA_GENERATED_DIR}. Run scripts/recomp.sh first.")
endif()

# The runtime ships a placeholder register_functions.cpp that defines the same
# function-table globals as the generated one; drop it from the runner.
get_target_property(_runner_sources ps2EntryRunner SOURCES)
list(FILTER _runner_sources EXCLUDE REGEX "src/runner/register_functions\.cpp$")
set_property(TARGET ps2EntryRunner PROPERTY SOURCES ${_runner_sources})

file(GLOB TTA_RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/*.cpp")
# Out of the unity batches: an edit to the game layer then rebuilds one small file, not a batch of generated code.
set_source_files_properties(${TTA_RUNTIME_SOURCES} TARGET_DIRECTORY ps2EntryRunner PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

# VU1 microcode compiled ahead of time by ps2_vu_aot (scripts/recomp.sh): an object library, built at -O3 outside
# the unity batches. Each file takes GCC several GB: at most 4 compile at once.
file(GLOB TTA_VU1_AOT_SOURCES CONFIGURE_DEPENDS "${TTA_GENERATED_DIR}/vu1/*.cpp")
if(TTA_VU1_AOT_SOURCES)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS tta_vu1_aot=4)
    add_library(tta_vu1_aot OBJECT ${TTA_VU1_AOT_SOURCES})
    target_link_libraries(tta_vu1_aot PRIVATE ps2_runtime)
    set_target_properties(tta_vu1_aot PROPERTIES UNITY_BUILD OFF JOB_POOL_COMPILE tta_vu1_aot)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(tta_vu1_aot PRIVATE -O3 -g0)
    endif()
    target_sources(ps2EntryRunner PRIVATE $<TARGET_OBJECTS:tta_vu1_aot>)
endif()

# register_functions.cpp fills the function table in one constructor of ~214,000 stores: GCC takes 35 min over it at
# -O2 -g, the tail of every full build. It runs once at startup, so build it alone and unoptimized.
set_source_files_properties("${TTA_GENERATED_DIR}/register_functions.cpp" TARGET_DIRECTORY ps2EntryRunner PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties("${TTA_GENERATED_DIR}/register_functions.cpp" TARGET_DIRECTORY ps2EntryRunner PROPERTIES COMPILE_OPTIONS "-O0;-g0")
endif()

target_sources(ps2EntryRunner PRIVATE ${TTA_GENERATED_SOURCES} ${TTA_RUNTIME_SOURCES})
target_include_directories(ps2EntryRunner PRIVATE "${TTA_GENERATED_DIR}")

# Line tables only for the runner's own sources (the generated code): full -g makes 2.8 GB of DWARF, which slows every
# batch, the link and the split. Backtraces through generated code keep file:line; the runtime library keeps full -g.
# (A per-source option would take the files out of their unity batches, hence the target option, after the config's -g.)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ps2EntryRunner PRIVATE $<$<CONFIG:RelWithDebInfo,Debug>:-g1>)
endif()

# Windows loads no image over 2 GB, and with debug info the runner is ~3 GB (2.8 GB of DWARF for the generated
# code, 106 MB of .text). Move the debug info to ps2EntryRunner.debug next to it; gdb finds it through the debug link.
# ps2EntryRunner is created in the submodule, where POST_BUILD cannot reach it: `tta_runner` (what
# scripts/build_runtime.sh builds) splits once per relink, tracked by a stamp file.
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND CMAKE_OBJCOPY AND CMAKE_STRIP)
    set(_tta_split_stamp "${CMAKE_BINARY_DIR}/ps2EntryRunner.split.stamp")
    add_custom_command(OUTPUT "${_tta_split_stamp}"
        COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug "$<TARGET_FILE:ps2EntryRunner>" "$<TARGET_FILE_DIR:ps2EntryRunner>/ps2EntryRunner.debug"
        COMMAND "${CMAKE_STRIP}" --strip-debug "$<TARGET_FILE:ps2EntryRunner>"
        COMMAND "${CMAKE_OBJCOPY}" "--add-gnu-debuglink=$<TARGET_FILE_DIR:ps2EntryRunner>/ps2EntryRunner.debug" "$<TARGET_FILE:ps2EntryRunner>"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_tta_split_stamp}"
        DEPENDS ps2EntryRunner
        COMMENT "Moving the runner's debug info to ps2EntryRunner.debug (Windows image size limit)"
        VERBATIM)
    add_custom_target(tta_runner ALL DEPENDS "${_tta_split_stamp}")
else()
    add_custom_target(tta_runner ALL DEPENDS ps2EntryRunner)
endif()

list(LENGTH TTA_GENERATED_SOURCES _tta_count)
message(STATUS "TLOTR_TTA_Recomp: linking ${_tta_count} generated source files from ${TTA_GENERATED_DIR}")
