#ifndef SC_WEB_FRAME_SINK_H
#define SC_WEB_FRAME_SINK_H

#include "common.h"

#include <stdbool.h>
#include <libavcodec/avcodec.h>

#include "frame_buffer.h"
#include "trait/frame_sink.h"
#include "util/thread.h"

struct sc_web_frame_sink {
    struct sc_frame_sink frame_sink; // frame sink trait

    struct sc_frame_buffer fb;

    sc_mutex mutex;
    bool has_frame;
    uint16_t frame_width;
    uint16_t frame_height;
};

bool
sc_web_frame_sink_init(struct sc_web_frame_sink *wfs);

void
sc_web_frame_sink_destroy(struct sc_web_frame_sink *wfs);

// Consume the latest frame. The caller must call av_frame_unref() on dst
// when done. Returns false if no frame is available.
bool
sc_web_frame_sink_consume(struct sc_web_frame_sink *wfs, AVFrame *dst);

// Size of the latest decoded frame (follows device rotation)
void
sc_web_frame_sink_get_size(struct sc_web_frame_sink *wfs,
                           uint16_t *width, uint16_t *height);

#endif
