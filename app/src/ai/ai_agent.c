#include "ai_agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
    "- Each screenshot includes [Screen: WxH pixels] showing exact dimensions.\n" \
    "- The screenshot image is EXACTLY WxH pixels. Use pixel coordinates.\n" \
    "- Valid X range: 0 to W-1. Valid Y range: 0 to H-1.\n" \
    "- (0,0) is the top-left corner. (W-1, H-1) is the bottom-right.\n" \
    "- To tap a UI element, estimate the CENTER of that element in pixels.\n" \
    "- Do NOT normalize coordinates to 0-1000 or percentages. Use raw pixels.\n" \
    "- Example: if screen is 1024x460 and a button is at the center, " \
    "tap (512, 230).\n\n" \
    "After each action, take a screenshot to verify the result before " \
    "deciding next steps.\n\n" \
    "Always confirm your actions by describing what you did."

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
        }
    }
    av_frame_free(&frame);

    // Add user message with screenshot (include resolution info)
    sc_mutex_lock(&agent->mutex);
    if (has_screenshot) {
        char enhanced[4096];
        snprintf(enhanced, sizeof(enhanced),
                 "[Screen: %dx%d pixels]\n%s", ss.width, ss.height, prompt);
        sc_ai_message_list_push_image(&agent->messages, enhanced,
                                       ss.base64_data);
    } else {
        sc_ai_message_list_push(&agent->messages, "user", prompt);
    }
    sc_mutex_unlock(&agent->mutex);

    if (has_screenshot) {
        sc_ai_screenshot_destroy(&ss);
    }

    log_to_train(agent, "user", prompt);

    // Call API in a loop (handle tool calls, max 3 iterations)
    int max_iterations = 3;
    for (int iter = 0; iter < max_iterations; iter++) {
        sc_mutex_lock(&agent->mutex);
        if (agent->stopped) {
            sc_mutex_unlock(&agent->mutex);
            return false;
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

        // Build request body while holding mutex (reads messages, fast)
        char *request_body = sc_openrouter_build_body(
            &safe_config, &agent->messages, tools_json);
        sc_mutex_unlock(&agent->mutex);

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
        break;
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
        sc_mutex_unlock(&agent->mutex);

        process_prompt(agent, prompt);
        free(prompt);

        sc_mutex_lock(&agent->mutex);
    }
    sc_mutex_unlock(&agent->mutex);

    return 0;
}

static int
auto_thread_fn(void *data) {
    struct sc_ai_agent *agent = data;

    while (true) {
        sc_mutex_lock(&agent->mutex);
        if (agent->stopped) {
            sc_mutex_unlock(&agent->mutex);
            break;
        }

        bool running = agent->auto_running;
        bool has_rules = agent->game_rules && agent->game_rules[0] != '\0';
        int interval = agent->auto_interval_ms;
        char *rules_copy = NULL;
        if (running && has_rules) {
            rules_copy = strdup(agent->game_rules);
        }
        sc_mutex_unlock(&agent->mutex);

        if (!running || !has_rules) {
            free(rules_copy);
            SDL_Delay(100);
            continue;
        }

        // Build prompt with game rules
        size_t prompt_len = strlen(rules_copy) + 256;
        char *prompt = malloc(prompt_len);
        if (prompt) {
            snprintf(prompt, prompt_len,
                "Follow the game rules below, look at the screenshot, "
                "and use the available tools to play:\n\n%s", rules_copy);
            sc_ai_agent_submit_prompt(agent, prompt);
            // prompt ownership transferred to submit_prompt
        }
        free(rules_copy);

        // Wait interval, checking stop every 100ms
        int waited = 0;
        while (waited < interval) {
            sc_mutex_lock(&agent->mutex);
            if (agent->stopped || !agent->auto_running) {
                sc_mutex_unlock(&agent->mutex);
                break;
            }
            sc_mutex_unlock(&agent->mutex);
            SDL_Delay(100);
            waited += 100;
        }
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
                                        : strdup("google/gemma-3-27b-it:free");
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

    agent->auto_interval_ms = 5000;
    agent->auto_running = false;
    agent->game_rules = NULL;

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
                       const char *base_url) {
    sc_mutex_lock(&agent->mutex);
    free((void *)agent->config.api_key);
    free((void *)agent->config.model);
    free((void *)agent->config.base_url);
    agent->config.api_key = api_key ? strdup(api_key) : NULL;
    agent->config.model = model ? strdup(model) : NULL;
    agent->config.base_url = base_url ? strdup(base_url) : NULL;
    sc_mutex_unlock(&agent->mutex);
}

void
sc_ai_agent_set_auto_running(struct sc_ai_agent *agent, bool running) {
    sc_mutex_lock(&agent->mutex);
    agent->auto_running = running;
    sc_mutex_unlock(&agent->mutex);
}

void
sc_ai_agent_set_auto_interval(struct sc_ai_agent *agent, int interval_ms) {
    sc_mutex_lock(&agent->mutex);
    agent->auto_interval_ms = interval_ms;
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
