cmake_minimum_required(VERSION 3.20)
project(cdsp-studio LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

# ==============================================================================
# Global Build & Language Configuration
# ==============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Qt Automoc, Autorcc, Autouic
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOGEN_PARALLEL "AUTO")

# Always generate compile_commands.json for tooling (IWYU, clang-tidy, LSP)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Option to run IWYU directly during target compilation
option(ENABLE_IWYU "Enable Include-What-You-Use during compilation" OFF)

# Build acceleration & memory optimization options
option(ENABLE_PCH "Enable Precompiled Headers (PCH) to speed up compilation and reduce memory usage" ON)
option(ENABLE_UNITY_BUILD "Enable Unity (Jumbo) build to combine translation units" OFF)
option(ENABLE_RUST_BACKEND "Build CDSP Studio with Rust CamillaDSP backend" OFF)

# ==============================================================================
# RPATH Configuration for Installed Binaries
# ==============================================================================
# Automatically include directories of all linked dependencies in install RPATH
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# ==============================================================================
# Dependencies
# ==============================================================================
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Concurrent Multimedia)
find_package(Threads REQUIRED)

# Extra platform-specific dependencies for cdsp-studio UI
set(EXTRA_PLATFORM_LIBS "")
if(APPLE)
    find_library(COCOA_LIB Cocoa REQUIRED)
    find_library(AVFOUNDATION_LIB AVFoundation REQUIRED)
    list(APPEND EXTRA_PLATFORM_LIBS ${COCOA_LIB} ${AVFOUNDATION_LIB})
endif()

# ==============================================================================
# DSP Processing Engine Dependency (Rust backend vs Native C cdsp)
# ==============================================================================
if(ENABLE_RUST_BACKEND)
    find_program(CARGO_EXECUTABLE cargo REQUIRED)
    set(RUST_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tests/rust_engine")
    set(RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/rust_target")
    
    if(CMAKE_BUILD_TYPE MATCHES "Debug")
        set(RUST_PROFILE "debug")
        set(RUST_BUILD_FLAG "")
    else()
        set(RUST_PROFILE "release")
        set(RUST_BUILD_FLAG "--release")
    endif()
    
    set(RUST_LIB_STATIC "${RUST_TARGET_DIR}/${RUST_PROFILE}/librust_engine.a")
    
    add_custom_command(
        OUTPUT "${RUST_LIB_STATIC}"
        COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${RUST_TARGET_DIR} ${CARGO_EXECUTABLE} build ${RUST_BUILD_FLAG}
        WORKING_DIRECTORY "${RUST_ENGINE_DIR}"
        DEPENDS "${RUST_ENGINE_DIR}/Cargo.toml" "${RUST_ENGINE_DIR}/src/lib.rs"
        COMMENT "Building Rust CamillaDSP engine static library..."
        VERBATIM
    )
    
    add_custom_target(cdsp_rust_engine_target DEPENDS "${RUST_LIB_STATIC}")
    
    add_library(cdsp::dsp STATIC IMPORTED GLOBAL)
    set_target_properties(cdsp::dsp PROPERTIES
        IMPORTED_LOCATION "${RUST_LIB_STATIC}"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    )
    add_dependencies(cdsp::dsp cdsp_rust_engine_target)
    if(APPLE)
        set_target_properties(cdsp::dsp PROPERTIES
            INTERFACE_LINK_LIBRARIES "-framework CoreAudio;-framework AudioToolbox;-framework Security;-framework SystemConfiguration"
        )
    endif()
else()
    if(NOT TARGET cdsp::dsp)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_BINARY_DIR}/cdsp_build" EXCLUDE_FROM_ALL)
    endif()
endif()

# ==============================================================================
# Source Files & Platform Resources
# ==============================================================================
file(GLOB_RECURSE SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/resources/*.qrc")

if(APPLE)
    enable_language(OBJCXX)
    set(APP_ICON_MACOS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app.icns")
    set_source_files_properties(${APP_ICON_MACOS} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    list(APPEND SOURCES
        ${APP_ICON_MACOS}
        "${CMAKE_CURRENT_SOURCE_DIR}/utils/MacUtils.mm"
    )
elseif(WIN32)
    set(APP_ICON_WIN "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc")
    list(APPEND SOURCES ${APP_ICON_WIN})
endif()

# ==============================================================================
# Target: cdsp-studio
# ==============================================================================
add_executable(cdsp-studio WIN32 ${SOURCES})

target_include_directories(cdsp-studio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(cdsp-studio PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    Qt6::Multimedia
    Threads::Threads
    cdsp::dsp
    ${EXTRA_PLATFORM_LIBS}
)

# ==============================================================================
# Build Optimization: Precompiled Headers (PCH) & Unity Build
# ==============================================================================
if(ENABLE_PCH)
    target_precompile_headers(cdsp-studio PRIVATE
        <QtCore>
        <QtGui>
        <QtWidgets>
        <vector>
        <string>
        <memory>
        <functional>
        <optional>
        <algorithm>
    )
    if(APPLE)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/utils/MacUtils.mm" PROPERTIES
            SKIP_PRECOMPILE_HEADERS ON
        )
    endif()
endif()

if(ENABLE_UNITY_BUILD)
    set_target_properties(cdsp-studio PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE 8
        UNITY_BUILD_MODE BATCH
    )
    if(APPLE)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/utils/MacUtils.mm" PROPERTIES
            SKIP_UNITY_BUILD_INCLUSION ON
        )
    endif()
endif()

# ==============================================================================
# Platform Properties & Link Options
# ==============================================================================
if(APPLE)
    set_target_properties(cdsp-studio PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "CDSP Studio"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "CDSP Studio"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.wangyue.cdspstudio"
        MACOSX_BUNDLE_ICON_FILE "app.icns"
        MACOSX_BUNDLE_COPYRIGHT "Copyright © 2026 Wang Yue. All rights reserved."
    )

    target_link_options(cdsp-studio PRIVATE -Wl,-dead_strip)

    add_custom_command(TARGET cdsp-studio POST_BUILD
        COMMAND codesign --force --sign - $<TARGET_FILE:cdsp-studio>
        COMMENT "Ad-hoc signing cdsp-studio executable..."
    )
elseif(WIN32)
    set_target_properties(cdsp-studio PROPERTIES
        OUTPUT_NAME "CDSPStudio"
    )
endif()


# ==============================================================================
# Include-What-You-Use (IWYU)
# ==============================================================================
find_program(IWYU_BIN NAMES include-what-you-use)
if(ENABLE_IWYU)
    if(IWYU_BIN)
        message(STATUS "Include-What-You-Use enabled for cdsp-studio")
        set(IWYU_MAPPING_FILE "${CMAKE_SOURCE_DIR}/tools/iwyu.imp")
        set(IWYU_OPTIONS
            ${IWYU_BIN}
            -Xiwyu --mapping_file=${IWYU_MAPPING_FILE}
            -Xiwyu --no_fwd_decls
            -Xiwyu --max_line_length=120
        )
        if(APPLE AND CMAKE_OSX_SYSROOT)
            list(APPEND IWYU_OPTIONS -isysroot ${CMAKE_OSX_SYSROOT})
        endif()
        set_target_properties(cdsp-studio PROPERTIES
            CXX_INCLUDE_WHAT_YOU_USE "${IWYU_OPTIONS}"
        )
    else()
        message(WARNING "ENABLE_IWYU is ON, but 'include-what-you-use' executable was not found.")
    endif()
endif()

# Standalone targets to check or fix includes across the project
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/run-iwyu.sh")
    add_custom_target(
        iwyu-studio
        COMMAND "${CMAKE_SOURCE_DIR}/tools/run-iwyu.sh" --check "${CMAKE_CURRENT_SOURCE_DIR}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Analyzing cdsp-studio includes with Include-What-You-Use..."
        USES_TERMINAL
    )
    add_custom_target(
        iwyu-check-studio
        COMMAND "${CMAKE_SOURCE_DIR}/tools/run-iwyu.sh" --check "${CMAKE_CURRENT_SOURCE_DIR}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Analyzing cdsp-studio includes with Include-What-You-Use..."
        USES_TERMINAL
    )
    add_custom_target(
        iwyu-fix-studio
        COMMAND "${CMAKE_SOURCE_DIR}/tools/run-iwyu.sh" --fix "${CMAKE_CURRENT_SOURCE_DIR}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Applying Include-What-You-Use include fixes automatically..."
        USES_TERMINAL
    )
endif()

# ==============================================================================
# Code Formatting (clang-format)
# ==============================================================================
find_program(CLANG_FORMAT_BIN NAMES clang-format)
if(CLANG_FORMAT_BIN)
    file(GLOB_RECURSE ALL_PROJECT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
    add_custom_target(
        format-studio
        COMMAND ${CLANG_FORMAT_BIN} -i -style=file ${ALL_PROJECT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-format on cdsp-studio source and header files..."
    )
    add_custom_target(
        format-check-studio
        COMMAND ${CLANG_FORMAT_BIN} --dry-run -Werror -style=file ${ALL_PROJECT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking cdsp-studio code formatting with clang-format..."
    )
endif()

# ==============================================================================
# Installation (Linux / Unix)
# ==============================================================================
if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
    install(TARGETS cdsp-studio
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/com.wangyue.cdspstudio.desktop"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app.png"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
        RENAME com.wangyue.cdspstudio.png
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app.png"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps
        RENAME com.wangyue.cdspstudio.png
    )

    # Post-install: Grant cap_sys_nice=ep capability for Real-Time priority scheduling (SCHED_FIFO)
    install(CODE "
        find_program(SETCAP_BIN setcap)
        if(SETCAP_BIN)
            set(TARGET_BIN \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/cdsp-studio\")
            if(EXISTS \"\${TARGET_BIN}\")
                message(STATUS \"Applying cap_sys_nice=ep capability to \${TARGET_BIN}...\")
                execute_process(
                    COMMAND \${SETCAP_BIN} cap_sys_nice=ep \${TARGET_BIN}
                    RESULT_VARIABLE SETCAP_RES
                )
                if(NOT SETCAP_RES EQUAL 0)
                    message(WARNING \"Failed to set capability (requires root/sudo). Run manually: sudo setcap cap_sys_nice=ep \${TARGET_BIN}\")
                endif()
            endif()
        else()
            message(WARNING \"'setcap' tool not found. Real-time scheduling capability was not applied.\")
        endif()
    ")

endif()



