#include "ai_ocr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <io.h>
#else
# include <unistd.h>
#endif

#include "../../deps/cjson/cJSON.h"
#include "util/file.h"
#include "util/log.h"
#include "util/process.h"

#define OCR_SCRIPT_NAME "ai_ocr_server.py"

// Write exactly len bytes to a pipe. Returns true on success.
static bool
pipe_write_all(sc_pipe pipe, const uint8_t *data, size_t len) {
#ifdef _WIN32
    while (len > 0) {
        DWORD written;
        if (!WriteFile(pipe, data, (DWORD)len, &written, NULL)) {
            return false;
        }
        data += written;
        len -= written;
    }
#else
    while (len > 0) {
        ssize_t w = write(pipe, data, len);
        if (w <= 0) {
            return false;
        }
        data += w;
        len -= (size_t)w;
    }
#endif
    return true;
}

// Send a length-prefixed message (4-byte BE length + payload)
static bool
pipe_send_msg(sc_pipe pipe, const uint8_t *data, size_t len) {
    uint8_t header[4];
    header[0] = (uint8_t)((len >> 24) & 0xFF);
    header[1] = (uint8_t)((len >> 16) & 0xFF);
    header[2] = (uint8_t)((len >> 8) & 0xFF);
    header[3] = (uint8_t)(len & 0xFF);
    if (!pipe_write_all(pipe, header, 4)) {
        return false;
    }
    if (len > 0) {
        return pipe_write_all(pipe, data, len);
    }
    return true;
}

// Receive a length-prefixed message. Caller must free *out_data.
// Returns true on success. Sets *out_len to payload length.
static bool
pipe_recv_msg(sc_pipe pipe, uint8_t **out_data, size_t *out_len) {
    char header[4];
    ssize_t r = sc_pipe_read_all(pipe, header, 4);
    if (r != 4) {
        return false;
    }
    uint32_t len = ((uint32_t)(uint8_t)header[0] << 24)
                 | ((uint32_t)(uint8_t)header[1] << 16)
                 | ((uint32_t)(uint8_t)header[2] << 8)
                 | ((uint32_t)(uint8_t)header[3]);

    if (len == 0) {
        *out_data = NULL;
        *out_len = 0;
        return true;
    }

    uint8_t *buf = malloc(len + 1);
    if (!buf) {
        return false;
    }

    r = sc_pipe_read_all(pipe, (char *)buf, len);
    if (r != (ssize_t)len) {
        free(buf);
        return false;
    }
    buf[len] = '\0'; // null-terminate for JSON parsing
    *out_data = buf;
    *out_len = len;
    return true;
}

// Find the OCR script path. Tries:
//   1. Same directory as the executable
//   2. PREFIX/share/scrcpy/
//   3. SCRCPY_OCR_SCRIPT environment variable
static char *
find_ocr_script(void) {
    // 1. Next to the executable
    char *local = sc_file_get_local_path(OCR_SCRIPT_NAME);
    if (local && sc_file_is_regular(local)) {
        return local;
    }
    free(local);

    // 2. Installed path
    const char *installed = PREFIX "/share/scrcpy/" OCR_SCRIPT_NAME;
    if (sc_file_is_regular(installed)) {
        return strdup(installed);
    }

    // 3. Environment variable
    const char *env = getenv("SCRCPY_OCR_SCRIPT");
    if (env && sc_file_is_regular(env)) {
        return strdup(env);
    }

    return NULL;
}

// Find a Python interpreter with PaddleOCR available. Tries:
//   1. SCRCPY_OCR_PYTHON environment variable
//   2. ocr-venv/bin/python next to the executable
//   3. ocr-venv/bin/python next to the script
//   4. "python3" (system PATH)
static char *
find_python(const char *script_path) {
    // 1. Environment variable
    const char *env = getenv("SCRCPY_OCR_PYTHON");
    if (env && sc_file_is_regular(env)) {
        return strdup(env);
    }

    // 2. ocr-venv next to the executable
    char *exe_venv = sc_file_get_local_path("ocr-venv/bin/python");
    if (exe_venv && sc_file_is_regular(exe_venv)) {
        return exe_venv;
    }
    free(exe_venv);

    // 3. ocr-venv next to the script
    if (script_path) {
        char *script_copy = strdup(script_path);
        if (script_copy) {
            char *slash = strrchr(script_copy, '/');
            if (slash) {
                *slash = '\0';
                size_t len = strlen(script_copy) + 32;
                char *venv_python = malloc(len);
                if (venv_python) {
                    snprintf(venv_python, len, "%s/ocr-venv/bin/python",
                             script_copy);
                    if (sc_file_is_regular(venv_python)) {
                        free(script_copy);
                        return venv_python;
                    }
                    free(venv_python);
                }
            }
            free(script_copy);
        }
    }

    // 4. System python3
    return strdup("python3");
}

bool
sc_ai_ocr_start(struct sc_ai_ocr *ocr) {
    memset(ocr, 0, sizeof(*ocr));

    char *script = find_ocr_script();
    if (!script) {
        LOGW("AI OCR: script not found (%s), OCR disabled", OCR_SCRIPT_NAME);
        return false;
    }

    char *python = find_python(script);
    LOGI("AI OCR: starting daemon: %s %s", python, script);

    const char *const argv[] = {python, script, NULL};

    sc_pipe pin, pout;
    enum sc_process_result r =
        sc_process_execute_p(argv, &ocr->pid, SC_PROCESS_NO_STDERR,
                             &pin, &pout, NULL);
    free(script);
    free(python);

    if (r != SC_PROCESS_SUCCESS) {
        LOGW("AI OCR: failed to start python (result=%d), OCR disabled", r);
        return false;
    }

    ocr->pipe_stdin = pin;
    ocr->pipe_stdout = pout;

    // Wait for "ready" signal
    uint8_t *msg_data;
    size_t msg_len;
    if (!pipe_recv_msg(ocr->pipe_stdout, &msg_data, &msg_len) || !msg_data) {
        LOGW("AI OCR: daemon did not send ready signal");
        goto fail;
    }

    cJSON *json = cJSON_Parse((char *)msg_data);
    free(msg_data);

    if (!json) {
        LOGW("AI OCR: invalid ready signal JSON");
        goto fail;
    }

    cJSON *status = cJSON_GetObjectItem(json, "status");
    if (!status || !cJSON_IsString(status)) {
        cJSON_Delete(json);
        LOGW("AI OCR: missing status in ready signal");
        goto fail;
    }

    if (strcmp(status->valuestring, "error") == 0) {
        cJSON *msg = cJSON_GetObjectItem(json, "message");
        LOGW("AI OCR: daemon error: %s",
             (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown");
        cJSON_Delete(json);
        goto fail;
    }

    cJSON_Delete(json);

    // Wait for "loaded" signal (model loading takes a few seconds)
    LOGI("AI OCR: waiting for model to load...");
    if (!pipe_recv_msg(ocr->pipe_stdout, &msg_data, &msg_len) || !msg_data) {
        LOGW("AI OCR: daemon did not send loaded signal");
        goto fail;
    }

    json = cJSON_Parse((char *)msg_data);
    free(msg_data);

    if (json) {
        status = cJSON_GetObjectItem(json, "status");
        if (status && cJSON_IsString(status)
                && strcmp(status->valuestring, "error") == 0) {
            cJSON *msg = cJSON_GetObjectItem(json, "message");
            LOGW("AI OCR: model load error: %s",
                 (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown");
            cJSON_Delete(json);
            goto fail;
        }
        cJSON_Delete(json);
    }

    ocr->running = true;
    LOGI("AI OCR: daemon ready and model loaded");
    return true;

fail:
    sc_pipe_close(ocr->pipe_stdin);
    sc_pipe_close(ocr->pipe_stdout);
    sc_process_terminate(ocr->pid);
    sc_process_close(ocr->pid);
    memset(ocr, 0, sizeof(*ocr));
    return false;
}

void
sc_ai_ocr_stop(struct sc_ai_ocr *ocr) {
    if (!ocr->running) {
        return;
    }

    LOGI("AI OCR: stopping daemon");

    // Send sentinel (length=0) to request graceful shutdown
    pipe_send_msg(ocr->pipe_stdin, NULL, 0);

    sc_pipe_close(ocr->pipe_stdin);
    sc_pipe_close(ocr->pipe_stdout);
    sc_process_wait(ocr->pid, true);

    ocr->running = false;
}

bool
sc_ai_ocr_process(struct sc_ai_ocr *ocr,
                   const uint8_t *jpeg_data, size_t jpeg_size,
                   struct sc_ai_ocr_result *result) {
    memset(result, 0, sizeof(*result));

    if (!ocr->running) {
        return false;
    }

restart:
    // Send JPEG data
    if (!pipe_send_msg(ocr->pipe_stdin, jpeg_data, jpeg_size)) {
        LOGW("AI OCR: failed to send data, restarting daemon");
        sc_pipe_close(ocr->pipe_stdin);
        sc_pipe_close(ocr->pipe_stdout);
        sc_process_terminate(ocr->pid);
        sc_process_close(ocr->pid);
        ocr->running = false;
        if (sc_ai_ocr_start(ocr)) {
            goto restart;
        }
        return false;
    }

    // Receive response
    uint8_t *msg_data;
    size_t msg_len;
    if (!pipe_recv_msg(ocr->pipe_stdout, &msg_data, &msg_len) || !msg_data) {
        LOGW("AI OCR: failed to receive response, restarting daemon");
        sc_pipe_close(ocr->pipe_stdin);
        sc_pipe_close(ocr->pipe_stdout);
        sc_process_terminate(ocr->pid);
        sc_process_close(ocr->pid);
        ocr->running = false;
        if (sc_ai_ocr_start(ocr)) {
            goto restart;
        }
        return false;
    }

    // Parse JSON response
    cJSON *json = cJSON_Parse((char *)msg_data);
    free(msg_data);

    if (!json) {
        LOGW("AI OCR: invalid response JSON");
        return false;
    }

    cJSON *error = cJSON_GetObjectItem(json, "error");
    if (error) {
        LOGW("AI OCR: processing error: %s",
             cJSON_IsString(error) ? error->valuestring : "unknown");
        cJSON_Delete(json);
        return false;
    }

    cJSON *texts = cJSON_GetObjectItem(json, "texts");
    if (!texts || !cJSON_IsArray(texts)) {
        cJSON_Delete(json);
        return false;
    }

    int n = cJSON_GetArraySize(texts);
    if (n > SC_AI_OCR_MAX_TEXTS) {
        n = SC_AI_OCR_MAX_TEXTS;
    }

    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(texts, i);
        cJSON *text = cJSON_GetObjectItem(item, "text");
        cJSON *center = cJSON_GetObjectItem(item, "center");
        cJSON *conf = cJSON_GetObjectItem(item, "conf");

        if (!text || !cJSON_IsString(text) || !center
                || !cJSON_IsArray(center) || cJSON_GetArraySize(center) != 2) {
            continue;
        }

        struct sc_ai_ocr_text *t = &result->texts[result->count];
        t->text = strdup(text->valuestring);
        t->center_x = cJSON_GetArrayItem(center, 0)->valueint;
        t->center_y = cJSON_GetArrayItem(center, 1)->valueint;
        t->confidence = conf ? (float)conf->valuedouble : 0.0f;
        result->count++;
    }

    cJSON_Delete(json);
    return true;
}

char *
sc_ai_ocr_format_prompt(const struct sc_ai_ocr_result *result) {
    if (!result || result->count == 0) {
        return NULL;
    }

    // Estimate buffer size: ~80 chars per entry + header
    size_t cap = 256 + result->count * 128;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }

    size_t len = 0;
    len += (size_t)snprintf(buf + len, cap - len,
        "[Detected UI text (use these coordinates for precise tapping):\n");

    for (size_t i = 0; i < result->count; i++) {
        const struct sc_ai_ocr_text *t = &result->texts[i];
        int conf_pct = (int)(t->confidence * 100);
        len += (size_t)snprintf(buf + len, cap - len,
            "- \"%s\" at center(%d, %d) conf=%d%%\n",
            t->text, t->center_x, t->center_y, conf_pct);
    }
    len += (size_t)snprintf(buf + len, cap - len, "]");

    return buf;
}

void
sc_ai_ocr_result_destroy(struct sc_ai_ocr_result *result) {
    for (size_t i = 0; i < result->count; i++) {
        free(result->texts[i].text);
    }
    result->count = 0;
}
