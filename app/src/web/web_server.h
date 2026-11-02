#ifndef SC_WEB_SERVER_H
#define SC_WEB_SERVER_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

#include "mongoose.h"
#include "hid/hid_keyboard.h"
#include "util/thread.h"

struct sc_web_video_sink;
struct sc_controller;
struct sc_web_frame_sink;
struct sc_web_tools;

struct sc_web_server {
    struct sc_web_video_sink *video_sink;
    struct sc_controller *controller;
    struct sc_web_frame_sink *frame_sink;
    struct sc_web_tools *tools;
    struct mg_mgr mgr;
    sc_thread thread;
    bool stopped;
    uint16_t port;
    uint16_t last_video_width;
    uint16_t last_video_height;
    uint16_t screen_width;
    uint16_t screen_height;
    // -K (UHID) mode: browser keys go through scrcpy's UHID keyboard so the
    // device sees a physical keyboard. hid_kb is owned by the web thread.
    bool uhid_keyboard;
    struct sc_hid_keyboard hid_kb;
};

bool
sc_web_server_init(struct sc_web_server *server,
                   struct sc_web_video_sink *video_sink,
                   struct sc_web_frame_sink *frame_sink,
                   struct sc_web_tools *tools,
                   struct sc_controller *controller,
                   uint16_t port, bool uhid_keyboard);

bool
sc_web_server_start(struct sc_web_server *server);

void
sc_web_server_stop(struct sc_web_server *server);

void
sc_web_server_join(struct sc_web_server *server);

void
sc_web_server_destroy(struct sc_web_server *server);

#endif
