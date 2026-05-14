cmake_minimum_required(VERSION 3.10)
project(doorbell C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Debug)

# Allow linking to targets defined in subdirectories (lvgl) from this directory.
if(POLICY CMP0079)
    cmake_policy(SET CMP0079 NEW)
endif()


set(LV_BUILD_CONF_PATH ${CMAKE_SOURCE_DIR}/lv_conf.h CACHE PATH "" FORCE)

set(LV_BUILD_SET_CONFIG_OPTS ON CACHE BOOL
    "create CMAKE variables from lv_conf_internal.h" FORCE)

# 1) 引入 LVGL 核心
add_subdirectory(${CMAKE_SOURCE_DIR}/../../external/lvgl/9.4/lv_port_linux/lvgl
                 ${CMAKE_BINARY_DIR}/lvgl_build)

# 2) 选择并加入移植层源码（从 lv_port_linux/src/lib/… 复制或引用）
set(LV_LINUX_BACKEND_SRC "")

if (CONFIG_LV_USE_EVDEV)
    message("Including EVDEV support")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EVDEV REQUIRED libevdev)

    list(APPEND PKG_CONFIG_LIB ${EVDEV_LIBRARIES})
    list(APPEND PKG_CONFIG_INC ${EVDEV_INCLUDE_DIRS})    

endif()

if (CONFIG_LV_USE_SDL OR CONFIG_LV_USE_DRAW_SDL)

    message("Including SDL2 support")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)

    list(APPEND PKG_CONFIG_LIB ${SDL2_LIBRARIES} ${SDL2_IMAGE_LIBRARIES})
    list(APPEND PKG_CONFIG_INC ${SDL2_INCLUDE_DIRS} ${SDL2_IMAGE_INCLUDE_DIRS})

    # list(APPEND LV_LINUX_BACKEND_SRC ${CMAKE_SOURCE_DIR}/src/lvgl_port_linux/sdl.c)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-app-1.0)
list(APPEND PKG_CONFIG_LIB ${GSTREAMER_LIBRARIES})
list(APPEND PKG_CONFIG_INC ${GSTREAMER_INCLUDE_DIRS})

if (CONFIG_LV_USE_LINUX_DRM)

    message("Including DRM support")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBDRM REQUIRED libdrm)

    list(APPEND PKG_CONFIG_LIB ${LIBDRM_LIBRARIES})
    list(APPEND PKG_CONFIG_INC ${LIBDRM_INCLUDE_DIRS})
    if (TARGET lvgl)
        target_include_directories(lvgl PUBLIC ${LIBDRM_INCLUDE_DIRS})
        target_link_libraries(lvgl PUBLIC ${LIBDRM_LIBRARIES})
    endif()
    # list(APPEND LV_LINUX_BACKEND_SRC ${CMAKE_SOURCE_DIR}/src/lvgl_port_linux/drm.c)

endif()

add_executable(doorbell
    src/main.cpp
    src/camera.cpp
	src/ui/settings.cpp
    src/utils/file_utils.cpp
    src/utils/image_utils.cpp
    src/utils/image_drawing.c
    src/utils/perf_logger.cpp
    src/utils/mp4_recorder.cpp
    src/ai/yolo_person_detector.cpp
    ${LV_LINUX_BACKEND_SRC}
)

target_compile_definitions(doorbell PRIVATE DISABLE_LIBJPEG)

if (CONFIG_LV_USE_LINUX_DRM_GBM_BUFFERS OR CONFIG_LV_LINUX_DRM_USE_EGL)

    message("Including GBM support")

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBGBM REQUIRED gbm)

    list(APPEND PKG_CONFIG_LIB ${LIBGBM_LIBRARIES})
    list(APPEND PKG_CONFIG_INC ${LIBGBM_INCLUDE_DIRS})
    if (TARGET lvgl)
        target_include_directories(lvgl PUBLIC ${LIBGBM_INCLUDE_DIRS})
        target_link_libraries(lvgl PUBLIC ${LIBGBM_LIBRARIES})
    endif()

    # Build GBM/DRM compatibility shims only when GBM paths are enabled.
    target_sources(doorbell PRIVATE src/drm_compat_shim.c)

endif()

# RGA (optional, accelerates YUV->ARGB)
# Prefer the sysroot-provided librga (e.g., 2.1.0) instead of the bundled prebuilt.
set(DOORBELL_TARGET_ARCH OFF)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64" OR CMAKE_CROSSCOMPILING)
    set(DOORBELL_TARGET_ARCH ON)
endif()

set(RGA_SYSROOT "${CMAKE_SOURCE_DIR}/../../debian/sysroots/debian11-aarch64")
set(RGA_HINT_PATH "${RGA_SYSROOT}/usr/lib/aarch64-linux-gnu")
if (DOORBELL_TARGET_ARCH)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(RGA REQUIRED librga)

    if (RGA_FOUND)
        list(APPEND PKG_CONFIG_LIB ${RGA_LIBRARIES})
        list(APPEND PKG_CONFIG_INC ${RGA_INCLUDE_DIRS})
    else()
        find_path(RGA_INCLUDE_DIR NAMES im2d.h RgaApi.h
            PATHS
            ${RGA_SYSROOT}/usr/include/rga
        )
        find_library(RGA_LIB NAMES rga librga
            PATHS
            ${RGA_HINT_PATH}
        )
        if (TARGET doorbell AND RGA_INCLUDE_DIR)
            target_include_directories(doorbell PRIVATE ${RGA_INCLUDE_DIR})
        endif()
    endif()
else()
    target_compile_definitions(doorbell PRIVATE DOORBELL_DISABLE_RGA DISABLE_RGA)
    message(STATUS "RGA disabled on non-aarch64 host build")
endif()
                                                
set(RKNN_RUNTIME_ROOT ${CMAKE_SOURCE_DIR}/../../external/rknpu2/runtime/RK356X/Linux/librknn_api)
set(DOORBELL_USE_RKNN_YOLO OFF)
if (DOORBELL_TARGET_ARCH)
    set(DOORBELL_USE_RKNN_YOLO ON)
endif()

if (DOORBELL_USE_RKNN_YOLO)
    set(RKNN_INCLUDE_HINT "${RKNN_RUNTIME_ROOT}/include")
    set(RKNNRT_SO_HINT "${RKNN_RUNTIME_ROOT}/aarch64/librknnrt.so")

    find_path(RKNN_INCLUDE_DIR NAMES rknn_api.h
        PATHS ${RKNN_INCLUDE_HINT}
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
    )

    if (EXISTS "${RKNNRT_SO_HINT}")
        set(RKNNRT_LIB "${RKNNRT_SO_HINT}")
    else()
    find_library(RKNNRT_LIB NAMES rknnrt librknnrt
        PATHS ${RKNN_RUNTIME_ROOT}/aarch64
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
    )
    endif()

    target_sources(doorbell PRIVATE
        src/ai/yolov8/cpp/yolov8.cpp
        src/ai/yolov8/cpp/postprocess.cpp
    )
    target_compile_definitions(doorbell PRIVATE DOORBELL_USE_RKNN_YOLO)

    if (RKNN_INCLUDE_DIR)
        target_include_directories(doorbell PRIVATE ${RKNN_INCLUDE_DIR})
    endif()
    if (RKNNRT_LIB)
        message(STATUS "Using RKNN runtime: ${RKNNRT_LIB}")
        target_link_libraries(doorbell PRIVATE ${RKNNRT_LIB})
    else()
        message(WARNING "librknnrt.so not found, YOLOv8 inference cannot run")
    endif()
else()
    message(STATUS "RKNN YOLO disabled on non-aarch64 host build")
endif()
if (DOORBELL_TARGET_ARCH AND TARGET doorbell AND RGA_LIB)
    target_link_libraries(doorbell PRIVATE ${RGA_LIB})
elseif(DOORBELL_TARGET_ARCH AND NOT RGA_FOUND)
    message(WARNING "librga not found; build will fall back to CPU conversion")
endif()

# 3) 依赖库：根据你选的后端添加
target_link_libraries(doorbell
    PRIVATE
      lvgl                    # 来自 add_subdirectory
      m pthread
      ${PKG_CONFIG_LIB}
      ${LIBDRM_LIBRARIES}
)

# Link GBM only when enabled in config.
if (CONFIG_LV_USE_LINUX_DRM_GBM_BUFFERS OR CONFIG_LV_LINUX_DRM_USE_EGL)
    target_link_libraries(doorbell PRIVATE ${LIBGBM_LIBRARIES})
endif()

target_include_directories(doorbell
    PRIVATE
	${CMAKE_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/src/ai/yolov8/cpp
	${CMAKE_SOURCE_DIR}/src/lvgl_port_linux  
    ${CMAKE_SOURCE_DIR}/../../external/rknpu2/examples/3rdparty/stb
    ${CMAKE_SOURCE_DIR}/../../external/lvgl/9.4/lv_port_linux/lvgl
    ${PKG_CONFIG_INC}
)