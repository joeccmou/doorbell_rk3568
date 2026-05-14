#include "yolo_person_detector.h"

#include <cstdio>

#ifdef DOORBELL_USE_RKNN_YOLO
#include <algorithm>
#include <cstring>

#include "utils/image_utils.h"
#endif

YoloPersonDetector::~YoloPersonDetector() {
#ifdef DOORBELL_USE_RKNN_YOLO
    if (ready_) {
        release_yolov8_model(&app_ctx_);
    }
#endif
}

bool YoloPersonDetector::load(const std::string &model_path) {
#ifdef DOORBELL_USE_RKNN_YOLO
	int ret;
	int class_num = 0;

	if (ready_) {
        release_yolov8_model(&app_ctx_);
        std::memset(&app_ctx_, 0, sizeof(app_ctx_));
        ready_ = false;
    }

    if (model_path.empty()) {
        std::fprintf(stderr, "[yolo] model path empty, detection disabled\n");
        return false;
    }

    if (init_yolov8_model(model_path.c_str(), &app_ctx_) != 0) {
        std::fprintf(stderr, "[yolo] init model failed: %s\n", model_path.c_str());
        return false;
    }

	if (app_ctx_.io_num.n_output >= 2)
    {
#if defined(RV1106_1103)
        class_num = app_ctx_.output_attrs[1].dims[3];
#else
        class_num = app_ctx_.output_attrs[1].dims[1];
#endif
    }

    ret = init_post_process(class_num);
    if (ret != 0)
    {
        std::fprintf(stderr, "init_post_process fail! ret=%d class_num=%d\n", ret, class_num);
        
		deinit_post_process();

		ret = release_yolov8_model(&app_ctx_);
		if (ret != 0)
		{
			std::fprintf(stderr, "release_yolov8_model fail! ret=%d\n", ret);			
		}
		return false;
    }

    ready_ = true;
    std::fprintf(stdout, "[yolo] model loaded: %s\n", model_path.c_str());
    return true;
#else
    (void)model_path;
    ready_ = false;
    std::fprintf(stderr, "[yolo] RKNN YOLO disabled in this build\n");
    return false;
#endif
}

bool YoloPersonDetector::detect_person(const uint8_t *rgb, uint32_t width, uint32_t height,
                                       std::vector<PersonBox> &boxes) {
#ifdef DOORBELL_USE_RKNN_YOLO
    boxes.clear();
    if (!ready_ || !rgb || width == 0 || height == 0) return false;

    const size_t rgb_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;

    image_buffer_t img{};
    img.width = static_cast<int>(width);
    img.height = static_cast<int>(height);
    img.format = IMAGE_FORMAT_RGB888;
    img.size = static_cast<int>(rgb_size);
    img.virt_addr = const_cast<uint8_t *>(rgb);
    img.fd = -1;

    object_detect_result_list results{};
    if (inference_yolov8_model(&app_ctx_, &img, &results) != 0) {
        return false;
    }

    for (int i = 0; i < results.count; ++i) {
        const auto &r = results.results[i];
        if (r.cls_id != 0) {
            continue;
        }
        PersonBox b;
        b.left = std::clamp(r.box.left, 0, static_cast<int>(width) - 1);
        b.top = std::clamp(r.box.top, 0, static_cast<int>(height) - 1);
        b.right = std::clamp(r.box.right, 0, static_cast<int>(width) - 1);
        b.bottom = std::clamp(r.box.bottom, 0, static_cast<int>(height) - 1);
        b.score = r.prop;
        if (b.right > b.left && b.bottom > b.top) {
            boxes.push_back(b);
        }
    }

    return !boxes.empty();
#else
    (void)rgb;
    (void)width;
    (void)height;
    boxes.clear();
    return false;
#endif
}
