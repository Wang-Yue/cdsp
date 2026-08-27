set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Search mode for libraries, headers, and programs
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# MinGW-w64 Cross-compilers
set(CMAKE_C_COMPILER /opt/homebrew/bin/x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER /opt/homebrew/bin/x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER /opt/homebrew/bin/x86_64-w64-mingw32-windres)

# Target Windows sysroot & Qt prefixes
set(MINGW_SYSROOT "/opt/homebrew/Cellar/mingw-w64/14.0.0_3/toolchain-x86_64/x86_64-w64-mingw32")
set(WIN_QT_DIR "${MINGW_SYSROOT}/qt6-static")
set(CMAKE_FIND_ROOT_PATH "${WIN_QT_DIR}" "${MINGW_SYSROOT}")
set(CMAKE_PREFIX_PATH "${WIN_QT_DIR}/lib/cmake" "${WIN_QT_DIR}" "${MINGW_SYSROOT}")
set(Qt6_DIR "${WIN_QT_DIR}/lib/cmake/Qt6")

# Host Qt6 tools for moc, uic, and rcc (running natively on macOS)
set(QT_HOST_PATH "/opt/homebrew/opt/qtbase/share/qt")
set(QT_MOC_EXECUTABLE "${QT_HOST_PATH}/libexec/moc")
set(CMAKE_AUTOMOC_EXECUTABLE "${QT_HOST_PATH}/libexec/moc")
set(QT_UIC_EXECUTABLE "${QT_HOST_PATH}/libexec/uic")
set(CMAKE_AUTOUIC_EXECUTABLE "${QT_HOST_PATH}/libexec/uic")
set(QT_RCC_EXECUTABLE "${QT_HOST_PATH}/libexec/rcc")
set(CMAKE_AUTORCC_EXECUTABLE "${QT_HOST_PATH}/libexec/rcc")

# Static library search suffixes & static linker flags
# Re-apply via CMAKE_PROJECT_INCLUDE so CMake's internal Platform/Windows-GNU.cmake does not reset suffixes to .dll.a
set(CMAKE_PROJECT_INCLUDE "${CMAKE_CURRENT_LIST_FILE}")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".lib")
set(CMAKE_C_FLAGS_INIT "-march=skylake -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "-march=skylake -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,-s")
set(CMAKE_CXX_STANDARD_LIBRARIES "-lgraphite2 -lharfbuzz -lrpcrt4 -llzma -ldeflate -ljbig -llerc -lzstd -lbrotlidec -lbrotlienc -lbrotlicommon -lbz2 -lwinpthread -lmsvcrt-os ${CMAKE_CXX_STANDARD_LIBRARIES}" CACHE STRING "" FORCE)
