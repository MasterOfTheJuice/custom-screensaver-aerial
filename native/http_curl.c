#define _GNU_SOURCE
#include "http_curl.h"

#include <curl/curl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef CURLcode (*curl_global_init_fn)(long);
typedef CURL *(*curl_easy_init_fn)(void);
typedef CURLcode (*curl_easy_setopt_fn)(CURL *, CURLoption, ...);
typedef CURLcode (*curl_easy_perform_fn)(CURL *);
typedef void (*curl_easy_cleanup_fn)(CURL *);
typedef CURLcode (*curl_easy_getinfo_fn)(CURL *, CURLINFO, ...);
typedef const char *(*curl_easy_strerror_fn)(CURLcode);
typedef char *(*curl_version_fn)(void);
typedef CURLM *(*curl_multi_init_fn)(void);
typedef CURLMcode (*curl_multi_add_fn)(CURLM *, CURL *);
typedef CURLMcode (*curl_multi_remove_fn)(CURLM *, CURL *);
typedef CURLMcode (*curl_multi_perform_fn)(CURLM *, int *);
typedef CURLMcode (*curl_multi_wait_fn)(CURLM *, struct curl_waitfd *, unsigned, int, int *);
typedef const char *(*curl_multi_strerror_real_fn)(CURLMcode);
typedef CURLMsg *(*curl_multi_info_read_fn)(CURLM *, int *);
typedef void (*curl_multi_cleanup_fn)(CURLM *);

struct curl_api {
    void *lib;
    curl_global_init_fn global_init;
    curl_easy_init_fn easy_init;
    curl_easy_setopt_fn easy_setopt;
    curl_easy_perform_fn easy_perform;
    curl_easy_cleanup_fn easy_cleanup;
    curl_easy_getinfo_fn easy_getinfo;
    curl_easy_strerror_fn easy_strerror;
    curl_version_fn version;
    curl_multi_init_fn multi_init;
    curl_multi_add_fn multi_add;
    curl_multi_remove_fn multi_remove;
    curl_multi_perform_fn multi_perform;
    curl_multi_wait_fn multi_wait;
    curl_multi_strerror_real_fn multi_strerror;
    curl_multi_info_read_fn info_read;
    curl_multi_cleanup_fn multi_cleanup;
    int ready;
    int logged;
};

static struct curl_api g_curl;
static int64_t g_bytes;

struct http_client {
    CURL *easy;
    CURLM *multi;
    char ca[512];
    int last_status;
};

struct xfer {
    uint8_t *dst;
    size_t cap;
    size_t got;
    int overflow;
    int64_t range_start;
    int64_t range_total;
    int saw_range;
    char *err;
    size_t err_len;
};

static void set_err(char *err, size_t err_len, const char *msg) {
    if (err == NULL || err_len == 0) {
        return;
    }
    snprintf(err, err_len, "%s", msg != NULL ? msg : "http error");
}

static int load_curl(char *err, size_t err_len) {
    const char *names[] = {"libcurl.so.5", "libcurl.so.4", "libcurl.so", NULL};
    int i;
    if (g_curl.ready) {
        return 0;
    }
    for (i = 0; names[i] != NULL; i++) {
        g_curl.lib = dlopen(names[i], RTLD_NOW);
        if (g_curl.lib != NULL) {
            break;
        }
    }
    if (g_curl.lib == NULL) {
        set_err(err, err_len, dlerror() != NULL ? dlerror() : "dlopen libcurl failed");
        return -1;
    }
#define LOAD(field, symbol)                                                                          \
    do {                                                                                             \
        g_curl.field = (typeof(g_curl.field))dlsym(g_curl.lib, symbol);                             \
        if (g_curl.field == NULL) {                                                                  \
            set_err(err, err_len, "missing " symbol);                                                \
            return -1;                                                                               \
        }                                                                                            \
    } while (0)
    LOAD(global_init, "curl_global_init");
    LOAD(easy_init, "curl_easy_init");
    LOAD(easy_setopt, "curl_easy_setopt");
    LOAD(easy_perform, "curl_easy_perform");
    LOAD(easy_cleanup, "curl_easy_cleanup");
    LOAD(easy_getinfo, "curl_easy_getinfo");
    LOAD(easy_strerror, "curl_easy_strerror");
    LOAD(version, "curl_version");
    LOAD(multi_init, "curl_multi_init");
    LOAD(multi_add, "curl_multi_add_handle");
    LOAD(multi_remove, "curl_multi_remove_handle");
    LOAD(multi_perform, "curl_multi_perform");
    LOAD(multi_wait, "curl_multi_wait");
    LOAD(multi_strerror, "curl_multi_strerror");
    LOAD(info_read, "curl_multi_info_read");
    LOAD(multi_cleanup, "curl_multi_cleanup");
#undef LOAD
    if (g_curl.global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        set_err(err, err_len, "curl_global_init failed");
        return -1;
    }
    g_curl.ready = 1;
    return 0;
}

int64_t http_bytes_downloaded(void) { return g_bytes; }

int http_last_status(const http_client *client) {
    return client != NULL ? client->last_status : 0;
}

http_client *http_client_create(const char *ca_file) {
    char err[256];
    http_client *client;
    if (ca_file == NULL || ca_file[0] == '\0') {
        fprintf(stderr, "[http] no CA file\n");
        return NULL;
    }
    if (load_curl(err, sizeof(err)) != 0) {
        fprintf(stderr, "[http] %s\n", err);
        return NULL;
    }
    client = calloc(1, sizeof(*client));
    if (client == NULL) {
        return NULL;
    }
    snprintf(client->ca, sizeof(client->ca), "%s", ca_file);
    client->easy = g_curl.easy_init();
    client->multi = g_curl.multi_init();
    if (client->easy == NULL || client->multi == NULL) {
        http_client_destroy(client);
        fprintf(stderr, "[http] curl handle alloc failed\n");
        return NULL;
    }
    if (!g_curl.logged) {
        fprintf(stderr, "[http] %s\n", g_curl.version());
        fprintf(stderr, "[http] CA bundle %s (peer verify on)\n", ca_file);
        g_curl.logged = 1;
    }
    return client;
}

void http_client_destroy(http_client *client) {
    if (client == NULL) {
        return;
    }
    if (client->easy != NULL) {
        g_curl.easy_cleanup(client->easy);
    }
    if (client->multi != NULL) {
        g_curl.multi_cleanup(client->multi);
    }
    free(client);
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct xfer *xfer = userdata;
    size_t n = size * nmemb;
    size_t room;
    if (n == 0) {
        return 0;
    }
    if (xfer->got >= xfer->cap) {
        xfer->overflow = 1;
        return 0;
    }
    room = xfer->cap - xfer->got;
    if (n > room) {
        xfer->overflow = 1;
        return 0;
    }
    memcpy(xfer->dst + xfer->got, ptr, n);
    xfer->got += n;
    g_bytes += (int64_t) n;
    return n;
}

static int header_has(const char *line, size_t len, const char *name) {
    size_t n = strlen(name);
    size_t i;
    if (len < n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = line[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char) (c - 'A' + 'a');
        }
        if (c != name[i]) {
            return 0;
        }
    }
    return 1;
}

static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct xfer *xfer = userdata;
    size_t n = size * nmemb;
    const char *line = ptr;
    /* "Content-Range: bytes START-END/TOTAL" */
    if (header_has(line, n, "content-range:")) {
        const char *p = line;
        const char *end = line + n;
        while (p < end && *p != ':') {
            p++;
        }
        if (p < end) {
            p++;
        }
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if ((size_t) (end - p) >= 6 && strncmp(p, "bytes ", 6) == 0) {
            p += 6;
        }
        if (p < end && *p != '*') {
            long long start = 0;
            long long total = -1;
            if (sscanf(p, "%lld-%*lld/%lld", &start, &total) >= 1) {
                xfer->range_start = (int64_t) start;
                xfer->saw_range = 1;
                if (total >= 0) {
                    xfer->range_total = (int64_t) total;
                }
            }
        }
    }
    return n;
}

/* Returning non-zero aborts the transfer. The multi loop is what watches EXIT;
 * this stays quiet so a finished range is not torn down by the progress tick. */
static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                       curl_off_t ulnow) {
    (void) clientp;
    (void) dltotal;
    (void) dlnow;
    (void) ultotal;
    (void) ulnow;
    return 0;
}

int http_get_range(http_client *client, const char *url, int64_t start, size_t len, uint8_t *dst,
                   int64_t *total, http_pump pump, void *pump_ctx, char *err, size_t err_len) {
    struct xfer xfer;
    char range[64];
    char curl_err[CURL_ERROR_SIZE];
    long code = 0;
    int running = 0;
    CURLcode done = CURLE_OK;
    int saw_done = 0;
    CURLMcode mc;
    int aborted = 0;

    if (client == NULL || url == NULL || dst == NULL || len == 0 || start < 0) {
        set_err(err, err_len, "bad range request");
        return -1;
    }
    memset(&xfer, 0, sizeof(xfer));
    xfer.dst = dst;
    xfer.cap = len;
    xfer.range_start = -1;
    xfer.range_total = -1;
    xfer.err = err;
    xfer.err_len = err_len;
    curl_err[0] = '\0';
    snprintf(range, sizeof(range), "%lld-%lld", (long long) start,
             (long long) (start + (int64_t) len - 1));

    g_curl.easy_setopt(client->easy, CURLOPT_URL, url);
    g_curl.easy_setopt(client->easy, CURLOPT_RANGE, range);
    g_curl.easy_setopt(client->easy, CURLOPT_WRITEFUNCTION, write_cb);
    g_curl.easy_setopt(client->easy, CURLOPT_WRITEDATA, &xfer);
    g_curl.easy_setopt(client->easy, CURLOPT_HEADERFUNCTION, header_cb);
    g_curl.easy_setopt(client->easy, CURLOPT_HEADERDATA, &xfer);
    g_curl.easy_setopt(client->easy, CURLOPT_ERRORBUFFER, curl_err);
    g_curl.easy_setopt(client->easy, CURLOPT_FOLLOWLOCATION, 1L);
    g_curl.easy_setopt(client->easy, CURLOPT_MAXREDIRS, 5L);
    g_curl.easy_setopt(client->easy, CURLOPT_USERAGENT, "aerial-acb/1.0");
    g_curl.easy_setopt(client->easy, CURLOPT_SSL_VERIFYPEER, 1L);
    g_curl.easy_setopt(client->easy, CURLOPT_SSL_VERIFYHOST, 2L);
    g_curl.easy_setopt(client->easy, CURLOPT_CAINFO, client->ca);
    g_curl.easy_setopt(client->easy, CURLOPT_HTTP_VERSION, (long) CURL_HTTP_VERSION_1_1);
    g_curl.easy_setopt(client->easy, CURLOPT_NOSIGNAL, 1L);
    g_curl.easy_setopt(client->easy, CURLOPT_CONNECTTIMEOUT, 10L);
    g_curl.easy_setopt(client->easy, CURLOPT_TIMEOUT, 30L);
    g_curl.easy_setopt(client->easy, CURLOPT_LOW_SPEED_LIMIT, 1000L);
    g_curl.easy_setopt(client->easy, CURLOPT_LOW_SPEED_TIME, 20L);
    g_curl.easy_setopt(client->easy, CURLOPT_NOPROGRESS, 0L);
    g_curl.easy_setopt(client->easy, CURLOPT_XFERINFOFUNCTION, progress_cb);
    g_curl.easy_setopt(client->easy, CURLOPT_XFERINFODATA, NULL);

    mc = g_curl.multi_add(client->multi, client->easy);
    if (mc != CURLM_OK) {
        set_err(err, err_len, g_curl.multi_strerror(mc));
        return -1;
    }
    do {
        mc = g_curl.multi_perform(client->multi, &running);
        if (mc != CURLM_OK) {
            break;
        }
        if (!running) {
            break;
        }
        if (pump != NULL && !pump(pump_ctx)) {
            aborted = 1;
            break;
        }
        g_curl.multi_wait(client->multi, NULL, 0, 20, NULL);
    } while (running);

    {
        int left = 0;
        CURLMsg *msg;
        while ((msg = g_curl.info_read(client->multi, &left)) != NULL) {
            if (msg->msg == CURLMSG_DONE) {
                done = msg->data.result;
                saw_done = 1;
            }
        }
    }
    g_curl.multi_remove(client->multi, client->easy);
    g_curl.easy_getinfo(client->easy, CURLINFO_RESPONSE_CODE, &code);
    client->last_status = (int) code;
    if (total != NULL && xfer.range_total > 0) {
        *total = xfer.range_total;
    }

    if (aborted || (pump != NULL && !pump(pump_ctx) && xfer.got != len)) {
        set_err(err, err_len, "aborted");
        return -2;
    }
    if (xfer.got == len && code == 206 && xfer.saw_range && xfer.range_start == start) {
        return 0;
    }
    if (xfer.got == len && code == 200 && start == 0) {
        return 0;
    }
    if (curl_err[0] != '\0') {
        snprintf(err, err_len, "HTTP %ld %s", code, curl_err);
    } else if (saw_done && done != CURLE_OK && code == 0) {
        snprintf(err, err_len, "%s", g_curl.easy_strerror(done));
    } else if (code != 0) {
        snprintf(err, err_len, "HTTP %ld range %s got %zu", code, range, xfer.got);
    } else if (mc != CURLM_OK) {
        set_err(err, err_len, g_curl.multi_strerror(mc));
    } else {
        set_err(err, err_len, "transfer failed");
    }
    return -1;
}
