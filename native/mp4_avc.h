/*
 * Progressive MP4/MOV reader for avc1: HTTP range reads, Annex-B access units.
 *
 * moov-at-end files are handled by walking top-level atom sizes and fetching
 * only the moov. Sample media is read in contiguous windows, not as a full
 * download. The TV's /tmp is large enough for one of these files and its RAM
 * is not, so nothing is written to disk.
 */
#pragma once

#include "http_curl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct avc_movie avc_movie;

typedef struct avc_au {
    const uint8_t *data;
    size_t size;
    /* Composition time from the start of this file, in nanoseconds. */
    int64_t pts_ns;
    int keyframe;
} avc_au;

/* `http` is not owned. It must stay alive for later sample reads, except a
 * window already filled stays valid across avc_movie_detach_http(). */
avc_movie *avc_movie_open(http_client *http, const char *url, http_pump pump, void *pump_ctx,
                          char *err, size_t err_len);
void avc_movie_close(avc_movie *movie);

/* Drop the HTTP handle. Cached sample bytes remain. */
void avc_movie_detach_http(avc_movie *movie);
void avc_movie_attach_http(avc_movie *movie, http_client *http);

int avc_movie_samples(const avc_movie *movie);
int avc_movie_width(const avc_movie *movie);
int avc_movie_height(const avc_movie *movie);
int avc_movie_fps_num(const avc_movie *movie);
int avc_movie_fps_den(const avc_movie *movie);
int64_t avc_movie_frame_ns(const avc_movie *movie);
double avc_movie_duration_s(const avc_movie *movie);
const char *avc_movie_url(const avc_movie *movie);

/* Fills `out`. The bytes stay valid until the next sample call or close. */
bool avc_movie_sample(avc_movie *movie, int index, avc_au *out, http_pump pump, void *pump_ctx,
                      char *err, size_t err_len);
