#include "acb_plane.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <AcbAPI.h>

#ifndef ACB_ATTACH_SINK_EARLY
#define ACB_ATTACH_SINK_EARLY 0
#endif

/*
 * The SDK header declares
 *
 *     int AcbAPI_setMediaVideoData(long acbId, const char *payload);
 *
 * with two parameters. The shipped library takes three. That is not a guess: the library
 * also exports the C++ method the wrapper forwards to, and its mangled name spells the
 * signature out -
 *
 *     _ZN3ACB7AcbCore17setMediaVideoDataESsPl
 *       -> ACB::AcbCore::setMediaVideoData(std::string, long *)
 *
 * - so the C wrapper is (acbId, payload, taskId), matching every other ACB entry point.
 * Verified on a 43UH6100 running webOS 3.4.0, and consistent with disassembly of the
 * webOS 2.1 and 4.5 builds of the same library.
 *
 * Declaring it here under an explicit assembler name binds to the real symbol with the
 * real prototype, without having to cast a wrongly-typed function pointer at the call
 * site and without touching the SDK header.
 */
extern int acb_set_media_video_data(long acb_id, const char *payload, long *task_id)
        __asm__("AcbAPI_setMediaVideoData");

/*
 * ACB registrations outlive the process that made them.
 *
 * If an app dies without calling AcbAPI_finalize - killed, crashed, or shut down by a
 * service restart - the ACB service keeps the registration, and the *next* run is refused
 * with "ACB object was already registered or sinkType(0) or purpose(1) is not valid" in
 * acb-client.error. The symptom is a black screen or a frozen first frame in a run whose
 * own logs look perfect, which is a thoroughly confusing thing to debug.
 *
 * Recovering by hand is `restart AcbService` on the TV; a reboot also works but is not
 * needed. Better to not leak in the first place, hence this handler.
 */
static long g_acb_id_for_cleanup = 0;

static void acb_cleanup_on_signal(int sig) {
    if (g_acb_id_for_cleanup != 0) {
        AcbAPI_finalize(g_acb_id_for_cleanup);
        AcbAPI_destroy(g_acb_id_for_cleanup);
        g_acb_id_for_cleanup = 0;
    }
    _exit(128 + sig);
}

static void acb_callback(long acb_id, long task_id, long event_type, long app_state,
                         long play_state, const char *reply) {
    fprintf(stderr, "[acb] acbId=%ld taskId=%ld event=%ld appState=%ld playState=%ld %s\n",
            acb_id, task_id, event_type, app_state, play_state, reply ? reply : "");
}

static void set_media_id(void *self, const char *media_id) {
    acb_plane *plane = self;
    if (media_id == NULL) {
        return;
    }
    /* Attaches this ACB instance to that pipeline. It has to happen before Load(), which
     * is why the seam has a hook here rather than reusing load_completed. */
    if (!AcbAPI_setMediaId(plane->acb_id, media_id)) {
        fprintf(stderr, "[acb] setMediaId(%s) failed\n", media_id);
        return;
    }
    fprintf(stderr, "[acb] bound to mediaId %s\n", media_id);
}

/*
 * Attach the video sink and declare the pipeline loaded.
 *
 * When this may be called is generation-dependent, and the two ends disagree:
 *
 *   webOS 3 and 4  Only after LOADCOMPLETED. Calling it as soon as Load() returns makes
 *                  ACB's setState hang and then fail with "Timeout To Receive Luna API
 *                  Response" on luna://com.webos.service.videosinkmanager/connect - the
 *                  sink never attaches and the screen stays black.
 *   webOS 2        LOADCOMPLETED does not arrive until feeding stops, so waiting for it
 *                  means the sink attaches after the clip has already played.
 *
 * So webOS 2 gets ACB_ATTACH_SINK_EARLY and everyone else follows the order ss4s uses.
 * Note that setDisplayWindow is *not* part of this: ss4s issues that right after Load()
 * returns on every generation, and it works.
 */
static void attach_sink(acb_plane *plane) {
    AcbAPI_setSinkType(plane->acb_id, SINK_TYPE_MAIN);
    AcbAPI_setState(plane->acb_id, APPSTATE_FOREGROUND, PLAYSTATE_LOADED, &plane->task_id);

    pthread_mutex_lock(&plane->lock);
    plane->state_loaded = true;
    bool play_now = plane->play_pending;
    plane->play_pending = false;
    pthread_mutex_unlock(&plane->lock);

    if (play_now) {
        AcbAPI_setState(plane->acb_id, APPSTATE_FOREGROUND, PLAYSTATE_PLAYING,
                        &plane->task_id);
    }
}

static void post_load(void *self, const smp_load_params *params) {
    acb_plane *plane = self;
    if (params->has_video) {
        plane->video_width = params->video_width;
        plane->video_height = params->video_height;
    }

    AcbAPI_setDisplayWindow(plane->acb_id, 0, 0, plane->video_width, plane->video_height,
                            true, &plane->task_id);
    fprintf(stderr, "[acb] display window %dx%d fullscreen\n", plane->video_width,
            plane->video_height);

#if ACB_ATTACH_SINK_EARLY
    attach_sink(plane);
#endif
}

static void load_completed(void *self, const char *media_id) {
    (void) media_id;
#if ACB_ATTACH_SINK_EARLY
    (void) self;
    /* Already done in post_load - see attach_sink. */
#else
    attach_sink((acb_plane *) self);
#endif
}

static void start_playing(void *self) {
    acb_plane *plane = self;

    pthread_mutex_lock(&plane->lock);
    bool loaded = plane->state_loaded;
    if (!loaded) {
        /* Too early - ACB would answer "Invalid State Request". load_completed will do it. */
        plane->play_pending = true;
    }
    pthread_mutex_unlock(&plane->lock);

    if (loaded) {
        AcbAPI_setState(plane->acb_id, APPSTATE_FOREGROUND, PLAYSTATE_PLAYING,
                        &plane->task_id);
    }
}

static void video_info(void *self, const char *info_json) {
    acb_plane *plane = self;
    if (info_json == NULL) {
        return;
    }
    /* Forwarded verbatim. This is how the video sink learns the decoded picture's real
     * geometry - on this generation the pipeline does not tell it directly. */
    acb_set_media_video_data(plane->acb_id, info_json, &plane->task_id);
}

static void post_unload(void *self) {
    acb_plane *plane = self;
    AcbAPI_setState(plane->acb_id, APPSTATE_FOREGROUND, PLAYSTATE_UNLOADED, &plane->task_id);
}

bool acb_plane_init(acb_plane *plane, sdl_shell *shell, const char *app_id, int video_width,
                    int video_height) {
    memset(plane, 0, sizeof(*plane));
    pthread_mutex_init(&plane->lock, NULL);
    plane->shell = shell;
    plane->video_width = video_width;
    plane->video_height = video_height;

    plane->acb_id = AcbAPI_create();
    if (plane->acb_id == 0) {
        fprintf(stderr, "[acb] AcbAPI_create failed\n");
        return false;
    }
    /* MSE is the player type a buffer-stream app registers as. */
    if (!AcbAPI_initialize(plane->acb_id, PLAYER_TYPE_MSE, app_id, acb_callback)) {
        fprintf(stderr, "[acb] AcbAPI_initialize failed\n");
        AcbAPI_destroy(plane->acb_id);
        plane->acb_id = 0;
        return false;
    }
    g_acb_id_for_cleanup = plane->acb_id;
    signal(SIGTERM, acb_cleanup_on_signal);
    signal(SIGINT, acb_cleanup_on_signal);
    return true;
}

void acb_plane_bind(acb_plane *plane, smp_video_plane *out) {
    memset(out, 0, sizeof(*out));
    out->self = plane;
    out->set_media_id = set_media_id;
    out->post_load = post_load;
    out->load_completed = load_completed;
    out->start_playing = start_playing;
    out->video_info = video_info;
    out->post_unload = post_unload;
    /* prepare_load stays empty: unlike webOS 5, nothing about this plane goes into the
     * Load payload. */
}

void acb_plane_destroy(acb_plane *plane) {
    if (plane->acb_id == 0) {
        return;
    }
    AcbAPI_finalize(plane->acb_id);
    AcbAPI_destroy(plane->acb_id);
    g_acb_id_for_cleanup = 0;
    plane->acb_id = 0;
    pthread_mutex_destroy(&plane->lock);
}
