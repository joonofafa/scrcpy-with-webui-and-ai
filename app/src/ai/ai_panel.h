#ifndef SC_AI_PANEL_H
#define SC_AI_PANEL_H

#include <stdbool.h>

struct sc_ai_agent;

typedef union SDL_Event SDL_Event;

#ifdef __cplusplus
extern "C" {
#endif

bool
sc_ai_panel_init(struct sc_ai_agent *agent);

void
sc_ai_panel_destroy(void);

bool
sc_ai_panel_handle_event(const SDL_Event *event);

void
sc_ai_panel_render(void);

bool
sc_ai_panel_is_open(void);

#ifdef __cplusplus
}
#endif

#endif
