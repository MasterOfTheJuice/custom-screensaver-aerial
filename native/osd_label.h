/*
 * Place-name overlay on the SDL UI plane.
 *
 * The ACB video is a hardware plane under this window. Clearing the renderer
 * to transparent (the same clear the player already does once at startup)
 * leaves that plane visible. White text with a dark shadow is drawn in the
 * QML's bottom-left name slot and faded the way the QML OSD fades:
 * 3 s in, then out over the last 5 s.
 *
 * SDL_ttf on the TV is 2.0.14, older than the SDK copy, so it is dlopen'd
 * and not linked. A missing font or library leaves the video untouched.
 */
#pragma once

#include <SDL.h>

typedef struct osd_label osd_label;

osd_label *osd_label_create(SDL_Window *window, SDL_Renderer *renderer, const char *font_path);
void osd_label_destroy(osd_label *osd);

/* `duration_s` is the title length, already reduced by a debug cap when one
 * is set. `opacity_pct` is 0..100 and matches settings.osdOpacity. */
void osd_label_set(osd_label *osd, const char *title, double duration_s, int opacity_pct);

/* `media_s` is this title's composition time. Redraws only while the fade
 * alpha is still moving. */
void osd_label_tick(osd_label *osd, double media_s);
