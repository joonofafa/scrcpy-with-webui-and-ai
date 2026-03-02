#include "openrouter.h"

#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "../../deps/cjson/cJSON.h"
#include "util/log.h"

#define DEFAULT_BASE_URL "https://openrouter.ai/api/v1"

struct curl_buffer {
    char *data;
    size_t size;
};

static size_t
curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    struct curl_buffer *buf = userp;
    char *ptr = realloc(buf->data, buf->size + total + 1);
    if (!ptr) {
        return 0;
    }
    buf->data = ptr;
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

bool
sc_openrouter_init(void) {
    CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (res != CURLE_OK) {
        LOGE("curl_global_init failed: %s", curl_easy_strerror(res));
        return false;
    }
    return true;
}

void
sc_openrouter_cleanup(void) {
    curl_global_cleanup();
}

static cJSON *
build_message_json(const struct sc_ai_message *msg) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }

    cJSON_AddStringToObject(obj, "role", msg->role);

    if (strcmp(msg->role, "tool") == 0) {
        if (msg->content) {
            cJSON_AddStringToObject(obj, "content", msg->content);
        }
        if (msg->tool_call_id) {
            cJSON_AddStringToObject(obj, "tool_call_id", msg->tool_call_id);
        }
        if (msg->name) {
            cJSON_AddStringToObject(obj, "name", msg->name);
        }
    } else if (strcmp(msg->role, "assistant") == 0 && msg->tool_calls_json) {
        if (msg->content) {
            cJSON_AddStringToObject(obj, "content", msg->content);
        } else {
            cJSON_AddNullToObject(obj, "content");
        }
        cJSON *tc = cJSON_Parse(msg->tool_calls_json);
        if (tc) {
            cJSON_AddItemToObject(obj, "tool_calls", tc);
        }
    } else {
        if (msg->content) {
            // Check if content starts with '[' — it's a JSON array (multimodal)
            if (msg->content[0] == '[') {
                cJSON *arr = cJSON_Parse(msg->content);
                if (arr) {
                    cJSON_AddItemToObject(obj, "content", arr);
                } else {
                    cJSON_AddStringToObject(obj, "content", msg->content);
                }
            } else {
                cJSON_AddStringToObject(obj, "content", msg->content);
            }
        }
    }

    return obj;
}

static cJSON *
build_request_json(const struct sc_openrouter_config *config,
                   const struct sc_ai_message_list *messages,
                   const char *tools_json) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", config->model);

    cJSON *msgs_arr = cJSON_CreateArray();
    if (!msgs_arr) {
        cJSON_Delete(root);
        return NULL;
    }

    // Find the last image message index so we only send that image,
    // replacing older images with text to keep payload small
    size_t last_image_idx = SIZE_MAX;
    for (size_t i = messages->count; i > 0; i--) {
        const struct sc_ai_message *m = &messages->messages[i - 1];
        if (m->content && m->content[0] == '[') {
            // Likely a multimodal JSON array (image message)
            last_image_idx = i - 1;
            break;
        }
    }

    for (size_t i = 0; i < messages->count; i++) {
        const struct sc_ai_message *m = &messages->messages[i];

        // For older image messages, strip the image and keep only text
        if (i != last_image_idx && m->content && m->content[0] == '['
                && m->role && strcmp(m->role, "user") == 0) {
            cJSON *arr = cJSON_Parse(m->content);
            if (arr && cJSON_IsArray(arr)) {
                // Extract text parts only
                char text_buf[1024] = "";
                size_t text_len = 0;
                cJSON *item;
                cJSON_ArrayForEach(item, arr) {
                    cJSON *type = cJSON_GetObjectItem(item, "type");
                    if (type && cJSON_IsString(type)
                            && strcmp(type->valuestring, "text") == 0) {
                        cJSON *text = cJSON_GetObjectItem(item, "text");
                        if (text && cJSON_IsString(text)) {
                            size_t tl = strlen(text->valuestring);
                            if (text_len + tl + 1 < sizeof(text_buf)) {
                                memcpy(text_buf + text_len,
                                       text->valuestring, tl);
                                text_len += tl;
                                text_buf[text_len] = '\0';
                            }
                        }
                    }
                }
                cJSON_Delete(arr);

                // Build a simple text-only user message
                cJSON *obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "role", "user");
                cJSON_AddStringToObject(obj, "content",
                    text_len > 0 ? text_buf : "(screenshot)");
                cJSON_AddItemToArray(msgs_arr, obj);
                continue;
            }
            cJSON_Delete(arr);
        }

        cJSON *msg = build_message_json(m);
        if (msg) {
            cJSON_AddItemToArray(msgs_arr, msg);
        }
    }
    cJSON_AddItemToObject(root, "messages", msgs_arr);

    if (tools_json) {
        cJSON *tools = cJSON_Parse(tools_json);
        if (tools) {
            cJSON_AddItemToObject(root, "tools", tools);
        }
    }

    return root;
}

static void
parse_tool_calls(cJSON *tc_arr, struct sc_ai_tool_call_list *out) {
    out->calls = NULL;
    out->count = 0;

    int n = cJSON_GetArraySize(tc_arr);
    if (n <= 0) {
        return;
    }

    out->calls = calloc(n, sizeof(struct sc_ai_tool_call));
    if (!out->calls) {
        return;
    }
    out->count = n;

    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(tc_arr, i);
        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *func = cJSON_GetObjectItem(item, "function");

        if (id && id->valuestring) {
            out->calls[i].id = strdup(id->valuestring);
        }
        if (func) {
            cJSON *name = cJSON_GetObjectItem(func, "name");
            cJSON *args = cJSON_GetObjectItem(func, "arguments");
            if (name && name->valuestring) {
                out->calls[i].function_name = strdup(name->valuestring);
            }
            if (args) {
                if (args->valuestring) {
                    out->calls[i].arguments_json = strdup(args->valuestring);
                } else {
                    char *s = cJSON_PrintUnformatted(args);
                    out->calls[i].arguments_json = s;
                }
            }
        }
    }
}

char *
sc_openrouter_build_body(const struct sc_openrouter_config *config,
                         const struct sc_ai_message_list *messages,
                         const char *tools_json) {
    cJSON *req = build_request_json(config, messages, tools_json);
    if (!req) {
        return NULL;
    }

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

struct sc_openrouter_response
sc_openrouter_chat_with_body(const struct sc_openrouter_config *config,
                             const char *body) {
    struct sc_openrouter_response resp = {0};

    if (!body) {
        resp.error = strdup("Failed to build request JSON");
        return resp;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        resp.error = strdup("Failed to init curl");
        return resp;
    }

    const char *base = config->base_url ? config->base_url : DEFAULT_BASE_URL;
    size_t url_len = strlen(base) + 32;
    char *url = malloc(url_len);
    if (!url) {
        resp.error = strdup("OOM");
        curl_easy_cleanup(curl);
        return resp;
    }
    snprintf(url, url_len, "%s/chat/completions", base);

    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
             config->api_key);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header);

    struct curl_buffer response_buf = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        resp.error = strdup(curl_easy_strerror(res));
        goto cleanup;
    }

    resp.raw_json = response_buf.data ? strdup(response_buf.data) : NULL;

    // Parse response
    cJSON *json = cJSON_Parse(response_buf.data);
    if (!json) {
        resp.error = strdup("Failed to parse response JSON");
        goto cleanup;
    }

    cJSON *error_obj = cJSON_GetObjectItem(json, "error");
    if (error_obj) {
        cJSON *err_msg = cJSON_GetObjectItem(error_obj, "message");
        if (err_msg && err_msg->valuestring) {
            resp.error = strdup(err_msg->valuestring);
        } else {
            resp.error = strdup("Unknown API error");
        }
        cJSON_Delete(json);
        goto cleanup;
    }

    cJSON *choices = cJSON_GetObjectItem(json, "choices");
    if (!choices || cJSON_GetArraySize(choices) == 0) {
        resp.error = strdup("No choices in response");
        cJSON_Delete(json);
        goto cleanup;
    }

    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    if (!message) {
        resp.error = strdup("No message in choice");
        cJSON_Delete(json);
        goto cleanup;
    }

    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && content->valuestring) {
        resp.content = strdup(content->valuestring);
    }

    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls)) {
        parse_tool_calls(tool_calls, &resp.tool_calls);
    }

    resp.success = true;
    cJSON_Delete(json);

cleanup:
    free(url);
    free(response_buf.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return resp;
}

struct sc_openrouter_response
sc_openrouter_chat(const struct sc_openrouter_config *config,
                   const struct sc_ai_message_list *messages,
                   const char *tools_json) {
    char *body = sc_openrouter_build_body(config, messages, tools_json);
    struct sc_openrouter_response resp =
        sc_openrouter_chat_with_body(config, body);
    free(body);
    return resp;
}

void
sc_openrouter_response_destroy(struct sc_openrouter_response *resp) {
    free(resp->content);
    resp->content = NULL;
    free(resp->raw_json);
    resp->raw_json = NULL;
    free(resp->error);
    resp->error = NULL;
    sc_ai_tool_call_list_destroy(&resp->tool_calls);
}

// Message list helpers

void
sc_ai_message_list_init(struct sc_ai_message_list *list) {
    list->messages = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool
ensure_capacity(struct sc_ai_message_list *list) {
    if (list->count < list->capacity) {
        return true;
    }
    size_t new_cap = list->capacity ? list->capacity * 2 : 8;
    struct sc_ai_message *new_msgs =
        realloc(list->messages, new_cap * sizeof(struct sc_ai_message));
    if (!new_msgs) {
        return false;
    }
    list->messages = new_msgs;
    list->capacity = new_cap;
    return true;
}

bool
sc_ai_message_list_push(struct sc_ai_message_list *list,
                        const char *role, const char *content) {
    if (!ensure_capacity(list)) {
        return false;
    }
    struct sc_ai_message *msg = &list->messages[list->count];
    memset(msg, 0, sizeof(*msg));
    msg->role = strdup(role);
    msg->content = content ? strdup(content) : NULL;
    list->count++;
    return true;
}

bool
sc_ai_message_list_push_tool_result(struct sc_ai_message_list *list,
                                    const char *tool_call_id,
                                    const char *name,
                                    const char *content) {
    if (!ensure_capacity(list)) {
        return false;
    }
    struct sc_ai_message *msg = &list->messages[list->count];
    memset(msg, 0, sizeof(*msg));
    msg->role = strdup("tool");
    msg->content = content ? strdup(content) : NULL;
    msg->tool_call_id = tool_call_id ? strdup(tool_call_id) : NULL;
    msg->name = name ? strdup(name) : NULL;
    list->count++;
    return true;
}

bool
sc_ai_message_list_push_assistant_tool_calls(struct sc_ai_message_list *list,
                                             const char *content,
                                             const char *tool_calls_json) {
    if (!ensure_capacity(list)) {
        return false;
    }
    struct sc_ai_message *msg = &list->messages[list->count];
    memset(msg, 0, sizeof(*msg));
    msg->role = strdup("assistant");
    msg->content = content ? strdup(content) : NULL;
    msg->tool_calls_json = tool_calls_json ? strdup(tool_calls_json) : NULL;
    list->count++;
    return true;
}

bool
sc_ai_message_list_push_image(struct sc_ai_message_list *list,
                              const char *text,
                              const char *base64_png) {
    if (!ensure_capacity(list)) {
        return false;
    }

    // Build multimodal content JSON array
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return false;
    }

    if (text) {
        cJSON *text_part = cJSON_CreateObject();
        cJSON_AddStringToObject(text_part, "type", "text");
        cJSON_AddStringToObject(text_part, "text", text);
        cJSON_AddItemToArray(arr, text_part);
    }

    cJSON *img_part = cJSON_CreateObject();
    cJSON_AddStringToObject(img_part, "type", "image_url");
    cJSON *img_url = cJSON_CreateObject();

    // Build data URI
    size_t uri_len = strlen("data:image/jpeg;base64,") + strlen(base64_png) + 1;
    char *uri = malloc(uri_len);
    if (!uri) {
        cJSON_Delete(arr);
        return false;
    }
    snprintf(uri, uri_len, "data:image/jpeg;base64,%s", base64_png);
    cJSON_AddStringToObject(img_url, "url", uri);
    free(uri);

    cJSON_AddItemToObject(img_part, "image_url", img_url);
    cJSON_AddItemToArray(arr, img_part);

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (!json_str) {
        return false;
    }

    struct sc_ai_message *msg = &list->messages[list->count];
    memset(msg, 0, sizeof(*msg));
    msg->role = strdup("user");
    msg->content = json_str; // takes ownership
    list->count++;
    return true;
}

void
sc_ai_message_list_destroy(struct sc_ai_message_list *list) {
    for (size_t i = 0; i < list->count; i++) {
        struct sc_ai_message *msg = &list->messages[i];
        free(msg->role);
        free(msg->content);
        free(msg->tool_call_id);
        free(msg->name);
        free(msg->tool_calls_json);
    }
    free(list->messages);
    list->messages = NULL;
    list->count = 0;
    list->capacity = 0;
}

void
sc_ai_tool_call_list_destroy(struct sc_ai_tool_call_list *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->calls[i].id);
        free(list->calls[i].function_name);
        free(list->calls[i].arguments_json);
    }
    free(list->calls);
    list->calls = NULL;
    list->count = 0;
}
