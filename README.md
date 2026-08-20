
若在ubuntu22.04以上系统编译本工程时可根据下面步骤进行，因为ubuntu22.04以上的glibc版本较高，和RK3568 SDK不兼容，所以在ubuntu22.04编译本工程时使用了debian11基础镜像
若在ubuntu20.04编译本工程则只需遵照user_cross_compile_setup.cmake的说明编译

第1步：创建docker镜像作为编译环境

（在项目根目录下执行）
docker build \
  --build-arg BUILD_UID="$(id -u)" \
  --build-arg BUILD_GID="$(id -g)" \
  --build-arg BUILD_USER="$(id -un)" \
  -t smart-doorbell-build:debian11-gcc10 \
  docker/debian11-cross

第2步：创建并进入容器

PROJECT_ROOT=/media/joeccmou/Data/smart_doorbell
SDK_ROOT=/media/joeccmou/Data/rk356x-linux-25250919
LVGL_SOURCE_ROOT=/home/joeccmou/Solution/CommonLibrary/lvgl/9.4/lvgl

docker run -it \
  --name doorbell-debian11-builder \
  --hostname debian11 \
  -v "${PROJECT_ROOT}:${PROJECT_ROOT}" \
  -v "${SDK_ROOT}:${SDK_ROOT}:ro" \
  -v "${LVGL_SOURCE_ROOT}:${LVGL_SOURCE_ROOT}:ro" \
  -w "${PROJECT_ROOT}/doorbell_rk3568" \
  smart-doorbell-build:debian11-gcc10 \
  bash

或 

docker start -ai doorbell-debian11-builder

第3步：执行 CMake 配置

cmake -S . -B build_arm64 \
  -DDOORBELL_SDK_ROOT=/media/joeccmou/Data/rk356x-linux-25250919 \
  -DDOORBELL_LVGL_ROOT=/home/joeccmou/Solution/CommonLibrary/lvgl/9.4/lvgl \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/user_cross_compile_setup.cmake"

第4步：编译和链接

cmake --build build_arm64 -j"$(nproc)"

（第1步创建dokcer镜像和第2步创建容器只要第一次构建时执行，后续可以直接启动创建好的容器（docker start -ai doorbell-debian11-builder）直接走第3步和第4步）
