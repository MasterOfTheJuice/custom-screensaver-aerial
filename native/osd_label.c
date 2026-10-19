#include "osd_label.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*ttf_init_fn)(void);
typedef void (*ttf_quit_fn)(void);
typedef void *(*ttf_open_fn)(const char *file, int ptsize);
typedef SDL_Surface *(*ttf_render_fn)(void *font, const char *text, SDL_Color fg);
typedef void (*ttf_close_fn)(void *font);
typedef const char *(*ttf_error_fn)(void);

struct osd_label {
    SDL_Renderer *renderer;
    int win_w;
    int win_h;
    int font_px;
    void *lib;
    void *font;
    ttf_render_fn render;
    ttf_close_fn close_font;
    ttf_quit_fn quit;
    SDL_Texture *text;
    SDL_Texture *shadow;
    int tex_w;
    int tex_h;
    char title[256];
    double duration_s;
    int opacity_pct;
    int drawn_alpha;
    int failed;
};

static void *load_sym(void *lib, const char *name) {
    void *sym = dlsym(lib, name);
    if (sym == NULL) {
        fprintf(stderr, "[osd] missing %s\n", name);
    }
    return sym;
}

osd_label *osd_label_create(SDL_Window *window, SDL_Renderer *renderer, const char *font_path) {
    osd_label *osd;
    ttf_init_fn init;
    ttf_open_fn open_font;
    ttf_error_fn err_fn;
    SDL_RendererInfo info;
    int w = 0;
    int h = 0;

    if (window == NULL || renderer == NULL || font_path == NULL || font_path[0] == '\0') {
        fprintf(stderr, "[osd] label off (no window or font path)\n");
        return NULL;
    }
    osd = calloc(1, sizeof(*osd));
    if (osd == NULL) {
        return NULL;
    }
    osd->renderer = renderer;
    osd->drawn_alpha = -1;
    SDL_GetWindowSize(window, &w, &h);
    if (w < 320 || h < 240) {
        w = 1920;
        h = 1080;
    }
    osd->win_w = w;
    osd->win_h = h;
    osd->font_px = h * 56 / 1080;
    if (osd->font_px < 28) {
        osd->font_px = 28;
    }
    if (SDL_GetRendererInfo(renderer, &info) == 0) {
        fprintf(stderr, "[osd] renderer %s window %dx%d font %d px\n", info.name, w, h, osd->font_px);
    }

    osd->lib = dlopen("libSDL2_ttf-2.0.so.0", RTLD_NOW);
    if (osd->lib == NULL) {
        osd->lib = dlopen("/usr/lib/libSDL2_ttf-2.0.so.0", RTLD_NOW);
    }
    if (osd->lib == NULL) {
        fprintf(stderr, "[osd] SDL_ttf unavailable (%s); video stays up, no label\n", dlerror());
        osd->failed = 1;
        return osd;
    }
    init = load_sym(osd->lib, "TTF_Init");
    open_font = load_sym(osd->lib, "TTF_OpenFont");
    osd->render = load_sym(osd->lib, "TTF_RenderUTF8_Blended");
    osd->close_font = load_sym(osd->lib, "TTF_CloseFont");
    osd->quit = load_sym(osd->lib, "TTF_Quit");
    /* 2.0.14 on this TV does not export TTF_GetError. Optional. */
    err_fn = dlsym(osd->lib, "TTF_GetError");
    if (init == NULL || open_font == NULL || osd->render == NULL || osd->close_font == NULL) {
        osd->failed = 1;
        return osd;
    }
    if (init() != 0) {
        fprintf(stderr, "[osd] TTF_Init failed: %s\n", err_fn != NULL ? err_fn() : "?");
        osd->failed = 1;
        return osd;
    }
    osd->font = open_font(font_path, osd->font_px);
    if (osd->font == NULL) {
        fprintf(stderr, "[osd] font %s failed: %s\n", font_path, err_fn != NULL ? err_fn() : "?");
        osd->failed = 1;
        return osd;
    }
    fprintf(stderr, "[osd] font %s\n", font_path);
    return osd;
}

void osd_label_destroy(osd_label *osd) {
    if (osd == NULL) {
        return;
    }
    if (osd->text != NULL) {
        SDL_DestroyTexture(osd->text);
    }
    if (osd->shadow != NULL) {
        SDL_DestroyTexture(osd->shadow);
    }
    if (osd->font != NULL && osd->close_font != NULL) {
        osd->close_font(osd->font);
    }
    if (osd->quit != NULL) {
        osd->quit();
    }
    if (osd->lib != NULL) {
        dlclose(osd->lib);
    }
    free(osd);
}

static void drop_textures(osd_label *osd) {
    if (osd->text != NULL) {
        SDL_DestroyTexture(osd->text);
        osd->text = NULL;
    }
    if (osd->shadow != NULL) {
        SDL_DestroyTexture(osd->shadow);
        osd->shadow = NULL;
    }
    osd->tex_w = 0;
    osd->tex_h = 0;
    osd->drawn_alpha = -1;
}

static SDL_Texture *texture_from(SDL_Renderer *renderer, SDL_Surface *surface) {
    SDL_Texture *tex;
    if (surface == NULL) {
        return NULL;
    }
    tex = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (tex != NULL) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    }
    return tex;
}

void osd_label_set(osd_label *osd, const char *title, double duration_s, int opacity_pct) {
    SDL_Color white = {255, 255, 255, 255};
    SDL_Color black = {0, 0, 0, 255};
    SDL_Surface *surf;
    if (osd == NULL) {
        return;
    }
    if (title == NULL) {
        title = "";
    }
    snprintf(osd->title, sizeof(osd->title), "%s", title);
    osd->duration_s = duration_s;
    if (opacity_pct < 0) {
        opacity_pct = 0;
    }
    if (opacity_pct > 100) {
        opacity_pct = 100;
    }
    osd->opacity_pct = opacity_pct;
    drop_textures(osd);
    if (osd->failed || osd->font == NULL || osd->title[0] == '\0' || opacity_pct <= 0) {
        fprintf(stderr, "[osd] title hidden opacity=%d text='%s'\n", opacity_pct, osd->title);
        return;
    }
    surf = osd->render(osd->font, osd->title, white);
    osd->text = texture_from(osd->renderer, surf);
    surf = osd->render(osd->font, osd->title, black);
    osd->shadow = texture_from(osd->renderer, surf);
    if (osd->text != NULL) {
        SDL_QueryTexture(osd->text, NULL, NULL, &osd->tex_w, &osd->tex_h);
    }
    fprintf(stderr, "[osd] title \"%s\" opacity=%d duration=%.1f size=%dx%d\n", osd->title,
            opacity_pct, duration_s, osd->tex_w, osd->tex_h);
}

static int alpha_for(const osd_label *osd, double media_s) {
    double target;
    double fin = 3.0;
    double fout = 5.0;
    double a;
    double duration;
    if (osd->failed || osd->text == NULL || osd->opacity_pct <= 0 || osd->title[0] == '\0') {
        return 0;
    }
    target = osd->opacity_pct / 100.0;
    duration = osd->duration_s;
    if (duration > 0.5 && duration < fin + fout + 0.5) {
        fin = duration * 0.2;
        if (fin < 0.4) {
            fin = 0.4;
        }
        fout = duration * 0.3;
    }
    if (media_s < 0) {
        media_s = 0;
    }
    if (media_s < fin) {
        a = target * (media_s / fin);
    } else if (duration > 0 && media_s >= duration - fout) {
        double t = (media_s - (duration - fout)) / fout;
        if (t < 0) {
            t = 0;
        }
        if (t > 1) {
            t = 1;
        }
        a = target * (1.0 - t);
    } else {
        a = target;
    }
    if (a < 0) {
        a = 0;
    }
    if (a > 1) {
        a = 1;
    }
    return (int) (a * 255.0 + 0.5);
}

void osd_label_tick(osd_label *osd, double media_s) {
    int alpha;
    int margin_x;
    int margin_y;
    int right;
    int y;
    int max_w;
    SDL_Rect dst;
    int dw;
    int dh;
    if (osd == NULL || osd->renderer == NULL) {
        return;
    }
    alpha = alpha_for(osd, media_s);
    /* Quantize so a 30 fps feed does not present on every access unit. */
    alpha = (alpha / 8) * 8;
    if (alpha == osd->drawn_alpha) {
        return;
    }
    SDL_SetRenderDrawBlendMode(osd->renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(osd->renderer, 0, 0, 0, 0);
    SDL_RenderClear(osd->renderer);
    if (alpha > 0 && osd->text != NULL && osd->tex_w > 0 && osd->tex_h > 0) {
        margin_y = osd->win_h * 65 / 1080;
        /* The QML name sits at a 65 px margin, 90% down the inset. On this
         * set the room camera frames the right of the panel and cuts near
         * x=550 at caption height, so that corner is only a tail
         * ("fornia", "Building") or is missing ("North Africa"). 40% across
         * and 74% down keeps a lower-band caption and puts the whole string
         * on the photographed part of the panel. */
        margin_x = osd->win_w * 40 / 100;
        right = osd->win_w * 4 / 100;
        if (margin_x < 16) {
            margin_x = 16;
        }
        if (right < 8) {
            right = 8;
        }
        max_w = osd->win_w - margin_x - right;
        dw = osd->tex_w;
        dh = osd->tex_h;
        if (dw > max_w && dw > 0) {
            dh = dh * max_w / dw;
            dw = max_w;
        }
        y = osd->win_h * 74 / 100;
        (void) margin_y;
        if (y + dh > osd->win_h - 8) {
            y = osd->win_h - 8 - dh;
        }
        if (y < 0) {
            y = 0;
        }
        dst.x = margin_x + 2;
        dst.y = y + 2;
        dst.w = dw;
        dst.h = dh;
        if (osd->shadow != NULL) {
            SDL_SetTextureAlphaMod(osd->shadow, (Uint8) alpha);
            SDL_RenderCopy(osd->renderer, osd->shadow, NULL, &dst);
        }
        dst.x = margin_x;
        dst.y = y;
        SDL_SetTextureAlphaMod(osd->text, (Uint8) alpha);
        SDL_RenderCopy(osd->renderer, osd->text, NULL, &dst);
    }
    SDL_RenderPresent(osd->renderer);
    osd->drawn_alpha = alpha;
}
