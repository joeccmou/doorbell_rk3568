#include "events/snapshot_service.h"

#include <cstring>
#include <filesystem>
#include <sstream>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

namespace {
bool has_factory(const char *name) {
    GstElementFactory *factory = gst_element_factory_find(name);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
}

std::string select_encoder_chain() {
    if (has_factory("jpegenc")) {
        return "videoconvert ! jpegenc quality=85";
    }
    if (has_factory("mppjpegenc")) {
        return "videoconvert ! video/x-raw,format=NV12 ! mppjpegenc";
    }
    if (has_factory("avenc_mjpeg")) {
        return "videoconvert ! avenc_mjpeg";
    }
    return "";
}

std::string bus_error_message(GstMessage *message) {
    if (!message || GST_MESSAGE_TYPE(message) != GST_MESSAGE_ERROR) return "";
    GError *gst_error = nullptr;
    gchar *debug = nullptr;
    gst_message_parse_error(message, &gst_error, &debug);
    std::ostringstream out;
    out << (gst_error ? gst_error->message : "unknown GStreamer error");
    if (debug && debug[0] != '\0') out << " (" << debug << ')';
    if (gst_error) g_error_free(gst_error);
    if (debug) g_free(debug);
    return out.str();
}
}

bool SnapshotService::save_rgb_jpeg(const std::string &output_path,
                                    const std::vector<uint8_t> &rgb,
                                    uint32_t width,
                                    uint32_t height,
                                    std::string *error) const {
    const size_t expected = static_cast<size_t>(width) * height * 3;
    if (output_path.empty() || width == 0 || height == 0 || rgb.size() < expected) {
        if (error) *error = "invalid RGB snapshot input";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(output_path).parent_path(), ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    if (!gst_is_initialized()) gst_init(nullptr, nullptr);

    const std::string encoder_chain = select_encoder_chain();
    if (encoder_chain.empty()) {
        if (error) *error = "no JPEG encoder found (jpegenc/mppjpegenc/avenc_mjpeg)";
        return false;
    }

    gchar *description = g_strdup_printf(
        "appsrc name=src is-live=false block=true format=time "
        "caps=video/x-raw,format=RGB,width=%u,height=%u,framerate=1/1 "
        "! %s ! filesink name=snapshot_sink sync=false async=false",
        width, height, encoder_chain.c_str());

    GError *parse_error = nullptr;
    GstElement *pipeline = gst_parse_launch(description, &parse_error);
    g_free(description);
    if (parse_error || !pipeline) {
        if (error) *error = parse_error ? parse_error->message : "create JPEG pipeline failed";
        if (parse_error) g_error_free(parse_error);
        if (pipeline) gst_object_unref(pipeline);
        return false;
    }

    GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "snapshot_sink");
    GstBus *bus = gst_element_get_bus(pipeline);
    if (!source || !sink || !bus) {
        if (error) {
            if (!source) {
                *error = "JPEG pipeline appsrc is missing";
            } else if (!sink) {
                *error = "JPEG pipeline filesink is missing";
            } else {
                *error = "JPEG pipeline bus is missing";
            }
        }
        if (source) gst_object_unref(source);
        if (sink) gst_object_unref(sink);
        if (bus) gst_object_unref(bus);
        gst_object_unref(pipeline);
        return false;
    }
    g_object_set(G_OBJECT(sink), "location", output_path.c_str(), nullptr);

    bool ok = true;
    const GstStateChangeReturn state_result = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (state_result == GST_STATE_CHANGE_FAILURE) {
        GstMessage *message = gst_bus_timed_pop_filtered(bus, GST_SECOND, GST_MESSAGE_ERROR);
        if (error) {
            const std::string detail = bus_error_message(message);
            *error = detail.empty() ? "JPEG pipeline failed to enter PLAYING" : detail;
        }
        if (message) gst_message_unref(message);
        ok = false;
    }

    GstBuffer *buffer = nullptr;
    if (ok) {
        buffer = gst_buffer_new_allocate(nullptr, expected, nullptr);
        if (!buffer) {
            if (error) *error = "allocate JPEG input buffer failed";
            ok = false;
        }
    }
    if (ok) {
        GstMapInfo map{};
        if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            if (error) *error = "map JPEG input buffer failed";
            gst_buffer_unref(buffer);
            buffer = nullptr;
            ok = false;
        } else {
            std::memcpy(map.data, rgb.data(), expected);
            gst_buffer_unmap(buffer, &map);
            GST_BUFFER_PTS(buffer) = 0;
            GST_BUFFER_DTS(buffer) = 0;
            GST_BUFFER_DURATION(buffer) = GST_SECOND;
        }
    }
    if (ok) {
        const GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
        buffer = nullptr; // push_buffer 无论成功失败都会接管 buffer。
        if (flow != GST_FLOW_OK) {
            if (error) *error = "push JPEG input failed, flow=" + std::to_string(static_cast<int>(flow));
            ok = false;
        }
    }
    if (ok) {
        const GstFlowReturn eos = gst_app_src_end_of_stream(GST_APP_SRC(source));
        if (eos != GST_FLOW_OK) {
            if (error) *error = "finish JPEG input failed, flow=" + std::to_string(static_cast<int>(eos));
            ok = false;
        }
    }
    if (ok) {
        GstMessage *message = gst_bus_timed_pop_filtered(
            bus, 5 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (!message) {
            if (error) *error = "JPEG pipeline timed out";
            ok = false;
        } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            if (error) *error = bus_error_message(message);
            ok = false;
        }
        if (message) gst_message_unref(message);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(source);
    gst_object_unref(sink);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    if (!ok) {
        std::filesystem::remove(output_path, ec);
    }
    return ok;
}
