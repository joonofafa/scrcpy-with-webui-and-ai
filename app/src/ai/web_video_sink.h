#ifndef SC_WEB_VIDEO_SINK_H
#define SC_WEB_VIDEO_SINK_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "trait/packet_sink.h"
#include "util/thread.h"

#define SC_WEB_VIDEO_SINK_QUEUE_SIZE 16

struct sc_web_video_sink_packet {
    uint8_t *data;
    size_t size;
    bool is_config;
    bool is_key;
};

struct sc_web_video_sink {
    struct sc_packet_sink packet_sink; // trait

    sc_mutex mutex;

    // SPS/PPS cache (sent to new WS clients immediately)
    uint8_t *config_data;
    size_t config_size;

    // Ring buffer (demuxer thread -> web thread)
    struct sc_web_video_sink_packet queue[SC_WEB_VIDEO_SINK_QUEUE_SIZE];
    unsigned queue_head;
    unsigned queue_tail;
    unsigned queue_count;

    uint16_t video_width;
    uint16_t video_height;
    bool opened;
    bool stopped;
};

bool
sc_web_video_sink_init(struct sc_web_video_sink *sink);

// Drain packets from the queue into out[]. Returns number of packets.
// Caller must free each out[i].data.
unsigned
sc_web_video_sink_drain(struct sc_web_video_sink *sink,
                        struct sc_web_video_sink_packet *out,
                        unsigned max);

// Get a copy of cached SPS/PPS config data. Caller must free *data.
bool
sc_web_video_sink_get_config(struct sc_web_video_sink *sink,
                             uint8_t **data, size_t *size);

// Get video dimensions
void
sc_web_video_sink_get_size(struct sc_web_video_sink *sink,
                           uint16_t *width, uint16_t *height);

void
sc_web_video_sink_stop(struct sc_web_video_sink *sink);

void
sc_web_video_sink_destroy(struct sc_web_video_sink *sink);

#endif
