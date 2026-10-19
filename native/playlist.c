#include "playlist.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The helper creates this directory mode 0777, not sticky, so it can unlink
 * a file written by root. See playlist.h for the test command. */
static const char *OVERRIDE_DIR = "/tmp/aerial-acb-debug";
static const char *OVERRIDE_PATH = "/tmp/aerial-acb-debug/override.json";
/* Sticky /tmp. Never opened. A root-owned file here used to survive unlink. */
static const char *OVERRIDE_OLD = "/tmp/aerial-acb-override.json";
static const char *BAD_URL =
        "https://sylvan.apple.com/itunes-assets/Aerials116/v4/00/00/00/not-a-real-aerial.mov";

void playlist_free(playlist *list) {
    int i;
    if (list == NULL) {
        return;
    }
    for (i = 0; i < list->count; i++) {
        free(list->urls[i]);
        free(list->names[i]);
        list->urls[i] = NULL;
        list->names[i] = NULL;
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

static int extract_string(const char *start, const char *end, const char *key, char *out,
                          size_t out_len) {
    char needle[96];
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
    return (p < end && *p == '"' && n > 0) ? 1 : 0;
}

static int add_item(playlist *list, const char *url, const char *key) {
    if (list->count >= PLAYLIST_MAX) {
        return -1;
    }
    list->urls[list->count] = strdup(url);
    list->names[list->count] = strdup(key != NULL ? key : "");
    if (list->urls[list->count] == NULL || list->names[list->count] == NULL) {
        free(list->urls[list->count]);
        free(list->names[list->count]);
        list->urls[list->count] = NULL;
        list->names[list->count] = NULL;
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
        tmp = list->names[i];
        list->names[i] = list->names[j];
        list->names[j] = tmp;
    }
}

/* Honour the override only when this process can delete it. A leftover
 * root-owned file on sticky /tmp cannot be deleted and must not apply. */
static void apply_override(playlist *list) {
    char buf[512];
    FILE *f;
    size_t n;
    const char *p;
    int cap = 0;
    int bad = 0;
    if (access(OVERRIDE_OLD, F_OK) == 0) {
        fprintf(stderr, "[play] ignoring leftover %s\n", OVERRIDE_OLD);
    }
    if (mkdir(OVERRIDE_DIR, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "[play] override dir %s: %s\n", OVERRIDE_DIR, strerror(errno));
    }
    chmod(OVERRIDE_DIR, 0777);
    f = fopen(OVERRIDE_PATH, "rb");
    if (f == NULL) {
        fprintf(stderr, "[play] debug override off (%s absent)\n", OVERRIDE_PATH);
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
            cap = atoi(p);
        }
    }
    p = strstr(buf, "badFirst");
    if (p != NULL) {
        const char *v = strchr(p, ':');
        if (v != NULL) {
            v++;
            while (*v == ' ' || *v == '\t' || *v == '"') {
                v++;
            }
            if (strncmp(v, "true", 4) == 0 || *v == '1') {
                bad = 1;
            }
        }
    }
    if (unlink(OVERRIDE_PATH) != 0) {
        fprintf(stderr, "[play] override unlink failed (%s); not applied\n", strerror(errno));
        return;
    }
    list->cap_sec = cap;
    list->bad_first = bad;
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
        char key[160];
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
        if (!extract_string(id, obj_end, "localizedNameKey", key, sizeof(key))) {
            key[0] = '\0';
        }
        /* H.264 first. url-1080-SDR in this catalog is HEVC; it is only a
         * fallback for an asset that has no AVC url, and the demuxer skips it
         * if the sample entry is not avc1. */
        pick = NULL;
        if (has_h) {
            pick = h264;
        } else if (has_s) {
            pick = sdr;
        }
        if (pick != NULL && add_item(list, pick, key) != 0) {
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

static int lookup_json_string(const char *json, size_t len, const char *key, char *out,
                              size_t out_len) {
    char needle[180];
    const char *p;
    const char *end;
    int n;
    if (json == NULL || key == NULL || key[0] == '\0') {
        return 0;
    }
    n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t) n >= sizeof(needle)) {
        return 0;
    }
    p = json;
    end = json + len;
    while (p < end) {
        const char *hit = find_key(p, end, needle);
        const char *q;
        int i;
        if (hit == NULL) {
            return 0;
        }
        q = hit + n;
        while (q < end && isspace((unsigned char) *q)) {
            q++;
        }
        if (q >= end || *q != ':') {
            p = hit + 1;
            continue;
        }
        q++;
        while (q < end && isspace((unsigned char) *q)) {
            q++;
        }
        if (q >= end || *q != '"') {
            p = hit + 1;
            continue;
        }
        q++;
        i = 0;
        while (q < end && *q != '"' && (size_t) i + 1 < out_len) {
            if (*q == '\\' && q + 1 < end) {
                q++;
            }
            out[i++] = *q++;
        }
        out[i] = '\0';
        return (q < end && *q == '"' && i > 0) ? 1 : 0;
    }
    return 0;
}

/* Community ids are not in the Apple locale tables. Turn the key into the
 * words a person would read, and log that same string. */
static void prettify_key(const char *key, char *out, size_t out_len) {
    const char *s = key != NULL ? key : "";
    const char *end;
    size_t i = 0;
    int new_word = 1;
    if (strncmp(s, "video_inspire_", 14) == 0) {
        s += 14;
    }
    end = s + strlen(s);
    while (end > s && isdigit((unsigned char) end[-1])) {
        end--;
    }
    if (end > s && end[-1] == '_') {
        end--;
    }
    if ((size_t) (end - s) > 5 && memcmp(end - 5, "_NAME", 5) == 0) {
        end -= 5;
    }
    for (; s < end && i + 1 < out_len; s++) {
        unsigned char c = (unsigned char) *s;
        if (c == '_') {
            if (i > 0 && out[i - 1] != ' ') {
                out[i++] = ' ';
            }
            new_word = 1;
            continue;
        }
        if (c == '-') {
            out[i++] = '-';
            new_word = 1;
            continue;
        }
        if (new_word && c >= 'a' && c <= 'z') {
            c = (unsigned char) (c - 'a' + 'A');
        }
        new_word = 0;
        out[i++] = (char) c;
    }
    out[i] = '\0';
    if (i == 0 && key != NULL) {
        snprintf(out, out_len, "%s", key);
    }
}

static void resolve_names(playlist *list, const char *json, size_t json_len) {
    int i;
    int from_locale = 0;
    for (i = 0; i < list->count; i++) {
        char display[256];
        char *replaced;
        display[0] = '\0';
        if (json != NULL &&
            lookup_json_string(json, json_len, list->names[i], display, sizeof(display))) {
            from_locale++;
        } else {
            prettify_key(list->names[i], display, sizeof(display));
        }
        replaced = strdup(display);
        if (replaced != NULL) {
            free(list->names[i]);
            list->names[i] = replaced;
        }
    }
    fprintf(stderr, "[play] place names %d from locale, %d from id\n", from_locale,
            list->count - from_locale);
}

static void dir_of(const char *file_path, char *out, size_t out_len) {
    char *slash;
    snprintf(out, out_len, "%s", file_path != NULL ? file_path : "");
    slash = strrchr(out, '/');
    if (slash != NULL) {
        *slash = '\0';
    }
}

static void read_settings(playlist *list, const char *videos_path) {
    char dir[512];
    char path[PATH_MAX];
    size_t len = 0;
    char *json;
    char lang[32];
    const char *p;
    if (videos_path == NULL || videos_path[0] == '\0') {
        return;
    }
    dir_of(videos_path, dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/settings.json", dir);
    json = read_file(path, &len);
    if (json == NULL) {
        fprintf(stderr, "[play] settings unreadable %s\n", path);
        return;
    }
    if (list->locale[0] == '\0' &&
        lookup_json_string(json, len, "localeLang", lang, sizeof(lang))) {
        snprintf(list->locale, sizeof(list->locale), "%s", lang);
    }
    if (list->osd_opacity < 0) {
        p = strstr(json, "\"osdOpacity\"");
        if (p != NULL) {
            p += 12;
            while (*p && !isdigit((unsigned char) *p)) {
                p++;
            }
            if (*p) {
                list->osd_opacity = atoi(p);
            }
        }
    }
    free(json);
    fprintf(stderr, "[play] settings %s\n", path);
}

static void load_locale_names(playlist *list, const char *videos_path) {
    char dir[512];
    char path[PATH_MAX];
    size_t len = 0;
    char *json = NULL;
    if (list->locale[0] == '\0') {
        snprintf(list->locale, sizeof(list->locale), "en-GB");
    }
    if (videos_path != NULL && videos_path[0] != '\0') {
        dir_of(videos_path, dir, sizeof(dir));
        snprintf(path, sizeof(path), "%s/locales/%s.json", dir, list->locale);
        json = read_file(path, &len);
        if (json == NULL) {
            fprintf(stderr, "[play] locale file missing %s\n", path);
        } else {
            fprintf(stderr, "[play] locale %s from %s\n", list->locale, path);
        }
    }
    resolve_names(list, json, len);
    free(json);
}

int playlist_load(playlist *list, const char *videos_path, const char *fallback_path,
                  const char *source_type, int play_lower, const char *locale, int osd_opacity) {
    uint32_t seed;
    memset(list, 0, sizeof(*list));
    list->osd_opacity = osd_opacity;
    if (locale != NULL) {
        snprintf(list->locale, sizeof(list->locale), "%s", locale);
    }
    if (source_type != NULL) {
        snprintf(list->source_type, sizeof(list->source_type), "%s", source_type);
    }
    if (list->locale[0] == '\0' || list->osd_opacity < 0) {
        read_settings(list, videos_path);
        if ((list->locale[0] == '\0' || list->osd_opacity < 0) && fallback_path != NULL) {
            read_settings(list, fallback_path);
        }
    }
    if (list->osd_opacity < 0) {
        list->osd_opacity = 60;
    }
    if (list->osd_opacity > 100) {
        list->osd_opacity = 100;
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
    load_locale_names(list, list->loaded_from[0] != '\0' ? list->loaded_from : videos_path);
    fprintf(stderr, "[play] osdOpacity=%d locale=%s\n", list->osd_opacity,
            list->locale[0] ? list->locale : "(none)");
    seed = (uint32_t) time(NULL);
    shuffle(list, seed);
    apply_override(list);
    if (list->bad_first && list->count < PLAYLIST_MAX) {
        int i;
        char *bad = strdup(BAD_URL);
        char *name = strdup("Debug bad URL");
        if (bad != NULL && name != NULL) {
            for (i = list->count; i > 0; i--) {
                list->urls[i] = list->urls[i - 1];
                list->names[i] = list->names[i - 1];
            }
            list->urls[0] = bad;
            list->names[0] = name;
            list->count++;
            fprintf(stderr, "[play] debug bad url inserted first\n");
        } else {
            free(bad);
            free(name);
        }
    }
    if (list->count > 0) {
        fprintf(stderr, "[play] first title \"%s\" url %s\n",
                list->names[0] != NULL ? list->names[0] : "", list->urls[0]);
    }
    return 0;
}
