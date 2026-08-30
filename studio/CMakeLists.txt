cmake_minimum_required(VERSION 3.20)
project(MonitorQt LANGUAGES C CXX)

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

# Extra platform-specific dependencies for MonitorQt UI
set(EXTRA_PLATFORM_LIBS "")
if(APPLE)
    find_library(COCOA_LIB Cocoa REQUIRED)
    list(APPEND EXTRA_PLATFORM_LIBS ${COCOA_LIB})
endif()

# Fetch Wang-Yue/cdsp (DSP processing engine)
include(FetchContent)
FetchContent_Declare(
    cdsp
    GIT_REPOSITORY https://github.com/Wang-Yue/cdsp.git
    GIT_TAG        main
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(cdsp)

# ==============================================================================
# Source Files & Platform Resources
# ==============================================================================
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.h" "src/*.qrc")

if(APPLE)
    enable_language(OBJCXX)
    set(APP_ICON_MACOS "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/icons/app.icns")
    set_source_files_properties(${APP_ICON_MACOS} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    list(APPEND SOURCES
        ${APP_ICON_MACOS}
        "${CMAKE_CURRENT_SOURCE_DIR}/src/utils/MacUtils.mm"
    )
elseif(WIN32)
    set(APP_ICON_WIN "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/app.rc")
    list(APPEND SOURCES ${APP_ICON_WIN})
endif()

# ==============================================================================
# Target: MonitorQt
# ==============================================================================
add_executable(MonitorQt WIN32 ${SOURCES})

target_include_directories(MonitorQt PRIVATE src)

target_link_libraries(MonitorQt PRIVATE
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
    target_precompile_headers(MonitorQt PRIVATE
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
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/utils/MacUtils.mm" PROPERTIES
            SKIP_PRECOMPILE_HEADERS ON
        )
    endif()
endif()

if(ENABLE_UNITY_BUILD)
    set_target_properties(MonitorQt PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE 8
        UNITY_BUILD_MODE BATCH
    )
    if(APPLE)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/utils/MacUtils.mm" PROPERTIES
            SKIP_UNITY_BUILD_INCLUSION ON
        )
    endif()
endif()

# ==============================================================================
# Platform Properties & Link Options
# ==============================================================================
if(APPLE)
    set_target_properties(MonitorQt PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "MonitorQt"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.wangyue.monitorqt"
        MACOSX_BUNDLE_ICON_FILE "app.icns"
        MACOSX_BUNDLE_COPYRIGHT "Copyright © 2026 Wang Yue. All rights reserved."
    )

    target_link_options(MonitorQt PRIVATE -Wl,-dead_strip)

    add_custom_command(TARGET MonitorQt POST_BUILD
        COMMAND codesign --force --sign - $<TARGET_FILE:MonitorQt>
        COMMENT "Ad-hoc signing MonitorQt executable..."
    )
endif()


# ==============================================================================
# Include-What-You-Use (IWYU)
# ==============================================================================
find_program(IWYU_BIN NAMES include-what-you-use)
if(ENABLE_IWYU)
    if(IWYU_BIN)
        message(STATUS "Include-What-You-Use enabled for MonitorQt")
        set(IWYU_MAPPING_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/iwyu.imp")
        set(IWYU_OPTIONS
            ${IWYU_BIN}
            -Xiwyu --mapping_file=${IWYU_MAPPING_FILE}
            -Xiwyu --no_fwd_decls
            -Xiwyu --max_line_length=120
        )
        if(APPLE AND CMAKE_OSX_SYSROOT)
            list(APPEND IWYU_OPTIONS -isysroot ${CMAKE_OSX_SYSROOT})
        endif()
        set_target_properties(MonitorQt PROPERTIES
            CXX_INCLUDE_WHAT_YOU_USE "${IWYU_OPTIONS}"
        )
    else()
        message(WARNING "ENABLE_IWYU is ON, but 'include-what-you-use' executable was not found.")
    endif()
endif()

# Standalone targets to check or fix includes across the project
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-iwyu.sh")
    add_custom_target(
        iwyu-qt
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-iwyu.sh" --check
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Analyzing MonitorQt includes with Include-What-You-Use..."
        USES_TERMINAL
    )
    add_custom_target(
        iwyu-check-qt
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-iwyu.sh" --check
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Analyzing MonitorQt includes with Include-What-You-Use..."
        USES_TERMINAL
    )
    add_custom_target(
        iwyu-fix-qt
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-iwyu.sh" --fix
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Applying Include-What-You-Use include fixes automatically..."
        USES_TERMINAL
    )
endif()

# ==============================================================================
# Code Formatting (clang-format)
# ==============================================================================
find_program(CLANG_FORMAT_BIN NAMES clang-format)
if(CLANG_FORMAT_BIN)
    file(GLOB_RECURSE ALL_PROJECT_SOURCES "src/*.cpp" "src/*.h")
    add_custom_target(
        format-qt
        COMMAND ${CLANG_FORMAT_BIN} -i -style=file ${ALL_PROJECT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-format on MonitorQt source and header files..."
    )
    add_custom_target(
        format-check-qt
        COMMAND ${CLANG_FORMAT_BIN} --dry-run -Werror -style=file ${ALL_PROJECT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking MonitorQt code formatting with clang-format..."
    )
endif()

# ==============================================================================
# Installation (Linux / Unix)
# ==============================================================================
if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
    install(TARGETS MonitorQt
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/com.wangyue.monitorqt.desktop"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/icons/app.png"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
        RENAME com.wangyue.monitorqt.png
    )
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/icons/app.png"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps
        RENAME com.wangyue.monitorqt.png
    )

    # Post-install: Grant cap_sys_nice=ep capability for Real-Time priority scheduling (SCHED_FIFO)
    install(CODE "
        find_program(SETCAP_BIN setcap)
        if(SETCAP_BIN)
            set(TARGET_BIN \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/MonitorQt\")
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

    # ALSA Rate Notify plugin for Linux / snd-aloop rate change detection
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ALSA QUIET alsa)
        if(ALSA_FOUND)
            add_subdirectory(plugins/alsa_rate_notify)
        endif()
    endif()
endif()


