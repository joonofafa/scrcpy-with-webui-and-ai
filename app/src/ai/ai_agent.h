#ifndef SC_AI_AGENT_H
#define SC_AI_AGENT_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ai/ai_frame_sink.h"
#include "ai/ai_tools.h"
#include "ai/openrouter.h"
#include "util/thread.h"

struct cJSON;

struct sc_ai_agent {
    struct sc_ai_frame_sink *frame_sink;
    struct sc_ai_tools tools;
    struct sc_openrouter_config config;
    char *vision_model; // VLM for screen analysis (NULL = use main model)

    // Worker thread
    sc_thread worker_thread;
    sc_mutex mutex;
    sc_cond cond;
    bool stopped;

    // Pending prompt (UI -> Worker)
    char *pending_prompt;
    bool has_pending_prompt;
    bool worker_busy; // true while worker is processing a prompt

    // Conversation history (Worker -> UI)
    struct sc_ai_message_list messages;

    // System prompt
    char *system_prompt;

    // Screenshot data (Worker -> UI for preview)
    uint8_t *latest_png_data;
    size_t latest_png_size;

    // Screen dimensions from frame sink
    uint16_t screen_width;
    uint16_t screen_height;

    // Auto-play system (completion-based loop)
    sc_thread auto_thread;
    bool auto_running;

    // Game rules for auto-play
    char *game_rules;

    // Guardrails
    int32_t last_touch_x;
    int32_t last_touch_y;
    int repeat_count;        // same-coordinate tap count
    uint32_t last_screen_hash;
    int same_screen_count;   // consecutive identical screenshot count

    // Train log
    char *train_log_path;

    // Recording
    bool recording;
    int record_count;
    char *record_dir;  // directory for current recording session

    // Train phase: decision tree JSON
    char *train_tree_json;
};

struct sc_ai_agent_params {
    struct sc_ai_frame_sink *frame_sink;
    struct sc_controller *controller;
    const char *api_key;
    const char *model;
    const char *base_url;
    const char *vision_model;
};

bool
sc_ai_agent_init(struct sc_ai_agent *agent,
                 const struct sc_ai_agent_params *params);

bool
sc_ai_agent_start(struct sc_ai_agent *agent);

void
sc_ai_agent_stop(struct sc_ai_agent *agent);

void
sc_ai_agent_join(struct sc_ai_agent *agent);

void
sc_ai_agent_destroy(struct sc_ai_agent *agent);

// Submit a prompt from UI thread. Takes ownership of prompt string.
void
sc_ai_agent_submit_prompt(struct sc_ai_agent *agent, char *prompt);

// Set API configuration at runtime
void
sc_ai_agent_set_config(struct sc_ai_agent *agent,
                       const char *api_key,
                       const char *model,
                       const char *base_url,
                       const char *vision_model);

// Auto-play control
void
sc_ai_agent_set_auto_running(struct sc_ai_agent *agent, bool running);

// Record a touch action for repeat detection
void
sc_ai_agent_record_touch(struct sc_ai_agent *agent, int32_t x, int32_t y);

// Game rules management
void
sc_ai_agent_set_game_rules(struct sc_ai_agent *agent, const char *rules);

// Summarize chat history into game rules via LLM call (caller must free result)
char *
sc_ai_agent_summarize_rules(struct sc_ai_agent *agent);

// Clear conversation history
void
sc_ai_agent_clear_history(struct sc_ai_agent *agent);

// Recording control
void
sc_ai_agent_set_recording(struct sc_ai_agent *agent, bool recording);

void
sc_ai_agent_clear_recording(struct sc_ai_agent *agent);

bool
sc_ai_agent_record_capture(struct sc_ai_agent *agent,
                           int32_t x, int32_t y,
                           uint16_t w, uint16_t h);

// Analyze screen with VLM (caller must free result, returns NULL on failure)
char *
sc_ai_agent_analyze_screen(struct sc_ai_agent *agent,
                           const char *base64_data,
                           uint16_t width, uint16_t height);

// Train phase: list recording sessions (caller must cJSON_Delete)
struct cJSON *
sc_ai_agent_list_sessions(void);

// Train phase: get captures from a session (caller must cJSON_Delete)
struct cJSON *
sc_ai_agent_get_session(const char *session_name);

// Train phase: analyze a single capture for state labeling (caller must free)
char *
sc_ai_agent_analyze_capture(struct sc_ai_agent *agent,
                            const char *session_name, int index);

// Train phase: set/get decision tree JSON
void
sc_ai_agent_set_train_tree(struct sc_ai_agent *agent, const char *json);

char *
sc_ai_agent_get_train_tree(struct sc_ai_agent *agent);

#endif
