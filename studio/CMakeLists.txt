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
elseif(WIN32)
    find_program(WINDEPLOYQT_BIN NAMES windeployqt.exe HINTS "${Qt6_DIR}/../../../bin")
    if(WINDEPLOYQT_BIN)
        add_custom_target(
            deploy-win
            COMMAND ${WINDEPLOYQT_BIN} $<TARGET_FILE:MonitorQt>
            DEPENDS MonitorQt
            COMMENT "Deploying Qt Windows DLLs alongside MonitorQt.exe..."
        )
    endif()
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
endif()

