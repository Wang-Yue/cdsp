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
    set(DEFAULT_ENABLE_COREAUDIO   ON)
    set(DEFAULT_ENABLE_ACCELERATE  ON)
    set(DEFAULT_ENABLE_LIBDISPATCH ON)
    set(DEFAULT_ENABLE_ALSA        OFF)
    set(DEFAULT_ENABLE_PIPEWIRE    OFF)
    set(DEFAULT_ENABLE_WASAPI      OFF)
    set(DEFAULT_ENABLE_ASIO        OFF)
    set(DEFAULT_ENABLE_FFTW        OFF)
    set(DEFAULT_ENABLE_OPENMP      OFF)
    set(DEFAULT_ENABLE_DBUS        OFF)
elseif(WIN32)
    set(DEFAULT_ENABLE_COREAUDIO   OFF)
    set(DEFAULT_ENABLE_ACCELERATE  OFF)
    set(DEFAULT_ENABLE_LIBDISPATCH OFF)
    set(DEFAULT_ENABLE_ALSA        OFF)
    set(DEFAULT_ENABLE_PIPEWIRE    OFF)
    set(DEFAULT_ENABLE_WASAPI      ON)
    set(DEFAULT_ENABLE_ASIO        ON)
    set(DEFAULT_ENABLE_FFTW        ON)
    set(DEFAULT_ENABLE_OPENMP      ON)
    set(DEFAULT_ENABLE_DBUS        OFF)
else()
    # Linux (Default)
    set(DEFAULT_ENABLE_COREAUDIO   OFF)
    set(DEFAULT_ENABLE_ACCELERATE  OFF)
    set(DEFAULT_ENABLE_LIBDISPATCH OFF)
    set(DEFAULT_ENABLE_ALSA        ON)
    set(DEFAULT_ENABLE_PIPEWIRE    ON)
    set(DEFAULT_ENABLE_WASAPI      OFF)
    set(DEFAULT_ENABLE_ASIO        OFF)
    set(DEFAULT_ENABLE_FFTW        ON)
    set(DEFAULT_ENABLE_OPENMP      ON)
    set(DEFAULT_ENABLE_DBUS        ON)
endif()

option(ENABLE_COREAUDIO   "Enable macOS CoreAudio backend"      ${DEFAULT_ENABLE_COREAUDIO})
option(ENABLE_ACCELERATE  "Enable Apple Accelerate (vDSP/vForce)" ${DEFAULT_ENABLE_ACCELERATE})
option(ENABLE_ALSA        "Enable Linux ALSA audio backend"     ${DEFAULT_ENABLE_ALSA})
option(ENABLE_PIPEWIRE    "Enable Linux PipeWire backend"       ${DEFAULT_ENABLE_PIPEWIRE})
option(ENABLE_WASAPI      "Enable Windows WASAPI audio backend" ${DEFAULT_ENABLE_WASAPI})
option(ENABLE_ASIO        "Enable Windows ASIO audio backend"   ${DEFAULT_ENABLE_ASIO})
option(ENABLE_FFTW        "Enable FFTW library for FFT"         ${DEFAULT_ENABLE_FFTW})
option(ENABLE_OPENMP      "Enable OpenMP parallelization"       ${DEFAULT_ENABLE_OPENMP})
option(ENABLE_WEBSOCKET   "Enable WebSocket control server"     ON)
option(ENABLE_DBUS        "Enable Linux DBus device monitoring" ${DEFAULT_ENABLE_DBUS})
option(ENABLE_LIBDISPATCH "Enable Apple libdispatch (GCD)"      ${DEFAULT_ENABLE_LIBDISPATCH})

option(ENABLE_LTO         "Enable Link Time Optimization"       ON)
option(ENABLE_NATIVE_ARCH "Enable native CPU optimization"      ON)
option(BUILD_TESTING      "Build test runner and unit tests"    ON)
option(BUILD_BENCHMARKS   "Build benchmark targets"             ON)

set(ENABLE_SANITIZER "none" CACHE STRING "Sanitizer to enable (none, address, thread, undefined, memory, leak, intsan, cfi)")

# -----------------------------------------------------------------------------
# Global Optimization & Build Settings
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

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
        -Wall
        -Wextra
        -O3
        -ffp-contract=fast
        -fno-math-errno
        -funroll-loops
        -fvisibility=hidden
    )

    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        add_compile_options(
            -mllvm -force-vector-interleave=8
        )
    elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        add_compile_options(
            -fassociative-math
            -fno-signed-zeros
            -fno-trapping-math
            -fvariable-expansion-in-unroller
            --param=max-variable-expansions-in-unroller=8
            --param=max-unroll-times=8
        )
    endif()

    if(ENABLE_NATIVE_ARCH AND NOT CMAKE_CROSSCOMPILING)
        if(APPLE)
            add_compile_options(-mcpu=native)
        else()
            add_compile_options(-march=native)
        endif()
    endif()
endif()

# Sanitizers
if(NOT ENABLE_SANITIZER STREQUAL "none")
    if(ENABLE_SANITIZER STREQUAL "address")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
    elseif(ENABLE_SANITIZER STREQUAL "thread")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=thread)
    elseif(ENABLE_SANITIZER STREQUAL "undefined")
        add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=undefined)
    elseif(ENABLE_SANITIZER STREQUAL "intsan")
        add_compile_options(-fsanitize=signed-integer-overflow,shift,integer-divide-by-zero -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=signed-integer-overflow,shift,integer-divide-by-zero)
    elseif(ENABLE_SANITIZER STREQUAL "memory")
        if(APPLE)
            message(FATAL_ERROR "MemorySanitizer is not supported on macOS")
        endif()
        set(ENABLE_ALSA OFF CACHE BOOL "Disabled under MemorySanitizer" FORCE)
        set(ENABLE_PIPEWIRE OFF CACHE BOOL "Disabled under MemorySanitizer" FORCE)
        set(ENABLE_DBUS OFF CACHE BOOL "Disabled under MemorySanitizer" FORCE)
        add_compile_options(-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=memory)
    elseif(ENABLE_SANITIZER STREQUAL "leak")
        if(APPLE)
            add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
            add_link_options(-fsanitize=address)
        else()
            add_compile_options(-fsanitize=leak -fno-omit-frame-pointer -g)
            add_link_options(-fsanitize=leak)
        endif()
    elseif(ENABLE_SANITIZER STREQUAL "cfi")
        add_compile_options(-flto -fvisibility=hidden -fsanitize=cfi -fno-omit-frame-pointer -g)
        add_link_options(-flto -fsanitize=cfi)
    else()
        message(FATAL_ERROR "Unknown sanitizer: ${ENABLE_SANITIZER}")
    endif()
endif()

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
# Core Library: dsp
# -----------------------------------------------------------------------------
add_library(dsp STATIC ${DSP_SRCS})
add_library(cdsp::dsp ALIAS dsp)

target_include_directories(dsp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Audio>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Backend>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Config>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/DoP>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Engine>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/FFT>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Filters>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Logging>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Mixer>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Pipeline>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Processors>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Public>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Resampler>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Server>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Utils>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/cdsp>
)

target_compile_definitions(dsp PUBLIC CDSP_BUILD_SHARED)

# -----------------------------------------------------------------------------
# Base Platform Dependencies
# -----------------------------------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(dsp PUBLIC Threads::Threads)

find_package(PkgConfig QUIET)

if(WIN32)
    target_compile_definitions(dsp PUBLIC
        COBJMACROS
        _WIN32
        UNICODE
        _UNICODE
        _USE_MATH_DEFINES
    )
    target_link_libraries(dsp PUBLIC ws2_32 winmm ole32 uuid ksuser ksguid avrt)
elseif(APPLE)
    target_link_libraries(dsp PUBLIC "-framework CoreFoundation" m)
else()
    target_compile_definitions(dsp PUBLIC _GNU_SOURCE)
    target_link_libraries(dsp PUBLIC m rt)
endif()

# -----------------------------------------------------------------------------
# Audio Backends
# -----------------------------------------------------------------------------
if(ENABLE_COREAUDIO)
    target_compile_definitions(dsp PUBLIC ENABLE_COREAUDIO)
    target_link_libraries(dsp PUBLIC "-framework CoreAudio" "-framework AudioToolbox")
endif()

if(ENABLE_ACCELERATE)
    target_compile_definitions(dsp PUBLIC ENABLE_ACCELERATE)
    target_link_libraries(dsp PUBLIC "-framework Accelerate")
endif()

if(ENABLE_WASAPI)
    target_compile_definitions(dsp PUBLIC ENABLE_WASAPI)
endif()

if(ENABLE_ASIO)
    target_compile_definitions(dsp PUBLIC ENABLE_ASIO)
endif()

if(ENABLE_ALSA)
    find_package(ALSA QUIET)
    if(TARGET ALSA::ALSA)
        target_link_libraries(dsp PUBLIC ALSA::ALSA)
        target_compile_definitions(dsp PUBLIC ENABLE_ALSA)
    elseif(PKG_CONFIG_FOUND)
        pkg_check_modules(ALSA QUIET IMPORTED_TARGET alsa)
        if(TARGET PkgConfig::ALSA)
            target_link_libraries(dsp PUBLIC PkgConfig::ALSA)
            target_compile_definitions(dsp PUBLIC ENABLE_ALSA)
        endif()
    endif()
endif()

if(ENABLE_PIPEWIRE AND PKG_CONFIG_FOUND)
    pkg_check_modules(PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
    if(TARGET PkgConfig::PIPEWIRE)
        target_link_libraries(dsp PUBLIC PkgConfig::PIPEWIRE)
        target_compile_definitions(dsp PUBLIC ENABLE_PIPEWIRE)
    endif()
endif()

if(ENABLE_DBUS AND PKG_CONFIG_FOUND)
    pkg_check_modules(DBUS QUIET IMPORTED_TARGET dbus-1)
    if(TARGET PkgConfig::DBUS)
        target_link_libraries(dsp PUBLIC PkgConfig::DBUS)
        target_compile_definitions(dsp PUBLIC ENABLE_DBUS)
    else()
        target_compile_definitions(dsp PUBLIC NO_DBUS)
    endif()
else()
    target_compile_definitions(dsp PUBLIC NO_DBUS)
endif()

if(ENABLE_WEBSOCKET)
    target_compile_definitions(dsp PUBLIC ENABLE_WEBSOCKET)
endif()

# -----------------------------------------------------------------------------
# Math, FFT & Acceleration Libraries
# -----------------------------------------------------------------------------
if(ENABLE_FFTW)
    set(FFTW_FOUND_FLAG FALSE)
    # 1. Search for headers and full absolute library paths first (prevents MSYS2 FFTW3Config.cmake relative path bugs)
    find_path(FFTW3_INCLUDE_DIR NAMES fftw3.h)
    find_library(FFTW3_LIB NAMES fftw3 libfftw3-3 libfftw3)
    find_library(FFTW3F_LIB NAMES fftw3f libfftw3f-3 libfftw3f)

    if(FFTW3_INCLUDE_DIR AND FFTW3_LIB AND FFTW3F_LIB)
        target_include_directories(dsp PUBLIC ${FFTW3_INCLUDE_DIR})
        target_link_libraries(dsp PUBLIC ${FFTW3_LIB} ${FFTW3F_LIB})
        target_compile_definitions(dsp PUBLIC ENABLE_FFTW)
        set(FFTW_FOUND_FLAG TRUE)
    elseif(PKG_CONFIG_FOUND)
        pkg_check_modules(FFTW3 QUIET IMPORTED_TARGET fftw3 fftw3f)
        if(TARGET PkgConfig::FFTW3)
            target_link_libraries(dsp PUBLIC PkgConfig::FFTW3)
            target_compile_definitions(dsp PUBLIC ENABLE_FFTW)
            set(FFTW_FOUND_FLAG TRUE)
        endif()
    endif()

    if(NOT FFTW_FOUND_FLAG)
        find_package(FFTW3 CONFIG QUIET)
        find_package(FFTW3f CONFIG QUIET)
        if(TARGET FFTW3::fftw3 AND TARGET FFTW3::fftw3f)
            target_link_libraries(dsp PUBLIC FFTW3::fftw3 FFTW3::fftw3f)
            target_compile_definitions(dsp PUBLIC ENABLE_FFTW)
            set(FFTW_FOUND_FLAG TRUE)
        endif()
    endif()

    if(FFTW_FOUND_FLAG)
        message(STATUS "FFTW3 found and enabled")
    else()
        message(STATUS "FFTW3 not found, falling back to built-in mixed-radix Real FFT")
    endif()
endif()



# -----------------------------------------------------------------------------
# Parallelization: OpenMP & libdispatch (GCD)
# -----------------------------------------------------------------------------
if(ENABLE_OPENMP)
    find_package(OpenMP QUIET)
    if(TARGET OpenMP::OpenMP_C)
        target_link_libraries(dsp PUBLIC OpenMP::OpenMP_C)
        target_compile_definitions(dsp PUBLIC ENABLE_OPENMP)
    endif()
endif()

if(ENABLE_LIBDISPATCH)
    set(DISPATCH_FOUND_FLAG FALSE)
    if(WIN32)
        target_compile_definitions(dsp PUBLIC HAVE_MODE_T ENABLE_LIBDISPATCH)
        target_link_libraries(dsp PUBLIC dispatch BlocksRuntime)
        set(DISPATCH_FOUND_FLAG TRUE)
    elseif(APPLE)
        # Built-in in Darwin libSystem
        target_compile_definitions(dsp PUBLIC ENABLE_LIBDISPATCH)
        set(DISPATCH_FOUND_FLAG TRUE)
    else()
        # 1. Discover Swift toolchain paths if swift is installed
        find_program(SWIFT_EXECUTABLE swift)
        set(SWIFT_SEARCH_PATHS "")
        if(SWIFT_EXECUTABLE)
            get_filename_component(SWIFT_REAL_PATH "${SWIFT_EXECUTABLE}" REALPATH)
            get_filename_component(SWIFT_BIN_DIR "${SWIFT_REAL_PATH}" DIRECTORY)
            get_filename_component(SWIFT_TOOLCHAIN_DIR "${SWIFT_BIN_DIR}" DIRECTORY)
            list(APPEND SWIFT_SEARCH_PATHS
                "${SWIFT_TOOLCHAIN_DIR}/lib/swift"
                "${SWIFT_TOOLCHAIN_DIR}/lib/swift/linux"
                "${SWIFT_TOOLCHAIN_DIR}/lib/swift/linux/${CMAKE_SYSTEM_PROCESSOR}"
                "${SWIFT_TOOLCHAIN_DIR}/lib/swift_static"
                "${SWIFT_TOOLCHAIN_DIR}/lib/swift_static/linux"
            )
        endif()

        # 2. Try pkg-config if available
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(DISPATCH QUIET IMPORTED_TARGET libdispatch dispatch)
            if(TARGET PkgConfig::DISPATCH)
                target_link_libraries(dsp PUBLIC PkgConfig::DISPATCH)
                target_compile_definitions(dsp PUBLIC ENABLE_LIBDISPATCH)
                set(DISPATCH_FOUND_FLAG TRUE)
            endif()
        endif()

        # 3. Fallback to searching header & libraries in Swift toolchain and system paths
        if(NOT DISPATCH_FOUND_FLAG)
            find_path(DISPATCH_INCLUDE_DIR NAMES dispatch/dispatch.h
                HINTS ${SWIFT_SEARCH_PATHS}
                PATH_SUFFIXES
                    dispatch
                    swift
                    swift/linux/${CMAKE_SYSTEM_PROCESSOR}
                    swift/linux
                    lib/swift
                    lib/swift/dispatch
                    usr/lib/swift
                    usr/libexec/swift/lib/swift
            )
            find_library(DISPATCH_LIB NAMES dispatch libdispatch
                HINTS ${SWIFT_SEARCH_PATHS}
                PATH_SUFFIXES
                    swift/linux/${CMAKE_SYSTEM_PROCESSOR}
                    swift/linux
                    lib/swift/linux
                    usr/lib/swift/linux
                    usr/libexec/swift/lib/swift/linux
            )
            find_library(BLOCKSRUNTIME_LIB NAMES BlocksRuntime libBlocksRuntime
                HINTS ${SWIFT_SEARCH_PATHS}
                PATH_SUFFIXES
                    swift/linux/${CMAKE_SYSTEM_PROCESSOR}
                    swift/linux
                    lib/swift/linux
                    usr/lib/swift/linux
                    usr/libexec/swift/lib/swift/linux
            )

            if(DISPATCH_INCLUDE_DIR AND DISPATCH_LIB)
                target_include_directories(dsp PUBLIC ${DISPATCH_INCLUDE_DIR})
                target_link_libraries(dsp PUBLIC ${DISPATCH_LIB})
                if(BLOCKSRUNTIME_LIB)
                    target_link_libraries(dsp PUBLIC ${BLOCKSRUNTIME_LIB})
                endif()
                get_filename_component(DISPATCH_LIB_DIR "${DISPATCH_LIB}" DIRECTORY)
                target_link_directories(dsp PUBLIC "${DISPATCH_LIB_DIR}")
                target_link_options(dsp PUBLIC "-Wl,-rpath,${DISPATCH_LIB_DIR}")
                target_compile_definitions(dsp PUBLIC ENABLE_LIBDISPATCH)
                set(DISPATCH_FOUND_FLAG TRUE)
            endif()
        endif()

        if(DISPATCH_FOUND_FLAG)
            message(STATUS "libdispatch (GCD) found and enabled: ${DISPATCH_LIB}")
        else()
            message(WARNING "ENABLE_LIBDISPATCH was requested but libdispatch headers or libraries were not found.")
        endif()
    endif()
endif()

# -----------------------------------------------------------------------------
# CLI Executable: cdsp
# -----------------------------------------------------------------------------
add_executable(cdsp main.c ${SERVER_SRCS})
target_link_libraries(cdsp PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,dsp>")
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
    target_link_libraries(dsp_test PUBLIC dsp)
    target_compile_definitions(dsp_test PUBLIC
        CDSP_TEST
        CDSP_COMBINED_TEST_SUITE
    )

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
    target_link_libraries(test_runner PRIVATE dsp_test)
    target_include_directories(test_runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Tests/CLibTests)
    target_compile_definitions(test_runner PRIVATE CDSP_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

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

    # Convenience target
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
        target_link_libraries(${bench_name} PRIVATE dsp)
        target_include_directories(${bench_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Tests/CLibTests)
        target_compile_definitions(${bench_name} PRIVATE CDSP_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
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

set_source_files_properties(
    DoP/dsd_encoder.c
    DoP/sigma_delta_modulator.c
    Tests/CLibTests/test_dsd.c
    Tests/CLibTests/test_resampler_matrix.c
    PROPERTIES COMPILE_OPTIONS "-fno-associative-math"
)
