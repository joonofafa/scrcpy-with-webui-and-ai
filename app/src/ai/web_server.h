#ifndef SC_WEB_SERVER_H
#define SC_WEB_SERVER_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

#include "mongoose.h"
#include "util/thread.h"

struct sc_ai_agent;
struct sc_web_video_sink;
struct sc_controller;

struct sc_web_server {
    struct sc_ai_agent *agent;
    struct sc_web_video_sink *video_sink;
    struct sc_controller *controller;
    struct mg_mgr mgr;
    sc_thread thread;
    bool stopped;
    uint16_t port;
    uint16_t last_video_width;
    uint16_t last_video_height;
};

bool
sc_web_server_init(struct sc_web_server *server,
                   struct sc_ai_agent *agent,
                   struct sc_web_video_sink *video_sink,
                   struct sc_controller *controller,
                   uint16_t port);

bool
sc_web_server_start(struct sc_web_server *server);

void
sc_web_server_stop(struct sc_web_server *server);

void
sc_web_server_join(struct sc_web_server *server);

void
sc_web_server_destroy(struct sc_web_server *server);

#endif
