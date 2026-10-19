/*
 * Playing an H.264 + AAC pair through starfish-media-pipeline on webOS 2 - 4.x.
 *
 * The whole of it:
 *
 *   1. bring up SDL and a fullscreen, transparent window
 *   2. create an ACB instance and bind it to the pipeline's media id
 *   3. Load() the pipeline with a BUFFERSTREAM payload
 *   4. wait for LOADCOMPLETED, at which point Play() is called for us
 *   5. Feed() access units in presentation order, in real time
 *   6. pushEOS() and Unload()
 *
 * The decoded video never passes through this process. It goes to a hardware plane behind
 * the SDL surface, which is why step 1 clears to transparent and then leaves the screen
 * alone.
 *
 * Compared with the webOS 5 sample next door, only step 2 differs - and the ABI variant of
 * media/smp/common this links against. Diffing the two mains is a good way to see exactly
 * how little the generations differ once the video plane is factored out.
 */
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>
#include <luna-service2/lunaservice.h>

#include "app_launch.h"
#include "es_file.h"
#include "acb_plane.h"
#include "http_curl.h"
#include "mp4_avc.h"
#include "pacer.h"
#include "osd_label.h"
#include "playlist.h"
#include "sdl_shell.h"
#include "smp_player.h"

/* See smp_payload.h. Overridden per generation from CMake. */
#ifndef SMP_PAUSE_AT_DECODE_TIME
#define SMP_PAUSE_AT_DECODE_TIME true
#endif

/* 1080p matches the webOS 4 Aerial asset (url-1080-H264). fullScreen on
 * setDisplayWindow scales this source rect to the panel. */
#define DEFAULT_FPS 30
#define DEFAULT_WIDTH 1920
#define DEFAULT_HEIGHT 1080

/* How long to wait before offering a buffer the pipeline has just refused. */
#define BUFFER_FULL_BACKOFF_NS (5 * 1000 * 1000)
/* How long to wait for ENDOFSTREAM after the last buffer has gone in. */
#define DRAIN_TIMEOUT_NS (5 * 1000000000LL)

/* HDMI is the panel's default picture and is not something we close. On exit the
 * helper asks SAM to raise returnTo (the app that was in front) before it drops
 * the card, because a card exit otherwise falls back to that HDMI input. */

static char return_to[128];

/* stderr is the only log. Cap it so a long playback cannot fill /tmp, and
 * remove it on the way out of main. A crash still leaves the file. */
#define LOG_CAP_BYTES (1024 * 1024)
static char g_log_path[PATH_MAX];

static void log_reopen(const char *path, const char *mode) {
    FILE *log = freopen(path, mode, stderr);
    if (log == NULL) {
        return;
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    dup2(fileno(stderr), fileno(stdout));
}

static void log_open(const char *path) {
    struct stat st;
    if (path == NULL || path[0] == '\0') {
        return;
    }
    if (stat(path, &st) == 0 && st.st_size > LOG_CAP_BYTES) {
        char rotated[PATH_MAX];
        snprintf(rotated, sizeof(rotated), "%s.1", path);
        rename(path, rotated);
    }
    log_reopen(path, "w");
    snprintf(g_log_path, sizeof(g_log_path), "%s", path);
    fprintf(stderr, "[main] log %s cap %d bytes, removed on clean exit\n", path, LOG_CAP_BYTES);
}

static void log_rotate_if_big(void) {
    struct stat st;
    char rotated[PATH_MAX];
    if (g_log_path[0] == '\0') {
        return;
    }
    if (stat(g_log_path, &st) != 0 || st.st_size <= LOG_CAP_BYTES) {
        return;
    }
    fflush(stderr);
    snprintf(rotated, sizeof(rotated), "%s.1", g_log_path);
    rename(g_log_path, rotated);
    log_reopen(g_log_path, "w");
    fprintf(stderr, "[main] log rotated at %ld bytes\n", (long) st.st_size);
}

static void log_remove(void) {
    char rotated[PATH_MAX];
    if (g_log_path[0] == '\0') {
        return;
    }
    fprintf(stderr, "[main] clean exit, removing log\n");
    fflush(stderr);
    unlink(g_log_path);
    snprintf(rotated, sizeof(rotated), "%s.1", g_log_path);
    unlink(rotated);
    g_log_path[0] = '\0';
}

static const char *title_of(const playlist *list, int index) {
    if (list == NULL || index < 0 || index >= list->count || list->names[index] == NULL) {
        return "";
    }
    return list->names[index];
}

/* Real previous apps only. Inputs and this screensaver stack are not relaunched. */
static bool return_to_allowed(const char *id) {
    size_t n;
    size_t i;
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    n = strlen(id);
    if (n > 80) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char c = id[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    if (strstr(id, "hdmi") != NULL || strstr(id, "livetv") != NULL ||
        strstr(id, "screensaver") != NULL) {
        return false;
    }
    if (strcmp(id, "org.aabytt.webos.custom-screensaver-aerial.acb") == 0) {
        return false;
    }
    return true;
}

static bool on_launch_reply(LSHandle *sh, LSMessage *message, void *ctx) {
    int *done = ctx;
    const char *payload = LSMessageGetPayload(message);
    (void) sh;
    fprintf(stderr, "[main] returnTo reply: %s\n", payload != NULL ? payload : "(null)");
    *done = 1;
    return true;
}

/* The app id is already on the bus while this process is alive (LSRegister of
 * self_id returns "already exists"). The role also allows an empty name and,
 * once added beside the generated role, the ".r" name below. */
static bool register_launch_client(LSHandle **sh, const char *self_id) {
    char app_service[160];
    const char *names[4];
    int i;
    int n = 0;

    names[n++] = "org.aabytt.webos.custom-screensaver-aerial.acb.r";
    names[n++] = "";
    names[n++] = "com.webos.media.client.aerialr";
    names[n++] = self_id;

    for (i = 0; i < n; i++) {
        LSError err;
        const char *shown = names[i][0] != '\0' ? names[i] : "(empty)";
        LSErrorInit(&err);
        if (LSRegister(names[i], sh, &err)) {
            fprintf(stderr, "[main] LSRegister '%s' ok\n", shown);
            return true;
        }
        fprintf(stderr, "[main] LSRegister '%s' failed: %s\n", shown,
                err.message != NULL ? err.message : "?");
        LSErrorFree(&err);
    }

    snprintf(app_service, sizeof(app_service), "%s.r", self_id);
    {
        LSError err;
        LSErrorInit(&err);
        if (LSRegisterApplicationService(app_service, self_id, sh, &err)) {
            fprintf(stderr, "[main] LSRegisterApplicationService '%s' ok\n", app_service);
            return true;
        }
        fprintf(stderr, "[main] LSRegisterApplicationService failed: %s\n",
                err.message != NULL ? err.message : "?");
        LSErrorFree(&err);
    }
    return false;
}

/* In-process luna call. /usr/bin/luna-send is mode 700 and cannot be exec'd from the jail. */
static void relaunch_return_to(const char *self_id) {
    LSError err;
    LSHandle *sh = NULL;
    GMainLoop *loop = NULL;
    char payload[160];
    int done = 0;
    gint64 deadline;

    if (!return_to_allowed(return_to)) {
        fprintf(stderr, "[main] not relaunching returnTo '%.80s'\n", return_to);
        return;
    }

    if (!register_launch_client(&sh, self_id)) {
        return;
    }

    loop = g_main_loop_new(NULL, FALSE);
    LSErrorInit(&err);
    if (!LSGmainAttach(sh, loop, &err)) {
        fprintf(stderr, "[main] LSGmainAttach failed: %s\n",
                err.message != NULL ? err.message : "?");
        LSErrorPrint(&err, stderr);
        LSErrorFree(&err);
        LSUnregister(sh, NULL);
        g_main_loop_unref(loop);
        return;
    }

    snprintf(payload, sizeof(payload), "{\"id\":\"%s\"}", return_to);
    fprintf(stderr, "[main] relaunch %s\n", payload);
    LSErrorInit(&err);
    if (!LSCallOneReply(sh, "luna://com.webos.applicationManager/launch", payload,
                        on_launch_reply, &done, NULL, &err)) {
        fprintf(stderr, "[main] LSCall launch failed: %s\n",
                err.message != NULL ? err.message : "?");
        LSErrorPrint(&err, stderr);
        LSErrorFree(&err);
    } else {
        deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
        while (!done && g_get_monotonic_time() < deadline) {
            g_main_context_iteration(NULL, FALSE);
            usleep(10 * 1000);
        }
        if (!done) {
            fprintf(stderr, "[main] relaunch timed out\n");
        }
    }

    LSErrorInit(&err);
    if (!LSUnregister(sh, &err)) {
        LSErrorFree(&err);
    }
    g_main_loop_unref(loop);
}

typedef struct options {
    char video_path[PATH_MAX];
    char audio_path[PATH_MAX];
    char videos_json[PATH_MAX];
    char source_type[64];
    int play_lower;
    int fps;
    int width;
    int height;
} options;

static char g_ca_path[PATH_MAX];

struct prefetch {
    pthread_t thread;
    int started;
    volatile int stop;
    volatile int ready;
    pthread_mutex_t mu;
    char url[PLAYLIST_URL];
    avc_movie *movie;
    char err[192];
};

struct pump_ctx {
    sdl_shell *shell;
};

static int main_pump(void *v) {
    struct pump_ctx *p = v;
    if (p == NULL || p->shell == NULL) {
        return 1;
    }
    return sdl_shell_pump(p->shell) ? 1 : 0;
}

static int prefetch_pump(void *v) {
    struct prefetch *p = v;
    return p->stop ? 0 : 1;
}

static void *prefetch_thread(void *arg) {
    struct prefetch *p = arg;
    http_client *http = http_client_create(g_ca_path);
    avc_movie *movie = NULL;
    char err[192];

    err[0] = '\0';
    if (http != NULL && !p->stop) {
        movie = avc_movie_open(http, p->url, prefetch_pump, p, err, sizeof(err));
        if (movie != NULL) {
            avc_au au;
            if (p->stop || !avc_movie_sample(movie, 0, &au, prefetch_pump, p, err, sizeof(err))) {
                avc_movie_close(movie);
                movie = NULL;
            } else {
                /* The window is in the movie. The curl handle belongs to this thread. */
                avc_movie_detach_http(movie);
            }
        }
    }
    http_client_destroy(http);
    pthread_mutex_lock(&p->mu);
    p->movie = movie;
    snprintf(p->err, sizeof(p->err), "%s", err);
    p->ready = 1;
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

static void prefetch_cancel(struct prefetch *p) {
    avc_movie *movie = NULL;
    if (!p->started) {
        return;
    }
    p->stop = 1;
    pthread_join(p->thread, NULL);
    pthread_mutex_lock(&p->mu);
    movie = p->movie;
    p->movie = NULL;
    pthread_mutex_unlock(&p->mu);
    avc_movie_close(movie);
    pthread_mutex_destroy(&p->mu);
    p->started = 0;
    p->ready = 0;
    p->stop = 0;
}

static void prefetch_start(struct prefetch *p, const char *url) {
    if (url == NULL || url[0] == '\0') {
        return;
    }
    if (p->started) {
        prefetch_cancel(p);
    }
    memset(p, 0, sizeof(*p));
    snprintf(p->url, sizeof(p->url), "%s", url);
    pthread_mutex_init(&p->mu, NULL);
    if (pthread_create(&p->thread, NULL, prefetch_thread, p) != 0) {
        pthread_mutex_destroy(&p->mu);
        fprintf(stderr, "[play] prefetch thread failed\n");
        return;
    }
    p->started = 1;
}

/* Waits until the prefetch of `url` finishes. NULL if it was for something else,
 * failed, or never started. Caller attaches its own HTTP client afterwards. */
static avc_movie *prefetch_take(struct prefetch *p, const char *url, sdl_shell *shell) {
    avc_movie *movie;
    if (!p->started || strcmp(p->url, url) != 0) {
        return NULL;
    }
    while (!p->ready) {
        if (shell != NULL && !sdl_shell_pump(shell)) {
            p->stop = 1;
            break;
        }
        usleep(20 * 1000);
    }
    pthread_join(p->thread, NULL);
    p->started = 0;
    pthread_mutex_lock(&p->mu);
    movie = p->movie;
    p->movie = NULL;
    if (movie == NULL && p->err[0] != '\0') {
        fprintf(stderr, "[play] prefetch failed: %s\n", p->err);
    }
    pthread_mutex_unlock(&p->mu);
    pthread_mutex_destroy(&p->mu);
    p->ready = 0;
    p->stop = 0;
    return movie;
}

static const char *url_leaf(const char *url) {
    const char *slash = strrchr(url, '/');
    return slash != NULL ? slash + 1 : url;
}

static void sibling_path(const char *name, char *out, size_t out_len) {
    char dir[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
    char *slash;
    if (len <= 0) {
        snprintf(out, out_len, "%s", name);
        return;
    }
    dir[len] = '\0';
    slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
    }
    snprintf(out, out_len, "%s/%s", dir, name);
}

static int net_pull(avc_movie *movie, const playlist *list, int *sample_i, int64_t pts_base,
                    sdl_shell *shell, es_sample *out) {
    avc_au au;
    char err[192];
    struct pump_ctx ctx;

    err[0] = '\0';
    if (movie == NULL || *sample_i >= avc_movie_samples(movie)) {
        return 0;
    }
    ctx.shell = shell;
    if (!avc_movie_sample(movie, *sample_i, &au, main_pump, &ctx, err, sizeof(err))) {
        fprintf(stderr, "[play] sample %d failed: %s\n", *sample_i, err);
        return 0;
    }
    if (list->cap_sec > 0 && au.pts_ns >= (int64_t) list->cap_sec * 1000000000LL) {
        fprintf(stderr, "[play] cap %ds reached on %s\n", list->cap_sec, url_leaf(avc_movie_url(movie)));
        return 0;
    }
    out->data = au.data;
    out->size = au.size;
    out->pts_ns = pts_base + au.pts_ns;
    (*sample_i)++;
    return 1;
}

/* Closes the current movie and opens the next URL that will demux.
 * Returns 0, -1 if every URL failed, -2 if the remote asked to quit. */
static int net_advance(http_client *http, playlist *list, struct prefetch *prefetch, sdl_shell *shell,
                       avc_movie **movie, int *index, int *sample_i, int64_t *pts_base, int64_t last_pts,
                       int64_t *switch_mark) {
    int tried;
    int64_t frame;
    int64_t t0;

    t0 = pacer_now_ns();
    *switch_mark = t0;
    frame = (*movie != NULL) ? avc_movie_frame_ns(*movie) : 33333333LL;
    if (frame <= 0) {
        frame = 33333333LL;
    }
    *pts_base = last_pts + frame;
    avc_movie_close(*movie);
    *movie = NULL;
    *sample_i = 0;
    for (tried = 0; tried < list->count; tried++) {
        const char *url;
        avc_movie *got;
        char err[192];
        struct pump_ctx ctx;

        *index = (*index + 1) % list->count;
        url = list->urls[*index];
        err[0] = '\0';
        fprintf(stderr, "[play] next %s\n", url);
        got = prefetch_take(prefetch, url, shell);
        if (got != NULL) {
            avc_movie_attach_http(got, http);
        } else {
            ctx.shell = shell;
            if (!sdl_shell_pump(shell)) {
                return -2;
            }
            got = avc_movie_open(http, url, main_pump, &ctx, err, sizeof(err));
            if (got == NULL) {
                fprintf(stderr, "[play] skip %s: %s\n", url_leaf(url), err);
                if (strstr(err, "SSL") != NULL || strstr(err, "certificate") != NULL) {
                    fprintf(stderr, "[play] TLS failed with verification still on; not disabling it\n");
                    break;
                }
            }
        }
        if (!sdl_shell_pump(shell)) {
            avc_movie_close(got);
            return -2;
        }
        if (got != NULL) {
            *movie = got;
            fprintf(stderr, "[play] switch open %.0f ms, network bytes %lld, title \"%s\"\n",
                    (double) (pacer_now_ns() - t0) / 1e6, (long long) http_bytes_downloaded(),
                    title_of(list, *index));
            if (list->count > 1) {
                prefetch_start(prefetch, list->urls[(*index + 1) % list->count]);
            }
            return 0;
        }
    }
    fprintf(stderr, "[play] every url failed; holding the last frame\n");
    return -1;
}

/* The ipk stages the sample streams next to the binary, and a native webOS app is not
 * launched with its own directory as the working directory. */
static void resolve_default_paths(options *opts) {
    /* Leave headroom for "/sample.h264" so the join below cannot be truncated. */
    char dir[PATH_MAX - 16];
    ssize_t len = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
    if (len <= 0) {
        snprintf(opts->video_path, sizeof(opts->video_path), "aerial.h264");
        snprintf(opts->audio_path, sizeof(opts->audio_path), "aerial.aac");
        return;
    }
    dir[len] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
    }
    snprintf(opts->video_path, sizeof(opts->video_path), "%s/aerial.h264", dir);
    snprintf(opts->audio_path, sizeof(opts->audio_path), "%s/aerial.aac", dir);
}

static bool parse_options(int argc, char *argv[], options *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->fps = DEFAULT_FPS;
    opts->width = DEFAULT_WIDTH;
    opts->height = DEFAULT_HEIGHT;
    resolve_default_paths(opts);

    for (int i = 1; i < argc; i++) {
        /* The app manager hands native apps a JSON launch-parameters object as argv[1].
         * It is handled separately, in main(); rejecting it here would stop the app from
         * starting at all, with no console to say why. */
        if (argv[i][0] == '{') {
            continue;
        }
        if (strcmp(argv[i], "--video") == 0 && i + 1 < argc) {
            snprintf(opts->video_path, sizeof(opts->video_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--audio") == 0 && i + 1 < argc) {
            snprintf(opts->audio_path, sizeof(opts->audio_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            opts->fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &opts->width, &opts->height) != 2) {
                return false;
            }
        } else {
            fprintf(stderr,
                    "usage: %s [--video sample.h264] [--audio sample.aac]\n"
                    "          [--fps 30] [--size 1280x720]\n",
                    argv[0]);
            return false;
        }
    }
    return opts->fps > 0 && opts->width > 0 && opts->height > 0;
}

int main(int argc, char *argv[]) {
    /* Do this before anything else that might want to complain: launched from the TV's
     * home screen there is no console, so without a log file nothing below is visible. */
    const char *launch_params = app_launch_params(argc, argv);
    char log_path[PATH_MAX];
    char opts_locale[32];
    int opts_opacity = -1;
    opts_locale[0] = '\0';
    if (app_launch_param_string(launch_params, "log", log_path, sizeof(log_path))) {
        log_open(log_path);
    }

    options opts;
    if (!parse_options(argc, argv, &opts)) {
        log_remove();
        return 2;
    }
    return_to[0] = '\0';
    if (launch_params != NULL) {
        fprintf(stderr, "[main] launch params: %s\n", launch_params);
        app_launch_param_string(launch_params, "video", opts.video_path, sizeof(opts.video_path));
        app_launch_param_string(launch_params, "audio", opts.audio_path, sizeof(opts.audio_path));
        app_launch_param_string(launch_params, "returnTo", return_to, sizeof(return_to));
        app_launch_param_string(launch_params, "videos", opts.videos_json, sizeof(opts.videos_json));
        app_launch_param_string(launch_params, "sourceType", opts.source_type, sizeof(opts.source_type));
        {
            char lower[16];
            if (app_launch_param_string(launch_params, "playLowerQuality", lower, sizeof(lower))) {
                opts.play_lower = (strcmp(lower, "true") == 0 || strcmp(lower, "1") == 0);
            }
        }
        app_launch_param_string(launch_params, "locale", opts_locale, sizeof(opts_locale));
        {
            char opacity[16];
            if (app_launch_param_string(launch_params, "osdOpacity", opacity, sizeof(opacity)) &&
                opacity[0] != '\0') {
                opts_opacity = atoi(opacity);
            }
        }
    }

    /* Set by the webOS launcher for native apps. The pipeline uses it to account for the
     * decoder resources this app holds, and will not play without a plausible value. */
    const char *app_id = getenv("APPID");
    if (app_id == NULL) {
        app_id = "org.aabytt.webos.custom-screensaver-aerial.acb";
        fprintf(stderr, "[main] APPID unset, assuming %s\n", app_id);
    }

    es_file *video = NULL;
    es_file *audio = es_file_open_adts(opts.audio_path);
    http_client *http = NULL;
    playlist *play = NULL;
    avc_movie *movie = NULL;
    struct prefetch next_movie;
    char videos_fallback[PATH_MAX];
    int use_network = 0;
    int movie_index = 0;
    int width = opts.width;
    int height = opts.height;
    int fps_num = opts.fps;
    int fps_den = 1;
    int exit_code = 0;
    int acb_ready = 0;
    int sample_i = 0;
    smp_player *player = NULL;
    acb_plane acb;
    es_sample video_sample;
    es_sample audio_sample;
    bool have_video = false;
    bool have_audio = false;
    int64_t clock_origin = 0;
    int64_t pts_base = 0;
    int64_t last_pts = 0;
    int64_t audio_base = 0;
    int64_t audio_last = 0;
    int64_t audio_delta = 21333333LL;
    int64_t switch_mark = 0;
    int movie_frames = 0;
    long fed_video = 0;
    long fed_audio = 0;
    memset(&acb, 0, sizeof(acb));
    memset(&video_sample, 0, sizeof(video_sample));
    memset(&audio_sample, 0, sizeof(audio_sample));
    memset(&next_movie, 0, sizeof(next_movie));
    if (audio == NULL) {
        log_remove();
        return 1;
    }

    pacer_uptime_ms(); /* start the clock */
    sdl_shell shell;
    if (!sdl_shell_preinit() || !sdl_shell_open_window(&shell, "SMP ACB")) {
        es_file_close(audio);
        log_remove();
        return 1;
    }
    /* Clear to transparent once, up front: from here on the screen belongs to the video
     * plane, and the sample has nothing of its own to draw. */
    sdl_shell_present_transparent(&shell);
    fprintf(stderr, "[main @%.0fms] SDL window up\n", pacer_uptime_ms());
    osd_label *label = NULL;

    sibling_path("cacert.pem", g_ca_path, sizeof(g_ca_path));
    sibling_path("videos.json", videos_fallback, sizeof(videos_fallback));
    http = http_client_create(g_ca_path);
    play = calloc(1, sizeof(*play));
    if (http != NULL && play != NULL) {
        const char *videos_path = opts.videos_json[0] != '\0'
                                          ? opts.videos_json
                                          : "/media/developer/apps/usr/palm/applications/"
                                            "org.aabytt.webos.custom-screensaver-aerial/assets/videos.json";
        int i;
        if (playlist_load(play, videos_path, videos_fallback, opts.source_type, opts.play_lower,
                          opts_locale, opts_opacity) == 0) {
            for (i = 0; i < play->count; i++) {
                char err[192];
                struct pump_ctx ctx;
                int64_t t0 = pacer_now_ns();
                ctx.shell = &shell;
                err[0] = '\0';
                fprintf(stderr, "[play] open %s\n", play->urls[i]);
                movie = avc_movie_open(http, play->urls[i], main_pump, &ctx, err, sizeof(err));
                if (movie != NULL) {
                    movie_index = i;
                    use_network = 1;
                    fprintf(stderr, "[play] playing %s title \"%s\" open %.0f ms, network bytes %lld\n",
                            url_leaf(play->urls[i]), title_of(play, i),
                            (double) (pacer_now_ns() - t0) / 1e6, (long long) http_bytes_downloaded());
                    break;
                }
                fprintf(stderr, "[play] skip %s: %s\n", url_leaf(play->urls[i]), err);
                if (strstr(err, "SSL") != NULL || strstr(err, "certificate") != NULL) {
                    fprintf(stderr, "[play] TLS failed with verification still on; not disabling it\n");
                    break;
                }
                if (!sdl_shell_pump(&shell)) {
                    break;
                }
            }
        }
    } else {
        fprintf(stderr, "[play] HTTP client unavailable, verification stays on\n");
    }
    if (use_network) {
        int w = avc_movie_width(movie);
        int h = avc_movie_height(movie);
        if (w >= 320 && w <= 3840 && h >= 240 && h <= 2160) {
            width = w;
            height = h;
        }
        if (avc_movie_fps_num(movie) > 0 && avc_movie_fps_den(movie) > 0) {
            fps_num = avc_movie_fps_num(movie);
            fps_den = avc_movie_fps_den(movie);
        }
        if (play->count > 1) {
            prefetch_start(&next_movie, play->urls[(movie_index + 1) % play->count]);
        }
    } else {
        fprintf(stderr, "[play] offline fallback %s\n", opts.video_path);
        video = es_file_open_h264(opts.video_path, opts.fps, 1);
        if (video == NULL) {
            fprintf(stderr, "[main] no network movie and no local clip\n");
            exit_code = 1;
            goto teardown;
        }
    }

    if (!acb_plane_init(&acb, &shell, app_id, width, height)) {
        exit_code = 1;
        goto teardown;
    }
    acb_ready = 1;

    smp_video_plane plane;
    acb_plane_bind(&acb, &plane);

    player = smp_player_create(&plane);
    if (player == NULL) {
        exit_code = 1;
        goto teardown;
    }

    {
    smp_load_params params = {
            .app_id = app_id,
            .pause_at_decode_time = SMP_PAUSE_AT_DECODE_TIME,
            .low_latency = true,
            .has_video = true,
            .video_codec = "H264",
            .video_width = width,
            .video_height = height,
            .video_fps_num = fps_num,
            .video_fps_den = fps_den,
            .has_audio = true,
            .audio_codec = "AAC",
            .audio_channels = es_file_channels(audio),
            .audio_sample_rate = es_file_sample_rate(audio),
            .aac_object_type = es_file_aac_object_type(audio),
    };

    /* GStreamer-CRITICAL "still has a parent registry0" (dvbin, lxvideosink,
     * aac_audiodec, audiosink) is printed by LG's plugin registry while Load
     * and Unload dispose features that are still parented. This process does
     * not call GStreamer. The lines are noise; the picture still plays. */
    fprintf(stderr, "[main @%.0fms] calling Load %dx%d fps %d/%d network=%d\n", pacer_uptime_ms(),
            width, height, fps_num, fps_den, use_network);
    if (!smp_player_load(player, &params)) {
        exit_code = 1;
        goto teardown;
    }
    }

    /*
     * Feed loop. Network movies share one ACB pipeline: the next file is another
     * sequence of Annex-B access units on the same timeline, with SPS/PPS repeated
     * on each keyframe. The local clip is the offline fallback and still loops
     * the way it did when it was the only source.
     *
     * clock_origin starts when the pipeline accepts the first buffer. Starting it
     * during Load would make the pacer skip ahead.
     */
    if (use_network && play != NULL) {
        char font_path[PATH_MAX];
        char dir[PATH_MAX];
        const char *videos_for_font = opts.videos_json[0] != '\0' ? opts.videos_json : videos_fallback;
        char *slash;
        snprintf(dir, sizeof(dir), "%s", videos_for_font);
        slash = strrchr(dir, '/');
        if (slash != NULL) {
            *slash = '\0';
        }
        snprintf(font_path, sizeof(font_path), "%s/SegoeUI-Light.ttf", dir);
        if (access(font_path, R_OK) != 0) {
            sibling_path("SegoeUI-Light.ttf", font_path, sizeof(font_path));
        }
        label = osd_label_create(shell.window, shell.renderer, font_path);
    }

    have_audio = es_file_next(audio, &audio_sample);

    if (use_network) {
        have_video = net_pull(movie, play, &sample_i, pts_base, &shell, &video_sample);
    } else {
        have_video = es_file_next(video, &video_sample);
    }

    while (exit_code == 0 && sdl_shell_pump(&shell)) {
        bool feed_video;
        const es_sample *sample;
        int64_t pts;
        smp_feed_result result;

        if (smp_player_errored(player)) {
            fprintf(stderr, "[main] pipeline error, stopping\n");
            exit_code = 1;
            break;
        }

        if (use_network) {
            if (!have_audio) {
                es_file_rewind(audio);
                audio_base = audio_last + audio_delta;
                have_audio = es_file_next(audio, &audio_sample);
                if (!have_audio) {
                    break;
                }
                continue;
            }
            if (!have_video) {
                int adv = net_advance(http, play, &next_movie, &shell, &movie, &movie_index,
                                      &sample_i, &pts_base, last_pts, &switch_mark);
                if (adv == -2) {
                    break;
                }
                if (adv != 0) {
                    int64_t until = pacer_now_ns() + 2000000000LL;
                    while (pacer_now_ns() < until && sdl_shell_pump(&shell)) {
                        usleep(20 * 1000);
                    }
                    continue;
                }
                movie_frames = 0;
                have_video = net_pull(movie, play, &sample_i, pts_base, &shell, &video_sample);
                continue;
            }
            {
                int64_t apts = audio_base + audio_sample.pts_ns;
                feed_video = video_sample.pts_ns <= apts;
                sample = feed_video ? &video_sample : &audio_sample;
                pts = feed_video ? video_sample.pts_ns : apts;
            }
        } else {
            if (!have_video && !have_audio) {
                pts_base = last_pts + (1000000000LL / opts.fps);
                es_file_rewind(video);
                es_file_rewind(audio);
                have_video = es_file_next(video, &video_sample);
                have_audio = es_file_next(audio, &audio_sample);
                fprintf(stderr, "[main] loop, next pts base %.1f s\n", (double) pts_base / 1e9);
                if (!have_video && !have_audio) {
                    break;
                }
                continue;
            }
            feed_video = have_video && (!have_audio || video_sample.pts_ns <= audio_sample.pts_ns);
            sample = feed_video ? &video_sample : &audio_sample;
            pts = pts_base + sample->pts_ns;
        }

        if (clock_origin != 0) {
            pacer_sleep_until(clock_origin, pts);
        }

        result = smp_player_feed(player, sample->data, sample->size, pts,
                                 feed_video ? SMP_ES_VIDEO : SMP_ES_AUDIO);
        if (result == SMP_FEED_BUFFER_FULL || result == SMP_FEED_PENDING) {
            pacer_sleep_ns(BUFFER_FULL_BACKOFF_NS);
            continue;
        }
        if (result != SMP_FEED_OK) {
            fprintf(stderr, "[main] feed failed, stopping\n");
            exit_code = 1;
            break;
        }

        if (clock_origin == 0) {
            clock_origin = pacer_now_ns() - pts;
        }
        if (pts > last_pts) {
            last_pts = pts;
        }
        if (feed_video) {
            fed_video++;
            if (use_network && movie_frames == 0) {
                double title_s = avc_movie_duration_s(movie);
                if (play->cap_sec > 0 && (title_s <= 0 || play->cap_sec < title_s)) {
                    title_s = play->cap_sec;
                }
                osd_label_set(label, title_of(play, movie_index), title_s, play->osd_opacity);
                if (switch_mark != 0) {
                    fprintf(stderr, "[play] first picture %s title \"%s\" gap %.0f ms\n",
                            url_leaf(avc_movie_url(movie)), title_of(play, movie_index),
                            (double) (pacer_now_ns() - switch_mark) / 1e6);
                    switch_mark = 0;
                } else {
                    fprintf(stderr, "[play] first picture %s title \"%s\" at %.0f ms, bytes %lld\n",
                            url_leaf(avc_movie_url(movie)), title_of(play, movie_index),
                            pacer_uptime_ms(), (long long) http_bytes_downloaded());
                }
            }
            if (use_network) {
                osd_label_tick(label, (double) (pts - pts_base) / 1e9);
            }
            if (use_network) {
                movie_frames++;
            }
            if (fed_video % 300 == 0) {
                log_rotate_if_big();
                if (use_network) {
                    fprintf(stderr,
                            "[main] fed %ld video / %ld audio, media %.1f s, %s, bytes %lld\n",
                            fed_video, fed_audio, (double) pts / 1e9, url_leaf(avc_movie_url(movie)),
                            (long long) http_bytes_downloaded());
                } else {
                    fprintf(stderr, "[main] fed %ld video / %ld audio, media time %.1f s\n",
                            fed_video, fed_audio, (double) pts / 1e9);
                }
            }
            if (use_network) {
                have_video = net_pull(movie, play, &sample_i, pts_base, &shell, &video_sample);
            } else {
                have_video = es_file_next(video, &video_sample);
            }
        } else {
            if (use_network && pts > audio_last) {
                audio_delta = pts - audio_last;
                audio_last = pts;
            }
            fed_audio++;
            have_audio = es_file_next(audio, &audio_sample);
        }
    }

    fprintf(stderr, "[main] feed loop done: %ld video, %ld audio units, bytes %lld\n", fed_video,
            fed_audio, (long long) http_bytes_downloaded());

teardown:
    /* Raise the previous app while this card still exists, then drop the sink. */
    relaunch_return_to(app_id);
    if (player != NULL) {
        smp_player_unload(player);
        smp_player_destroy(player);
    }
    if (acb_ready) {
        acb_plane_destroy(&acb);
    }
    sdl_shell_close(&shell);
    prefetch_cancel(&next_movie);
    avc_movie_close(movie);
    http_client_destroy(http);
    if (play != NULL) {
        playlist_free(play);
        free(play);
    }
    es_file_close(video);
    es_file_close(audio);
    osd_label_destroy(label);
    log_remove();
    return exit_code;
}
