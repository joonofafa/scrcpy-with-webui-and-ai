#ifndef SC_AI_AGENT_BRIDGE_H
#define SC_AI_AGENT_BRIDGE_H

// C-compatible bridge for ai_panel.cpp to call ai_agent functions
// without including C11 atomics headers that are incompatible with C++

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to avoid C11 atomic includes in C++
struct sc_ai_agent;

// Bridge functions that wrap ai_agent calls
void
sc_ai_agent_bridge_lock(struct sc_ai_agent *agent);

void
sc_ai_agent_bridge_unlock(struct sc_ai_agent *agent);

void
sc_ai_agent_bridge_submit_prompt(struct sc_ai_agent *agent, char *prompt);

void
sc_ai_agent_bridge_set_config(struct sc_ai_agent *agent,
                              const char *api_key,
                              const char *model,
                              const char *base_url);

void
sc_ai_agent_bridge_clear_history(struct sc_ai_agent *agent);

bool
sc_ai_agent_bridge_add_macro(struct sc_ai_agent *agent, const char *prompt);

void
sc_ai_agent_bridge_remove_macro(struct sc_ai_agent *agent, size_t index);

void
sc_ai_agent_bridge_set_macro_running(struct sc_ai_agent *agent, bool running);

void
sc_ai_agent_bridge_set_macro_interval(struct sc_ai_agent *agent,
                                      int interval_ms);

// Accessors for agent state (called while locked)
size_t
sc_ai_agent_bridge_get_message_count(struct sc_ai_agent *agent);

const char *
sc_ai_agent_bridge_get_message_role(struct sc_ai_agent *agent, size_t index);

const char *
sc_ai_agent_bridge_get_message_content(struct sc_ai_agent *agent, size_t index);

const char *
sc_ai_agent_bridge_get_message_name(struct sc_ai_agent *agent, size_t index);

uint16_t
sc_ai_agent_bridge_get_screen_width(struct sc_ai_agent *agent);

uint16_t
sc_ai_agent_bridge_get_screen_height(struct sc_ai_agent *agent);

size_t
sc_ai_agent_bridge_get_macro_count(struct sc_ai_agent *agent);

const char *
sc_ai_agent_bridge_get_macro_prompt(struct sc_ai_agent *agent, size_t index);

size_t
sc_ai_agent_bridge_get_macro_current(struct sc_ai_agent *agent);

bool
sc_ai_agent_bridge_get_macro_running(struct sc_ai_agent *agent);

const char *
sc_ai_agent_bridge_get_config_api_key(struct sc_ai_agent *agent);

const char *
sc_ai_agent_bridge_get_config_model(struct sc_ai_agent *agent);

const char *
sc_ai_agent_bridge_get_config_base_url(struct sc_ai_agent *agent);

#ifdef __cplusplus
}
#endif

#endif
