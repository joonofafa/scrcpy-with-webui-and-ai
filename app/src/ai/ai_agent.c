#include "ai_agent.h"

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
    "You are an AI assistant controlling an Android device via scrcpy. " \
    "You can see the device screen via screenshots and interact with it " \
    "using touch, key, and text input tools.\n\n" \
    "When analyzing the screen:\n" \
    "1. Describe what you see on the screen\n" \
    "2. Identify UI elements, text, buttons, etc.\n" \
    "3. Use the available tools to interact with the device\n\n" \
    "CRITICAL - Screen coordinates:\n" \
    "- Each screenshot has a caption like 'Screenshot WxH' showing dimensions.\n" \
    "- Use raw pixel coordinates. Valid X: 0 to W-1, Valid Y: 0 to H-1.\n" \
    "- (0,0) is top-left. Do NOT normalize to 0-1000 or percentages.\n\n" \
    "Tapping strategy (STRICT RULES):\n" \
    "- VLM analysis provides cx, cy = the EXACT tap coordinates.\n" \
    "- Call position_click with x=cx, y=cy from the VLM line.\n" \
    "- Example: '\"Start\" cx=423 cy=187 [button]' → tap x=423, y=187.\n" \
    "- NEVER guess or estimate coordinates. ONLY use cx/cy from VLM.\n" \
    "- If a tap did not work (screen unchanged), try a DIFFERENT element.\n\n" \
    "IMPORTANT - Before every action:\n" \
    "- State the element name, its cx/cy from VLM, and WHY.\n" \
    "- After the action, take a screenshot to verify the result.\n" \
    "- If the screen did not change, your tap missed. Try a different " \
    "element — do NOT repeat the same position.\n\n" \
    "AUTONOMOUS MODE:\n" \
    "- You have game rules. Follow them step by step WITHOUT asking.\n" \
    "- NEVER ask the user what to do. ALWAYS decide and act.\n" \
    "- Every response MUST include at least one tool call.\n" \
    "- If unsure, take a screenshot first, then act on what you see."

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

    char sys_prompt[1280];
    snprintf(sys_prompt, sizeof(sys_prompt),
        "You are a screen analyzer for an Android game automation system. "
        "Analyze the screenshot and list every UI element with bounding box.\n\n"
        "Output format:\n"
        "SCREEN: (brief scene description)\n"
        "ELEMENTS:\n"
        "- \"text or icon\" x=NNN y=NNN w=NNN h=NNN [button/text/icon/card]\n"
        "...\n\n"
        "CRITICAL rules:\n"
        "- Image is EXACTLY %dx%d pixels. x: 0..%d, y: 0..%d\n"
        "- x,y = top-left corner of the element bounding box\n"
        "- w,h = width and height of the bounding box\n"
        "- The tap target = center of box: cx=x+w/2, cy=y+h/2\n"
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
    LOGI("AI VLM: %s", simplified ? simplified : resp.content);
    sc_openrouter_response_destroy(&resp);
    return simplified;
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

        // Add as user message
        char auto_text[8192];
        if (auto_vlm && auto_desc) {
            snprintf(auto_text, sizeof(auto_text),
                     "Screenshot %dx%d (fresh)\n"
                     "=== VLM ANALYSIS (use these EXACT coordinates) ===\n"
                     "%s\n"
                     "=== END VLM ANALYSIS ===\n\n"
                     "Continue playing. Decide your next action.",
                     auto_ss.width, auto_ss.height, auto_desc);
            sc_ai_message_list_push(&agent->messages, "user", auto_text);
        } else {
            snprintf(auto_text, sizeof(auto_text),
                     "Screenshot %dx%d (fresh)\nContinue playing.",
                     auto_ss.width, auto_ss.height);
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
