#include "ai_agent_bridge.h"
#include "ai_agent.h"

void
sc_ai_agent_bridge_lock(struct sc_ai_agent *agent) {
    sc_mutex_lock(&agent->mutex);
}

void
sc_ai_agent_bridge_unlock(struct sc_ai_agent *agent) {
    sc_mutex_unlock(&agent->mutex);
}

void
sc_ai_agent_bridge_submit_prompt(struct sc_ai_agent *agent, char *prompt) {
    sc_ai_agent_submit_prompt(agent, prompt);
}

void
sc_ai_agent_bridge_set_config(struct sc_ai_agent *agent,
                              const char *api_key,
                              const char *model,
                              const char *base_url) {
    sc_ai_agent_set_config(agent, api_key, model, base_url);
}

void
sc_ai_agent_bridge_clear_history(struct sc_ai_agent *agent) {
    sc_ai_agent_clear_history(agent);
}

bool
sc_ai_agent_bridge_add_macro(struct sc_ai_agent *agent, const char *prompt) {
    return sc_ai_agent_add_macro(agent, prompt);
}

void
sc_ai_agent_bridge_remove_macro(struct sc_ai_agent *agent, size_t index) {
    sc_ai_agent_remove_macro(agent, index);
}

void
sc_ai_agent_bridge_set_macro_running(struct sc_ai_agent *agent, bool running) {
    sc_ai_agent_set_macro_running(agent, running);
}

void
sc_ai_agent_bridge_set_macro_interval(struct sc_ai_agent *agent,
                                      int interval_ms) {
    sc_ai_agent_set_macro_interval(agent, interval_ms);
}

size_t
sc_ai_agent_bridge_get_message_count(struct sc_ai_agent *agent) {
    return agent->messages.count;
}

const char *
sc_ai_agent_bridge_get_message_role(struct sc_ai_agent *agent, size_t index) {
    if (index >= agent->messages.count) return NULL;
    return agent->messages.messages[index].role;
}

const char *
sc_ai_agent_bridge_get_message_content(struct sc_ai_agent *agent, size_t index) {
    if (index >= agent->messages.count) return NULL;
    return agent->messages.messages[index].content;
}

const char *
sc_ai_agent_bridge_get_message_name(struct sc_ai_agent *agent, size_t index) {
    if (index >= agent->messages.count) return NULL;
    return agent->messages.messages[index].name;
}

uint16_t
sc_ai_agent_bridge_get_screen_width(struct sc_ai_agent *agent) {
    return agent->screen_width;
}

uint16_t
sc_ai_agent_bridge_get_screen_height(struct sc_ai_agent *agent) {
    return agent->screen_height;
}

size_t
sc_ai_agent_bridge_get_macro_count(struct sc_ai_agent *agent) {
    return agent->macro_count;
}

const char *
sc_ai_agent_bridge_get_macro_prompt(struct sc_ai_agent *agent, size_t index) {
    if (index >= agent->macro_count) return NULL;
    return agent->macros[index].prompt;
}

size_t
sc_ai_agent_bridge_get_macro_current(struct sc_ai_agent *agent) {
    return agent->macro_current;
}

bool
sc_ai_agent_bridge_get_macro_running(struct sc_ai_agent *agent) {
    return agent->macro_running;
}

const char *
sc_ai_agent_bridge_get_config_api_key(struct sc_ai_agent *agent) {
    return agent->config.api_key;
}

const char *
sc_ai_agent_bridge_get_config_model(struct sc_ai_agent *agent) {
    return agent->config.model;
}

const char *
sc_ai_agent_bridge_get_config_base_url(struct sc_ai_agent *agent) {
    return agent->config.base_url;
}
