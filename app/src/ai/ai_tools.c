#include "ai_tools.h"

#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <libavutil/frame.h>

#include "ai/ai_agent.h"
#include "ai/ai_frame_sink.h"
#include "ai/screenshot.h"
#include "../../deps/cjson/cJSON.h"
#include "android/input.h"
#include "android/keycodes.h"
#include "control_msg.h"
#include "controller.h"
#include "coords.h"
#include "util/log.h"
#include "util/thread.h"

void
sc_ai_tools_init(struct sc_ai_tools *tools, struct sc_controller *controller,
                 uint16_t screen_width, uint16_t screen_height) {
    tools->controller = controller;
    tools->screen_width = screen_width;
    tools->screen_height = screen_height;
}

void
sc_ai_tools_set_screen_size(struct sc_ai_tools *tools,
                            uint16_t width, uint16_t height) {
    tools->screen_width = width;
    tools->screen_height = height;
}

void
sc_ai_tools_set_frame_size(struct sc_ai_tools *tools,
                           uint16_t width, uint16_t height) {
    tools->frame_width = width;
    tools->frame_height = height;
}

void
sc_ai_tools_set_agent(struct sc_ai_tools *tools, struct sc_ai_agent *agent) {
    tools->agent = agent;
}

static const char TOOLS_JSON[] =
"["
"  {"
"    \"type\": \"function\","
"    \"function\": {"
"      \"name\": \"position_click\","
"      \"description\": \"Tap at a specific screen position (touch down + up)\","
"      \"parameters\": {"
"        \"type\": \"object\","
"        \"properties\": {"
"          \"x\": {\"type\": \"integer\", \"description\": \"X coordinate in pixels\"},"
"          \"y\": {\"type\": \"integer\", \"description\": \"Y coordinate in pixels\"}"
"        },"
"        \"required\": [\"x\", \"y\"]"
"      }"
"    }"
"  },"
"  {"
"    \"type\": \"function\","
"    \"function\": {"
"      \"name\": \"position_long_press\","
"      \"description\": \"Long press at a specific screen position (touch down, hold for ms, then up)\","
"      \"parameters\": {"
"        \"type\": \"object\","
"        \"properties\": {"
"          \"x\": {\"type\": \"integer\", \"description\": \"X coordinate in pixels\"},"
"          \"y\": {\"type\": \"integer\", \"description\": \"Y coordinate in pixels\"},"
"          \"duration_ms\": {\"type\": \"integer\", \"description\": \"Hold duration in milliseconds\", \"default\": 500}"
"        },"
"        \"required\": [\"x\", \"y\"]"
"      }"
"    }"
"  },"
"  {"
"    \"type\": \"function\","
"    \"function\": {"
"      \"name\": \"swipe\","
"      \"description\": \"Swipe from one position to another\","
"      \"parameters\": {"
"        \"type\": \"object\","
"        \"properties\": {"
"          \"x1\": {\"type\": \"integer\", \"description\": \"Start X coordinate\"},"
"          \"y1\": {\"type\": \"integer\", \"description\": \"Start Y coordinate\"},"
"          \"x2\": {\"type\": \"integer\", \"description\": \"End X coordinate\"},"
"          \"y2\": {\"type\": \"integer\", \"description\": \"End Y coordinate\"},"
"          \"duration_ms\": {\"type\": \"integer\", \"description\": \"Swipe duration in ms\", \"default\": 300}"
"        },"
"        \"required\": [\"x1\", \"y1\", \"x2\", \"y2\"]"
"      }"
"    }"
"  },"
"  {"
"    \"type\": \"function\","
"    \"function\": {"
"      \"name\": \"key_press\","
"      \"description\": \"Press and release an Android key (down + up)\","
"      \"parameters\": {"
"        \"type\": \"object\","
"        \"properties\": {"
"          \"keycode\": {\"type\": \"integer\", \"description\": \"Android keycode (e.g. 4=BACK, 3=HOME, 24=VOLUME_UP, 25=VOLUME_DOWN, 26=POWER, 66=ENTER)\"}"
"        },"
"        \"required\": [\"keycode\"]"
"      }"
"    }"
"  },"
"  {"
"    \"type\": \"function\","
"    \"function\": {"
"      \"name\": \"key_down\","
"      \"description\": \"Press down an Android key (without releasing)\","
"      \"parameters\": {"
"        \"type\": \"object\","
"        \"properties\": {"
"          \"keycode\": {\"type\": \"integer\", \"description\": \"Android keycode\"}"
"        },"
"        \"required\": [\"keycode\"]"
"      }"
"    }"
"  },"
"  {"
"    \"type\": \"function\","
"    \"function\": {"
"      \"name\": \"key_up\","
"      \"description\": \"Release an Android key\","
"      \"parameters\": {"
"        \"type\": \"object\","
"        \"properties\": {"
"          \"keycode\": {\"type\": \"integer\", \"description\": \"Android keycode\"}"
"        },"
"        \"required\": [\"keycode\"]"
"      }"
"    }"
"  },"
"  {"
"    \"type\": \"function\","
"    \"function\": {"
"      \"name\": \"input_text\","
"      \"description\": \"Type text string on the device\","
"      \"parameters\": {"
"        \"type\": \"object\","
"        \"properties\": {"
"          \"text\": {\"type\": \"string\", \"description\": \"Text to type\"}"
"        },"
"        \"required\": [\"text\"]"
"      }"
"    }"
"  },"
"  {"
"    \"type\": \"function\","
"    \"function\": {"
"      \"name\": \"screenshot\","
"      \"description\": \"Take a fresh screenshot of the device screen. Use this after performing actions to verify the result before deciding next steps.\","
"      \"parameters\": {"
"        \"type\": \"object\","
"        \"properties\": {},"
"        \"required\": []"
"      }"
"    }"
"  }"
"]";

const char *
sc_ai_tools_get_definitions(void) {
    return TOOLS_JSON;
}

static bool
inject_touch(struct sc_controller *controller,
             uint16_t sw, uint16_t sh,
             int32_t x, int32_t y,
             enum android_motionevent_action action) {
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT;
    msg.inject_touch_event.action = action;
    msg.inject_touch_event.action_button = 0;
    msg.inject_touch_event.buttons = 0;
    msg.inject_touch_event.pointer_id = SC_POINTER_ID_GENERIC_FINGER;
    msg.inject_touch_event.position.screen_size.width = sw;
    msg.inject_touch_event.position.screen_size.height = sh;
    msg.inject_touch_event.position.point.x = x;
    msg.inject_touch_event.position.point.y = y;
    msg.inject_touch_event.pressure = action == AMOTION_EVENT_ACTION_UP
                                       ? 0.0f : 1.0f;

    if (!sc_controller_push_msg(controller, &msg)) {
        LOGW("AI: could not inject touch event");
        return false;
    }
    return true;
}

static bool
inject_keycode(struct sc_controller *controller,
               enum android_keycode keycode,
               enum android_keyevent_action action) {
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_INJECT_KEYCODE;
    msg.inject_keycode.action = action;
    msg.inject_keycode.keycode = keycode;
    msg.inject_keycode.repeat = 0;
    msg.inject_keycode.metastate = AMETA_NONE;

    if (!sc_controller_push_msg(controller, &msg)) {
        LOGW("AI: could not inject keycode");
        return false;
    }
    return true;
}

static void
clamp_coords(int32_t *x, int32_t *y, uint16_t sw, uint16_t sh) {
    if (*x < 0) {
        LOGW("AI tool: clamping x=%d to 0", *x);
        *x = 0;
    }
    if (*y < 0) {
        LOGW("AI tool: clamping y=%d to 0", *y);
        *y = 0;
    }
    if (sw > 0 && *x >= sw) {
        LOGW("AI tool: clamping x=%d to %d (screen_width)", *x, sw - 1);
        *x = sw - 1;
    }
    if (sh > 0 && *y >= sh) {
        LOGW("AI tool: clamping y=%d to %d (screen_height)", *y, sh - 1);
        *y = sh - 1;
    }
}

static char *
tool_position_click(struct sc_ai_tools *tools, cJSON *args) {
    cJSON *jx = cJSON_GetObjectItem(args, "x");
    cJSON *jy = cJSON_GetObjectItem(args, "y");
    if (!jx || !jy) {
        return strdup("{\"error\": \"missing x or y\"}");
    }

    int32_t x = jx->valueint;
    int32_t y = jy->valueint;
    uint16_t sw = tools->screen_width;
    uint16_t sh = tools->screen_height;
    clamp_coords(&x, &y, sw, sh);

    // Scale from downscaled AI coordinates to original frame coordinates
    uint16_t fw = tools->frame_width ? tools->frame_width : sw;
    uint16_t fh = tools->frame_height ? tools->frame_height : sh;
    int32_t fx = (int32_t)((int64_t)x * fw / sw);
    int32_t fy = (int32_t)((int64_t)y * fh / sh);

    LOGI("AI tool: position_click(%d, %d) screen=%dx%d -> frame(%d, %d) frame=%dx%d",
         x, y, sw, sh, fx, fy, fw, fh);

    bool ok = inject_touch(tools->controller, fw, fh, fx, fy,
                           AMOTION_EVENT_ACTION_DOWN);
    if (ok) {
        ok = inject_touch(tools->controller, fw, fh, fx, fy,
                          AMOTION_EVENT_ACTION_UP);
    }

    // Brief delay to let the UI respond to the tap
    if (ok) {
        SDL_Delay(150);
    }

    return ok ? strdup("{\"success\": true}")
              : strdup("{\"error\": \"failed to inject touch\"}");
}

static char *
tool_position_long_press(struct sc_ai_tools *tools, cJSON *args) {
    cJSON *jx = cJSON_GetObjectItem(args, "x");
    cJSON *jy = cJSON_GetObjectItem(args, "y");
    cJSON *jms = cJSON_GetObjectItem(args, "duration_ms");
    if (!jx || !jy) {
        return strdup("{\"error\": \"missing x or y\"}");
    }

    int32_t x = jx->valueint;
    int32_t y = jy->valueint;
    int duration_ms = jms ? jms->valueint : 500;
    uint16_t sw = tools->screen_width;
    uint16_t sh = tools->screen_height;
    clamp_coords(&x, &y, sw, sh);

    // Scale from downscaled AI coordinates to original frame coordinates
    uint16_t fw = tools->frame_width ? tools->frame_width : sw;
    uint16_t fh = tools->frame_height ? tools->frame_height : sh;
    int32_t fx = (int32_t)((int64_t)x * fw / sw);
    int32_t fy = (int32_t)((int64_t)y * fh / sh);

    LOGI("AI tool: position_long_press(%d, %d, %dms) screen=%dx%d -> frame(%d, %d) frame=%dx%d",
         x, y, duration_ms, sw, sh, fx, fy, fw, fh);

    bool ok = inject_touch(tools->controller, fw, fh, fx, fy,
                           AMOTION_EVENT_ACTION_DOWN);
    if (ok) {
        SDL_Delay(duration_ms);
        ok = inject_touch(tools->controller, fw, fh, fx, fy,
                          AMOTION_EVENT_ACTION_UP);
    }

    return ok ? strdup("{\"success\": true}")
              : strdup("{\"error\": \"failed to inject long press\"}");
}

static char *
tool_swipe(struct sc_ai_tools *tools, cJSON *args) {
    cJSON *jx1 = cJSON_GetObjectItem(args, "x1");
    cJSON *jy1 = cJSON_GetObjectItem(args, "y1");
    cJSON *jx2 = cJSON_GetObjectItem(args, "x2");
    cJSON *jy2 = cJSON_GetObjectItem(args, "y2");
    cJSON *jms = cJSON_GetObjectItem(args, "duration_ms");

    if (!jx1 || !jy1 || !jx2 || !jy2) {
        return strdup("{\"error\": \"missing coordinates\"}");
    }

    int32_t x1 = jx1->valueint, y1 = jy1->valueint;
    int32_t x2 = jx2->valueint, y2 = jy2->valueint;
    int duration_ms = jms ? jms->valueint : 300;
    uint16_t sw = tools->screen_width;
    uint16_t sh = tools->screen_height;
    clamp_coords(&x1, &y1, sw, sh);
    clamp_coords(&x2, &y2, sw, sh);

    // Scale from downscaled AI coordinates to original frame coordinates
    uint16_t fw = tools->frame_width ? tools->frame_width : sw;
    uint16_t fh = tools->frame_height ? tools->frame_height : sh;
    int32_t fx1 = (int32_t)((int64_t)x1 * fw / sw);
    int32_t fy1 = (int32_t)((int64_t)y1 * fh / sh);
    int32_t fx2 = (int32_t)((int64_t)x2 * fw / sw);
    int32_t fy2 = (int32_t)((int64_t)y2 * fh / sh);

    LOGI("AI tool: swipe(%d,%d -> %d,%d, %dms) screen=%dx%d -> frame(%d,%d -> %d,%d) frame=%dx%d",
         x1, y1, x2, y2, duration_ms, sw, sh, fx1, fy1, fx2, fy2, fw, fh);

    bool ok = inject_touch(tools->controller, fw, fh, fx1, fy1,
                           AMOTION_EVENT_ACTION_DOWN);
    if (!ok) {
        return strdup("{\"error\": \"failed to inject swipe down\"}");
    }

    // Interpolate points over the duration
    int steps = duration_ms / 16; // ~60fps
    if (steps < 2) {
        steps = 2;
    }
    for (int i = 1; i <= steps; i++) {
        float t = (float) i / steps;
        int32_t cx = fx1 + (int32_t)((fx2 - fx1) * t);
        int32_t cy = fy1 + (int32_t)((fy2 - fy1) * t);
        SDL_Delay(16);
        inject_touch(tools->controller, fw, fh, cx, cy,
                     AMOTION_EVENT_ACTION_MOVE);
    }

    ok = inject_touch(tools->controller, fw, fh, fx2, fy2,
                      AMOTION_EVENT_ACTION_UP);

    // Brief delay to let the UI respond to the swipe
    if (ok) {
        SDL_Delay(200);
    }

    return ok ? strdup("{\"success\": true}")
              : strdup("{\"error\": \"failed to inject swipe up\"}");
}

static char *
tool_key_press(struct sc_ai_tools *tools, cJSON *args) {
    cJSON *jkc = cJSON_GetObjectItem(args, "keycode");
    if (!jkc) {
        return strdup("{\"error\": \"missing keycode\"}");
    }

    enum android_keycode kc = (enum android_keycode) jkc->valueint;
    LOGI("AI tool: key_press(%d)", kc);

    bool ok = inject_keycode(tools->controller, kc, AKEY_EVENT_ACTION_DOWN);
    if (ok) {
        ok = inject_keycode(tools->controller, kc, AKEY_EVENT_ACTION_UP);
    }

    // Brief delay to let the UI respond to the key press
    if (ok) {
        SDL_Delay(150);
    }

    return ok ? strdup("{\"success\": true}")
              : strdup("{\"error\": \"failed to inject keycode\"}");
}

static char *
tool_key_down(struct sc_ai_tools *tools, cJSON *args) {
    cJSON *jkc = cJSON_GetObjectItem(args, "keycode");
    if (!jkc) {
        return strdup("{\"error\": \"missing keycode\"}");
    }

    LOGI("AI tool: key_down(%d)", jkc->valueint);
    bool ok = inject_keycode(tools->controller,
                             (enum android_keycode) jkc->valueint,
                             AKEY_EVENT_ACTION_DOWN);

    return ok ? strdup("{\"success\": true}")
              : strdup("{\"error\": \"failed to inject key down\"}");
}

static char *
tool_key_up(struct sc_ai_tools *tools, cJSON *args) {
    cJSON *jkc = cJSON_GetObjectItem(args, "keycode");
    if (!jkc) {
        return strdup("{\"error\": \"missing keycode\"}");
    }

    LOGI("AI tool: key_up(%d)", jkc->valueint);
    bool ok = inject_keycode(tools->controller,
                             (enum android_keycode) jkc->valueint,
                             AKEY_EVENT_ACTION_UP);

    return ok ? strdup("{\"success\": true}")
              : strdup("{\"error\": \"failed to inject key up\"}");
}

static char *
tool_input_text(struct sc_ai_tools *tools, cJSON *args) {
    cJSON *jtext = cJSON_GetObjectItem(args, "text");
    if (!jtext || !jtext->valuestring) {
        return strdup("{\"error\": \"missing text\"}");
    }

    const char *text = jtext->valuestring;
    LOGI("AI tool: input_text(\"%s\")", text);

    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_INJECT_TEXT;
    msg.inject_text.text = strdup(text);
    if (!msg.inject_text.text) {
        return strdup("{\"error\": \"OOM\"}");
    }

    bool ok = sc_controller_push_msg(tools->controller, &msg);
    if (!ok) {
        free(msg.inject_text.text);
        return strdup("{\"error\": \"failed to inject text\"}");
    }

    return strdup("{\"success\": true}");
}

static char *
tool_screenshot(struct sc_ai_tools *tools, cJSON *args) {
    (void) args;

    if (!tools->agent) {
        return strdup("{\"error\": \"agent not available\"}");
    }

    struct sc_ai_agent *agent = tools->agent;

    // Capture fresh screenshot
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        return strdup("{\"error\": \"could not allocate frame\"}");
    }

    bool has_screenshot = sc_ai_frame_sink_consume(agent->frame_sink, frame);
    if (!has_screenshot) {
        av_frame_free(&frame);
        return strdup("{\"error\": \"no frame available\"}");
    }

    uint16_t orig_w = (uint16_t) frame->width;
    uint16_t orig_h = (uint16_t) frame->height;

    struct sc_ai_screenshot ss = {0};
    if (!sc_ai_screenshot_encode(&ss, frame)) {
        av_frame_free(&frame);
        return strdup("{\"error\": \"screenshot encode failed\"}");
    }
    av_frame_free(&frame);

    uint16_t w = ss.width;
    uint16_t h = ss.height;

    // Update agent state and add screenshot as user image message
    sc_mutex_lock(&agent->mutex);
    free(agent->latest_png_data);
    agent->latest_png_data = malloc(ss.png_size);
    if (agent->latest_png_data) {
        memcpy(agent->latest_png_data, ss.png_data, ss.png_size);
        agent->latest_png_size = ss.png_size;
    }
    agent->screen_width = w;
    agent->screen_height = h;
    sc_ai_tools_set_screen_size(tools, w, h);
    sc_ai_tools_set_frame_size(tools, orig_w, orig_h);

    char text[256];
    snprintf(text, sizeof(text),
             "[Screen: %dx%d pixels] (fresh screenshot)", w, h);
    sc_ai_message_list_push_image(&agent->messages, text, ss.base64_data);
    sc_mutex_unlock(&agent->mutex);

    sc_ai_screenshot_destroy(&ss);

    char result[256];
    snprintf(result, sizeof(result),
             "{\"success\": true, \"width\": %d, \"height\": %d}", w, h);
    return strdup(result);
}

char *
sc_ai_tools_execute(struct sc_ai_tools *tools,
                    const char *function_name,
                    const char *arguments_json) {
    cJSON *args = NULL;
    if (arguments_json && arguments_json[0]) {
        args = cJSON_Parse(arguments_json);
    }
    if (!args) {
        // Default to empty object for tools with no required parameters
        args = cJSON_CreateObject();
    }

    char *result;
    if (strcmp(function_name, "position_click") == 0) {
        result = tool_position_click(tools, args);
    } else if (strcmp(function_name, "position_long_press") == 0) {
        result = tool_position_long_press(tools, args);
    } else if (strcmp(function_name, "swipe") == 0) {
        result = tool_swipe(tools, args);
    } else if (strcmp(function_name, "key_press") == 0) {
        result = tool_key_press(tools, args);
    } else if (strcmp(function_name, "key_down") == 0) {
        result = tool_key_down(tools, args);
    } else if (strcmp(function_name, "key_up") == 0) {
        result = tool_key_up(tools, args);
    } else if (strcmp(function_name, "input_text") == 0) {
        result = tool_input_text(tools, args);
    } else if (strcmp(function_name, "screenshot") == 0) {
        result = tool_screenshot(tools, args);
    } else {
        size_t len = strlen(function_name) + 64;
        result = malloc(len);
        if (result) {
            snprintf(result, len, "{\"error\": \"unknown tool: %s\"}",
                     function_name);
        }
    }

    cJSON_Delete(args);
    return result;
}
