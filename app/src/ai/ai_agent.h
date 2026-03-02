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

struct sc_ai_agent {
    struct sc_ai_frame_sink *frame_sink;
    struct sc_ai_tools tools;
    struct sc_openrouter_config config;

    // Worker thread
    sc_thread worker_thread;
    sc_mutex mutex;
    sc_cond cond;
    bool stopped;

    // Pending prompt (UI -> Worker)
    char *pending_prompt;
    bool has_pending_prompt;

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

    // Auto-play system (replaces macro system)
    sc_thread auto_thread;
    bool auto_running;
    int auto_interval_ms; // 5000, 10000, 15000

    // Game rules for auto-play
    char *game_rules;

    // Train log
    char *train_log_path;
};

struct sc_ai_agent_params {
    struct sc_ai_frame_sink *frame_sink;
    struct sc_controller *controller;
    const char *api_key;
    const char *model;
    const char *base_url;
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
                       const char *base_url);

// Auto-play control
void
sc_ai_agent_set_auto_running(struct sc_ai_agent *agent, bool running);

void
sc_ai_agent_set_auto_interval(struct sc_ai_agent *agent, int interval_ms);

// Game rules management
void
sc_ai_agent_set_game_rules(struct sc_ai_agent *agent, const char *rules);

// Summarize chat history into game rules via LLM call (caller must free result)
char *
sc_ai_agent_summarize_rules(struct sc_ai_agent *agent);

// Clear conversation history
void
sc_ai_agent_clear_history(struct sc_ai_agent *agent);

#endif
