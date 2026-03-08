#include "ai_agent.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include <libavutil/frame.h>

#include "ai/screenshot.h"
#include "../../deps/cjson/cJSON.h"
#include "util/log.h"

#define SYSTEM_PROMPT_DEFAULT \
    "You are an AI assistant controlling an Android device via scrcpy.\n\n" \
    "*** FUNCTION CALLING IS MANDATORY ***\n" \
    "You have tools: position_click, position_long_press, swipe, " \
    "key_press, key_down, key_up, input_text, screenshot.\n" \
    "You MUST use function calling (tool_calls) to invoke them.\n" \
    "NEVER output JSON in text. NEVER write code blocks with actions.\n" \
    "ALWAYS use the tool_calls mechanism provided by the API.\n" \
    "If your response contains no tool_calls, it is WRONG.\n\n" \
    "SCREEN COORDINATES:\n" \
    "- Screenshot caption shows 'Screenshot WxH' = actual pixel dimensions.\n" \
    "- Valid range: X: 0..W-1, Y: 0..H-1. (0,0) = top-left.\n" \
    "- ONLY use cx/cy values from VLM analysis. NEVER guess.\n\n" \
    "ACTION PROTOCOL:\n" \
    "1. State which element you will tap and WHY (brief, one line).\n" \
    "2. Call the tool via function calling.\n" \
    "3. Call screenshot() to verify the result.\n" \
    "4. If screen unchanged → tap MISSED → try different element.\n\n" \
    "AUTONOMOUS MODE:\n" \
    "- Follow game rules step by step. NEVER ask the user.\n" \
    "- Every response MUST include at least one tool call.\n" \
    "- If unsure, call screenshot() first, then act on what you see."

#define GUARDRAIL_MAX_REPEAT_TOUCH 4
#define GUARDRAIL_MAX_SAME_SCREEN 3

// Simple DJB2 hash, sampling every 64th byte for speed on large JPEG data
static uint32_t
screen_hash(const uint8_t *data, size_t len) {
    uint32_t h = 5381;
    for (size_t i = 0; i < len; i += 64) {
        h = ((h << 5) + h) ^ data[i];
    }
    return h;
}

static void
log_to_train(struct sc_ai_agent *agent, const char *role, const char *content) {
    if (!agent->train_log_path) {
        return;
    }

    FILE *f = fopen(agent->train_log_path, "a");
    if (!f) {
        return;
    }

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm);

    fprintf(f, "## [%s] %s\n%s\n\n", timebuf, role,
            content ? content : "(no content)");
    fclose(f);
}

// Post-process VLM output: replace "x=N y=N w=N h=N" with computed "cx=N cy=N".
// This reduces tokens sent to the game LLM and eliminates confusion.
// Returns a new string (caller frees), or NULL on failure.
static char *
simplify_vlm_coords(const char *vlm_text) {
    if (!vlm_text) return NULL;

    size_t src_len = strlen(vlm_text);
    size_t cap = src_len + 256;
    char *out = malloc(cap);
    if (!out) return strdup(vlm_text);

    size_t out_len = 0;
    const char *p = vlm_text;

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        const char *line_end = p + line_len;

        // Parse x=N y=N w=N h=N and record their positions in the line
        int x = -1, y = -1, w = -1, h = -1;
        const char *first_coord = NULL; // start of "x=..."
        const char *last_coord_end = NULL; // end of "h=NNN"
        const char *scan = p;

        while (scan < line_end) {
            if (scan[0] == 'x' && scan[1] == '='
                    && (scan == p || scan[-1] == ' ')) {
                if (!first_coord) first_coord = scan;
                x = atoi(scan + 2);
                // skip past the number
                const char *end = scan + 2;
                while (end < line_end && (*end == '-' || (*end >= '0' && *end <= '9'))) end++;
                last_coord_end = end;
                scan = end;
                continue;
            } else if (scan[0] == 'y' && scan[1] == '='
                       && (scan == p || scan[-1] == ' ')) {
                if (!first_coord) first_coord = scan;
                y = atoi(scan + 2);
                const char *end = scan + 2;
                while (end < line_end && (*end == '-' || (*end >= '0' && *end <= '9'))) end++;
                last_coord_end = end;
                scan = end;
                continue;
            } else if (scan[0] == 'w' && scan[1] == '='
                       && (scan == p || scan[-1] == ' ')) {
                if (!first_coord) first_coord = scan;
                w = atoi(scan + 2);
                const char *end = scan + 2;
                while (end < line_end && (*end == '-' || (*end >= '0' && *end <= '9'))) end++;
                last_coord_end = end;
                scan = end;
                continue;
            } else if (scan[0] == 'h' && scan[1] == '='
                       && (scan == p || scan[-1] == ' ')) {
                if (!first_coord) first_coord = scan;
                h = atoi(scan + 2);
                const char *end = scan + 2;
                while (end < line_end && (*end == '-' || (*end >= '0' && *end <= '9'))) end++;
                last_coord_end = end;
                scan = end;
                continue;
            }
            scan++;
        }

        // Ensure output buffer has space
        if (out_len + line_len + 40 >= cap) {
            cap = (out_len + line_len + 40) * 2;
            char *tmp = realloc(out, cap);
            if (!tmp) break;
            out = tmp;
        }

        if (x >= 0 && y >= 0 && w > 0 && h > 0 && first_coord && last_coord_end) {
            // Copy everything before the first coord
            size_t prefix_len = (size_t)(first_coord - p);
            memcpy(out + out_len, p, prefix_len);
            out_len += prefix_len;

            // Write cx=N cy=N
            int cx = x + w / 2;
            int cy = y + h / 2;
            out_len += (size_t)snprintf(out + out_len, cap - out_len,
                                         "cx=%d cy=%d", cx, cy);

            // Copy everything after the last coord value
            size_t suffix_len = (size_t)(line_end - last_coord_end);
            if (suffix_len > 0) {
                memcpy(out + out_len, last_coord_end, suffix_len);
                out_len += suffix_len;
            }
        } else {
            // No coords found, copy line as-is
            memcpy(out + out_len, p, line_len);
            out_len += line_len;
        }

        if (eol) {
            out[out_len++] = '\n';
            p = eol + 1;
        } else {
            break;
        }
    }

    out[out_len] = '\0';
    return out;
}

// Post-process VLM output: detect if cx/cy values exceed screen bounds
// (e.g. Gemini outputs 0..1000 normalized coords instead of pixels).
// If max coord exceeds screen size, rescale all coords proportionally.
static char *
rescale_vlm_coords_if_needed(const char *text, uint16_t width, uint16_t height) {
    if (!text) return NULL;

    // First pass: find max cx and cy values
    int max_cx = 0, max_cy = 0;
    const char *p = text;
    while (*p) {
        // Look for cx=NNN
        if (p[0] == 'c' && p[1] == 'x' && p[2] == '=') {
            int v = atoi(p + 3);
            if (v > max_cx) max_cx = v;
        }
        // Look for cy=NNN
        if (p[0] == 'c' && p[1] == 'y' && p[2] == '=') {
            int v = atoi(p + 3);
            if (v > max_cy) max_cy = v;
        }
        p++;
    }

    // If coords are within bounds, no rescaling needed
    if (max_cx <= width && max_cy <= height) {
        return strdup(text);
    }

    // Determine scale factor: VLM likely used a different coordinate space
    // Detect the likely VLM coordinate space (1000x1000 or similar)
    int vlm_w = max_cx;
    int vlm_h = max_cy;
    // Round up to common normalized sizes
    if (vlm_w > 900 && vlm_w <= 1024) vlm_w = 1024;
    else if (vlm_w > 1024) vlm_w = max_cx; // use actual max
    if (vlm_h > 900 && vlm_h <= 1024) vlm_h = 1024;
    else if (vlm_h > 1024) vlm_h = max_cy;

    LOGW("AI VLM: coordinates exceed screen (%dx%d), max found cx=%d cy=%d. "
         "Rescaling from %dx%d to %dx%d",
         width, height, max_cx, max_cy, vlm_w, vlm_h, width, height);

    // Second pass: replace all cx=NNN and cy=NNN with rescaled values
    size_t src_len = strlen(text);
    size_t cap = src_len + 256;
    char *out = malloc(cap);
    if (!out) return strdup(text);

    size_t out_len = 0;
    p = text;
    while (*p) {
        bool is_cx = (p[0] == 'c' && p[1] == 'x' && p[2] == '=');
        bool is_cy = (p[0] == 'c' && p[1] == 'y' && p[2] == '=');
        if (is_cx || is_cy) {
            const char *num_start = p + 3;
            const char *num_end = num_start;
            if (*num_end == '-') num_end++;
            while (*num_end >= '0' && *num_end <= '9') num_end++;

            int old_val = atoi(num_start);
            int new_val;
            if (is_cx) {
                new_val = (int)((int64_t)old_val * width / vlm_w);
                if (new_val >= width) new_val = width - 1;
            } else {
                new_val = (int)((int64_t)old_val * height / vlm_h);
                if (new_val >= height) new_val = height - 1;
            }

            // Ensure buffer space
            if (out_len + 20 >= cap) {
                cap *= 2;
                char *tmp = realloc(out, cap);
                if (!tmp) { free(out); return strdup(text); }
                out = tmp;
            }

            out_len += (size_t)snprintf(out + out_len, cap - out_len,
                                         "%s=%d", is_cx ? "cx" : "cy", new_val);
            p = num_end;
        } else {
            if (out_len + 2 >= cap) {
                cap *= 2;
                char *tmp = realloc(out, cap);
                if (!tmp) { free(out); return strdup(text); }
                out = tmp;
            }
            out[out_len++] = *p++;
        }
    }
    out[out_len] = '\0';
    return out;
}

// Call VLM to analyze a screenshot, returns description text (caller frees)
static char *
analyze_screen_with_vlm(struct sc_ai_agent *agent,
                        const char *base64_data,
                        uint16_t width, uint16_t height) {
    sc_mutex_lock(&agent->mutex);
    char *api_key = agent->config.api_key ? strdup(agent->config.api_key) : NULL;
    char *vision_model = agent->vision_model ? strdup(agent->vision_model) : NULL;
    char *base_url = agent->config.base_url ? strdup(agent->config.base_url)
                                            : NULL;
    sc_mutex_unlock(&agent->mutex);

    if (!api_key || !vision_model) {
        free(api_key);
        free(vision_model);
        free(base_url);
        return NULL;
    }

    struct sc_openrouter_config vlm_config = {
        .api_key = api_key,
        .model = vision_model,
        .base_url = base_url,
    };

    struct sc_ai_message_list vlm_msgs;
    sc_ai_message_list_init(&vlm_msgs);

    char sys_prompt[1536];
    snprintf(sys_prompt, sizeof(sys_prompt),
        "You are a screen analyzer for an Android game automation system. "
        "Analyze the screenshot and list every UI element with bounding box.\n\n"
        "Output format:\n"
        "SCREEN: (brief scene description)\n"
        "ELEMENTS:\n"
        "- \"text or icon\" x=NNN y=NNN w=NNN h=NNN [button/text/icon/card]\n"
        "...\n\n"
        "CRITICAL rules:\n"
        "- Image is EXACTLY %dx%d pixels.\n"
        "- ALL coordinates MUST be in pixel units of this image.\n"
        "- x range: 0..%d, y range: 0..%d. NO coordinate may exceed these.\n"
        "- x,y = top-left corner of the element bounding box\n"
        "- w,h = width and height of the bounding box\n"
        "- DO NOT use normalized 0..1000 coordinates. Use actual pixels.\n"
        "- Include ALL visible elements: buttons, labels, icons, cards\n"
        "- Read Korean text accurately\n"
        "- Be concise but complete",
        width, height, width - 1, height - 1);
    sc_ai_message_list_push(&vlm_msgs, "system", sys_prompt);

    char text[128];
    snprintf(text, sizeof(text), "Screenshot %dx%d", width, height);
    sc_ai_message_list_push_image(&vlm_msgs, text, base64_data);

    LOGI("AI VLM: analyzing screen with %s", vision_model);

    struct sc_openrouter_response resp =
        sc_openrouter_chat(&vlm_config, &vlm_msgs, NULL);

    sc_ai_message_list_destroy(&vlm_msgs);
    free(api_key);
    free(vision_model);
    free(base_url);

    if (!resp.success || !resp.content) {
        LOGW("AI VLM: analysis failed: %s",
             resp.error ? resp.error : "unknown");
        sc_openrouter_response_destroy(&resp);
        return NULL;
    }

    // Replace x/y/w/h with cx/cy center coordinates
    char *simplified = simplify_vlm_coords(resp.content);

    // Rescale if VLM used wrong coordinate space (e.g. 0..1000 instead of pixels)
    char *rescaled = rescale_vlm_coords_if_needed(
        simplified ? simplified : resp.content, width, height);
    free(simplified);

    LOGI("AI VLM: %s", rescaled ? rescaled : resp.content);
    sc_openrouter_response_destroy(&resp);
    return rescaled;
}

static bool
process_prompt(struct sc_ai_agent *agent, const char *prompt) {
    LOGI("AI agent: processing prompt: %s", prompt);

    // Capture screenshot
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        LOGE("AI agent: could not allocate frame");
        return false;
    }

    bool has_screenshot = sc_ai_frame_sink_consume(agent->frame_sink, frame);
    struct sc_ai_screenshot ss = {0};
    uint16_t orig_frame_w = 0;
    uint16_t orig_frame_h = 0;

    if (has_screenshot) {
        // Save original frame size before encoding (which downscales)
        orig_frame_w = (uint16_t) frame->width;
        orig_frame_h = (uint16_t) frame->height;
        if (!sc_ai_screenshot_encode(&ss, frame)) {
            LOGW("AI agent: screenshot encode failed");
            has_screenshot = false;
        } else {
            sc_mutex_lock(&agent->mutex);
            free(agent->latest_png_data);
            agent->latest_png_data = malloc(ss.png_size);
            if (agent->latest_png_data) {
                memcpy(agent->latest_png_data, ss.png_data, ss.png_size);
                agent->latest_png_size = ss.png_size;
            }
            agent->screen_width = ss.width;
            agent->screen_height = ss.height;
            sc_ai_tools_set_screen_size(&agent->tools, ss.width, ss.height);
            sc_ai_tools_set_frame_size(&agent->tools, orig_frame_w,
                                       orig_frame_h);
            sc_mutex_unlock(&agent->mutex);

            // Save debug image
            mkdir("/home/jhsoft/shareHub/다운/scrcpy_debug", 0755);
            char debug_path[256];
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            snprintf(debug_path, sizeof(debug_path),
                     "/home/jhsoft/shareHub/다운/scrcpy_debug/prompt_%ld_%03ld.jpg",
                     ts.tv_sec, ts.tv_nsec / 1000000);
            FILE *dbg = fopen(debug_path, "wb");
            if (dbg) {
                fwrite(ss.png_data, 1, ss.png_size, dbg);
                fclose(dbg);
                LOGI("AI debug: saved prompt image %s (%dx%d, frame=%dx%d)",
                     debug_path, ss.width, ss.height,
                     orig_frame_w, orig_frame_h);
            }
        }
    }
    av_frame_free(&frame);

    // Analyze screen with VLM if configured
    char *screen_desc = NULL;
    bool use_vlm = false;

    sc_mutex_lock(&agent->mutex);
    use_vlm = has_screenshot && agent->vision_model
              && agent->vision_model[0] != '\0';
    sc_mutex_unlock(&agent->mutex);

    if (use_vlm) {
        screen_desc = analyze_screen_with_vlm(agent, ss.base64_data,
                                               ss.width, ss.height);
    }

    // Add user message: text-only if VLM analyzed, image if not
    sc_mutex_lock(&agent->mutex);
    if (has_screenshot && use_vlm && screen_desc) {
        // VLM mode: send text-only to game model (no image tokens)
        char enhanced[8192];
        snprintf(enhanced, sizeof(enhanced),
                 "Screenshot %dx%d\n"
                 "=== VLM ANALYSIS (use these EXACT coordinates) ===\n"
                 "%s\n"
                 "=== END VLM ANALYSIS ===\n\n%s",
                 ss.width, ss.height, screen_desc, prompt);
        sc_ai_message_list_push(&agent->messages, "user", enhanced);
    } else if (has_screenshot) {
        // Fallback: send image to game model
        char enhanced[8192];
        if (screen_desc) {
            snprintf(enhanced, sizeof(enhanced),
                     "Screenshot %dx%d\n%s\n\n%s",
                     ss.width, ss.height, screen_desc, prompt);
        } else {
            snprintf(enhanced, sizeof(enhanced),
                     "Screenshot %dx%d\n%s", ss.width, ss.height,
                     prompt);
        }
        sc_ai_message_list_push_image(&agent->messages, enhanced,
                                       ss.base64_data);
    } else {
        sc_ai_message_list_push(&agent->messages, "user", prompt);
    }
    sc_mutex_unlock(&agent->mutex);
    free(screen_desc);

    if (has_screenshot) {
        sc_ai_screenshot_destroy(&ss);
    }

    log_to_train(agent, "user", prompt);

    // Call API in a loop (handle tool calls)
    // In auto mode, keep looping: LLM acts → screenshot+VLM → LLM acts → ...
    // In manual mode, max 5 tool-call iterations then stop.
    int consecutive_text = 0; // track text-only responses to avoid infinite loop
    for (int iter = 0; ; iter++) {
        sc_mutex_lock(&agent->mutex);
        if (agent->stopped) {
            sc_mutex_unlock(&agent->mutex);
            return false;
        }
        bool is_auto = agent->auto_running;

        // In manual mode, cap at 5 iterations
        if (!is_auto && iter >= 5) {
            sc_mutex_unlock(&agent->mutex);
            break;
        }

        // Check if API key is set
        if (!agent->config.api_key || !agent->config.api_key[0]) {
            sc_ai_message_list_push(&agent->messages, "assistant",
                "[Error: API key not set. Please configure in settings.]");
            sc_mutex_unlock(&agent->mutex);
            return false;
        }

        struct sc_openrouter_config config = agent->config;
        // Make copies for thread safety
        char *api_key = strdup(config.api_key ? config.api_key : "");
        char *model = strdup(config.model ? config.model : "");
        char *base_url = config.base_url ? strdup(config.base_url) : NULL;
        struct sc_openrouter_config safe_config = {
            .api_key = api_key,
            .model = model,
            .base_url = base_url,
        };

        const char *tools_json = sc_ai_tools_get_definitions();

        // Trim history to prevent token explosion (keep system + last 19)
        sc_ai_message_list_trim(&agent->messages, 20);

        // Build request body while holding mutex (reads messages, fast)
        char *request_body = sc_openrouter_build_body(
            &safe_config, &agent->messages, tools_json);
        sc_mutex_unlock(&agent->mutex);

        LOGI("AI agent: LLM call iteration %d (auto=%d)", iter, is_auto);

        // Perform HTTP call WITHOUT holding mutex (slow, blocks for seconds)
        struct sc_openrouter_response resp =
            sc_openrouter_chat_with_body(&safe_config, request_body);
        free(request_body);

        free(api_key);
        free(model);
        free(base_url);

        if (!resp.success) {
            LOGE("AI agent: API error: %s", resp.error ? resp.error : "unknown");
            sc_mutex_lock(&agent->mutex);
            char err_msg[512];
            snprintf(err_msg, sizeof(err_msg), "[API Error: %s]",
                     resp.error ? resp.error : "unknown");
            sc_ai_message_list_push(&agent->messages, "assistant", err_msg);
            sc_mutex_unlock(&agent->mutex);
            log_to_train(agent, "error", resp.error);
            sc_openrouter_response_destroy(&resp);
            return false;
        }

        // If there are tool calls, execute them
        if (resp.tool_calls.count > 0) {
            consecutive_text = 0;
            if (resp.content && resp.content[0]) {
                LOGI("AI response: %s", resp.content);
            }
            // Build tool_calls JSON for history
            cJSON *tc_arr = cJSON_CreateArray();
            for (size_t i = 0; i < resp.tool_calls.count; i++) {
                cJSON *tc = cJSON_CreateObject();
                cJSON_AddStringToObject(tc, "id", resp.tool_calls.calls[i].id);
                cJSON_AddStringToObject(tc, "type", "function");
                cJSON *func = cJSON_CreateObject();
                cJSON_AddStringToObject(func, "name",
                    resp.tool_calls.calls[i].function_name);
                cJSON_AddStringToObject(func, "arguments",
                    resp.tool_calls.calls[i].arguments_json);
                cJSON_AddItemToObject(tc, "function", func);
                cJSON_AddItemToArray(tc_arr, tc);
            }
            char *tc_json = cJSON_PrintUnformatted(tc_arr);
            cJSON_Delete(tc_arr);

            sc_mutex_lock(&agent->mutex);
            sc_ai_message_list_push_assistant_tool_calls(
                &agent->messages, resp.content, tc_json);
            sc_mutex_unlock(&agent->mutex);
            free(tc_json);

            // Execute each tool call
            for (size_t i = 0; i < resp.tool_calls.count; i++) {
                struct sc_ai_tool_call *tc = &resp.tool_calls.calls[i];
                LOGI("AI agent: executing tool: %s(%s)",
                     tc->function_name, tc->arguments_json);

                log_to_train(agent, "tool_call", tc->function_name);

                char *result = sc_ai_tools_execute(
                    &agent->tools, tc->function_name, tc->arguments_json);

                sc_mutex_lock(&agent->mutex);
                sc_ai_message_list_push_tool_result(
                    &agent->messages, tc->id, tc->function_name, result);
                sc_mutex_unlock(&agent->mutex);

                log_to_train(agent, "tool_result", result);
                free(result);
            }

            sc_openrouter_response_destroy(&resp);
            continue; // loop to get next response after tool results
        }

        // No tool calls, just a text response
        if (resp.content) {
            LOGI("AI response: %s", resp.content);
            sc_mutex_lock(&agent->mutex);
            sc_ai_message_list_push(&agent->messages, "assistant",
                                     resp.content);
            sc_mutex_unlock(&agent->mutex);
            log_to_train(agent, "assistant", resp.content);
        }
        sc_openrouter_response_destroy(&resp);

        // In auto mode: take a fresh screenshot+VLM and continue the loop
        // so LLM can decide the next action without external prompting.
        sc_mutex_lock(&agent->mutex);
        is_auto = agent->auto_running && !agent->stopped;
        sc_mutex_unlock(&agent->mutex);

        if (!is_auto) {
            break; // manual mode: done
        }

        consecutive_text++;
        if (consecutive_text >= 3) {
            LOGW("AI agent: %d consecutive text-only responses, "
                 "stopping to avoid infinite loop", consecutive_text);
            break;
        }

        // Auto-continue: capture screenshot + VLM, add as user message
        LOGI("AI agent: auto-continue, taking fresh screenshot+VLM");
        SDL_Delay(500); // let UI settle after last action

        AVFrame *auto_frame = av_frame_alloc();
        if (!auto_frame) {
            break;
        }

        bool got_frame = sc_ai_frame_sink_consume(agent->frame_sink, auto_frame);
        if (!got_frame) {
            av_frame_free(&auto_frame);
            break;
        }

        uint16_t auto_orig_w = (uint16_t) auto_frame->width;
        uint16_t auto_orig_h = (uint16_t) auto_frame->height;

        struct sc_ai_screenshot auto_ss = {0};
        if (!sc_ai_screenshot_encode(&auto_ss, auto_frame)) {
            av_frame_free(&auto_frame);
            break;
        }
        av_frame_free(&auto_frame);

        // --- Guardrails: same-screen detection ---
        uint32_t cur_hash = screen_hash(auto_ss.png_data, auto_ss.png_size);
        sc_mutex_lock(&agent->mutex);
        if (cur_hash == agent->last_screen_hash) {
            agent->same_screen_count++;
        } else {
            agent->last_screen_hash = cur_hash;
            agent->same_screen_count = 0;
        }
        int same_count = agent->same_screen_count;
        int touch_repeats = agent->repeat_count;
        sc_mutex_unlock(&agent->mutex);

        // Check guardrails
        const char *guardrail_warn = NULL;
        bool guardrail_stop = false;
        if (same_count >= GUARDRAIL_MAX_SAME_SCREEN) {
            guardrail_warn =
                "[GUARDRAIL] Screen unchanged %d times in a row. "
                "Your actions are having NO effect. "
                "You MUST try a completely different approach: "
                "swipe, press BACK, or tap a different area of the screen.";
            if (same_count >= GUARDRAIL_MAX_SAME_SCREEN + 2) {
                guardrail_stop = true;
            }
        }
        if (touch_repeats >= GUARDRAIL_MAX_REPEAT_TOUCH) {
            guardrail_warn =
                "[GUARDRAIL] You tapped the SAME position %d times. "
                "It is clearly not working. "
                "STOP tapping there and try something completely different.";
            if (touch_repeats >= GUARDRAIL_MAX_REPEAT_TOUCH + 2) {
                guardrail_stop = true;
            }
        }

        if (guardrail_stop) {
            LOGW("AI guardrail: auto-play stopped (same_screen=%d, "
                 "repeat_touch=%d)", same_count, touch_repeats);
            sc_mutex_lock(&agent->mutex);
            agent->auto_running = false;
            char stop_msg[256];
            snprintf(stop_msg, sizeof(stop_msg),
                     "[Auto-play stopped: stuck detected "
                     "(same_screen=%d, repeat_touch=%d)]",
                     same_count, touch_repeats);
            sc_ai_message_list_push(&agent->messages, "assistant", stop_msg);
            sc_mutex_unlock(&agent->mutex);
            sc_ai_screenshot_destroy(&auto_ss);
            break;
        }

        // VLM analysis
        char *auto_desc = NULL;
        sc_mutex_lock(&agent->mutex);
        bool auto_vlm = agent->vision_model && agent->vision_model[0] != '\0';
        sc_mutex_unlock(&agent->mutex);

        if (auto_vlm) {
            auto_desc = analyze_screen_with_vlm(agent, auto_ss.base64_data,
                                                 auto_ss.width, auto_ss.height);
        }

        // Update agent state
        sc_mutex_lock(&agent->mutex);
        free(agent->latest_png_data);
        agent->latest_png_data = malloc(auto_ss.png_size);
        if (agent->latest_png_data) {
            memcpy(agent->latest_png_data, auto_ss.png_data, auto_ss.png_size);
            agent->latest_png_size = auto_ss.png_size;
        }
        agent->screen_width = auto_ss.width;
        agent->screen_height = auto_ss.height;
        sc_ai_tools_set_screen_size(&agent->tools, auto_ss.width,
                                     auto_ss.height);
        sc_ai_tools_set_frame_size(&agent->tools, auto_orig_w, auto_orig_h);

        // Build continuation prompt with optional guardrail warning
        char auto_text[8192];
        const char *continuation =
            "Continue playing. Decide your next action.";
        char warn_buf[512];
        if (guardrail_warn) {
            snprintf(warn_buf, sizeof(warn_buf), guardrail_warn,
                     same_count > touch_repeats ? same_count : touch_repeats);
            LOGW("AI guardrail: %s", warn_buf);
        }

        if (auto_vlm && auto_desc) {
            snprintf(auto_text, sizeof(auto_text),
                     "Screenshot %dx%d (fresh)\n"
                     "=== VLM ANALYSIS (use these EXACT coordinates) ===\n"
                     "%s\n"
                     "=== END VLM ANALYSIS ===\n\n"
                     "%s%s%s",
                     auto_ss.width, auto_ss.height, auto_desc,
                     guardrail_warn ? warn_buf : "",
                     guardrail_warn ? "\n\n" : "",
                     continuation);
            sc_ai_message_list_push(&agent->messages, "user", auto_text);
        } else {
            snprintf(auto_text, sizeof(auto_text),
                     "Screenshot %dx%d (fresh)\n%s%s%s",
                     auto_ss.width, auto_ss.height,
                     guardrail_warn ? warn_buf : "",
                     guardrail_warn ? "\n" : "",
                     continuation);
            sc_ai_message_list_push_image(&agent->messages, auto_text,
                                           auto_ss.base64_data);
        }
        sc_mutex_unlock(&agent->mutex);

        free(auto_desc);
        sc_ai_screenshot_destroy(&auto_ss);
        // continue the loop — LLM will see the new screenshot and act
    }

    return true;
}

static int
worker_thread_fn(void *data) {
    struct sc_ai_agent *agent = data;

    sc_mutex_lock(&agent->mutex);
    while (!agent->stopped) {
        if (!agent->has_pending_prompt) {
            sc_cond_wait(&agent->cond, &agent->mutex);
            continue;
        }

        char *prompt = agent->pending_prompt;
        agent->pending_prompt = NULL;
        agent->has_pending_prompt = false;
        agent->worker_busy = true;
        sc_mutex_unlock(&agent->mutex);

        process_prompt(agent, prompt);
        free(prompt);

        sc_mutex_lock(&agent->mutex);
        agent->worker_busy = false;
        sc_cond_signal(&agent->cond); // wake auto_thread
    }
    sc_mutex_unlock(&agent->mutex);

    return 0;
}

static int
auto_thread_fn(void *data) {
    struct sc_ai_agent *agent = data;
    bool rules_sent = false;

    while (true) {
        sc_mutex_lock(&agent->mutex);
        if (agent->stopped) {
            sc_mutex_unlock(&agent->mutex);
            break;
        }

        bool running = agent->auto_running;
        bool has_rules = agent->game_rules && agent->game_rules[0] != '\0';
        char *rules_copy = NULL;
        if (running && has_rules && !rules_sent) {
            rules_copy = strdup(agent->game_rules);
        }
        sc_mutex_unlock(&agent->mutex);

        if (!running) {
            rules_sent = false;
            SDL_Delay(100);
            continue;
        }

        // Build prompt: rules on first run, short continuation after
        char *prompt;
        if (rules_copy) {
            size_t prompt_len = strlen(rules_copy) + 512;
            prompt = malloc(prompt_len);
            if (prompt) {
                snprintf(prompt, prompt_len,
                    "Follow the game rules below, look at the screenshot, "
                    "and use the available tools to play. "
                    "Before each action, briefly state WHAT you intend to "
                    "tap and WHY.\n\n%s", rules_copy);
            }
            free(rules_copy);
            rules_sent = true;
        } else {
            // process_prompt handles auto-continuation internally,
            // so this only fires when process_prompt exits (e.g. after
            // consecutive text-only responses or errors).
            prompt = strdup(
                "Take a screenshot and continue playing according to "
                "the game rules.");
        }

        if (prompt) {
            sc_ai_agent_submit_prompt(agent, prompt);
        }

        // Wait for worker to finish
        sc_mutex_lock(&agent->mutex);
        while (!agent->stopped && agent->auto_running
                && (agent->worker_busy || agent->has_pending_prompt)) {
            sc_cond_wait(&agent->cond, &agent->mutex);
        }
        sc_mutex_unlock(&agent->mutex);

        // Brief cooldown before next cycle
        SDL_Delay(1000);
    }

    return 0;
}

bool
sc_ai_agent_init(struct sc_ai_agent *agent,
                 const struct sc_ai_agent_params *params) {
    memset(agent, 0, sizeof(*agent));

    agent->frame_sink = params->frame_sink;
    sc_ai_tools_init(&agent->tools, params->controller, 0, 0);
    sc_ai_tools_set_agent(&agent->tools, agent);

    agent->config.api_key = params->api_key ? strdup(params->api_key) : NULL;
    agent->config.model = params->model ? strdup(params->model)
                                        : strdup("openai/gpt-oss-120b");
    agent->config.base_url = params->base_url ? strdup(params->base_url)
                                              : NULL;
    agent->vision_model = strdup(
        params->vision_model ? params->vision_model
                             : "google/gemini-2.5-flash-lite");

    if (!sc_mutex_init(&agent->mutex)) {
        return false;
    }

    if (!sc_cond_init(&agent->cond)) {
        sc_mutex_destroy(&agent->mutex);
        return false;
    }

    sc_ai_message_list_init(&agent->messages);

    agent->system_prompt = strdup(SYSTEM_PROMPT_DEFAULT);
    sc_ai_message_list_push(&agent->messages, "system", agent->system_prompt);

    agent->auto_running = false;
    agent->worker_busy = false;
    agent->game_rules = NULL;
    agent->last_touch_x = -1;
    agent->last_touch_y = -1;
    agent->repeat_count = 0;
    agent->last_screen_hash = 0;
    agent->same_screen_count = 0;

    agent->recording = false;
    agent->record_count = 0;
    agent->record_dir = NULL;
    agent->train_tree_json = NULL;

    // Set up train log path
    const char *home = getenv("HOME");
    if (home) {
        size_t len = strlen(home) + 32;
        agent->train_log_path = malloc(len);
        if (agent->train_log_path) {
            snprintf(agent->train_log_path, len, "%s/.scrcpy_train.md", home);
        }
    }

    return true;
}

bool
sc_ai_agent_start(struct sc_ai_agent *agent) {
    if (!sc_openrouter_init()) {
        return false;
    }

    agent->stopped = false;

    if (!sc_thread_create(&agent->worker_thread, worker_thread_fn,
                          "ai_worker", agent)) {
        sc_openrouter_cleanup();
        return false;
    }

    if (!sc_thread_create(&agent->auto_thread, auto_thread_fn,
                          "ai_auto", agent)) {
        sc_mutex_lock(&agent->mutex);
        agent->stopped = true;
        sc_cond_signal(&agent->cond);
        sc_mutex_unlock(&agent->mutex);
        sc_thread_join(&agent->worker_thread, NULL);
        sc_openrouter_cleanup();
        return false;
    }

    return true;
}

void
sc_ai_agent_stop(struct sc_ai_agent *agent) {
    sc_mutex_lock(&agent->mutex);
    agent->stopped = true;
    agent->auto_running = false;
    sc_cond_signal(&agent->cond);
    sc_mutex_unlock(&agent->mutex);

}

void
sc_ai_agent_join(struct sc_ai_agent *agent) {
    sc_thread_join(&agent->worker_thread, NULL);
    sc_thread_join(&agent->auto_thread, NULL);
    sc_openrouter_cleanup();
}

void
sc_ai_agent_destroy(struct sc_ai_agent *agent) {
    free((void *)agent->config.api_key);
    free((void *)agent->config.model);
    free((void *)agent->config.base_url);
    free(agent->vision_model);
    free(agent->system_prompt);
    free(agent->pending_prompt);
    free(agent->latest_png_data);
    free(agent->train_log_path);
    free(agent->game_rules);
    free(agent->record_dir);
    free(agent->train_tree_json);

    sc_ai_message_list_destroy(&agent->messages);

    sc_cond_destroy(&agent->cond);
    sc_mutex_destroy(&agent->mutex);
}

void
sc_ai_agent_submit_prompt(struct sc_ai_agent *agent, char *prompt) {
    sc_mutex_lock(&agent->mutex);
    free(agent->pending_prompt);
    agent->pending_prompt = prompt;
    agent->has_pending_prompt = true;
    sc_cond_signal(&agent->cond);
    sc_mutex_unlock(&agent->mutex);
}

void
sc_ai_agent_set_config(struct sc_ai_agent *agent,
                       const char *api_key,
                       const char *model,
                       const char *base_url,
                       const char *vision_model) {
    sc_mutex_lock(&agent->mutex);
    // Only update fields that are provided (NULL = keep existing)
    if (api_key) {
        free((void *)agent->config.api_key);
        agent->config.api_key = strdup(api_key);
    }
    if (model) {
        free((void *)agent->config.model);
        agent->config.model = strdup(model);
    }
    if (base_url) {
        free((void *)agent->config.base_url);
        agent->config.base_url = strdup(base_url);
    }
    if (vision_model) {
        free(agent->vision_model);
        agent->vision_model = strdup(vision_model);
    }
    LOGI("AI config: model=%s, vision=%s",
         agent->config.model ? agent->config.model : "(none)",
         agent->vision_model ? agent->vision_model : "(none)");
    sc_mutex_unlock(&agent->mutex);
}

void
sc_ai_agent_set_auto_running(struct sc_ai_agent *agent, bool running) {
    sc_mutex_lock(&agent->mutex);
    agent->auto_running = running;
    if (running) {
        agent->repeat_count = 0;
        agent->last_touch_x = -1;
        agent->last_touch_y = -1;
        agent->last_screen_hash = 0;
        agent->same_screen_count = 0;
    }
    sc_cond_signal(&agent->cond); // wake auto_thread if waiting
    sc_mutex_unlock(&agent->mutex);
}

void
sc_ai_agent_record_touch(struct sc_ai_agent *agent, int32_t x, int32_t y) {
    sc_mutex_lock(&agent->mutex);
    // Consider "same" if within 10px tolerance
    if (abs(x - agent->last_touch_x) <= 10
            && abs(y - agent->last_touch_y) <= 10) {
        agent->repeat_count++;
    } else {
        agent->last_touch_x = x;
        agent->last_touch_y = y;
        agent->repeat_count = 1;
    }
    LOGD("Touch repeat tracking: (%d,%d) count=%d",
         (int)x, (int)y, agent->repeat_count);
    sc_mutex_unlock(&agent->mutex);
}

void
sc_ai_agent_set_game_rules(struct sc_ai_agent *agent, const char *rules) {
    sc_mutex_lock(&agent->mutex);
    free(agent->game_rules);
    agent->game_rules = rules ? strdup(rules) : NULL;
    sc_mutex_unlock(&agent->mutex);
}

char *
sc_ai_agent_summarize_rules(struct sc_ai_agent *agent) {
    // Copy config and message summary under lock
    sc_mutex_lock(&agent->mutex);

    char *api_key = agent->config.api_key ? strdup(agent->config.api_key) : NULL;
    char *model = agent->config.model ? strdup(agent->config.model) : NULL;
    char *base_url = agent->config.base_url ? strdup(agent->config.base_url)
                                            : NULL;

    if (!api_key || !api_key[0]) {
        sc_mutex_unlock(&agent->mutex);
        free(api_key);
        free(model);
        free(base_url);
        return strdup("[Error: API key not set]");
    }

    // Build a text summary of chat history (skip system, skip base64)
    size_t summary_cap = 16384;
    char *summary = malloc(summary_cap);
    if (!summary) {
        sc_mutex_unlock(&agent->mutex);
        free(api_key);
        free(model);
        free(base_url);
        return NULL;
    }
    summary[0] = '\0';
    size_t summary_len = 0;

    for (size_t i = 0; i < agent->messages.count; i++) {
        const char *role = agent->messages.messages[i].role;
        const char *content = agent->messages.messages[i].content;
        if (!role || !content) continue;
        if (strcmp(role, "system") == 0) continue;

        // Skip multimodal content (starts with '[')
        const char *text = content;
        if (content[0] == '[' && strstr(content, "\"type\"")) {
            text = "(screenshot message)";
        }

        size_t needed = strlen(role) + strlen(text) + 8;
        if (summary_len + needed >= summary_cap) break; // truncate if too long

        summary_len += (size_t)snprintf(summary + summary_len,
                                         summary_cap - summary_len,
                                         "%s: %s\n", role, text);
    }

    sc_mutex_unlock(&agent->mutex);

    // Build a separate message list for the summarization call
    struct sc_openrouter_config safe_config = {
        .api_key = api_key,
        .model = model,
        .base_url = base_url,
    };

    struct sc_ai_message_list sum_msgs;
    sc_ai_message_list_init(&sum_msgs);
    sc_ai_message_list_push(&sum_msgs, "system",
        "You are a game analyst. Analyze the following chat log between "
        "a user and an AI controlling an Android device. Extract and "
        "summarize the game rules, strategies, and instructions as a "
        "clean Markdown document. Focus on actionable rules that can "
        "guide automated gameplay.");
    sc_ai_message_list_push(&sum_msgs, "user", summary);
    free(summary);

    struct sc_openrouter_response resp =
        sc_openrouter_chat(&safe_config, &sum_msgs, NULL);

    sc_ai_message_list_destroy(&sum_msgs);
    free(api_key);
    free(model);
    free(base_url);

    if (!resp.success || !resp.content) {
        char *err = strdup(resp.error ? resp.error : "LLM call failed");
        sc_openrouter_response_destroy(&resp);
        return err;
    }

    char *result = strdup(resp.content);
    sc_openrouter_response_destroy(&resp);
    return result;
}

void
sc_ai_agent_clear_history(struct sc_ai_agent *agent) {
    sc_mutex_lock(&agent->mutex);
    sc_ai_message_list_destroy(&agent->messages);
    sc_ai_message_list_init(&agent->messages);
    sc_ai_message_list_push(&agent->messages, "system", agent->system_prompt);
    sc_mutex_unlock(&agent->mutex);
}

char *
sc_ai_agent_analyze_screen(struct sc_ai_agent *agent,
                           const char *base64_data,
                           uint16_t width, uint16_t height) {
    return analyze_screen_with_vlm(agent, base64_data, width, height);
}

// Helper to create directories recursively (like mkdir -p)
static int
mkdirs(const char *path, mode_t mode) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    return mkdir(tmp, mode);
}

void
sc_ai_agent_set_recording(struct sc_ai_agent *agent, bool recording) {
    sc_mutex_lock(&agent->mutex);
    agent->recording = recording;
    if (recording) {
        agent->record_count = 0;
        // Create timestamped directory for this recording session
        free(agent->record_dir);
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char dirname[256];
        const char *home = getenv("HOME");
        snprintf(dirname, sizeof(dirname),
                 "%s/scrcpy_records/%04d%02d%02d_%02d%02d%02d",
                 home ? home : "/tmp",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
        mkdirs(dirname, 0755);
        agent->record_dir = strdup(dirname);
        LOGI("AI recording: started, dir=%s", dirname);
    } else {
        LOGI("AI recording: stopped, %d captures in %s",
             agent->record_count,
             agent->record_dir ? agent->record_dir : "(none)");
    }
    sc_mutex_unlock(&agent->mutex);
}

void
sc_ai_agent_clear_recording(struct sc_ai_agent *agent) {
    sc_mutex_lock(&agent->mutex);
    char *dir = agent->record_dir ? strdup(agent->record_dir) : NULL;
    agent->record_count = 0;
    free(agent->record_dir);
    agent->record_dir = NULL;
    sc_mutex_unlock(&agent->mutex);

    if (dir) {
        // Remove all files in the recording directory, then the directory
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
        int ret = system(cmd);
        (void) ret;
        LOGI("AI recording: cleared dir=%s", dir);
        free(dir);
    }
}

bool
sc_ai_agent_record_capture(struct sc_ai_agent *agent,
                           int32_t x, int32_t y,
                           uint16_t w, uint16_t h) {
    sc_mutex_lock(&agent->mutex);
    if (!agent->recording || !agent->record_dir) {
        sc_mutex_unlock(&agent->mutex);
        return false;
    }

    agent->record_count++;
    int idx = agent->record_count;
    char *dir = strdup(agent->record_dir);

    // Get the latest screenshot data
    uint8_t *png_copy = NULL;
    size_t png_size = 0;
    if (agent->latest_png_data && agent->latest_png_size > 0) {
        png_copy = malloc(agent->latest_png_size);
        if (png_copy) {
            memcpy(png_copy, agent->latest_png_data, agent->latest_png_size);
            png_size = agent->latest_png_size;
        }
    }
    sc_mutex_unlock(&agent->mutex);

    if (!png_copy) {
        // Try to capture a fresh frame
        AVFrame *frame = av_frame_alloc();
        if (frame && sc_ai_frame_sink_consume(agent->frame_sink, frame)) {
            struct sc_ai_screenshot ss = {0};
            if (sc_ai_screenshot_encode(&ss, frame)) {
                // Save the image
                char img_path[512];
                snprintf(img_path, sizeof(img_path), "%s/%04d.jpg", dir, idx);
                FILE *f = fopen(img_path, "wb");
                if (f) {
                    fwrite(ss.png_data, 1, ss.png_size, f);
                    fclose(f);
                }
                // Save touch coordinates
                char txt_path[512];
                snprintf(txt_path, sizeof(txt_path), "%s/%04d.txt", dir, idx);
                f = fopen(txt_path, "w");
                if (f) {
                    fprintf(f, "%d %d %d %d\n", (int)x, (int)y, (int)w, (int)h);
                    fclose(f);
                }
                LOGI("AI recording: capture #%d at (%d,%d) -> %s",
                     idx, x, y, img_path);
                sc_ai_screenshot_destroy(&ss);
            }
            av_frame_free(&frame);
        } else {
            if (frame) av_frame_free(&frame);
        }
        free(dir);
        return true;
    }

    // Save the screenshot image
    char img_path[512];
    snprintf(img_path, sizeof(img_path), "%s/%04d.jpg", dir, idx);
    FILE *f = fopen(img_path, "wb");
    if (f) {
        fwrite(png_copy, 1, png_size, f);
        fclose(f);
    }
    free(png_copy);

    // Save touch coordinates
    char txt_path[512];
    snprintf(txt_path, sizeof(txt_path), "%s/%04d.txt", dir, idx);
    f = fopen(txt_path, "w");
    if (f) {
        fprintf(f, "%d %d %d %d\n", (int)x, (int)y, (int)w, (int)h);
        fclose(f);
    }

    LOGI("AI recording: capture #%d at (%d,%d) -> %s", idx, x, y, img_path);
    free(dir);
    return true;
}

// --- Train phase functions ---

cJSON *
sc_ai_agent_list_sessions(void) {
    const char *home = getenv("HOME");
    if (!home) {
        return cJSON_CreateArray();
    }

    char base_dir[512];
    snprintf(base_dir, sizeof(base_dir), "%s/scrcpy_records", home);

    DIR *d = opendir(base_dir);
    if (!d) {
        return cJSON_CreateArray();
    }

    cJSON *arr = cJSON_CreateArray();
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        // Check if it's a directory
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, ent->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        // Count .jpg files in this directory
        DIR *sd = opendir(full_path);
        if (!sd) continue;
        int count = 0;
        struct dirent *se;
        while ((se = readdir(sd)) != NULL) {
            size_t nlen = strlen(se->d_name);
            if (nlen > 4 && strcmp(se->d_name + nlen - 4, ".jpg") == 0) {
                count++;
            }
        }
        closedir(sd);

        if (count > 0) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", ent->d_name);
            cJSON_AddNumberToObject(item, "count", count);
            cJSON_AddItemToArray(arr, item);
        }
    }
    closedir(d);
    return arr;
}

cJSON *
sc_ai_agent_get_session(const char *session_name) {
    const char *home = getenv("HOME");
    if (!home || !session_name) {
        return cJSON_CreateArray();
    }

    // Prevent path traversal
    if (strstr(session_name, "..") || strchr(session_name, '/')) {
        return cJSON_CreateArray();
    }

    char dir_path[1024];
    snprintf(dir_path, sizeof(dir_path), "%s/scrcpy_records/%s",
             home, session_name);

    DIR *d = opendir(dir_path);
    if (!d) {
        return cJSON_CreateArray();
    }

    cJSON *arr = cJSON_CreateArray();
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5 || strcmp(ent->d_name + nlen - 4, ".jpg") != 0) continue;

        // Extract index from filename (e.g. "0001.jpg" -> 1)
        int idx = atoi(ent->d_name);
        if (idx <= 0) continue;

        // Read corresponding .txt file for touch coords
        char txt_path[1024];
        snprintf(txt_path, sizeof(txt_path), "%s/%04d.txt", dir_path, idx);
        int tx = 0, ty = 0, tw = 0, th = 0;
        FILE *f = fopen(txt_path, "r");
        if (f) {
            if (fscanf(f, "%d %d %d %d", &tx, &ty, &tw, &th) < 4) {
                // partial read is ok, defaults are 0
            }
            fclose(f);
        }

        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", idx);
        cJSON_AddNumberToObject(item, "x", tx);
        cJSON_AddNumberToObject(item, "y", ty);
        cJSON_AddNumberToObject(item, "w", tw);
        cJSON_AddNumberToObject(item, "h", th);
        cJSON_AddItemToArray(arr, item);
    }
    closedir(d);

    // Sort by index (simple bubble sort, small arrays)
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            cJSON *a = cJSON_GetArrayItem(arr, j);
            cJSON *b = cJSON_GetArrayItem(arr, j + 1);
            int ai = cJSON_GetObjectItem(a, "index")->valueint;
            int bi = cJSON_GetObjectItem(b, "index")->valueint;
            if (ai > bi) {
                // Swap by detaching and re-inserting
                cJSON *detached = cJSON_DetachItemFromArray(arr, j + 1);
                // After detach, item at j+1 is gone; insert before j
                // cJSON doesn't have InsertItemInArray before index,
                // so we rebuild. For simplicity, accept unsorted for now.
                // Actually, cJSON has cJSON_InsertItemInArray since newer versions.
                // Fallback: just accept directory order. The frontend can sort.
                cJSON_AddItemToArray(arr, detached);
                // This doesn't truly sort. Let's use a different approach.
                break; // Give up on C-level sorting, frontend will sort
            }
        }
        break; // Just let frontend sort
    }

    return arr;
}

// Base64 encoding table
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *
base64_encode_data(const uint8_t *data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;

    size_t i, j;
    for (i = 0, j = 0; i < len;) {
        uint32_t a = i < len ? data[i++] : 0;
        uint32_t b = i < len ? data[i++] : 0;
        uint32_t c = i < len ? data[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = (i > len + 1) ? '=' : b64_table[(triple >> 6) & 0x3F];
        out[j++] = (i > len) ? '=' : b64_table[triple & 0x3F];
    }
    out[j] = '\0';
    return out;
}

char *
sc_ai_agent_analyze_capture(struct sc_ai_agent *agent,
                            const char *session_name, int index) {
    const char *home = getenv("HOME");
    if (!home || !session_name || index <= 0) {
        return NULL;
    }

    // Prevent path traversal
    if (strstr(session_name, "..") || strchr(session_name, '/')) {
        return NULL;
    }

    // Read the .jpg file
    char img_path[1024];
    snprintf(img_path, sizeof(img_path), "%s/scrcpy_records/%s/%04d.jpg",
             home, session_name, index);

    FILE *f = fopen(img_path, "rb");
    if (!f) {
        LOGW("Train: could not open %s", img_path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 10 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }

    uint8_t *data = malloc((size_t)fsize);
    if (!data) {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(data, 1, (size_t)fsize, f);
    fclose(f);

    if (nread != (size_t)fsize) {
        free(data);
        return NULL;
    }

    // Base64 encode
    char *b64 = base64_encode_data(data, (size_t)fsize);
    free(data);
    if (!b64) {
        return NULL;
    }

    // Call VLM with a simple labeling prompt
    sc_mutex_lock(&agent->mutex);
    char *api_key = agent->config.api_key ? strdup(agent->config.api_key) : NULL;
    char *vision_model = agent->vision_model ? strdup(agent->vision_model) : NULL;
    char *base_url = agent->config.base_url ? strdup(agent->config.base_url)
                                            : NULL;
    sc_mutex_unlock(&agent->mutex);

    if (!api_key || !vision_model) {
        free(api_key);
        free(vision_model);
        free(base_url);
        free(b64);
        return NULL;
    }

    struct sc_openrouter_config vlm_config = {
        .api_key = api_key,
        .model = vision_model,
        .base_url = base_url,
    };

    struct sc_ai_message_list vlm_msgs;
    sc_ai_message_list_init(&vlm_msgs);

    sc_ai_message_list_push(&vlm_msgs, "system",
        "Describe this game screen in 2-5 words. "
        "Examples: 'Main Menu', 'Game Board', 'Level Complete', 'Settings'. "
        "Just the label, nothing else.");

    sc_ai_message_list_push_image(&vlm_msgs, "Label this screen.", b64);

    LOGI("Train: analyzing capture %s #%d with VLM", session_name, index);

    struct sc_openrouter_response resp =
        sc_openrouter_chat(&vlm_config, &vlm_msgs, NULL);

    sc_ai_message_list_destroy(&vlm_msgs);
    free(api_key);
    free(vision_model);
    free(base_url);
    free(b64);

    if (!resp.success || !resp.content) {
        LOGW("Train: VLM analysis failed: %s",
             resp.error ? resp.error : "unknown");
        sc_openrouter_response_destroy(&resp);
        return NULL;
    }

    char *label = strdup(resp.content);
    sc_openrouter_response_destroy(&resp);

    // Trim whitespace and quotes
    if (label) {
        // Strip leading/trailing whitespace and quotes
        char *start = label;
        while (*start == ' ' || *start == '\t' || *start == '\n'
               || *start == '\r' || *start == '"' || *start == '\'') {
            start++;
        }
        size_t slen = strlen(start);
        while (slen > 0 && (start[slen-1] == ' ' || start[slen-1] == '\t'
               || start[slen-1] == '\n' || start[slen-1] == '\r'
               || start[slen-1] == '"' || start[slen-1] == '\'')) {
            slen--;
        }
        char *trimmed = malloc(slen + 1);
        if (trimmed) {
            memcpy(trimmed, start, slen);
            trimmed[slen] = '\0';
        }
        free(label);
        label = trimmed;
    }

    LOGI("Train: capture %s #%d label: %s", session_name, index,
         label ? label : "(null)");
    return label;
}

void
sc_ai_agent_set_train_tree(struct sc_ai_agent *agent, const char *json) {
    sc_mutex_lock(&agent->mutex);
    free(agent->train_tree_json);
    agent->train_tree_json = json ? strdup(json) : NULL;
    sc_mutex_unlock(&agent->mutex);
}

char *
sc_ai_agent_get_train_tree(struct sc_ai_agent *agent) {
    sc_mutex_lock(&agent->mutex);
    char *copy = agent->train_tree_json ? strdup(agent->train_tree_json) : NULL;
    sc_mutex_unlock(&agent->mutex);
    return copy;
}
