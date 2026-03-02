#include "web_server.h"

#include <string.h>

#include "ai_agent.h"
#include "web_ui.h"
#include "web_video_sink.h"
#include "cJSON.h"
#include "controller.h"
#include "control_msg.h"
#include "android/input.h"
#include "android/keycodes.h"
#include "util/log.h"

// Filter base64 image data from multimodal content for the web UI.
// If content starts with '[' (JSON array for multimodal), extract only
// text parts and replace image_url parts with "[Screenshot]".
static char *
filter_content_for_web(const char *content) {
    if (!content || content[0] != '[') {
        return NULL; // not multimodal, use as-is
    }

    cJSON *arr = cJSON_Parse(content);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return NULL;
    }

    // Build filtered text
    size_t cap = 1024;
    char *result = malloc(cap);
    if (!result) {
        cJSON_Delete(arr);
        return NULL;
    }
    result[0] = '\0';
    size_t len = 0;

    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        cJSON *type = cJSON_GetObjectItem(item, "type");
        if (!type || !cJSON_IsString(type)) continue;

        const char *append = NULL;
        if (strcmp(type->valuestring, "text") == 0) {
            cJSON *text = cJSON_GetObjectItem(item, "text");
            if (text && cJSON_IsString(text)) {
                append = text->valuestring;
            }
        } else if (strcmp(type->valuestring, "image_url") == 0) {
            append = "[Screenshot]";
        }

        if (append) {
            size_t alen = strlen(append);
            if (len + alen + 2 >= cap) {
                cap = (len + alen + 2) * 2;
                char *tmp = realloc(result, cap);
                if (!tmp) break;
                result = tmp;
            }
            if (len > 0) {
                result[len++] = '\n';
            }
            memcpy(result + len, append, alen);
            len += alen;
            result[len] = '\0';
        }
    }

    cJSON_Delete(arr);
    return result;
}

static cJSON *
build_state_json(struct sc_ai_agent *agent) {
    cJSON *root = cJSON_CreateObject();

    sc_mutex_lock(&agent->mutex);

    // Screen dimensions
    cJSON_AddNumberToObject(root, "screen_width", agent->screen_width);
    cJSON_AddNumberToObject(root, "screen_height", agent->screen_height);

    // Messages (with base64 filtering)
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    for (size_t i = 0; i < agent->messages.count; i++) {
        cJSON *m = cJSON_CreateObject();
        const char *role = agent->messages.messages[i].role;
        const char *content = agent->messages.messages[i].content;
        const char *name = agent->messages.messages[i].name;
        cJSON_AddStringToObject(m, "role", role ? role : "");

        // Filter base64 from multimodal content
        char *filtered = filter_content_for_web(content);
        if (filtered) {
            cJSON_AddStringToObject(m, "content", filtered);
            free(filtered);
        } else {
            cJSON_AddStringToObject(m, "content", content ? content : "");
        }

        if (name) {
            cJSON_AddStringToObject(m, "name", name);
        }
        const char *tc_json = agent->messages.messages[i].tool_calls_json;
        if (tc_json) {
            cJSON_AddStringToObject(m, "tool_calls", tc_json);
        }
        cJSON_AddItemToArray(msgs, m);
    }

    // Auto-play state
    cJSON_AddBoolToObject(root, "auto_running", agent->auto_running);
    cJSON_AddNumberToObject(root, "auto_interval_ms", agent->auto_interval_ms);
    cJSON_AddStringToObject(root, "game_rules",
        agent->game_rules ? agent->game_rules : "");

    // Config (mask API key)
    const char *api_key = agent->config.api_key;
    if (api_key && strlen(api_key) > 8) {
        cJSON_AddStringToObject(root, "config_api_key", "sk-****");
    } else {
        cJSON_AddStringToObject(root, "config_api_key",
                                api_key ? api_key : "");
    }
    cJSON_AddStringToObject(root, "config_model",
        agent->config.model ? agent->config.model : "");
    cJSON_AddStringToObject(root, "config_base_url",
        agent->config.base_url ? agent->config.base_url : "");

    sc_mutex_unlock(&agent->mutex);

    return root;
}

static void
send_json_response(struct mg_connection *c, cJSON *json) {
    char *str = cJSON_PrintUnformatted(json);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\n",
                  "%s", str);
    free(str);
    cJSON_Delete(json);
}

static void
send_ok(struct mg_connection *c) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\n",
                  "{\"ok\":true}");
}

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
    }

    cJSON_Delete(json);
}

static void
handle_event(struct mg_connection *c, int ev, void *ev_data) {
    struct sc_web_server *server = (struct sc_web_server *)c->fn_data;

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        struct sc_ai_agent *agent = server->agent;

        // CORS preflight
        if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            mg_http_reply(c, 204,
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\n",
                "");
            return;
        }

        // WebSocket upgrade: /ws/video
        // Set data[0] BEFORE upgrade so MG_EV_WS_OPEN handler sees it
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

        // GET / — serve SPA
        if (mg_match(hm->method, mg_str("GET"), NULL)
                && mg_match(hm->uri, mg_str("/"), NULL)) {
            mg_http_reply(c, 200,
                "Content-Type: text/html; charset=utf-8\r\n",
                "%s", WEB_UI_HTML);
            return;
        }

        // GET /api/state
        if (mg_match(hm->method, mg_str("GET"), NULL)
                && mg_match(hm->uri, mg_str("/api/state"), NULL)) {
            cJSON *state = build_state_json(agent);
            send_json_response(c, state);
            return;
        }

        // POST /api/prompt
        if (mg_match(hm->method, mg_str("POST"), NULL)
                && mg_match(hm->uri, mg_str("/api/prompt"), NULL)) {
            cJSON *body = parse_body(hm);
            if (!body) {
                send_error(c, 400, "invalid json");
                return;
            }
            cJSON *prompt_item = cJSON_GetObjectItem(body, "prompt");
            if (!cJSON_IsString(prompt_item)) {
                cJSON_Delete(body);
                send_error(c, 400, "missing prompt field");
                return;
            }
            char *prompt = strdup(prompt_item->valuestring);
            cJSON_Delete(body);
            sc_ai_agent_submit_prompt(agent, prompt);
            send_ok(c);
            return;
        }

        // GET /api/game-rules
        if (mg_match(hm->method, mg_str("GET"), NULL)
                && mg_match(hm->uri, mg_str("/api/game-rules"), NULL)) {
            sc_mutex_lock(&agent->mutex);
            const char *rules = agent->game_rules ? agent->game_rules : "";
            cJSON *json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "rules", rules);
            sc_mutex_unlock(&agent->mutex);
            send_json_response(c, json);
            return;
        }

        // POST /api/game-rules
        if (mg_match(hm->method, mg_str("POST"), NULL)
                && mg_match(hm->uri, mg_str("/api/game-rules"), NULL)) {
            cJSON *body = parse_body(hm);
            if (!body) {
                send_error(c, 400, "invalid json");
                return;
            }
            cJSON *rules_item = cJSON_GetObjectItem(body, "rules");
            if (!cJSON_IsString(rules_item)) {
                cJSON_Delete(body);
                send_error(c, 400, "missing rules field");
                return;
            }
            sc_ai_agent_set_game_rules(agent, rules_item->valuestring);
            cJSON_Delete(body);
            send_ok(c);
            return;
        }

        // POST /api/game-rules/summarize
        if (mg_match(hm->method, mg_str("POST"), NULL)
                && mg_match(hm->uri, mg_str("/api/game-rules/summarize"), NULL)) {
            char *result = sc_ai_agent_summarize_rules(agent);
            cJSON *json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "rules", result ? result : "");
            free(result);
            send_json_response(c, json);
            return;
        }

        // POST /api/auto/start
        if (mg_match(hm->method, mg_str("POST"), NULL)
                && mg_match(hm->uri, mg_str("/api/auto/start"), NULL)) {
            cJSON *body = parse_body(hm);
            if (body) {
                cJSON *ms = cJSON_GetObjectItem(body, "interval_ms");
                if (cJSON_IsNumber(ms)) {
                    sc_ai_agent_set_auto_interval(agent, (int)ms->valuedouble);
                }
                cJSON_Delete(body);
            }
            sc_ai_agent_set_auto_running(agent, true);
            send_ok(c);
            return;
        }

        // POST /api/auto/stop
        if (mg_match(hm->method, mg_str("POST"), NULL)
                && mg_match(hm->uri, mg_str("/api/auto/stop"), NULL)) {
            sc_ai_agent_set_auto_running(agent, false);
            send_ok(c);
            return;
        }

        // POST /api/config
        if (mg_match(hm->method, mg_str("POST"), NULL)
                && mg_match(hm->uri, mg_str("/api/config"), NULL)) {
            cJSON *body = parse_body(hm);
            if (!body) {
                send_error(c, 400, "invalid json");
                return;
            }
            cJSON *ak = cJSON_GetObjectItem(body, "api_key");
            cJSON *md = cJSON_GetObjectItem(body, "model");
            cJSON *bu = cJSON_GetObjectItem(body, "base_url");
            const char *api_key_val = (cJSON_IsString(ak) && ak->valuestring[0])
                                  ? ak->valuestring : NULL;
            const char *model = (cJSON_IsString(md) && md->valuestring[0])
                                ? md->valuestring : NULL;
            const char *base_url = (cJSON_IsString(bu) && bu->valuestring[0])
                                   ? bu->valuestring : NULL;
            sc_ai_agent_set_config(agent, api_key_val, model, base_url);
            cJSON_Delete(body);
            send_ok(c);
            return;
        }

        // POST /api/clear
        if (mg_match(hm->method, mg_str("POST"), NULL)
                && mg_match(hm->uri, mg_str("/api/clear"), NULL)) {
            sc_ai_agent_clear_history(agent);
            send_ok(c);
            return;
        }

        // 404
        send_error(c, 404, "not found");

    } else if (ev == MG_EV_WS_OPEN) {
        // New WebSocket connection opened
        if (c->data[0] == 'V' && server->video_sink) {
            // Send video dimensions as text frame
            uint16_t w, h;
            sc_web_video_sink_get_size(server->video_sink, &w, &h);

            char dim_json[64];
            snprintf(dim_json, sizeof(dim_json),
                     "{\"width\":%u,\"height\":%u}", (unsigned)w, (unsigned)h);
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
                char dim_json[64];
                snprintf(dim_json, sizeof(dim_json),
                         "{\"width\":%u,\"height\":%u}",
                         (unsigned)cur_w, (unsigned)cur_h);
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
                   struct sc_ai_agent *agent,
                   struct sc_web_video_sink *video_sink,
                   struct sc_controller *controller,
                   uint16_t port) {
    server->agent = agent;
    server->video_sink = video_sink;
    server->controller = controller;
    server->port = port;
    server->stopped = false;
    server->last_video_width = 0;
    server->last_video_height = 0;

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
