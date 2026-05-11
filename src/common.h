#ifndef _RKNN_MODEL_ZOO_COMMON_H_
#define _RKNN_MODEL_ZOO_COMMON_H_

#include <stdint.h>

typedef enum {
    IMAGE_FORMAT_GRAY8 = 0,
    IMAGE_FORMAT_RGB888 = 1,
    IMAGE_FORMAT_RGBA8888 = 2,
    IMAGE_FORMAT_YUV420SP_NV12 = 3,
    IMAGE_FORMAT_YUV420SP_NV21 = 4,
    IMAGE_FORMAT_BGRA8888 = 5,
} image_format_t;

typedef struct {
    int left;
    int top;
    int right;
    int bottom;
} image_rect_t;

typedef struct {
    int width;
    int height;
    image_format_t format;
    int size;
    uint8_t *virt_addr;
    int fd;
} image_buffer_t;

#endif
