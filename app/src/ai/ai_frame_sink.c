#include "ai_frame_sink.h"

#include "util/log.h"

#define DOWNCAST(SINK) container_of(SINK, struct sc_ai_frame_sink, frame_sink)

static bool
sc_ai_frame_sink_open(struct sc_frame_sink *sink, const AVCodecContext *ctx) {
    struct sc_ai_frame_sink *afs = DOWNCAST(sink);

    sc_mutex_lock(&afs->mutex);
    afs->frame_width = ctx->width;
    afs->frame_height = ctx->height;
    sc_mutex_unlock(&afs->mutex);

    return true;
}

static void
sc_ai_frame_sink_close(struct sc_frame_sink *sink) {
    (void) sink;
}

static bool
sc_ai_frame_sink_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_ai_frame_sink *afs = DOWNCAST(sink);

    bool previous_skipped;
    bool ok = sc_frame_buffer_push(&afs->fb, frame, &previous_skipped);
    if (!ok) {
        return false;
    }

    sc_mutex_lock(&afs->mutex);
    afs->has_frame = true;
    sc_mutex_unlock(&afs->mutex);

    return true;
}

bool
sc_ai_frame_sink_init(struct sc_ai_frame_sink *afs) {
    if (!sc_frame_buffer_init(&afs->fb)) {
        return false;
    }

    if (!sc_mutex_init(&afs->mutex)) {
        sc_frame_buffer_destroy(&afs->fb);
        return false;
    }

    afs->has_frame = false;
    afs->frame_width = 0;
    afs->frame_height = 0;

    static const struct sc_frame_sink_ops ops = {
        .open = sc_ai_frame_sink_open,
        .close = sc_ai_frame_sink_close,
        .push = sc_ai_frame_sink_push,
    };

    afs->frame_sink.ops = &ops;

    return true;
}

void
sc_ai_frame_sink_destroy(struct sc_ai_frame_sink *afs) {
    sc_mutex_destroy(&afs->mutex);
    sc_frame_buffer_destroy(&afs->fb);
}

bool
sc_ai_frame_sink_consume(struct sc_ai_frame_sink *afs, AVFrame *dst) {
    sc_mutex_lock(&afs->mutex);
    bool has = afs->has_frame;
    sc_mutex_unlock(&afs->mutex);

    if (!has) {
        return false;
    }

    sc_frame_buffer_consume(&afs->fb, dst);
    return true;
}
