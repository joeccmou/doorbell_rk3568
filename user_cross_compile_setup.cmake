# Usage:
# cmake -DCMAKE_TOOLCHAIN_FILE=./user_cross_compile_setup.cmake -B build -S .
# make  -C build -j  或 cmake --build build -j

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(tools /usr/bin)
set(CMAKE_C_COMPILER ${tools}/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER ${tools}/aarch64-linux-gnu-g++)

# If necessary, set STAGING_DIR
# if not work, please try(in shell command): export STAGING_DIR=/home/ubuntu/Your_SDK/out/xxx/openwrt/staging_dir/target
set(CMAKE_SYSROOT "/media/joeccmou/Data/rk356x-linux-25250919/debian/sysroots/debian11-aarch64")
set(ENV{STAGING_DIR} "/media/joeccmou/Data/rk356x-linux-25250919/debian/sysroots/debian11-aarch64")
set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(ENV{PKG_CONFIG_SYSROOT_DIR} $ENV{STAGING_DIR})
set(ENV{PKG_CONFIG_LIBDIR} "$ENV{STAGING_DIR}/usr/lib/aarch64-linux-gnu/pkgconfig:$ENV{STAGING_DIR}/usr/lib/pkgconfig:$ENV{STAGING_DIR}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")


