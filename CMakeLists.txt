cmake_minimum_required(VERSION 3.20)
project(cdsp VERSION 5.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Symlink compile_commands.json to source root for seamless LSP support (clangd / VS Code)
if(CMAKE_EXPORT_COMPILE_COMMANDS AND NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    file(CREATE_LINK
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
        RESULT _compile_commands_link_result
        SYMBOLIC
        COPY_ON_ERROR
    )
endif()

# -----------------------------------------------------------------------------
# Options & Platform Defaults
# -----------------------------------------------------------------------------
if(APPLE)
    set(DEFAULT_ENABLE_COREAUDIO ON)
    set(DEFAULT_ENABLE_ACCELERATE ON)
    set(DEFAULT_ENABLE_LIBDISPATCH ON)
    set(DEFAULT_ENABLE_ALSA OFF)
    set(DEFAULT_ENABLE_PIPEWIRE OFF)
    set(DEFAULT_ENABLE_WASAPI OFF)
    set(DEFAULT_ENABLE_ASIO OFF)
    set(DEFAULT_ENABLE_FFTW OFF)
    set(DEFAULT_ENABLE_DBUS OFF)
elseif(WIN32)
    set(DEFAULT_ENABLE_COREAUDIO OFF)
    set(DEFAULT_ENABLE_ACCELERATE OFF)
    set(DEFAULT_ENABLE_LIBDISPATCH OFF)
    set(DEFAULT_ENABLE_ALSA OFF)
    set(DEFAULT_ENABLE_PIPEWIRE OFF)
    set(DEFAULT_ENABLE_WASAPI ON)
    set(DEFAULT_ENABLE_ASIO ON)
    set(DEFAULT_ENABLE_FFTW ON)
    set(DEFAULT_ENABLE_DBUS OFF)
else()
    # Linux (Default)
    set(DEFAULT_ENABLE_COREAUDIO OFF)
    set(DEFAULT_ENABLE_ACCELERATE OFF)
    set(DEFAULT_ENABLE_LIBDISPATCH OFF)
    set(DEFAULT_ENABLE_ALSA ON)
    set(DEFAULT_ENABLE_PIPEWIRE ON)
    set(DEFAULT_ENABLE_WASAPI OFF)
    set(DEFAULT_ENABLE_ASIO OFF)
    set(DEFAULT_ENABLE_FFTW ON)
    set(DEFAULT_ENABLE_DBUS ON)
endif()

option(ENABLE_COREAUDIO   "Enable macOS CoreAudio backend"      ${DEFAULT_ENABLE_COREAUDIO})
option(ENABLE_ACCELERATE  "Enable Apple Accelerate (vDSP/vForce)" ${DEFAULT_ENABLE_ACCELERATE})
option(ENABLE_ALSA        "Enable Linux ALSA audio backend"     ${DEFAULT_ENABLE_ALSA})
option(ENABLE_PIPEWIRE    "Enable Linux PipeWire backend"       ${DEFAULT_ENABLE_PIPEWIRE})
option(ENABLE_WASAPI      "Enable Windows WASAPI audio backend" ${DEFAULT_ENABLE_WASAPI})
option(ENABLE_ASIO        "Enable Windows ASIO audio backend"   ${DEFAULT_ENABLE_ASIO})
option(ENABLE_FFTW        "Enable FFTW library for FFT"         ${DEFAULT_ENABLE_FFTW})
option(ENABLE_BLAS        "Enable OpenBLAS library"             OFF)
option(ENABLE_OPENMP      "Enable OpenMP parallelization"       OFF)
option(ENABLE_WEBSOCKET   "Enable WebSocket control server"     ON)
option(ENABLE_DBUS        "Enable Linux DBus device monitoring" ${DEFAULT_ENABLE_DBUS})
option(ENABLE_LIBDISPATCH "Enable Apple libdispatch (GCD)"      ${DEFAULT_ENABLE_LIBDISPATCH})

option(ENABLE_LTO         "Enable Link Time Optimization"       ON)
option(ENABLE_NATIVE_ARCH "Enable native CPU optimization"      ON)
option(BUILD_TESTING      "Build test runner and unit tests"    ON)
option(BUILD_BENCHMARKS   "Build benchmark targets"             ON)

set(ENABLE_SANITIZER "none" CACHE STRING "Sanitizer to enable (none, address, thread, undefined, memory, leak, intsan, cfi)")

# -----------------------------------------------------------------------------
# Global Compiler / Linker Configurations
# -----------------------------------------------------------------------------
if(ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(STATUS "IPO/LTO not supported: ${ipo_error}")
    endif()
endif()

# Default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Common compiler flags
set(CDSP_C_FLAGS "")
set(CDSP_C_DEFINITIONS "CDSP_BUILD_SHARED")

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    list(APPEND CDSP_C_FLAGS
        -Wall
        -Wextra
        -O3
        -ffp-contract=fast
        -fno-math-errno
        -funroll-loops
        -fvisibility=hidden
    )

    if(ENABLE_NATIVE_ARCH AND NOT CMAKE_CROSSCOMPILING)
        if(APPLE)
            list(APPEND CDSP_C_FLAGS -mcpu=native)
        else()
            list(APPEND CDSP_C_FLAGS -march=native)
        endif()
    endif()
endif()

# Sanitizers
if(NOT ENABLE_SANITIZER STREQUAL "none")
    if(ENABLE_SANITIZER STREQUAL "address")
        list(APPEND CDSP_C_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        link_libraries(-fsanitize=address,undefined)
    elseif(ENABLE_SANITIZER STREQUAL "thread")
        list(APPEND CDSP_C_FLAGS -fsanitize=thread -fno-omit-frame-pointer -g)
        link_libraries(-fsanitize=thread)
    elseif(ENABLE_SANITIZER STREQUAL "undefined")
        list(APPEND CDSP_C_FLAGS -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
        link_libraries(-fsanitize=undefined)
    elseif(ENABLE_SANITIZER STREQUAL "intsan")
        list(APPEND CDSP_C_FLAGS -fsanitize=signed-integer-overflow,shift,integer-divide-by-zero -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
        link_libraries(-fsanitize=signed-integer-overflow,shift,integer-divide-by-zero)
    elseif(ENABLE_SANITIZER STREQUAL "memory")
        if(APPLE)
            message(FATAL_ERROR "MemorySanitizer is not supported on macOS")
        endif()
        set(ENABLE_ALSA OFF CACHE BOOL "Disabled under MemorySanitizer" FORCE)
        set(ENABLE_PIPEWIRE OFF CACHE BOOL "Disabled under MemorySanitizer" FORCE)
        set(ENABLE_DBUS OFF CACHE BOOL "Disabled under MemorySanitizer" FORCE)
        list(APPEND CDSP_C_FLAGS -fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -g)
        link_libraries(-fsanitize=memory)
    elseif(ENABLE_SANITIZER STREQUAL "leak")
        if(APPLE)
            list(APPEND CDSP_C_FLAGS -fsanitize=address -fno-omit-frame-pointer -g)
            link_libraries(-fsanitize=address)
        else()
            list(APPEND CDSP_C_FLAGS -fsanitize=leak -fno-omit-frame-pointer -g)
            link_libraries(-fsanitize=leak)
        endif()
    elseif(ENABLE_SANITIZER STREQUAL "cfi")
        list(APPEND CDSP_C_FLAGS -flto -fvisibility=hidden -fsanitize=cfi -fno-omit-frame-pointer -g)
        link_libraries(-flto -fsanitize=cfi)
    else()
        message(FATAL_ERROR "Unknown sanitizer: ${ENABLE_SANITIZER}")
    endif()
endif()

# Feature Preprocessor Definitions
if(ENABLE_COREAUDIO)
    list(APPEND CDSP_C_DEFINITIONS ENABLE_COREAUDIO)
endif()
if(ENABLE_ACCELERATE)
    list(APPEND CDSP_C_DEFINITIONS ENABLE_ACCELERATE)
endif()
if(ENABLE_ALSA)
    list(APPEND CDSP_C_DEFINITIONS ENABLE_ALSA)
endif()
if(ENABLE_PIPEWIRE)
    list(APPEND CDSP_C_DEFINITIONS ENABLE_PIPEWIRE)
endif()
if(ENABLE_WASAPI)
    list(APPEND CDSP_C_DEFINITIONS ENABLE_WASAPI)
endif()
if(ENABLE_ASIO)
    list(APPEND CDSP_C_DEFINITIONS ENABLE_ASIO)
endif()
if(ENABLE_FFTW)
    list(APPEND CDSP_C_DEFINITIONS ENABLE_FFTW)
endif()
if(ENABLE_BLAS)
    list(APPEND CDSP_C_DEFINITIONS ENABLE_BLAS)
endif()
if(ENABLE_WEBSOCKET)
    list(APPEND CDSP_C_DEFINITIONS ENABLE_WEBSOCKET)
endif()
if(ENABLE_OPENMP)
    list(APPEND CDSP_C_DEFINITIONS ENABLE_OPENMP)
endif()
if(ENABLE_LIBDISPATCH)
    list(APPEND CDSP_C_DEFINITIONS ENABLE_LIBDISPATCH)
endif()

# Platform-specific definitions and dependencies
find_package(Threads REQUIRED)
set(CDSP_PLATFORM_DEPS Threads::Threads)
set(CDSP_PLATFORM_INCLUDES "")

if(WIN32)
    list(APPEND CDSP_C_DEFINITIONS
        COBJMACROS
        _WIN32
        UNICODE
        _UNICODE
        _USE_MATH_DEFINES
    )
    if(ENABLE_LIBDISPATCH)
        list(APPEND CDSP_C_DEFINITIONS HAVE_MODE_T)
        list(APPEND CDSP_PLATFORM_DEPS dispatch BlocksRuntime)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/win_deps/include")
        list(APPEND CDSP_PLATFORM_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/win_deps/include")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/win_deps/lib")
        link_directories("${CMAKE_CURRENT_SOURCE_DIR}/win_deps/lib")
    endif()
    if(ENABLE_FFTW)
        list(APPEND CDSP_PLATFORM_DEPS fftw3 fftw3f)
    endif()
    if(ENABLE_BLAS)
        list(APPEND CDSP_PLATFORM_DEPS openblas)
    endif()
    list(APPEND CDSP_PLATFORM_DEPS ws2_32 winmm ole32 uuid ksuser ksguid avrt)
elseif(APPLE)
    list(APPEND CDSP_PLATFORM_DEPS "-framework CoreFoundation")
    if(ENABLE_COREAUDIO)
        list(APPEND CDSP_PLATFORM_DEPS "-framework CoreAudio" "-framework AudioToolbox")
    endif()
    if(ENABLE_ACCELERATE)
        list(APPEND CDSP_PLATFORM_DEPS "-framework Accelerate")
    endif()
    if(ENABLE_FFTW)
        find_path(FFTW3_INCLUDE_DIR fftw3.h PATHS /opt/homebrew/include /usr/local/include)
        find_library(FFTW3_LIB fftw3 PATHS /opt/homebrew/lib /usr/local/lib)
        find_library(FFTW3F_LIB fftw3f PATHS /opt/homebrew/lib /usr/local/lib)
        if(FFTW3_INCLUDE_DIR)
            list(APPEND CDSP_PLATFORM_INCLUDES ${FFTW3_INCLUDE_DIR})
        endif()
        if(FFTW3_LIB AND FFTW3F_LIB)
            list(APPEND CDSP_PLATFORM_DEPS ${FFTW3_LIB} ${FFTW3F_LIB})
        else()
            list(APPEND CDSP_PLATFORM_DEPS fftw3 fftw3f)
        endif()
    endif()
    list(APPEND CDSP_PLATFORM_DEPS m)
else()
    # Linux
    list(APPEND CDSP_C_DEFINITIONS _GNU_SOURCE)
    find_package(PkgConfig QUIET)

    if(ENABLE_DBUS)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(DBUS dbus-1)
            if(DBUS_FOUND)
                list(APPEND CDSP_PLATFORM_INCLUDES ${DBUS_INCLUDE_DIRS})
                list(APPEND CDSP_PLATFORM_DEPS ${DBUS_LIBRARIES})
            else()
                list(APPEND CDSP_PLATFORM_DEPS dbus-1)
            endif()
        else()
            list(APPEND CDSP_PLATFORM_DEPS dbus-1)
        endif()
    else()
        list(APPEND CDSP_C_DEFINITIONS NO_DBUS)
    endif()

    if(ENABLE_LIBDISPATCH)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(DISPATCH libdispatch)
        endif()
        if(DISPATCH_FOUND)
            list(APPEND CDSP_PLATFORM_INCLUDES ${DISPATCH_INCLUDE_DIRS})
            list(APPEND CDSP_PLATFORM_DEPS ${DISPATCH_LIBRARIES})
        else()
            find_path(SWIFT_DISPATCH_INCLUDE_DIR
                NAMES dispatch/dispatch.h
                PATHS
                    /usr/lib/swift
                    /usr/lib/swift/linux
                    /usr/lib/swift/linux/x86_64
                    /usr/lib/swift/linux/aarch64
                    /usr/libexec/swift/lib/swift
                    /usr/libexec/swift/lib/swift/linux
                    /usr/include
            )
            find_library(SWIFT_DISPATCH_LIB
                NAMES dispatch libdispatch
                PATHS
                    /usr/lib/swift/linux
                    /usr/lib/swift/linux/x86_64
                    /usr/lib/swift/linux/aarch64
                    /usr/libexec/swift/lib/swift/linux
                    /usr/lib/x86_64-linux-gnu
                    /usr/lib/aarch64-linux-gnu
                    /usr/lib
            )
            find_library(SWIFT_BLOCKSRUNTIME_LIB
                NAMES BlocksRuntime libBlocksRuntime
                PATHS
                    /usr/lib/swift/linux
                    /usr/lib/swift/linux/x86_64
                    /usr/lib/swift/linux/aarch64
                    /usr/libexec/swift/lib/swift/linux
                    /usr/lib/x86_64-linux-gnu
                    /usr/lib/aarch64-linux-gnu
                    /usr/lib
            )

            if(SWIFT_DISPATCH_INCLUDE_DIR)
                list(APPEND CDSP_PLATFORM_INCLUDES ${SWIFT_DISPATCH_INCLUDE_DIR})
            endif()

            if(SWIFT_DISPATCH_LIB)
                list(APPEND CDSP_PLATFORM_DEPS ${SWIFT_DISPATCH_LIB})
                get_filename_component(SWIFT_DISPATCH_LIB_DIR "${SWIFT_DISPATCH_LIB}" DIRECTORY)
                set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH}:${SWIFT_DISPATCH_LIB_DIR}")
                set(CMAKE_BUILD_RPATH "${CMAKE_BUILD_RPATH}:${SWIFT_DISPATCH_LIB_DIR}")
            else()
                list(APPEND CDSP_PLATFORM_DEPS dispatch)
            endif()

            if(SWIFT_BLOCKSRUNTIME_LIB)
                list(APPEND CDSP_PLATFORM_DEPS ${SWIFT_BLOCKSRUNTIME_LIB})
            else()
                list(APPEND CDSP_PLATFORM_DEPS BlocksRuntime)
            endif()
        endif()
    elseif(ENABLE_OPENMP)
        find_package(OpenMP REQUIRED)
        list(APPEND CDSP_PLATFORM_DEPS OpenMP::OpenMP_C)
    endif()

    if(ENABLE_ALSA)
        list(APPEND CDSP_PLATFORM_DEPS asound)
    endif()

    if(ENABLE_PIPEWIRE)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(PIPEWIRE libpipewire-0.3)
            if(PIPEWIRE_FOUND)
                list(APPEND CDSP_PLATFORM_INCLUDES ${PIPEWIRE_INCLUDE_DIRS})
                list(APPEND CDSP_PLATFORM_DEPS ${PIPEWIRE_LIBRARIES})
            else()
                list(APPEND CDSP_PLATFORM_DEPS pipewire-0.3)
            endif()
        else()
            list(APPEND CDSP_PLATFORM_DEPS pipewire-0.3)
        endif()
    endif()

    if(ENABLE_FFTW)
        list(APPEND CDSP_PLATFORM_DEPS fftw3 fftw3f)
    endif()

    if(ENABLE_BLAS)
        list(APPEND CDSP_PLATFORM_DEPS openblas)
    endif()

    list(APPEND CDSP_PLATFORM_DEPS m rt)
endif()

# -----------------------------------------------------------------------------
# Include Directories
# -----------------------------------------------------------------------------
set(CDSP_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Audio
    ${CMAKE_CURRENT_SOURCE_DIR}/Backend
    ${CMAKE_CURRENT_SOURCE_DIR}/Config
    ${CMAKE_CURRENT_SOURCE_DIR}/DoP
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine
    ${CMAKE_CURRENT_SOURCE_DIR}/FFT
    ${CMAKE_CURRENT_SOURCE_DIR}/Filters
    ${CMAKE_CURRENT_SOURCE_DIR}/Logging
    ${CMAKE_CURRENT_SOURCE_DIR}/Mixer
    ${CMAKE_CURRENT_SOURCE_DIR}/Pipeline
    ${CMAKE_CURRENT_SOURCE_DIR}/Processors
    ${CMAKE_CURRENT_SOURCE_DIR}/Public
    ${CMAKE_CURRENT_SOURCE_DIR}/Resampler
    ${CMAKE_CURRENT_SOURCE_DIR}/Server
    ${CMAKE_CURRENT_SOURCE_DIR}/Utils
    ${CDSP_PLATFORM_INCLUDES}
)

# -----------------------------------------------------------------------------
# Source Files Selection
# -----------------------------------------------------------------------------
file(GLOB DSP_SRCS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Audio/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Backend/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Config/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/DoP/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/FFT/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Filters/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Logging/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Mixer/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Pipeline/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Processors/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Public/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Resampler/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Utils/*.c"
)

if(ENABLE_WEBSOCKET)
    file(GLOB SERVER_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Server/*.c")
else()
    set(SERVER_SRCS "")
endif()

# -----------------------------------------------------------------------------
# Main Library: dsp
# -----------------------------------------------------------------------------
add_library(dsp STATIC ${DSP_SRCS})
add_library(cdsp::dsp ALIAS dsp)

target_include_directories(dsp PUBLIC ${CDSP_INCLUDE_DIRS})
target_compile_definitions(dsp PUBLIC ${CDSP_C_DEFINITIONS})
target_compile_options(dsp PUBLIC ${CDSP_C_FLAGS})
target_link_libraries(dsp PUBLIC ${CDSP_PLATFORM_DEPS})

# -----------------------------------------------------------------------------
# CLI Executable: cdsp
# -----------------------------------------------------------------------------
add_executable(cdsp main.c ${SERVER_SRCS})
target_link_libraries(cdsp PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,dsp>")
target_include_directories(cdsp PRIVATE ${CDSP_INCLUDE_DIRS})
target_compile_definitions(cdsp PRIVATE ${CDSP_C_DEFINITIONS})
target_compile_options(cdsp PRIVATE ${CDSP_C_FLAGS})

# Output binary to bin/ inside build directory or top-level bin
set_target_properties(cdsp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# -----------------------------------------------------------------------------
# Testing: libdsp_test & CTest Test Infrastructure
# -----------------------------------------------------------------------------
if(BUILD_TESTING)
    include(CTest)

    # Optional Rust Harness Build (if cargo is installed)
    find_program(CARGO_EXECUTABLE cargo)
    if(CARGO_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/RustHarnesses/Cargo.toml")
        add_custom_target(test-rust-build
            COMMAND ${CARGO_EXECUTABLE} build --release
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Tests/RustHarnesses
            COMMENT "Building Rust harness binaries..."
        )
    endif()

    add_library(dsp_test STATIC ${DSP_SRCS})
    target_include_directories(dsp_test PUBLIC ${CDSP_INCLUDE_DIRS})
    target_compile_definitions(dsp_test PUBLIC
        ${CDSP_C_DEFINITIONS}
        CDSP_TEST
        CDSP_COMBINED_TEST_SUITE
    )
    target_compile_options(dsp_test PUBLIC ${CDSP_C_FLAGS})
    target_link_libraries(dsp_test PUBLIC ${CDSP_PLATFORM_DEPS})

    file(GLOB TEST_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CLibTests/test_*.c")
    list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_runner_main\\.c$")
    list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_filter_benchmark\\.c$")
    list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_dsd_benchmark\\.c$")
    list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_pipeline_benchmark\\.c$")
    list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_resampler_matrix\\.c$")

    if(NOT ENABLE_WEBSOCKET)
        list(FILTER TEST_SRCS EXCLUDE REGEX ".*/test_websocket_server\\.c$")
    endif()

    add_executable(test_runner
        Tests/CLibTests/test_runner_main.c
        ${TEST_SRCS}
        ${SERVER_SRCS}
    )
    target_include_directories(test_runner PRIVATE
        ${CDSP_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/CLibTests
    )
    target_compile_definitions(test_runner PRIVATE
        ${CDSP_C_DEFINITIONS}
        CDSP_TEST
        CDSP_COMBINED_TEST_SUITE
        CDSP_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_options(test_runner PRIVATE ${CDSP_C_FLAGS})
    target_link_libraries(test_runner PRIVATE dsp_test)

    if(TARGET test-rust-build)
        add_dependencies(test_runner test-rust-build)
    endif()

    if(WIN32)
        target_link_options(test_runner PRIVATE
            -Wl,--wrap,malloc -Wl,--wrap,calloc -Wl,--wrap,realloc -Wl,--wrap,free
        )
    endif()

    set_target_properties(test_runner PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Tests/CLibTests/bin"
    )

    # CTest Test Discovery: Register each unit test case as an individual CTest test
    set(CDSP_DISCOVERED_TESTS_FILE "${CMAKE_CURRENT_BINARY_DIR}/cdsp_tests.cmake")
    if(NOT EXISTS "${CDSP_DISCOVERED_TESTS_FILE}")
        file(WRITE "${CDSP_DISCOVERED_TESTS_FILE}" "# Automatically populated by test_runner discovery\nadd_test(NAME test_runner_suite COMMAND test_runner)\nset_tests_properties(test_runner_suite PROPERTIES LABELS \"unit\")\n")
    endif()
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${CDSP_DISCOVERED_TESTS_FILE}")

    add_custom_command(
        TARGET test_runner POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:test_runner>
            -DCTEST_FILE=${CDSP_DISCOVERED_TESTS_FILE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/Tools/CdspDiscoverTests.cmake
        COMMENT "Discovering CTest test cases from test_runner..."
        VERBATIM
    )

    # Convenience targets
    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -j
        DEPENDS test_runner
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running unit test suite in parallel via CTest..."
    )
endif()

# -----------------------------------------------------------------------------
# Benchmarks
# -----------------------------------------------------------------------------
if(BUILD_BENCHMARKS)
    set(BENCHMARK_NAMES
        test_filter_benchmark
        test_dsd_benchmark
        test_pipeline_benchmark
        test_resampler_matrix
    )

    set(BENCH_TARGETS "")
    foreach(bench_name ${BENCHMARK_NAMES})
        add_executable(${bench_name} Tests/CLibTests/${bench_name}.c)
        target_include_directories(${bench_name} PRIVATE
            ${CDSP_INCLUDE_DIRS}
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/CLibTests
        )
        target_compile_definitions(${bench_name} PRIVATE
            ${CDSP_C_DEFINITIONS}
            CDSP_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
        )
        target_compile_options(${bench_name} PRIVATE ${CDSP_C_FLAGS})
        target_link_libraries(${bench_name} PRIVATE dsp)
        set_target_properties(${bench_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Tests/CLibTests/bin"
        )
        list(APPEND BENCH_TARGETS ${bench_name})
    endforeach()

    add_custom_target(bench
        COMMAND ${CMAKE_BINARY_DIR}/Tests/CLibTests/bin/test_filter_benchmark
        COMMAND ${CMAKE_BINARY_DIR}/Tests/CLibTests/bin/test_dsd_benchmark
        COMMAND ${CMAKE_BINARY_DIR}/Tests/CLibTests/bin/test_pipeline_benchmark
        COMMAND ${CMAKE_BINARY_DIR}/Tests/CLibTests/bin/test_resampler_matrix
        DEPENDS ${BENCH_TARGETS}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running C benchmarks..."
    )
endif()

# -----------------------------------------------------------------------------
# Custom Targets: Call Graph Audit, Clang-Format
# -----------------------------------------------------------------------------
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_target(callgraph-audit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Tools/generate_callgraph.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running Static Call Graph Audit..."
    )
endif()

find_program(CLANG_FORMAT_BIN clang-format)
if(CLANG_FORMAT_BIN)
    file(GLOB_RECURSE ALL_C_AND_H_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    )
    list(FILTER ALL_C_AND_H_FILES EXCLUDE REGEX ".*/build/.*")
    list(FILTER ALL_C_AND_H_FILES EXCLUDE REGEX ".*/\\.build/.*")
    list(FILTER ALL_C_AND_H_FILES EXCLUDE REGEX ".*/Tests/RustHarnesses/.*")

    add_custom_target(format
        COMMAND ${CLANG_FORMAT_BIN} -i ${ALL_C_AND_H_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting all C and H source files with clang-format..."
    )
endif()

# Include-What-You-Use (IWYU) targets
find_program(IWYU_TOOL_BIN NAMES iwyu_tool iwyu_tool.py PATHS /usr/local/bin /opt/homebrew/bin)
find_program(FIX_INCLUDES_BIN NAMES fix_includes.py fix_includes PATHS /usr/local/bin /opt/homebrew/bin)
if(IWYU_TOOL_BIN)
    add_custom_target(iwyu
        COMMAND ${IWYU_TOOL_BIN} -p ${CMAKE_BINARY_DIR} -- -Xiwyu --mapping_file=${CMAKE_CURRENT_SOURCE_DIR}/Tools/iwyu.imp -Xiwyu --no_fwd_decls
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running Include-What-You-Use analysis..."
    )
    if(FIX_INCLUDES_BIN)
        add_custom_target(iwyu-fix
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/iwyu
            COMMAND ${CMAKE_COMMAND} -E echo "Running IWYU and applying include fixes..."
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Applying Include-What-You-Use fixes..."
        )
    endif()
endif()

# -----------------------------------------------------------------------------
# Installation Rules
# -----------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS cdsp dsp
    EXPORT cdspTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY Public/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cdsp
    FILES_MATCHING PATTERN "*.h"
)
