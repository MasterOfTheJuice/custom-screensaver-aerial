/*
 * The webOS 2 - 4.x video plane: libAcbAPI, the "appswitching control block".
 *
 * Before the SDL exported window existed, an app got a video plane by talking to ACB. The
 * shape is different from webOS 5 in an important way: nothing about the plane travels in
 * the Load payload. Instead ACB is told the pipeline's media id out of band, and then
 * follows the pipeline's state by hand - loaded, playing, unloaded - so the compositor
 * knows when to show the plane.
 *
 * That is why the smp_video_plane seam has so many hooks: they exist for this backend.
 * The webOS 5 sample leaves most of them empty.
 */
#pragma once

#include <pthread.h>
#include <stdbool.h>

#include "sdl_shell.h"
#include "smp_player.h"

typedef struct acb_plane {
    sdl_shell *shell;
    long acb_id;
    /* ACB calls are asynchronous and hand back a task id; the samples only ever log it. */
    long task_id;
    int video_width;
    int video_height;

    /* ACB insists on LOADED before PLAYING, but the two triggers arrive out of order and
     * on different threads: the first buffer is accepted (feed thread) before the pipeline
     * reports LOADCOMPLETED (pipeline thread). So PLAYING is held back until LOADED has
     * gone out. */
    pthread_mutex_t lock;
    bool state_loaded;
    bool play_pending;
} acb_plane;

bool acb_plane_init(acb_plane *plane, sdl_shell *shell, const char *app_id, int video_width,
                    int video_height);

void acb_plane_bind(acb_plane *plane, smp_video_plane *out);

void acb_plane_destroy(acb_plane *plane);
