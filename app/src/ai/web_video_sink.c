#include "web_video_sink.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

#include "util/log.h"

static bool
sc_web_video_sink_open(struct sc_packet_sink *ps, AVCodecContext *ctx) {
    struct sc_web_video_sink *sink =
        container_of(ps, struct sc_web_video_sink, packet_sink);

    sc_mutex_lock(&sink->mutex);
    sink->video_width = ctx->width;
    sink->video_height = ctx->height;
    sink->opened = true;
    sc_mutex_unlock(&sink->mutex);

    LOGI("Web video sink opened: %dx%d", ctx->width, ctx->height);
    return true;
}

static void
sc_web_video_sink_close(struct sc_packet_sink *ps) {
    struct sc_web_video_sink *sink =
        container_of(ps, struct sc_web_video_sink, packet_sink);

    sc_mutex_lock(&sink->mutex);
    sink->stopped = true;
    sc_mutex_unlock(&sink->mutex);

    LOGD("Web video sink closed");
}

// Check if data starts with Annex-B start code
static inline bool
has_start_code(const uint8_t *data, size_t size) {
    if (size >= 4 && data[0] == 0 && data[1] == 0
            && data[2] == 0 && data[3] == 1) {
        return true;
    }
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        return true;
    }
    return false;
}

// Ensure all NAL units in the packet have Annex-B start codes.
// Returns a newly allocated buffer (caller must free), or NULL on failure.
static uint8_t *
ensure_annexb(const uint8_t *data, size_t size,
              const uint8_t *config, size_t config_size,
              size_t *out_size) {
    if (size == 0) {
        *out_size = 0;
        return NULL;
    }

    // If packet starts with cached config (merged by packet_merger),
    // strip the config prefix and prepend a start code to the frame data.
    // The config is sent separately on WebSocket connect.
    if (config && config_size > 0 && size > config_size
            && memcmp(data, config, config_size) == 0) {
        const uint8_t *frame = data + config_size;
        size_t frame_size = size - config_size;
        // Frame portion might already have a start code
        if (has_start_code(frame, frame_size)) {
            // Config + already-Annex-B frame: just keep config + frame as-is
            uint8_t *copy = malloc(size);
            if (!copy) { *out_size = 0; return NULL; }
            memcpy(copy, data, size);
            *out_size = size;
            return copy;
        }
        // Prepend start code to frame, keep config prefix
        uint8_t *out = malloc(config_size + 4 + frame_size);
        if (!out) { *out_size = 0; return NULL; }
        memcpy(out, config, config_size);
        out[config_size] = 0;
        out[config_size + 1] = 0;
        out[config_size + 2] = 0;
        out[config_size + 3] = 1;
        memcpy(out + config_size + 4, frame, frame_size);
        *out_size = config_size + 4 + frame_size;
        return out;
    }

    // Already Annex-B: copy as-is
    if (has_start_code(data, size)) {
        uint8_t *copy = malloc(size);
        if (!copy) { *out_size = 0; return NULL; }
        memcpy(copy, data, size);
        *out_size = size;
        return copy;
    }

    // Raw NAL unit without start code: prepend one
    uint8_t *out = malloc(4 + size);
    if (!out) { *out_size = 0; return NULL; }
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 1;
    memcpy(out + 4, data, size);
    *out_size = 4 + size;
    return out;
}

static bool
sc_web_video_sink_push(struct sc_packet_sink *ps, const AVPacket *packet) {
    struct sc_web_video_sink *sink =
        container_of(ps, struct sc_web_video_sink, packet_sink);

    bool is_config = packet->pts == AV_NOPTS_VALUE;
    bool is_key = (packet->flags & AV_PKT_FLAG_KEY) != 0;

    sc_mutex_lock(&sink->mutex);

    // Cache config (SPS/PPS)
    if (is_config) {
        free(sink->config_data);
        sink->config_data = malloc(packet->size);
        if (sink->config_data) {
            memcpy(sink->config_data, packet->data, packet->size);
            sink->config_size = packet->size;
        } else {
            sink->config_size = 0;
        }
    }

    // Convert to Annex-B format for browser jmuxer
    size_t out_size;
    uint8_t *copy = ensure_annexb(packet->data, packet->size,
                                  sink->config_data, sink->config_size,
                                  &out_size);
    if (!copy) {
        sc_mutex_unlock(&sink->mutex);
        return true; // non-fatal, just drop
    }


    // If queue is full, drop oldest
    if (sink->queue_count == SC_WEB_VIDEO_SINK_QUEUE_SIZE) {
        free(sink->queue[sink->queue_head].data);
        sink->queue_head =
            (sink->queue_head + 1) % SC_WEB_VIDEO_SINK_QUEUE_SIZE;
        sink->queue_count--;
    }

    struct sc_web_video_sink_packet *entry = &sink->queue[sink->queue_tail];
    entry->data = copy;
    entry->size = out_size;
    entry->is_config = is_config;
    entry->is_key = is_key;
    sink->queue_tail = (sink->queue_tail + 1) % SC_WEB_VIDEO_SINK_QUEUE_SIZE;
    sink->queue_count++;

    sc_mutex_unlock(&sink->mutex);

    return true;
}

static void
sc_web_video_sink_disable(struct sc_packet_sink *ps) {
    (void) ps;
}

bool
sc_web_video_sink_init(struct sc_web_video_sink *sink) {
    bool ok = sc_mutex_init(&sink->mutex);
    if (!ok) {
        return false;
    }

    sink->config_data = NULL;
    sink->config_size = 0;
    sink->queue_head = 0;
    sink->queue_tail = 0;
    sink->queue_count = 0;
    sink->video_width = 0;
    sink->video_height = 0;
    sink->opened = false;
    sink->stopped = false;

    static const struct sc_packet_sink_ops ops = {
        .open = sc_web_video_sink_open,
        .close = sc_web_video_sink_close,
        .push = sc_web_video_sink_push,
        .disable = sc_web_video_sink_disable,
    };

    sink->packet_sink.ops = &ops;

    return true;
}

unsigned
sc_web_video_sink_drain(struct sc_web_video_sink *sink,
                        struct sc_web_video_sink_packet *out,
                        unsigned max) {
    sc_mutex_lock(&sink->mutex);

    unsigned count = 0;
    while (sink->queue_count > 0 && count < max) {
        out[count] = sink->queue[sink->queue_head];
        sink->queue_head =
            (sink->queue_head + 1) % SC_WEB_VIDEO_SINK_QUEUE_SIZE;
        sink->queue_count--;
        count++;
    }

    sc_mutex_unlock(&sink->mutex);
    return count;
}

bool
sc_web_video_sink_get_config(struct sc_web_video_sink *sink,
                             uint8_t **data, size_t *size) {
    sc_mutex_lock(&sink->mutex);

    if (!sink->config_data || sink->config_size == 0) {
        sc_mutex_unlock(&sink->mutex);
        *data = NULL;
        *size = 0;
        return false;
    }

    *data = malloc(sink->config_size);
    if (!*data) {
        sc_mutex_unlock(&sink->mutex);
        *size = 0;
        return false;
    }

    memcpy(*data, sink->config_data, sink->config_size);
    *size = sink->config_size;

    sc_mutex_unlock(&sink->mutex);
    return true;
}

void
sc_web_video_sink_get_size(struct sc_web_video_sink *sink,
                           uint16_t *width, uint16_t *height) {
    sc_mutex_lock(&sink->mutex);
    *width = sink->video_width;
    *height = sink->video_height;
    sc_mutex_unlock(&sink->mutex);
}

void
sc_web_video_sink_stop(struct sc_web_video_sink *sink) {
    sc_mutex_lock(&sink->mutex);
    sink->stopped = true;
    sc_mutex_unlock(&sink->mutex);
}

void
sc_web_video_sink_destroy(struct sc_web_video_sink *sink) {
    // Free any remaining queued packets
    while (sink->queue_count > 0) {
        free(sink->queue[sink->queue_head].data);
        sink->queue_head =
            (sink->queue_head + 1) % SC_WEB_VIDEO_SINK_QUEUE_SIZE;
        sink->queue_count--;
    }

    free(sink->config_data);
    sc_mutex_destroy(&sink->mutex);
}
