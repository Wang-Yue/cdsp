set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# MinGW-w64 Cross-compilers
set(CMAKE_C_COMPILER /opt/homebrew/bin/x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER /opt/homebrew/bin/x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER /opt/homebrew/bin/x86_64-w64-mingw32-windres)

# Target Windows sysroot & Qt prefixes
if(NOT WIN_QT_DIR)
    set(WIN_QT_DIR "/Users/wangyue/Qt6-Win/6.5.2/mingw_64")
endif()
if(NOT MINGW_SYSROOT)
    set(MINGW_SYSROOT "/opt/homebrew/Cellar/mingw-w64/14.0.0_3/toolchain-x86_64/x86_64-w64-mingw32")
endif()

set(CMAKE_FIND_ROOT_PATH "${WIN_QT_DIR}" "${MINGW_SYSROOT}")
set(CMAKE_PREFIX_PATH "${WIN_QT_DIR}/lib/cmake" "${WIN_QT_DIR}" "${MINGW_SYSROOT}")

# Search mode for libraries, headers, and programs
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(Qt6_DIR "${WIN_QT_DIR}/lib/cmake/Qt6")

# Host Qt6 tools for moc, uic, and rcc (running natively on macOS)
if(NOT QT_HOST_PATH)
    if(EXISTS "/Users/wangyue/Qt6.8.2/6.8.2/macos")
        set(QT_HOST_PATH "/Users/wangyue/Qt6.8.2/6.8.2/macos")
    elseif(EXISTS "/opt/homebrew/opt/qt6")
        set(QT_HOST_PATH "/opt/homebrew/opt/qt6")
    endif()
endif()

if(QT_HOST_PATH)
    if(EXISTS "${QT_HOST_PATH}/libexec/moc")
        set(QT_MOC_EXECUTABLE "${QT_HOST_PATH}/libexec/moc")
        set(CMAKE_AUTOMOC_EXECUTABLE "${QT_HOST_PATH}/libexec/moc")
    endif()
    if(EXISTS "${QT_HOST_PATH}/libexec/uic")
        set(QT_UIC_EXECUTABLE "${QT_HOST_PATH}/libexec/uic")
        set(CMAKE_AUTOUIC_EXECUTABLE "${QT_HOST_PATH}/libexec/uic")
    endif()
    if(EXISTS "${QT_HOST_PATH}/libexec/rcc")
        set(QT_RCC_EXECUTABLE "${QT_HOST_PATH}/libexec/rcc")
        set(CMAKE_AUTORCC_EXECUTABLE "${QT_HOST_PATH}/libexec/rcc")
    endif()
endif()
