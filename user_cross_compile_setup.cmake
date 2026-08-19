# 项目目录结构：
# smart_doorbell
#   ——doorbell_rk3568
# rk356x-linux-25250919（DOORBELL_SDK_ROOT）
#   ——app
#     ——doorbell_rk3568（符号链接到smart_doorbell/doorbell_rk3568）

# 交叉编译：
# 在smart_doorbell/doorbell_rk3568目录下执行：
# cmake -S . -B build_arm64 -DDOORBELL_SDK_ROOT=/media/joeccmou/Data/rk356x-linux-25250919 -DCMAKE_TOOLCHAIN_FILE="$PWD/user_cross_compile_setup.cmake"
# cmake --build build_arm64 -j
# 或者在 rk356x-linux-25250919/app/doorbell_rk3568 目录下执行：
# cmake -S . -B build_arm64 -DCMAKE_TOOLCHAIN_FILE="$PWD/user_cross_compile_setup.cmake"
# cmake --build build_arm64 -j

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(tools /usr/bin)
set(CMAKE_C_COMPILER ${tools}/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER ${tools}/aarch64-linux-gnu-g++)

function(doorbell_toolchain_is_sdk_root out_var candidate)
    if(NOT candidate)
        set(${out_var} OFF PARENT_SCOPE)
        return()
    endif()

    get_filename_component(candidate_abs "${candidate}" ABSOLUTE)
    if(EXISTS "${candidate_abs}/external/rknpu2/runtime/RK356X/Linux/librknn_api"
       AND EXISTS "${candidate_abs}/debian/sysroots/debian11-aarch64")
        set(${out_var} ON PARENT_SCOPE)
        set(DOORBELL_SDK_ROOT "${candidate_abs}" CACHE PATH "Rockchip SDK root for doorbell_rk3568" FORCE)
    else()
        set(${out_var} OFF PARENT_SCOPE)
    endif()
endfunction()

set(DOORBELL_SDK_ROOT_CANDIDATES "")
if(DEFINED DOORBELL_SDK_ROOT AND NOT "${DOORBELL_SDK_ROOT}" STREQUAL "")
    list(APPEND DOORBELL_SDK_ROOT_CANDIDATES "${DOORBELL_SDK_ROOT}")
endif()
if(DEFINED ENV{DOORBELL_SDK_ROOT} AND NOT "$ENV{DOORBELL_SDK_ROOT}" STREQUAL "")
    list(APPEND DOORBELL_SDK_ROOT_CANDIDATES "$ENV{DOORBELL_SDK_ROOT}")
endif()
list(APPEND DOORBELL_SDK_ROOT_CANDIDATES
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_LIST_DIR}/../../rk356x-linux-25250919"
)

set(DOORBELL_SDK_ROOT_FOUND OFF)
foreach(candidate IN LISTS DOORBELL_SDK_ROOT_CANDIDATES)
    doorbell_toolchain_is_sdk_root(DOORBELL_SDK_ROOT_FOUND "${candidate}")
    if(DOORBELL_SDK_ROOT_FOUND)
        break()
    endif()
endforeach()

if(NOT DOORBELL_SDK_ROOT_FOUND)
    message(FATAL_ERROR
        "Could not locate Rockchip SDK root. Pass "
        "-DDOORBELL_SDK_ROOT=/path/to/rk356x-linux-25250919 or set the "
        "DOORBELL_SDK_ROOT environment variable."
    )
endif()

# If necessary, set STAGING_DIR
# if not work, please try(in shell command): export STAGING_DIR=/home/ubuntu/Your_SDK/out/xxx/openwrt/staging_dir/target
set(CMAKE_SYSROOT "${DOORBELL_SDK_ROOT}/debian/sysroots/debian11-aarch64")
set(ENV{STAGING_DIR} "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(ENV{PKG_CONFIG_SYSROOT_DIR} $ENV{STAGING_DIR})
set(ENV{PKG_CONFIG_LIBDIR} "$ENV{STAGING_DIR}/usr/lib/aarch64-linux-gnu/pkgconfig:$ENV{STAGING_DIR}/usr/lib/pkgconfig:$ENV{STAGING_DIR}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
