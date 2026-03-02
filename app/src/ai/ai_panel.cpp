// ai_panel.cpp — Dear ImGui UI for the AI control panel
// This file is C++ and must NOT include headers that use <stdatomic.h>.
// Use ai_agent_bridge.h for all agent access.

#include <cstdlib>
#include <cstring>
#include <cstdio>

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

extern "C" {
#include "ai/ai_agent_bridge.h"
#include "ai/ai_panel.h"
}

static struct {
    SDL_Window *window;
    SDL_GLContext gl_context;
    struct sc_ai_agent *agent;
    bool open;

    // Screenshot info
    int screenshot_w;
    int screenshot_h;

    // UI state
    char prompt_buf[2048];
    char api_key_buf[256];
    char model_buf[256];
    char base_url_buf[512];
    char macro_prompt_buf[512];

    // Macro interval selection
    int macro_interval_idx; // 0=1s, 1=3s, 2=5s, 3=10s

    bool scroll_to_bottom;
    bool settings_changed;
} panel;

static const int MACRO_INTERVALS[] = {1000, 3000, 5000, 10000};
static const char *MACRO_INTERVAL_LABELS[] = {"1s", "3s", "5s", "10s"};

static void
render_screenshot_section() {
    ImGui::Text("Screenshot Preview");
    ImGui::Separator();

    sc_ai_agent_bridge_lock(panel.agent);
    panel.screenshot_w = sc_ai_agent_bridge_get_screen_width(panel.agent);
    panel.screenshot_h = sc_ai_agent_bridge_get_screen_height(panel.agent);
    sc_ai_agent_bridge_unlock(panel.agent);

    if (panel.screenshot_w > 0 && panel.screenshot_h > 0) {
        ImGui::Text("Screen: %dx%d", panel.screenshot_w, panel.screenshot_h);
    } else {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                          "No screenshot captured yet");
    }

    if (ImGui::Button("Take Screenshot", ImVec2(-1, 0))) {
        char *prompt = strdup("[Take a screenshot and describe what you see]");
        if (prompt) {
            sc_ai_agent_bridge_submit_prompt(panel.agent, prompt);
            panel.scroll_to_bottom = true;
        }
    }
    ImGui::Spacing();
}

static void
copy_all_chat(struct sc_ai_agent *agent) {
    // Build a plain-text copy of the entire chat
    size_t count = sc_ai_agent_bridge_get_message_count(agent);
    // Estimate buffer size
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        const char *role = sc_ai_agent_bridge_get_message_role(agent, i);
        const char *content = sc_ai_agent_bridge_get_message_content(agent, i);
        if (!role || !content) continue;
        if (strcmp(role, "system") == 0) continue;
        total += strlen(role) + strlen(content) + 16;
    }
    if (total == 0) return;

    char *buf = (char *)malloc(total + 1);
    if (!buf) return;
    buf[0] = '\0';
    size_t offset = 0;

    for (size_t i = 0; i < count; i++) {
        const char *role = sc_ai_agent_bridge_get_message_role(agent, i);
        const char *content = sc_ai_agent_bridge_get_message_content(agent, i);
        if (!role || !content) continue;
        if (strcmp(role, "system") == 0) continue;

        const char *display = content;
        // Skip image JSON content
        if (content[0] == '[' && strstr(content, "image_url")) {
            display = "[Screenshot]";
        }

        int written = snprintf(buf + offset, total + 1 - offset,
                               "[%s] %s\n", role, display);
        if (written > 0) offset += (size_t)written;
    }

    ImGui::SetClipboardText(buf);
    free(buf);
}

static void
render_chat_section() {
    ImGui::Text("Chat");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy All")) {
        sc_ai_agent_bridge_lock(panel.agent);
        copy_all_chat(panel.agent);
        sc_ai_agent_bridge_unlock(panel.agent);
    }
    ImGui::Separator();

    float chat_height = ImGui::GetContentRegionAvail().y - 120;
    if (chat_height < 100) chat_height = 100;

    ImGui::BeginChild("ChatHistory", ImVec2(0, chat_height), true,
                      ImGuiWindowFlags_HorizontalScrollbar);

    sc_ai_agent_bridge_lock(panel.agent);
    size_t count = sc_ai_agent_bridge_get_message_count(panel.agent);
    for (size_t i = 0; i < count; i++) {
        const char *role =
            sc_ai_agent_bridge_get_message_role(panel.agent, i);
        const char *content =
            sc_ai_agent_bridge_get_message_content(panel.agent, i);
        const char *name =
            sc_ai_agent_bridge_get_message_name(panel.agent, i);

        if (!role || !content) continue;
        if (strcmp(role, "system") == 0) continue;

        ImGui::PushID((int)i);

        ImVec4 color;
        const char *prefix;
        if (strcmp(role, "user") == 0) {
            color = ImVec4(0.4f, 0.8f, 1.0f, 1.0f);
            prefix = "You";
        } else if (strcmp(role, "assistant") == 0) {
            color = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
            prefix = "AI";
        } else if (strcmp(role, "tool") == 0) {
            color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
            prefix = name ? name : "tool";
        } else {
            color = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
            prefix = role;
        }

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("[%s]", prefix);
        ImGui::PopStyleColor();
        ImGui::SameLine();

        const char *display_content;
        bool is_image = (content[0] == '[' && strstr(content, "image_url"));
        if (is_image) {
            display_content = "[Screenshot + message sent]";
        } else {
            display_content = content;
        }

        ImGui::TextWrapped("%s", display_content);

        // Right-click context menu for copying individual message
        if (ImGui::BeginPopupContextItem("msg_ctx")) {
            if (ImGui::Selectable("Copy")) {
                ImGui::SetClipboardText(display_content);
            }
            ImGui::EndPopup();
        }

        ImGui::Spacing();
        ImGui::PopID();
    }
    sc_ai_agent_bridge_unlock(panel.agent);

    if (panel.scroll_to_bottom) {
        ImGui::SetScrollHereY(1.0f);
        panel.scroll_to_bottom = false;
    }

    ImGui::EndChild();

    // Input area
    ImGui::Spacing();
    float send_btn_width = 60;
    float clear_btn_width = 50;
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - send_btn_width
                         - clear_btn_width - 16);
    bool enter_pressed = ImGui::InputText("##prompt", panel.prompt_buf,
                                           sizeof(panel.prompt_buf),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();

    if ((ImGui::Button("Send", ImVec2(send_btn_width, 0)) || enter_pressed)
            && panel.prompt_buf[0] != '\0') {
        char *prompt = strdup(panel.prompt_buf);
        if (prompt) {
            sc_ai_agent_bridge_submit_prompt(panel.agent, prompt);
            panel.prompt_buf[0] = '\0';
            panel.scroll_to_bottom = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(clear_btn_width, 0))) {
        sc_ai_agent_bridge_clear_history(panel.agent);
    }
}

static void
render_macro_section() {
    if (!ImGui::CollapsingHeader("Macros")) {
        return;
    }

    sc_ai_agent_bridge_lock(panel.agent);
    size_t count = sc_ai_agent_bridge_get_macro_count(panel.agent);
    bool running = sc_ai_agent_bridge_get_macro_running(panel.agent);
    size_t current = sc_ai_agent_bridge_get_macro_current(panel.agent);
    for (size_t i = 0; i < count; i++) {
        ImGui::PushID((int)i);

        bool is_current = running && (i == current);
        const char *prompt =
            sc_ai_agent_bridge_get_macro_prompt(panel.agent, i);

        if (is_current) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "<-");
            ImGui::SameLine();
        }

        ImGui::TextWrapped("[%u] %s", (unsigned)i, prompt ? prompt : "");
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            sc_ai_agent_bridge_unlock(panel.agent);
            sc_ai_agent_bridge_remove_macro(panel.agent, i);
            ImGui::PopID();
            return;
        }
        ImGui::PopID();
    }
    sc_ai_agent_bridge_unlock(panel.agent);

    ImGui::InputText("Prompt", panel.macro_prompt_buf,
                     sizeof(panel.macro_prompt_buf));
    if (ImGui::Button("Add Macro") && panel.macro_prompt_buf[0]) {
        sc_ai_agent_bridge_add_macro(panel.agent, panel.macro_prompt_buf);
        panel.macro_prompt_buf[0] = '\0';
    }

    ImGui::Spacing();

    ImGui::Combo("Interval", &panel.macro_interval_idx,
                 MACRO_INTERVAL_LABELS, 4);

    if (running) {
        if (ImGui::Button("Stop Macro", ImVec2(-1, 0))) {
            sc_ai_agent_bridge_set_macro_running(panel.agent, false);
        }
    } else {
        if (ImGui::Button("Start Macro", ImVec2(-1, 0))) {
            sc_ai_agent_bridge_set_macro_interval(panel.agent,
                MACRO_INTERVALS[panel.macro_interval_idx]);
            sc_ai_agent_bridge_set_macro_running(panel.agent, true);
        }
    }
}

static void
render_settings_section() {
    if (!ImGui::CollapsingHeader("Settings")) {
        return;
    }

    if (ImGui::InputText("API Key", panel.api_key_buf,
                         sizeof(panel.api_key_buf),
                         ImGuiInputTextFlags_Password)) {
        panel.settings_changed = true;
    }
    if (ImGui::InputText("Model", panel.model_buf,
                         sizeof(panel.model_buf))) {
        panel.settings_changed = true;
    }
    if (ImGui::InputText("Base URL", panel.base_url_buf,
                         sizeof(panel.base_url_buf))) {
        panel.settings_changed = true;
    }

    if (panel.settings_changed && ImGui::Button("Apply Settings")) {
        sc_ai_agent_bridge_set_config(panel.agent,
            panel.api_key_buf[0] ? panel.api_key_buf : NULL,
            panel.model_buf[0] ? panel.model_buf : NULL,
            panel.base_url_buf[0] ? panel.base_url_buf : NULL);
        panel.settings_changed = false;
    }
}

extern "C" bool
sc_ai_panel_init(struct sc_ai_agent *agent) {
    memset(&panel, 0, sizeof(panel));
    panel.agent = agent;
    panel.macro_interval_idx = 1; // 3s default

    // Initialize settings from agent config
    sc_ai_agent_bridge_lock(agent);
    const char *key = sc_ai_agent_bridge_get_config_api_key(agent);
    const char *model = sc_ai_agent_bridge_get_config_model(agent);
    const char *base = sc_ai_agent_bridge_get_config_base_url(agent);
    if (key) {
        snprintf(panel.api_key_buf, sizeof(panel.api_key_buf), "%s", key);
    }
    if (model) {
        snprintf(panel.model_buf, sizeof(panel.model_buf), "%s", model);
    }
    if (base) {
        snprintf(panel.base_url_buf, sizeof(panel.base_url_buf), "%s", base);
    }
    sc_ai_agent_bridge_unlock(agent);

    // Create SDL window with OpenGL context
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    panel.window = SDL_CreateWindow(
        "scrcpy AI Panel",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        480, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    if (!panel.window) {
        return false;
    }

    panel.gl_context = SDL_GL_CreateContext(panel.window);
    if (!panel.gl_context) {
        SDL_DestroyWindow(panel.window);
        return false;
    }

    SDL_GL_MakeCurrent(panel.window, panel.gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Load a Korean-capable system font
    ImFontConfig font_cfg;
    font_cfg.OversampleH = 2;
    font_cfg.OversampleV = 1;
    const ImWchar *korean_ranges = io.Fonts->GetGlyphRangesKorean();

    const char *font_paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\malgun.ttf",
        "C:\\Windows\\Fonts\\malgungbd.ttf",
        "C:\\Windows\\Fonts\\segoeui.ttf",
#else
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#endif
        NULL,
    };

    bool font_loaded = false;
    for (int i = 0; font_paths[i]; i++) {
        ImFont *font = io.Fonts->AddFontFromFileTTF(
            font_paths[i], 16.0f, &font_cfg, korean_ranges);
        if (font) {
            font_loaded = true;
            break;
        }
    }
    if (!font_loaded) {
        io.Fonts->AddFontDefault();
    }

    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(1.2f);

    ImGui_ImplSDL2_InitForOpenGL(panel.window, panel.gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    panel.open = true;
    return true;
}

extern "C" void
sc_ai_panel_destroy(void) {
    if (!panel.window) {
        return;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(panel.gl_context);
    SDL_DestroyWindow(panel.window);
    panel.window = NULL;
    panel.open = false;
}

extern "C" bool
sc_ai_panel_handle_event(const SDL_Event *event) {
    if (!panel.open || !panel.window) {
        return false;
    }

    Uint32 window_id = SDL_GetWindowID(panel.window);
    bool for_us = false;

    switch (event->type) {
        case SDL_WINDOWEVENT:
            for_us = (event->window.windowID == window_id);
            if (for_us && event->window.event == SDL_WINDOWEVENT_CLOSE) {
                panel.open = false;
                return true;
            }
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            for_us = (event->key.windowID == window_id);
            break;
        case SDL_TEXTINPUT:
            for_us = (event->text.windowID == window_id);
            break;
        case SDL_MOUSEMOTION:
            for_us = (event->motion.windowID == window_id);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            for_us = (event->button.windowID == window_id);
            break;
        case SDL_MOUSEWHEEL:
            for_us = (event->wheel.windowID == window_id);
            break;
        default:
            break;
    }

    if (!for_us) {
        return false;
    }

    SDL_GL_MakeCurrent(panel.window, panel.gl_context);
    ImGui_ImplSDL2_ProcessEvent(event);
    return true;
}

extern "C" void
sc_ai_panel_render(void) {
    if (!panel.open || !panel.window) {
        return;
    }

    SDL_GL_MakeCurrent(panel.window, panel.gl_context);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("AI Control", NULL, flags);

    render_screenshot_section();
    render_chat_section();
    ImGui::Spacing();
    render_macro_section();
    ImGui::Spacing();
    render_settings_section();

    ImGui::End();

    ImGui::Render();
    int display_w, display_h;
    SDL_GL_GetDrawableSize(panel.window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(panel.window);
}

extern "C" bool
sc_ai_panel_is_open(void) {
    return panel.open;
}
