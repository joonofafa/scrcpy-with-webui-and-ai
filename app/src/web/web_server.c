#include "web_server.h"

#include <string.h>

#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>

#include "web_frame_sink.h"
#include "web_tools.h"
#include "screenshot.h"
#include "web_video_sink.h"
#include "cJSON.h"
#include "controller.h"
#include "control_msg.h"
#include "android/input.h"
#include "android/keycodes.h"
#include "util/log.h"

static void
send_error(struct mg_connection *c, int code, const char *msg) {
    mg_http_reply(c, code, "Content-Type: application/json\r\n"
                           "Access-Control-Allow-Origin: *\r\n",
                  "{\"error\":\"%s\"}", msg);
}

static cJSON *
parse_body(struct mg_http_message *hm) {
    if (hm->body.len == 0) {
        return NULL;
    }
    // mg_str is not null-terminated, so copy
    char *buf = malloc(hm->body.len + 1);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, hm->body.buf, hm->body.len);
    buf[hm->body.len] = '\0';
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

// Map key name string to Android keycode
static enum android_keycode
keyname_to_keycode(const char *key) {
    if (strcmp(key, "back") == 0) return AKEYCODE_BACK;
    if (strcmp(key, "home") == 0) return AKEYCODE_HOME;
    if (strcmp(key, "app_switch") == 0) return AKEYCODE_APP_SWITCH;
    if (strcmp(key, "volume_up") == 0) return AKEYCODE_VOLUME_UP;
    if (strcmp(key, "volume_down") == 0) return AKEYCODE_VOLUME_DOWN;
    if (strcmp(key, "power") == 0) return AKEYCODE_POWER;
    return AKEYCODE_UNKNOWN;
}

// Handle control WebSocket messages (touch, key)
static void
handle_ws_control_msg(struct sc_web_server *server, struct mg_ws_message *wm) {
    if (!server->controller) {
        return;
    }

    // Parse the WS text message as JSON
    char *buf = malloc(wm->data.len + 1);
    if (!buf) return;
    memcpy(buf, wm->data.buf, wm->data.len);
    buf[wm->data.len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) return;

    cJSON *type_item = cJSON_GetObjectItem(json, "type");
    if (!type_item || !cJSON_IsString(type_item)) {
        cJSON_Delete(json);
        return;
    }

    const char *type = type_item->valuestring;

    if (strcmp(type, "touch") == 0) {
        cJSON *action_item = cJSON_GetObjectItem(json, "action");
        cJSON *x_item = cJSON_GetObjectItem(json, "x");
        cJSON *y_item = cJSON_GetObjectItem(json, "y");
        cJSON *w_item = cJSON_GetObjectItem(json, "w");
        cJSON *h_item = cJSON_GetObjectItem(json, "h");

        if (!action_item || !cJSON_IsString(action_item)
                || !x_item || !cJSON_IsNumber(x_item)
                || !y_item || !cJSON_IsNumber(y_item)
                || !w_item || !cJSON_IsNumber(w_item)
                || !h_item || !cJSON_IsNumber(h_item)) {
            cJSON_Delete(json);
            return;
        }

        const char *action_str = action_item->valuestring;
        enum android_motionevent_action action;
        float pressure;
        if (strcmp(action_str, "down") == 0) {
            action = AMOTION_EVENT_ACTION_DOWN;
            pressure = 1.0f;
        } else if (strcmp(action_str, "move") == 0) {
            action = AMOTION_EVENT_ACTION_MOVE;
            pressure = 1.0f;
        } else if (strcmp(action_str, "up") == 0) {
            action = AMOTION_EVENT_ACTION_UP;
            pressure = 0.0f;
        } else {
            cJSON_Delete(json);
            return;
        }

        int32_t x = (int32_t) x_item->valuedouble;
        int32_t y = (int32_t) y_item->valuedouble;
        uint16_t w = (uint16_t) w_item->valuedouble;
        uint16_t h = (uint16_t) h_item->valuedouble;

        struct sc_control_msg msg;
        msg.type = SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT;
        msg.inject_touch_event.action = action;
        msg.inject_touch_event.action_button = 0;
        msg.inject_touch_event.buttons = 0;
        msg.inject_touch_event.pointer_id = SC_POINTER_ID_GENERIC_FINGER;
        msg.inject_touch_event.position.screen_size.width = w;
        msg.inject_touch_event.position.screen_size.height = h;
        msg.inject_touch_event.position.point.x = x;
        msg.inject_touch_event.position.point.y = y;
        msg.inject_touch_event.pressure = pressure;

        LOGD("Web touch: action=%s x=%d y=%d w=%u h=%u",
             action_str, x, y, (unsigned)w, (unsigned)h);
        if (!sc_controller_push_msg(server->controller, &msg)) {
            LOGW("Could not push web touch event");
        }
    } else if (strcmp(type, "key") == 0) {
        cJSON *key_item = cJSON_GetObjectItem(json, "key");
        if (!key_item || !cJSON_IsString(key_item)) {
            cJSON_Delete(json);
            return;
        }

        enum android_keycode keycode = keyname_to_keycode(key_item->valuestring);
        if (keycode == AKEYCODE_UNKNOWN) {
            cJSON_Delete(json);
            return;
        }

        // Send key down + key up
        struct sc_control_msg msg_down;
        msg_down.type = SC_CONTROL_MSG_TYPE_INJECT_KEYCODE;
        msg_down.inject_keycode.action = AKEY_EVENT_ACTION_DOWN;
        msg_down.inject_keycode.keycode = keycode;
        msg_down.inject_keycode.repeat = 0;
        msg_down.inject_keycode.metastate = AMETA_NONE;

        struct sc_control_msg msg_up;
        msg_up.type = SC_CONTROL_MSG_TYPE_INJECT_KEYCODE;
        msg_up.inject_keycode.action = AKEY_EVENT_ACTION_UP;
        msg_up.inject_keycode.keycode = keycode;
        msg_up.inject_keycode.repeat = 0;
        msg_up.inject_keycode.metastate = AMETA_NONE;

        LOGI("Web key: %s (keycode=%d)", key_item->valuestring, keycode);
        if (!sc_controller_push_msg(server->controller, &msg_down)) {
            LOGW("Could not push web key down event");
        }
        if (!sc_controller_push_msg(server->controller, &msg_up)) {
            LOGW("Could not push web key up event");
        }
    } else if (strcmp(type, "keyevent") == 0) {
        // Browser keyboard event: {type:"keyevent", action:"down"|"up",
        //                          keycode: <android_keycode>, metastate: <int>}
        cJSON *action_item = cJSON_GetObjectItem(json, "action");
        cJSON *keycode_item = cJSON_GetObjectItem(json, "keycode");
        if (!action_item || !cJSON_IsString(action_item)
                || !keycode_item || !cJSON_IsNumber(keycode_item)) {
            cJSON_Delete(json);
            return;
        }

        const char *action_str = action_item->valuestring;
        enum android_keyevent_action action;
        if (strcmp(action_str, "down") == 0) {
            action = AKEY_EVENT_ACTION_DOWN;
        } else if (strcmp(action_str, "up") == 0) {
            action = AKEY_EVENT_ACTION_UP;
        } else {
            cJSON_Delete(json);
            return;
        }

        cJSON *meta_item = cJSON_GetObjectItem(json, "metastate");
        enum android_metastate metastate = (meta_item && cJSON_IsNumber(meta_item))
            ? (enum android_metastate) meta_item->valueint
            : AMETA_NONE;

        struct sc_control_msg msg;
        msg.type = SC_CONTROL_MSG_TYPE_INJECT_KEYCODE;
        msg.inject_keycode.action = action;
        msg.inject_keycode.keycode = (enum android_keycode) keycode_item->valueint;
        msg.inject_keycode.repeat = 0;
        msg.inject_keycode.metastate = metastate;

        LOGD("Web keyevent: %s keycode=%d meta=%d", action_str,
             keycode_item->valueint, (int) metastate);
        if (!sc_controller_push_msg(server->controller, &msg)) {
            LOGW("Could not push web keyevent");
        }
    } else if (strcmp(type, "text") == 0) {
        // Text injection: {type:"text", text:"hello"}
        cJSON *text_item = cJSON_GetObjectItem(json, "text");
        if (!text_item || !cJSON_IsString(text_item)
                || strlen(text_item->valuestring) == 0) {
            cJSON_Delete(json);
            return;
        }

        struct sc_control_msg msg;
        msg.type = SC_CONTROL_MSG_TYPE_INJECT_TEXT;
        msg.inject_text.text = strdup(text_item->valuestring);
        if (!msg.inject_text.text) {
            cJSON_Delete(json);
            return;
        }

        LOGD("Web text: \"%s\"", text_item->valuestring);
        if (!sc_controller_push_msg(server->controller, &msg)) {
            free(msg.inject_text.text);
            LOGW("Could not push web text event");
        }
    }

    cJSON_Delete(json);
}

static void
handle_event(struct mg_connection *c, int ev, void *ev_data) {
    struct sc_web_server *server = (struct sc_web_server *)c->fn_data;

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        // CORS preflight
        if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            mg_http_reply(c, 204,
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\n",
                "");
            return;
        }

        // ============================================================
        // Internal API (for Python backend)
        // ============================================================

        // GET /internal/screenshot -- capture frame, return JPEG binary
        if (mg_match(hm->method, mg_str("GET"), NULL)
                && mg_match(hm->uri, mg_str("/internal/screenshot"), NULL)) {
            if (!server->frame_sink) {
                send_error(c, 500, "no frame sink");
                return;
            }
            AVFrame *frame = av_frame_alloc();
            if (!frame) {
                send_error(c, 500, "alloc failed");
                return;
            }
            if (!sc_web_frame_sink_consume(server->frame_sink, frame)) {
                av_frame_free(&frame);
                send_error(c, 503, "no frame available");
                return;
            }
            uint16_t orig_w = (uint16_t)frame->width;
            uint16_t orig_h = (uint16_t)frame->height;

            struct sc_web_screenshot ss = {0};
            if (!sc_web_screenshot_encode(&ss, frame)) {
                av_frame_free(&frame);
                send_error(c, 500, "encode failed");
                return;
            }
            av_frame_free(&frame);

            // Update screen/frame sizes on tools
            sc_web_tools_set_screen_size(server->tools, ss.width, ss.height);
            sc_web_tools_set_frame_size(server->tools, orig_w, orig_h);
            server->screen_width = ss.width;
            server->screen_height = ss.height;

            // Send JPEG binary with dimension headers
            c->send.len = 0;
            mg_printf(c,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %lu\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Expose-Headers: X-Screenshot-Width, X-Screenshot-Height, X-Frame-Width, X-Frame-Height\r\n"
                "X-Screenshot-Width: %u\r\n"
                "X-Screenshot-Height: %u\r\n"
                "X-Frame-Width: %u\r\n"
                "X-Frame-Height: %u\r\n"
                "\r\n",
                (unsigned long)ss.png_size,
                (unsigned)ss.width, (unsigned)ss.height,
                (unsigned)orig_w, (unsigned)orig_h);
            mg_send(c, ss.png_data, ss.png_size);
            sc_web_screenshot_destroy(&ss);
            return;
        }

        // POST /internal/click -- inject tap (DOWN/MOVE/UP sequence)
        if (mg_match(hm->method, mg_str("POST"), NULL)
                && mg_match(hm->uri, mg_str("/internal/click"), NULL)) {
            cJSON *body = parse_body(hm);
            if (!body) { send_error(c, 400, "invalid json"); return; }
            cJSON *jx = cJSON_GetObjectItem(body, "x");
            cJSON *jy = cJSON_GetObjectItem(body, "y");
            if (!jx || !jy) {
                cJSON_Delete(body);
                send_error(c, 400, "missing x or y");
                return;
            }
            char args[128];
            cJSON *jw = cJSON_GetObjectItem(body, "w");
            cJSON *jh = cJSON_GetObjectItem(body, "h");
            if (jw && jh) {
                snprintf(args, sizeof(args), "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                         jx->valueint, jy->valueint, jw->valueint, jh->valueint);
            } else {
                snprintf(args, sizeof(args), "{\"x\":%d,\"y\":%d}",
                         jx->valueint, jy->valueint);
            }
            cJSON_Delete(body);
            char *result = sc_web_tools_execute(server->tools,
                                                "position_click", args);
            mg_http_reply(c, 200,
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n",
                "%s", result ? result : "{\"error\":\"null\"}");
            free(result);
            return;
        }

        // POST /internal/long_press
        if (mg_match(hm->method, mg_str("POST"), NULL)
                && mg_match(hm->uri, mg_str("/internal/long_press"), NULL)) {
            cJSON *body = parse_body(hm);
            if (!body) { send_error(c, 400, "invalid json"); return; }
            char args[128];
            cJSON *jx = cJSON_GetObjectItem(body, "x");
            cJSON *jy = cJSON_GetObjectItem(body, "y");
            cJSON *jms = cJSON_GetObjectItem(body, "duration_ms");
            if (!jx || !jy) {
                cJSON_Delete(body);
                send_error(c, 400, "missing x or y");
                return;
            }
            snprintf(args, sizeof(args), "{\"x\":%d,\"y\":%d,\"duration_ms\":%d}",
                     jx->valueint, jy->valueint,
                     jms ? jms->valueint : 500);
            cJSON_Delete(body);
            char *result = sc_web_tools_execute(server->tools,
                                                "position_long_press", args);
            mg_http_reply(c, 200,
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n",
                "%s", result ? result : "{\"error\":\"null\"}");
            free(result);
            return;
        }

        // POST /internal/swipe
        if (mg_match(hm->method, mg_str("POST"), NULL)
                && mg_match(hm->uri, mg_str("/internal/swipe"), NULL)) {
            cJSON *body = parse_body(hm);
            if (!body) { send_error(c, 400, "invalid json"); return; }
            char *str = cJSON_PrintUnformatted(body);
            cJSON_Delete(body);
            char *result = sc_web_tools_execute(server->tools, "swipe",
                                                str ? str : "{}");
            free(str);
            mg_http_reply(c, 200,
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n",
                "%s", result ? result : "{\"error\":\"null\"}");
            free(result);
            return;
        }

        // POST /internal/key
        if (mg_match(hm->method, mg_str("POST"), NULL)
                && mg_match(hm->uri, mg_str("/internal/key"), NULL)) {
            cJSON *body = parse_body(hm);
            if (!body) { send_error(c, 400, "invalid json"); return; }
            cJSON *jkc = cJSON_GetObjectItem(body, "keycode");
            cJSON *jact = cJSON_GetObjectItem(body, "action");
            if (!jkc) {
                cJSON_Delete(body);
                send_error(c, 400, "missing keycode");
                return;
            }
            const char *action = jact && jact->valuestring
                                 ? jact->valuestring : "press";
            char args[64];
            snprintf(args, sizeof(args), "{\"keycode\":%d}", jkc->valueint);
            char *result;
            if (strcmp(action, "down") == 0) {
                result = sc_web_tools_execute(server->tools, "key_down", args);
            } else if (strcmp(action, "up") == 0) {
                result = sc_web_tools_execute(server->tools, "key_up", args);
            } else {
                result = sc_web_tools_execute(server->tools, "key_press", args);
            }
            cJSON_Delete(body);
            mg_http_reply(c, 200,
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n",
                "%s", result ? result : "{\"error\":\"null\"}");
            free(result);
            return;
        }

        // POST /internal/text
        if (mg_match(hm->method, mg_str("POST"), NULL)
                && mg_match(hm->uri, mg_str("/internal/text"), NULL)) {
            cJSON *body = parse_body(hm);
            if (!body) { send_error(c, 400, "invalid json"); return; }
            char *str = cJSON_PrintUnformatted(body);
            cJSON_Delete(body);
            char *result = sc_web_tools_execute(server->tools, "input_text",
                                                str ? str : "{}");
            free(str);
            mg_http_reply(c, 200,
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n",
                "%s", result ? result : "{\"error\":\"null\"}");
            free(result);
            return;
        }

        // GET /internal/info
        if (mg_match(hm->method, mg_str("GET"), NULL)
                && mg_match(hm->uri, mg_str("/internal/info"), NULL)) {
            uint16_t sw = server->screen_width;
            uint16_t sh = server->screen_height;
            uint16_t fw = server->tools->frame_width;
            uint16_t fh = server->tools->frame_height;
            mg_http_reply(c, 200,
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n",
                "{\"screenshot_width\":%u,\"screenshot_height\":%u,"
                "\"frame_width\":%u,\"frame_height\":%u}",
                (unsigned)sw, (unsigned)sh, (unsigned)fw, (unsigned)fh);
            return;
        }

        // ============================================================
        // WebSocket upgrades
        // ============================================================

        // WebSocket upgrade: /ws/video
        if (mg_match(hm->method, mg_str("GET"), NULL)
                && mg_match(hm->uri, mg_str("/ws/video"), NULL)) {
            c->data[0] = 'V';
            mg_ws_upgrade(c, hm, NULL);
            return;
        }

        // WebSocket upgrade: /ws/control
        if (mg_match(hm->method, mg_str("GET"), NULL)
                && mg_match(hm->uri, mg_str("/ws/control"), NULL)) {
            c->data[0] = 'C';
            mg_ws_upgrade(c, hm, NULL);
            return;
        }

        // 404
        send_error(c, 404, "not found");

    } else if (ev == MG_EV_WS_OPEN) {
        // New WebSocket connection opened
        if (c->data[0] == 'V' && server->video_sink) {
            // Send video dimensions and codec as text frame
            uint16_t w, h;
            sc_web_video_sink_get_size(server->video_sink, &w, &h);
            uint32_t codec_id =
                sc_web_video_sink_get_codec_id(server->video_sink);
            const char *codec_name =
                (codec_id == AV_CODEC_ID_HEVC) ? "h265" : "h264";

            char dim_json[96];
            snprintf(dim_json, sizeof(dim_json),
                     "{\"width\":%u,\"height\":%u,\"codec\":\"%s\"}",
                     (unsigned)w, (unsigned)h, codec_name);
            mg_ws_send(c, dim_json, strlen(dim_json), WEBSOCKET_OP_TEXT);

            // Send cached SPS/PPS config as binary
            uint8_t *config_data;
            size_t config_size;
            if (sc_web_video_sink_get_config(server->video_sink,
                                             &config_data, &config_size)) {
                LOGI("Sending config: %zu bytes, first bytes: "
                     "%02x %02x %02x %02x %02x",
                     config_size,
                     config_size > 0 ? config_data[0] : 0,
                     config_size > 1 ? config_data[1] : 0,
                     config_size > 2 ? config_data[2] : 0,
                     config_size > 3 ? config_data[3] : 0,
                     config_size > 4 ? config_data[4] : 0);
                mg_ws_send(c, (const char *)config_data, config_size,
                           WEBSOCKET_OP_BINARY);
                free(config_data);
            } else {
                LOGW("No config data available for video WS client");
            }

            // Send cached last keyframe for instant display
            uint8_t *kf_data;
            size_t kf_size;
            if (sc_web_video_sink_get_keyframe(server->video_sink,
                                               &kf_data, &kf_size)) {
                mg_ws_send(c, (const char *)kf_data, kf_size,
                           WEBSOCKET_OP_BINARY);
                free(kf_data);
                LOGI("Sent cached keyframe: %zu bytes", kf_size);
            }

            LOGI("Video WebSocket client connected: %ux%u",
                 (unsigned)w, (unsigned)h);
        } else if (c->data[0] == 'C') {
            LOGI("Control WebSocket client connected (controller=%p)",
                 (void *)server->controller);
        }

    } else if (ev == MG_EV_WS_MSG) {
        // WebSocket message received
        if (c->data[0] == 'C') {
            struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
            LOGD("Control WS msg received: %.*s",
                 (int)(wm->data.len > 200 ? 200 : wm->data.len),
                 wm->data.buf);
            handle_ws_control_msg(server, wm);
        }
    }
}

static int SDLCALL
web_thread_fn(void *data) {
    struct sc_web_server *server = (struct sc_web_server *)data;

    struct sc_web_video_sink_packet packets[SC_WEB_VIDEO_SINK_QUEUE_SIZE];

    while (!server->stopped) {
        // Drain video packets and broadcast to all 'V' WS clients
        if (server->video_sink) {
            // Check for dimension changes (device rotation)
            uint16_t cur_w, cur_h;
            sc_web_video_sink_get_size(server->video_sink, &cur_w, &cur_h);
            if (cur_w && cur_h &&
                    (cur_w != server->last_video_width
                     || cur_h != server->last_video_height)) {
                server->last_video_width = cur_w;
                server->last_video_height = cur_h;
                uint32_t codec_id =
                    sc_web_video_sink_get_codec_id(server->video_sink);
                const char *codec_name =
                    (codec_id == AV_CODEC_ID_HEVC) ? "h265" : "h264";
                char dim_json[96];
                snprintf(dim_json, sizeof(dim_json),
                         "{\"width\":%u,\"height\":%u,\"codec\":\"%s\"}",
                         (unsigned)cur_w, (unsigned)cur_h, codec_name);
                for (struct mg_connection *c = server->mgr.conns;
                        c != NULL; c = c->next) {
                    if (c->data[0] == 'V' && c->is_websocket) {
                        mg_ws_send(c, dim_json, strlen(dim_json),
                                   WEBSOCKET_OP_TEXT);
                    }
                }
                LOGI("Video dimensions changed: %ux%u",
                     (unsigned)cur_w, (unsigned)cur_h);
            }

            unsigned count = sc_web_video_sink_drain(
                server->video_sink, packets, SC_WEB_VIDEO_SINK_QUEUE_SIZE);

            if (count > 0) {
                for (struct mg_connection *c = server->mgr.conns;
                        c != NULL; c = c->next) {
                    if (c->data[0] != 'V') continue;
                    if (!c->is_websocket) continue;

                    // Backpressure: skip if send buffer too large
                    if (c->send.len > 512 * 1024) continue;

                    for (unsigned i = 0; i < count; i++) {
                        mg_ws_send(c,
                                   (const char *)packets[i].data,
                                   packets[i].size,
                                   WEBSOCKET_OP_BINARY);
                    }
                }

                // Free drained packets
                for (unsigned i = 0; i < count; i++) {
                    free(packets[i].data);
                }
            }
        }

        mg_mgr_poll(&server->mgr, 10);
    }

    return 0;
}

bool
sc_web_server_init(struct sc_web_server *server,
                   struct sc_web_video_sink *video_sink,
                   struct sc_web_frame_sink *frame_sink,
                   struct sc_web_tools *tools,
                   struct sc_controller *controller,
                   uint16_t port) {
    server->video_sink = video_sink;
    server->frame_sink = frame_sink;
    server->tools = tools;
    server->controller = controller;
    server->port = port;
    server->stopped = false;
    server->last_video_width = 0;
    server->last_video_height = 0;
    server->screen_width = 0;
    server->screen_height = 0;

    mg_mgr_init(&server->mgr);

    char url[64];
    snprintf(url, sizeof(url), "http://0.0.0.0:%u", (unsigned)port);

    struct mg_connection *c = mg_http_listen(&server->mgr, url,
                                             handle_event, server);
    if (!c) {
        LOGE("Failed to start web server on %s", url);
        mg_mgr_free(&server->mgr);
        return false;
    }

    LOGI("Web server listening on %s", url);
    return true;
}

bool
sc_web_server_start(struct sc_web_server *server) {
    bool ok = sc_thread_create(&server->thread, web_thread_fn,
                               "scrcpy-web", server);
    if (!ok) {
        LOGE("Could not start web server thread");
        return false;
    }
    return true;
}

void
sc_web_server_stop(struct sc_web_server *server) {
    server->stopped = true;
}

void
sc_web_server_join(struct sc_web_server *server) {
    sc_thread_join(&server->thread, NULL);
}

void
sc_web_server_destroy(struct sc_web_server *server) {
    mg_mgr_free(&server->mgr);
}
