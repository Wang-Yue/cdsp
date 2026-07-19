# CMake Toolchain file for cross-compiling Monitor-Qt from macOS to Raspberry Pi (Linux ARM64 / aarch64)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Set compilers (ARM64 Linux cross compilers)
if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER aarch64-unknown-linux-gnu-gcc)
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER aarch64-unknown-linux-gnu-g++)
endif()

# Host Qt for macOS (matches target Qt MOC/UIC/RCC tools)
get_filename_component(USER_HOME "~" REALPATH)
if(EXISTS "${USER_HOME}/Qt6-Mac/6.5.2/macos")
    set(QT_HOST_PATH "${USER_HOME}/Qt6-Mac/6.5.2/macos" CACHE PATH "Host Qt Path")
endif()

# Sysroot path (defaults to ~/rpi-sysroot if not set via -DCMAKE_SYSROOT=...)
if(NOT DEFINED CMAKE_SYSROOT)
    set(CMAKE_SYSROOT "${USER_HOME}/rpi-sysroot")
endif()

if(EXISTS "${CMAKE_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -isystem ${CMAKE_SYSROOT}/usr/include/aarch64-linux-gnu -isystem ${CMAKE_SYSROOT}/usr/include/dbus-1.0 -isystem ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/dbus-1.0/include -B${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu -B${CMAKE_SYSROOT}/usr/lib")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -isystem ${CMAKE_SYSROOT}/usr/include/aarch64-linux-gnu -isystem ${CMAKE_SYSROOT}/usr/include/dbus-1.0 -isystem ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/dbus-1.0/include -B${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu -B${CMAKE_SYSROOT}/usr/lib")

    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath-link,${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu:${CMAKE_SYSROOT}/lib/aarch64-linux-gnu -Wl,--allow-shlib-undefined")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-rpath-link,${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu:${CMAKE_SYSROOT}/lib/aarch64-linux-gnu -Wl,--allow-shlib-undefined")

    list(APPEND CMAKE_PREFIX_PATH
        "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/cmake"
        "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/cmake/Qt6"
    )

    set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
endif()
