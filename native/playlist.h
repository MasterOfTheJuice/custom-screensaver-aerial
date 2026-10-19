/*
 * The webOS 4 playlist: the same videos.json the QML loads, reduced to the
 * URLs this H.264 pipeline can play, plus the place name the QML would draw.
 */
#pragma once

#define PLAYLIST_MAX 512
#define PLAYLIST_URL 1024

typedef struct playlist {
    char *urls[PLAYLIST_MAX];
    /* Display name for urls[i]. Locale string when videos.json's
     * localizedNameKey is in the locale file the QML loads. Otherwise a
     * readable form of that key (community ids are not in the Apple tables). */
    char *names[PLAYLIST_MAX];
    int count;
    /* 0 means play each asset to the end. A positive value is the debug cap. */
    int cap_sec;
    int bad_first;
    /* 0 hides the place name. This is the QML opacity control (settings.json
     * osdOpacity). The QML has no separate show-name flag. */
    int osd_opacity;
    char source_type[64];
    char locale[32];
    char loaded_from[512];
} playlist;

void playlist_free(playlist *list);

/*
 * Reads videos.json at `videos_path`, then `fallback_path`.
 * Preference is url-1080-H264, then url-1080-SDR. `source_type` and
 * `play_lower` are recorded; H.264 still wins because the ACB pipeline
 * below is an H.264 decoder (the 1080 SDR/HDR entries in this catalog are HEVC).
 *
 * `locale` and `osd_opacity` come from the launch parameters when the QML
 * passed them. An empty locale or osd_opacity < 0 falls back to settings.json
 * next to videos.json (localeLang, osdOpacity). Missing opacity becomes 60.
 *
 * Debug override (tests only):
 *   mkdir -m 0777 /tmp/aerial-acb-debug
 *   echo '{"capSec":45}' > /tmp/aerial-acb-debug/override.json
 * The directory must be mode 0777 and not sticky, so the jailed helper can
 * unlink a file root created. capSec and badFirst are applied only after that
 * unlink succeeds. A file the helper cannot remove is ignored, so a leftover
 * cannot change the next launch.
 *
 * /tmp/aerial-acb-override.json is never read. That path is on sticky /tmp,
 * the jail cannot unlink a root-owned file there, and a leftover used to
 * cap every later run.
 */
int playlist_load(playlist *list, const char *videos_path, const char *fallback_path,
                  const char *source_type, int play_lower, const char *locale, int osd_opacity);
