#include "web_frame_sink.h"

#include "util/log.h"

#define DOWNCAST(SINK) container_of(SINK, struct sc_web_frame_sink, frame_sink)

static bool
sc_web_frame_sink_open(struct sc_frame_sink *sink, const AVCodecContext *ctx) {
    struct sc_web_frame_sink *wfs = DOWNCAST(sink);

    sc_mutex_lock(&wfs->mutex);
    wfs->frame_width = ctx->width;
    wfs->frame_height = ctx->height;
    sc_mutex_unlock(&wfs->mutex);

    return true;
}

static void
sc_web_frame_sink_close(struct sc_frame_sink *sink) {
    (void) sink;
}

static bool
sc_web_frame_sink_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_web_frame_sink *wfs = DOWNCAST(sink);

    bool previous_skipped;
    bool ok = sc_frame_buffer_push(&wfs->fb, frame, &previous_skipped);
    if (!ok) {
        return false;
    }

    sc_mutex_lock(&wfs->mutex);
    wfs->has_frame = true;
    // The size changes on device rotation without the sink being reopened
    wfs->frame_width = frame->width;
    wfs->frame_height = frame->height;
    sc_mutex_unlock(&wfs->mutex);

    return true;
}

bool
sc_web_frame_sink_init(struct sc_web_frame_sink *wfs) {
    if (!sc_frame_buffer_init(&wfs->fb)) {
        return false;
    }

    if (!sc_mutex_init(&wfs->mutex)) {
        sc_frame_buffer_destroy(&wfs->fb);
        return false;
    }

    wfs->has_frame = false;
    wfs->frame_width = 0;
    wfs->frame_height = 0;

    static const struct sc_frame_sink_ops ops = {
        .open = sc_web_frame_sink_open,
        .close = sc_web_frame_sink_close,
        .push = sc_web_frame_sink_push,
    };

    wfs->frame_sink.ops = &ops;

    return true;
}

void
sc_web_frame_sink_destroy(struct sc_web_frame_sink *wfs) {
    sc_mutex_destroy(&wfs->mutex);
    sc_frame_buffer_destroy(&wfs->fb);
}

bool
sc_web_frame_sink_consume(struct sc_web_frame_sink *wfs, AVFrame *dst) {
    sc_mutex_lock(&wfs->mutex);
    bool has = wfs->has_frame;
    sc_mutex_unlock(&wfs->mutex);

    if (!has) {
        return false;
    }

    sc_frame_buffer_consume(&wfs->fb, dst);
    return true;
}

void
sc_web_frame_sink_get_size(struct sc_web_frame_sink *wfs,
                           uint16_t *width, uint16_t *height) {
    sc_mutex_lock(&wfs->mutex);
    *width = wfs->frame_width;
    *height = wfs->frame_height;
    sc_mutex_unlock(&wfs->mutex);
}
