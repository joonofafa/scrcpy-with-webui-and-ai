#ifndef SC_AI_OPENROUTER_H
#define SC_AI_OPENROUTER_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>

// Forward declaration - cJSON is used internally
struct cJSON;

struct sc_openrouter_config {
    const char *api_key;
    const char *model;
    const char *base_url; // defaults to "https://openrouter.ai/api/v1"
};

// A single message in the conversation
struct sc_ai_message {
    char *role;    // "system", "user", "assistant", "tool"
    char *content; // text content (may be NULL for tool_calls messages)
    // For multimodal messages, content can be JSON array string
    // For tool results
    char *tool_call_id;
    char *name; // function name for tool messages
    // For assistant messages with tool_calls
    char *tool_calls_json; // raw JSON array of tool_calls
};

struct sc_ai_message_list {
    struct sc_ai_message *messages;
    size_t count;
    size_t capacity;
};

// Tool call parsed from assistant response
struct sc_ai_tool_call {
    char *id;
    char *function_name;
    char *arguments_json;
};

struct sc_ai_tool_call_list {
    struct sc_ai_tool_call *calls;
    size_t count;
};

// API response
struct sc_openrouter_response {
    char *content;         // text content (may be NULL)
    struct sc_ai_tool_call_list tool_calls;
    char *raw_json;        // full response JSON
    bool success;
    char *error;           // error message if !success
};

// Initialize the OpenRouter client (call once at startup)
bool
sc_openrouter_init(void);

// Cleanup the OpenRouter client (call once at shutdown)
void
sc_openrouter_cleanup(void);

// Send a chat completion request.
// tools_json: OpenAI function calling tools JSON array string, or NULL.
// Returns a response that must be freed with sc_openrouter_response_destroy().
struct sc_openrouter_response
sc_openrouter_chat(const struct sc_openrouter_config *config,
                   const struct sc_ai_message_list *messages,
                   const char *tools_json);

// Build the request body JSON string from messages (caller must free).
// This reads from messages but does not perform any network I/O.
char *
sc_openrouter_build_body(const struct sc_openrouter_config *config,
                         const struct sc_ai_message_list *messages,
                         const char *tools_json);

// Send a chat completion request with a pre-built body string.
// Use sc_openrouter_build_body() to create the body.
struct sc_openrouter_response
sc_openrouter_chat_with_body(const struct sc_openrouter_config *config,
                             const char *body);

void
sc_openrouter_response_destroy(struct sc_openrouter_response *resp);

// Message list helpers
void
sc_ai_message_list_init(struct sc_ai_message_list *list);

bool
sc_ai_message_list_push(struct sc_ai_message_list *list,
                        const char *role, const char *content);

bool
sc_ai_message_list_push_tool_result(struct sc_ai_message_list *list,
                                    const char *tool_call_id,
                                    const char *name,
                                    const char *content);

bool
sc_ai_message_list_push_assistant_tool_calls(struct sc_ai_message_list *list,
                                             const char *content,
                                             const char *tool_calls_json);

bool
sc_ai_message_list_push_image(struct sc_ai_message_list *list,
                              const char *text,
                              const char *base64_png);

void
sc_ai_message_list_destroy(struct sc_ai_message_list *list);

void
sc_ai_tool_call_list_destroy(struct sc_ai_tool_call_list *list);

#endif
