# CMake toolchain file for cross-compiling to Windows 64-bit using MinGW-w64
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Cross-compilers
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Target environment sysroot
set(CMAKE_FIND_ROOT_PATH
    /opt/homebrew/opt/mingw-w64/toolchain-x86_64/x86_64-w64-mingw32
    /opt/homebrew/opt/mingw-w64/toolchain-x86_64
)

# Search rules
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
