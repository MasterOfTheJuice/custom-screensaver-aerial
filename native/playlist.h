/*
 * The webOS 4 playlist: the same videos.json the QML loads, reduced to the
 * URLs this H.264 pipeline can play.
 */
#pragma once

#define PLAYLIST_MAX 512
#define PLAYLIST_URL 1024

typedef struct playlist {
    char *urls[PLAYLIST_MAX];
    int count;
    /* 0 means play each asset to the end. A positive value is the debug cap. */
    int cap_sec;
    int bad_first;
    char source_type[64];
    char loaded_from[512];
} playlist;

void playlist_free(playlist *list);

/*
 * Reads videos.json at `videos_path`, then `fallback_path`.
 * Preference is url-1080-H264, then url-1080-SDR. `source_type` and
 * `play_lower` are recorded; H.264 still wins because the ACB pipeline
 * below is an H.264 decoder (the 1080 SDR/HDR entries in this catalog are HEVC).
 *
 * If `/tmp/aerial-acb-override.json` exists it may set capSec and badFirst.
 * That file is removed after it is read so a later launch is not capped.
 * Both are off when the file is absent.
 */
int playlist_load(playlist *list, const char *videos_path, const char *fallback_path,
                  const char *source_type, int play_lower);
