// Compatibility shims for older libgbm/libdrm that lack gbm_bo_get_fd_for_plane
// and drmCloseBufferHandle. Only built when GBM is enabled in lv_conf.h.

#include "lv_conf.h"

#if LV_USE_LINUX_DRM_GBM_BUFFERS || LV_LINUX_DRM_USE_EGL

#include <errno.h>
#include <stdint.h>
#include <xf86drm.h>
#include <gbm.h>

#ifndef HAVE_GBM_BO_GET_FD_FOR_PLANE
int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane) {
    if (!bo || plane != 0) {
        errno = ENOSYS;
        return -1;
    }

    // Fallback: export handle 0 via PRIME
    union gbm_bo_handle h = gbm_bo_get_handle(bo);
    if (!h.u32) {
        errno = ENOSYS;
        return -1;
    }

    int drm_fd = gbm_device_get_fd(gbm_bo_get_device(bo));
    if (drm_fd < 0) {
        errno = ENOSYS;
        return -1;
    }

    int dma_fd = -1;
    if (drmPrimeHandleToFD(drm_fd, h.u32, DRM_CLOEXEC, &dma_fd) != 0) {
        return -1;
    }
    return dma_fd;
}
#endif

#ifndef HAVE_DRM_CLOSE_BUFFER_HANDLE
int drmCloseBufferHandle(int fd, uint32_t handle) {
    struct drm_gem_close arg;
    arg.handle = handle;
    return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &arg);
}
#endif

#endif // LV_USE_LINUX_DRM_GBM_BUFFERS || LV_LINUX_DRM_USE_EGL
