#include "playlist.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *OVERRIDE_PATH = "/tmp/aerial-acb-override.json";
static const char *BAD_URL =
        "https://sylvan.apple.com/itunes-assets/Aerials116/v4/00/00/00/not-a-real-aerial.mov";

void playlist_free(playlist *list) {
    int i;
    if (list == NULL) {
        return;
    }
    for (i = 0; i < list->count; i++) {
        free(list->urls[i]);
        list->urls[i] = NULL;
    }
    list->count = 0;
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f;
    long n;
    char *buf;
    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || n > 2 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t) n + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *out_len = (size_t) n;
    return buf;
}

static const char *find_key(const char *start, const char *end, const char *key) {
    size_t klen = strlen(key);
    const char *p = start;
    while (p + klen <= end) {
        if (memcmp(p, key, klen) == 0) {
            return p;
        }
        p++;
    }
    return NULL;
}

static int extract_url(const char *start, const char *end, const char *key, char *out, size_t out_len) {
    char needle[64];
    const char *p;
    int n;
    n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t) n >= sizeof(needle)) {
        return 0;
    }
    p = find_key(start, end, needle);
    if (p == NULL) {
        return 0;
    }
    p += n;
    while (p < end && isspace((unsigned char) *p)) {
        p++;
    }
    if (p >= end || *p != ':') {
        return 0;
    }
    p++;
    while (p < end && isspace((unsigned char) *p)) {
        p++;
    }
    if (p >= end || *p != '"') {
        return 0;
    }
    p++;
    n = 0;
    while (p < end && *p != '"' && (size_t) n + 1 < out_len) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    if (p >= end || *p != '"' || n < 12) {
        return 0;
    }
    if (strncmp(out, "https://", 8) != 0) {
        return 0;
    }
    return 1;
}

static int add_url(playlist *list, const char *url) {
    if (list->count >= PLAYLIST_MAX) {
        return -1;
    }
    list->urls[list->count] = strdup(url);
    if (list->urls[list->count] == NULL) {
        return -1;
    }
    list->count++;
    return 0;
}

static void shuffle(playlist *list, uint32_t seed) {
    int i;
    fprintf(stderr, "[play] shuffle seed %u\n", seed);
    for (i = list->count - 1; i > 0; i--) {
        uint32_t j;
        char *tmp;
        seed = seed * 1664525u + 1013904223u;
        j = seed % (uint32_t) (i + 1);
        tmp = list->urls[i];
        list->urls[i] = list->urls[j];
        list->urls[j] = tmp;
    }
}

static void apply_override(playlist *list) {
    char buf[512];
    FILE *f;
    size_t n;
    const char *p;
    f = fopen(OVERRIDE_PATH, "rb");
    if (f == NULL) {
        return;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    p = strstr(buf, "capSec");
    if (p != NULL) {
        p += 6;
        while (*p && !isdigit((unsigned char) *p)) {
            p++;
        }
        if (*p) {
            list->cap_sec = atoi(p);
        }
    }
    p = strstr(buf, "badFirst");
    if (p != NULL) {
        if (strstr(p, "true") != NULL || strstr(p, "1") != NULL) {
            list->bad_first = 1;
        }
    }
    if (unlink(OVERRIDE_PATH) != 0) {
        fprintf(stderr, "[play] override unlink failed\n");
    }
    fprintf(stderr, "[play] debug override capSec=%d badFirst=%d (file removed)\n", list->cap_sec,
            list->bad_first);
}

static int load_from(playlist *list, const char *path) {
    size_t len = 0;
    char *json;
    const char *p;
    const char *end;
    json = read_file(path, &len);
    if (json == NULL) {
        return -1;
    }
    p = json;
    end = json + len;
    while (p < end) {
        const char *id = find_key(p, end, "\"id\"");
        const char *next;
        const char *obj_end;
        char h264[PLAYLIST_URL];
        char sdr[PLAYLIST_URL];
        int has_h;
        int has_s;
        const char *pick;
        if (id == NULL) {
            break;
        }
        next = find_key(id + 4, end, "\"id\"");
        obj_end = next != NULL ? next : end;
        has_h = extract_url(id, obj_end, "url-1080-H264", h264, sizeof(h264));
        has_s = extract_url(id, obj_end, "url-1080-SDR", sdr, sizeof(sdr));
        /* H.264 first. url-1080-SDR in this catalog is HEVC; it is only a
         * fallback for an asset that has no AVC url, and the demuxer skips it
         * if the sample entry is not avc1. */
        pick = NULL;
        if (has_h) {
            pick = h264;
        } else if (has_s) {
            pick = sdr;
        }
        if (pick != NULL && add_url(list, pick) != 0) {
            break;
        }
        p = obj_end;
    }
    free(json);
    if (list->count > 0) {
        snprintf(list->loaded_from, sizeof(list->loaded_from), "%s", path);
    }
    return list->count > 0 ? 0 : -1;
}

int playlist_load(playlist *list, const char *videos_path, const char *fallback_path,
                  const char *source_type, int play_lower) {
    uint32_t seed;
    memset(list, 0, sizeof(*list));
    if (source_type != NULL) {
        snprintf(list->source_type, sizeof(list->source_type), "%s", source_type);
    }
    if (videos_path != NULL && videos_path[0] != '\0') {
        load_from(list, videos_path);
    }
    if (list->count == 0 && fallback_path != NULL) {
        load_from(list, fallback_path);
    }
    fprintf(stderr, "[play] sourceType=%s playLower=%d urls=%d from %s\n",
            list->source_type[0] ? list->source_type : "(none)", play_lower, list->count,
            list->loaded_from[0] ? list->loaded_from : "(none)");
    fprintf(stderr, "[play] webOS 4 selects url-1080-H264 then url-1080-SDR\n");
    if (list->count == 0) {
        return -1;
    }
    seed = (uint32_t) time(NULL);
    shuffle(list, seed);
    apply_override(list);
    if (list->bad_first && list->count < PLAYLIST_MAX) {
        int i;
        char *bad = strdup(BAD_URL);
        if (bad != NULL) {
            for (i = list->count; i > 0; i--) {
                list->urls[i] = list->urls[i - 1];
            }
            list->urls[0] = bad;
            list->count++;
            fprintf(stderr, "[play] debug bad url inserted first\n");
        }
    }
    if (list->count > 0) {
        fprintf(stderr, "[play] first url %s\n", list->urls[0]);
    }
    return 0;
}
