#ifndef SC_AI_FRAME_SINK_H
#define SC_AI_FRAME_SINK_H

#include "common.h"

#include <stdbool.h>
#include <libavcodec/avcodec.h>

#include "frame_buffer.h"
#include "trait/frame_sink.h"
#include "util/thread.h"

struct sc_ai_frame_sink {
    struct sc_frame_sink frame_sink; // frame sink trait

    struct sc_frame_buffer fb;

    sc_mutex mutex;
    bool has_frame;
    uint16_t frame_width;
    uint16_t frame_height;
};

bool
sc_ai_frame_sink_init(struct sc_ai_frame_sink *afs);

void
sc_ai_frame_sink_destroy(struct sc_ai_frame_sink *afs);

// Consume the latest frame. The caller must call av_frame_unref() on dst
// when done. Returns false if no frame is available.
bool
sc_ai_frame_sink_consume(struct sc_ai_frame_sink *afs, AVFrame *dst);

#endif
