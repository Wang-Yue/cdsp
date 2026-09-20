# CMake toolchain file for cross-compiling to Windows 64-bit using MinGW-w64
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Search mode for libraries, headers, and programs
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# MinGW-w64 Cross-compilers
find_program(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc PATHS /opt/homebrew/bin /usr/local/bin)
find_program(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++ PATHS /opt/homebrew/bin /usr/local/bin)
find_program(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres PATHS /opt/homebrew/bin /usr/local/bin)

# Target Windows sysroot & Qt prefixes
if(NOT MINGW_SYSROOT)
    if(EXISTS "/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32")
        set(MINGW_SYSROOT "/opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32")
    elseif(EXISTS "/opt/homebrew/Cellar/mingw-w64")
        file(GLOB MINGW_CELLAR_DIRS "/opt/homebrew/Cellar/mingw-w64/*/toolchain-x86_64/x86_64-w64-mingw32")
        list(GET MINGW_CELLAR_DIRS 0 MINGW_SYSROOT)
    endif()
endif()

if(EXISTS "${MINGW_SYSROOT}/qt6-static")
    set(WIN_QT_DIR "${MINGW_SYSROOT}/qt6-static")
    set(Qt6_DIR "${WIN_QT_DIR}/lib/cmake/Qt6")
    set(CMAKE_PREFIX_PATH "${WIN_QT_DIR}/lib/cmake" "${WIN_QT_DIR}" "${MINGW_SYSROOT}")
endif()

set(CMAKE_FIND_ROOT_PATH
    ${WIN_QT_DIR}
    ${MINGW_SYSROOT}
    /opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32
    /opt/homebrew/opt/mingw-w64/toolchain-x86_64
)

# Host Qt6 tools for moc, uic, and rcc (running natively on macOS host)
if(NOT QT_HOST_PATH)
    if(EXISTS "/opt/homebrew/opt/qtbase/share/qt")
        set(QT_HOST_PATH "/opt/homebrew/opt/qtbase/share/qt")
    elseif(EXISTS "/opt/homebrew/opt/qt@6/share/qt")
        set(QT_HOST_PATH "/opt/homebrew/opt/qt@6/share/qt")
    elseif(EXISTS "/opt/homebrew/opt/qt/share/qt")
        set(QT_HOST_PATH "/opt/homebrew/opt/qt/share/qt")
    endif()
endif()

if(QT_HOST_PATH)
    set(QT_MOC_EXECUTABLE "${QT_HOST_PATH}/libexec/moc")
    set(CMAKE_AUTOMOC_EXECUTABLE "${QT_HOST_PATH}/libexec/moc")
    set(QT_UIC_EXECUTABLE "${QT_HOST_PATH}/libexec/uic")
    set(CMAKE_AUTOUIC_EXECUTABLE "${QT_HOST_PATH}/libexec/uic")
    set(QT_RCC_EXECUTABLE "${QT_HOST_PATH}/libexec/rcc")
    set(CMAKE_AUTORCC_EXECUTABLE "${QT_HOST_PATH}/libexec/rcc")
endif()

# Static library search suffixes & static linker flags
set(CMAKE_PROJECT_INCLUDE "${CMAKE_CURRENT_LIST_FILE}")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".lib")
set(CMAKE_C_FLAGS_INIT "-ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "-ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,-s")
set(CMAKE_CXX_STANDARD_LIBRARIES "-lgraphite2 -lharfbuzz -lrpcrt4 -llzma -ldeflate -ljbig -llerc -lzstd -lbrotlidec -lbrotlienc -lbrotlicommon -lbz2 -lwinpthread -lmsvcrt-os ${CMAKE_CXX_STANDARD_LIBRARIES}" CACHE STRING "" FORCE)
