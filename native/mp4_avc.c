#include "mp4_avc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One access unit's location in the file. PTS is composition time. */
struct sample {
    int64_t offset;
    uint32_t size;
    int64_t pts_ns;
    uint8_t key;
};

struct avc_movie {
    http_client *http;
    char *url;
    int64_t file_size;
    int width;
    int height;
    int fps_num;
    int fps_den;
    int64_t frame_ns;
    double duration_s;
    int nal_len;
    uint8_t *sps;
    size_t sps_len;
    uint8_t *pps;
    size_t pps_len;
    struct sample *samples;
    int nsamples;
    uint8_t *window;
    size_t window_cap;
    int64_t window_off;
    size_t window_len;
    uint8_t *au;
    size_t au_cap;
    size_t au_len;
};

struct cursor {
    const uint8_t *p;
    size_t n;
    size_t i;
};

static void set_err(char *err, size_t err_len, const char *msg) {
    if (err != NULL && err_len > 0) {
        snprintf(err, err_len, "%s", msg);
    }
}

static int rd32(struct cursor *c, uint32_t *out) {
    const uint8_t *b;
    if (c->i + 4 > c->n) {
        return -1;
    }
    b = c->p + c->i;
    *out = ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | b[3];
    c->i += 4;
    return 0;
}

static int rd16(struct cursor *c, uint16_t *out) {
    const uint8_t *b;
    if (c->i + 2 > c->n) {
        return -1;
    }
    b = c->p + c->i;
    *out = (uint16_t) ((b[0] << 8) | b[1]);
    c->i += 2;
    return 0;
}

static int rd8(struct cursor *c, uint8_t *out) {
    if (c->i + 1 > c->n) {
        return -1;
    }
    *out = c->p[c->i++];
    return 0;
}

static int skip(struct cursor *c, size_t n) {
    if (c->i + n > c->n) {
        return -1;
    }
    c->i += n;
    return 0;
}

static uint32_t u32_at(const uint8_t *b) {
    return ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | b[3];
}

static uint64_t u64_at(const uint8_t *b) {
    return ((uint64_t) u32_at(b) << 32) | u32_at(b + 4);
}

/* Apple writes signed composition offsets into a version-0 ctts box.
 * Version 0 is specified as unsigned; the negative values are two's complement
 * and are one frame of B-frame reorder. Read them as int32. */
static int32_t s32_at(const uint8_t *b) {
    return (int32_t) u32_at(b);
}

struct atom {
    uint64_t size;
    int header;
    char type[4];
};

static int read_atom_header(const uint8_t *b, size_t n, struct atom *a) {
    uint32_t size32;
    if (n < 8) {
        return -1;
    }
    size32 = u32_at(b);
    memcpy(a->type, b + 4, 4);
    if (size32 == 1) {
        if (n < 16) {
            return -1;
        }
        a->size = u64_at(b + 8);
        a->header = 16;
    } else if (size32 == 0) {
        a->size = 0;
        a->header = 8;
    } else {
        a->size = size32;
        a->header = 8;
    }
    if (a->size != 0 && a->size < (uint64_t) a->header) {
        return -1;
    }
    return 0;
}

static int find_child(const uint8_t *buf, size_t len, size_t start, const char *type, size_t *pay,
                      size_t *pay_len) {
    size_t off = start;
    while (off + 8 <= len) {
        struct atom a;
        if (read_atom_header(buf + off, len - off, &a) != 0) {
            return -1;
        }
        if (a.size == 0) {
            a.size = len - off;
        }
        if (off + a.size > len) {
            return -1;
        }
        if (memcmp(a.type, type, 4) == 0) {
            *pay = off + (size_t) a.header;
            *pay_len = (size_t) a.size - (size_t) a.header;
            return 0;
        }
        off += (size_t) a.size;
    }
    return -1;
}

static int copy_nals(const uint8_t *p, size_t n, int count_mask, uint8_t **out, size_t *out_len) {
    struct cursor c;
    uint8_t count;
    uint8_t i;
    size_t total = 0;
    size_t at = 0;
    c.p = p;
    c.n = n;
    c.i = 0;
    if (rd8(&c, &count) != 0) {
        return -1;
    }
    count = (uint8_t) (count & count_mask);
    for (i = 0; i < count; i++) {
        uint16_t ln;
        if (rd16(&c, &ln) != 0 || skip(&c, ln) != 0) {
            return -1;
        }
        total += ln;
    }
    if (total == 0) {
        return -1;
    }
    *out = malloc(total);
    if (*out == NULL) {
        return -1;
    }
    c.i = 1;
    for (i = 0; i < count; i++) {
        uint16_t ln = 0;
        if (rd16(&c, &ln) != 0) {
            free(*out);
            return -1;
        }
        memcpy(*out + at, c.p + c.i, ln);
        at += ln;
        c.i += ln;
    }
    *out_len = total;
    return (int) (c.i);
}

static int parse_avcc(const uint8_t *buf, size_t len, avc_movie *movie) {
    struct cursor c;
    uint8_t ver, profile, compat, level, lensz, reserved;
    int used;
    c.p = buf;
    c.n = len;
    c.i = 0;
    if (rd8(&c, &ver) || rd8(&c, &profile) || rd8(&c, &compat) || rd8(&c, &level) ||
        rd8(&c, &lensz)) {
        return -1;
    }
    (void) ver;
    (void) profile;
    (void) compat;
    (void) level;
    movie->nal_len = (lensz & 3) + 1;
    if (movie->nal_len != 4 && movie->nal_len != 2 && movie->nal_len != 1) {
        return -1;
    }
    used = copy_nals(buf + c.i, len - c.i, 0x1f, &movie->sps, &movie->sps_len);
    if (used < 0) {
        return -1;
    }
    c.i += (size_t) used;
    used = copy_nals(buf + c.i, len - c.i, 0xff, &movie->pps, &movie->pps_len);
    if (used < 0) {
        return -1;
    }
    (void) reserved;
    return 0;
}

static int parse_stsd(const uint8_t *buf, size_t len, avc_movie *movie, char *err, size_t err_len) {
    struct cursor c;
    uint32_t count;
    uint32_t entry_size;
    char typ[4];
    const uint8_t *entry;
    size_t entry_len;
    size_t child;
    c.p = buf;
    c.n = len;
    c.i = 0;
    /* full box */
    if (skip(&c, 4) || rd32(&c, &count) || count < 1) {
        set_err(err, err_len, "bad stsd");
        return -1;
    }
    if (rd32(&c, &entry_size) || entry_size < 86 || c.i + entry_size - 4 > len) {
        set_err(err, err_len, "bad sample entry");
        return -1;
    }
    memcpy(typ, c.p + c.i, 4);
    if (memcmp(typ, "avc1", 4) != 0 && memcmp(typ, "avc3", 4) != 0) {
        snprintf(err, err_len, "codec %c%c%c%c is not avc", typ[0], typ[1], typ[2], typ[3]);
        return -1;
    }
    entry = c.p + c.i - 4;
    entry_len = entry_size;
    /* VisualSampleEntry fixed fields end at byte 86 of the box. */
    if (entry_len < 86) {
        set_err(err, err_len, "short avc1");
        return -1;
    }
    movie->width = (entry[32] << 8) | entry[33];
    movie->height = (entry[34] << 8) | entry[35];
    child = 86;
    while (child + 8 <= entry_len) {
        struct atom a;
        if (read_atom_header(entry + child, entry_len - child, &a) != 0 || a.size == 0 ||
            child + a.size > entry_len) {
            break;
        }
        if (memcmp(a.type, "avcC", 4) == 0) {
            if (parse_avcc(entry + child + (size_t) a.header,
                           (size_t) a.size - (size_t) a.header, movie) != 0) {
                set_err(err, err_len, "bad avcC");
                return -1;
            }
            return 0;
        }
        child += (size_t) a.size;
    }
    set_err(err, err_len, "no avcC");
    return -1;
}

struct stsc_ent {
    uint32_t first;
    uint32_t per;
};

static int build_samples(const uint8_t *stbl, size_t stbl_len, avc_movie *movie, char *err,
                         size_t err_len) {
    size_t pay, pay_len;
    const uint8_t *stsz;
    const uint8_t *stts;
    const uint8_t *stsc;
    const uint8_t *stco;
    const uint8_t *ctts = NULL;
    const uint8_t *stss = NULL;
    size_t stsz_len, stts_len, stsc_len, stco_len, ctts_len = 0, stss_len = 0;
    uint32_t sample_size, sample_count, i;
    uint32_t stts_count, stsc_count, chunk_count;
    int use_co64 = 0;
    struct stsc_ent *ents = NULL;
    int64_t *chunks = NULL;
    uint32_t *sizes = NULL;
    int stsc_i;
    int produced;
    int rc = -1;
    uint32_t stts_i, stts_left, stts_delta;
    uint32_t ctts_i = 0, ctts_left = 0, ctts_count = 0;
    int32_t ctts_off = 0;
    int64_t dts = 0;
    uint32_t timescale;

    if (find_child(stbl, stbl_len, 0, "stsz", &pay, &pay_len) != 0) {
        set_err(err, err_len, "no stsz");
        return -1;
    }
    stsz = stbl + pay;
    stsz_len = pay_len;
    if (find_child(stbl, stbl_len, 0, "stts", &pay, &pay_len) != 0) {
        set_err(err, err_len, "no stts");
        return -1;
    }
    stts = stbl + pay;
    stts_len = pay_len;
    if (find_child(stbl, stbl_len, 0, "stsc", &pay, &pay_len) != 0) {
        set_err(err, err_len, "no stsc");
        return -1;
    }
    stsc = stbl + pay;
    stsc_len = pay_len;
    if (find_child(stbl, stbl_len, 0, "stco", &pay, &pay_len) == 0) {
        stco = stbl + pay;
        stco_len = pay_len;
    } else if (find_child(stbl, stbl_len, 0, "co64", &pay, &pay_len) == 0) {
        stco = stbl + pay;
        stco_len = pay_len;
        use_co64 = 1;
    } else {
        set_err(err, err_len, "no stco");
        return -1;
    }
    if (find_child(stbl, stbl_len, 0, "ctts", &pay, &pay_len) == 0) {
        ctts = stbl + pay;
        ctts_len = pay_len;
    }
    if (find_child(stbl, stbl_len, 0, "stss", &pay, &pay_len) == 0) {
        stss = stbl + pay;
        stss_len = pay_len;
    }

    if (stsz_len < 12 || stts_len < 8 || stsc_len < 8 || stco_len < 8) {
        set_err(err, err_len, "short sample tables");
        return -1;
    }
    sample_size = u32_at(stsz + 4);
    sample_count = u32_at(stsz + 8);
    if (sample_count == 0 || sample_count > 200000) {
        set_err(err, err_len, "sample count");
        return -1;
    }
    if (sample_size == 0 && stsz_len < 12 + (size_t) sample_count * 4) {
        set_err(err, err_len, "stsz truncated");
        return -1;
    }
    timescale = (uint32_t) movie->fps_num;
    if (timescale == 0) {
        set_err(err, err_len, "no timescale");
        return -1;
    }

    sizes = malloc(sizeof(*sizes) * sample_count);
    movie->samples = calloc(sample_count, sizeof(*movie->samples));
    if (sizes == NULL || movie->samples == NULL) {
        set_err(err, err_len, "out of memory");
        goto done;
    }
    for (i = 0; i < sample_count; i++) {
        sizes[i] = sample_size != 0 ? sample_size : u32_at(stsz + 12 + i * 4);
        if (sizes[i] == 0 || sizes[i] > 8 * 1024 * 1024) {
            set_err(err, err_len, "sample size");
            goto done;
        }
    }

    stts_count = u32_at(stts + 4);
    if (stts_count == 0 || stts_len < 8 + (size_t) stts_count * 8) {
        set_err(err, err_len, "bad stts");
        goto done;
    }
    movie->fps_den = (int) u32_at(stts + 12);
    if (movie->fps_den <= 0) {
        movie->fps_den = 1;
    }
    movie->frame_ns = (int64_t) movie->fps_den * 1000000000LL / timescale;

    stsc_count = u32_at(stsc + 4);
    chunk_count = u32_at(stco + 4);
    if (stsc_count == 0 || chunk_count == 0 || stsc_len < 8 + (size_t) stsc_count * 12) {
        set_err(err, err_len, "bad stsc");
        goto done;
    }
    if (!use_co64 && stco_len < 8 + (size_t) chunk_count * 4) {
        set_err(err, err_len, "bad stco");
        goto done;
    }
    if (use_co64 && stco_len < 8 + (size_t) chunk_count * 8) {
        set_err(err, err_len, "bad co64");
        goto done;
    }
    ents = malloc(sizeof(*ents) * stsc_count);
    chunks = malloc(sizeof(*chunks) * chunk_count);
    if (ents == NULL || chunks == NULL) {
        set_err(err, err_len, "out of memory");
        goto done;
    }
    for (i = 0; i < stsc_count; i++) {
        ents[i].first = u32_at(stsc + 8 + i * 12);
        ents[i].per = u32_at(stsc + 12 + i * 12);
        if (ents[i].per == 0) {
            set_err(err, err_len, "empty chunk");
            goto done;
        }
    }
    for (i = 0; i < chunk_count; i++) {
        chunks[i] = use_co64 ? (int64_t) u64_at(stco + 8 + i * 8) : (int64_t) u32_at(stco + 8 + i * 4);
    }

    produced = 0;
    stsc_i = 0;
    for (i = 1; i <= chunk_count; i++) {
        uint32_t s;
        int64_t off;
        while (stsc_i + 1 < (int) stsc_count && ents[stsc_i + 1].first <= i) {
            stsc_i++;
        }
        off = chunks[i - 1];
        for (s = 0; s < ents[stsc_i].per; s++) {
            if (produced >= (int) sample_count) {
                set_err(err, err_len, "too many samples");
                goto done;
            }
            movie->samples[produced].offset = off;
            movie->samples[produced].size = sizes[produced];
            off += sizes[produced];
            produced++;
        }
    }
    if (produced != (int) sample_count) {
        snprintf(err, err_len, "sample map %d != %u", produced, sample_count);
        goto done;
    }

    stts_i = 0;
    stts_left = u32_at(stts + 8);
    stts_delta = u32_at(stts + 12);
    if (ctts != NULL && ctts_len >= 8) {
        ctts_count = u32_at(ctts + 4);
        if (ctts_count > 0 && ctts_len >= 16) {
            ctts_left = u32_at(ctts + 8);
            ctts_off = s32_at(ctts + 12);
        }
    }
    for (i = 0; i < sample_count; i++) {
        int64_t pts = dts + (int64_t) ctts_off;
        if (pts < 0) {
            pts = 0;
        }
        movie->samples[i].pts_ns = pts * 1000000000LL / (int64_t) timescale;
        dts += stts_delta;
        if (stts_left > 0) {
            stts_left--;
        }
        if (stts_left == 0 && stts_i + 1 < stts_count) {
            stts_i++;
            stts_left = u32_at(stts + 8 + stts_i * 8);
            stts_delta = u32_at(stts + 12 + stts_i * 8);
        }
        if (ctts_count > 0 && ctts_left > 0) {
            ctts_left--;
        }
        if (ctts_count > 0 && ctts_left == 0 && ctts_i + 1 < ctts_count) {
            ctts_i++;
            ctts_left = u32_at(ctts + 8 + ctts_i * 8);
            ctts_off = s32_at(ctts + 12 + ctts_i * 8);
        }
    }

    if (stss == NULL) {
        for (i = 0; i < sample_count; i++) {
            movie->samples[i].key = 1;
        }
    } else if (stss_len >= 8) {
        uint32_t nsync = u32_at(stss + 4);
        if (stss_len < 8 + (size_t) nsync * 4) {
            set_err(err, err_len, "bad stss");
            goto done;
        }
        for (i = 0; i < nsync; i++) {
            uint32_t num = u32_at(stss + 8 + i * 4);
            if (num >= 1 && num <= sample_count) {
                movie->samples[num - 1].key = 1;
            }
        }
    }
    movie->nsamples = (int) sample_count;
    rc = 0;

done:
    free(sizes);
    free(ents);
    free(chunks);
    return rc;
}

static int parse_moov(const uint8_t *moov, size_t len, avc_movie *movie, char *err, size_t err_len) {
    size_t off = 0;
    int found = 0;
    while (off + 8 <= len) {
        struct atom a;
        size_t trak;
        size_t trak_len;
        size_t pay, pay_len;
        const uint8_t *hdlr;
        if (read_atom_header(moov + off, len - off, &a) != 0) {
            break;
        }
        if (a.size == 0 || off + a.size > len) {
            break;
        }
        if (memcmp(a.type, "trak", 4) != 0) {
            off += (size_t) a.size;
            continue;
        }
        trak = off + (size_t) a.header;
        trak_len = (size_t) a.size - (size_t) a.header;
        if (find_child(moov, trak + trak_len, trak, "mdia", &pay, &pay_len) != 0) {
            off += (size_t) a.size;
            continue;
        }
        /* find hdlr inside this mdia */
        {
            size_t mdia = pay;
            size_t mdia_end = pay + pay_len;
            size_t hpos, hlen;
            if (find_child(moov, mdia_end, mdia, "hdlr", &hpos, &hlen) != 0 || hlen < 12) {
                off += (size_t) a.size;
                continue;
            }
            hdlr = moov + hpos;
            /* full box (4) + pre_defined (4) + handler */
            if (memcmp(hdlr + 8, "vide", 4) != 0) {
                off += (size_t) a.size;
                continue;
            }
            if (find_child(moov, mdia_end, mdia, "mdhd", &hpos, &hlen) == 0 && hlen >= 20) {
                const uint8_t *mdhd = moov + hpos;
                uint32_t timescale;
                uint64_t duration;
                if (mdhd[0] == 1 && hlen >= 32) {
                    timescale = u32_at(mdhd + 20);
                    duration = u64_at(mdhd + 24);
                } else {
                    timescale = u32_at(mdhd + 12);
                    duration = u32_at(mdhd + 16);
                }
                movie->fps_num = (int) timescale;
                if (timescale > 0) {
                    movie->duration_s = (double) duration / (double) timescale;
                }
            }
            if (find_child(moov, mdia_end, mdia, "minf", &hpos, &hlen) != 0) {
                set_err(err, err_len, "no minf");
                return -1;
            }
            {
                size_t minf = hpos;
                size_t minf_end = hpos + hlen;
                size_t spos, slen;
                if (find_child(moov, minf_end, minf, "stbl", &spos, &slen) != 0) {
                    set_err(err, err_len, "no stbl");
                    return -1;
                }
                if (find_child(moov, spos + slen, spos, "stsd", &hpos, &hlen) != 0 ||
                    parse_stsd(moov + hpos, hlen, movie, err, err_len) != 0) {
                    return -1;
                }
                if (build_samples(moov + spos, slen, movie, err, err_len) != 0) {
                    return -1;
                }
                found = 1;
                break;
            }
        }
    }
    if (!found) {
        set_err(err, err_len, "no avc video trak");
        return -1;
    }
    return 0;
}

static int download(http_client *http, const char *url, int64_t start, size_t len, uint8_t *dst,
                    int64_t *total, http_pump pump, void *ctx, char *err, size_t err_len) {
    return http_get_range(http, url, start, len, dst, total, pump, ctx, err, err_len);
}

avc_movie *avc_movie_open(http_client *http, const char *url, http_pump pump, void *pump_ctx,
                          char *err, size_t err_len) {
    avc_movie *movie;
    uint8_t hdr[16];
    int64_t file_size = -1;
    int64_t off = 0;
    uint8_t *moov = NULL;
    size_t moov_len = 0;

    if (http == NULL || url == NULL) {
        set_err(err, err_len, "no http");
        return NULL;
    }
    movie = calloc(1, sizeof(*movie));
    if (movie == NULL) {
        set_err(err, err_len, "out of memory");
        return NULL;
    }
    movie->http = http;
    movie->url = strdup(url);
    movie->window_off = -1;
    if (movie->url == NULL) {
        set_err(err, err_len, "out of memory");
        avc_movie_close(movie);
        return NULL;
    }

    /* Top-level atoms are a few dozen bytes of header. mdat is skipped by size,
     * which is how a moov that sits after a 200 MB mdat stays a small fetch. */
    if (download(http, url, 0, 16, hdr, &file_size, pump, pump_ctx, err, err_len) != 0) {
        avc_movie_close(movie);
        return NULL;
    }
    if (file_size <= 0) {
        set_err(err, err_len, "no content length");
        avc_movie_close(movie);
        return NULL;
    }
    movie->file_size = file_size;
    while (off + 8 < file_size) {
        uint8_t head[16];
        struct atom a;
        size_t want = 16;
        if (file_size - off < 16) {
            want = (size_t) (file_size - off);
        }
        if (want < 8) {
            break;
        }
        if (download(http, url, off, want, head, NULL, pump, pump_ctx, err, err_len) != 0) {
            avc_movie_close(movie);
            return NULL;
        }
        if (read_atom_header(head, want, &a) != 0) {
            set_err(err, err_len, "bad atom");
            avc_movie_close(movie);
            return NULL;
        }
        if (a.size == 0) {
            a.size = (uint64_t) (file_size - off);
        }
        if (a.size < (uint64_t) a.header || off + (int64_t) a.size > file_size) {
            snprintf(err, err_len, "atom %.4s past eof", a.type);
            avc_movie_close(movie);
            return NULL;
        }
        if (memcmp(a.type, "moov", 4) == 0) {
            if (a.size > 8 * 1024 * 1024) {
                set_err(err, err_len, "moov too large");
                avc_movie_close(movie);
                return NULL;
            }
            moov_len = (size_t) a.size;
            moov = malloc(moov_len);
            if (moov == NULL ||
                download(http, url, off, moov_len, moov, NULL, pump, pump_ctx, err, err_len) != 0) {
                free(moov);
                if (err[0] == '\0') {
                    set_err(err, err_len, "moov download failed");
                }
                avc_movie_close(movie);
                return NULL;
            }
            if (parse_moov(moov + a.header, moov_len - (size_t) a.header, movie, err, err_len) != 0) {
                free(moov);
                avc_movie_close(movie);
                return NULL;
            }
            free(moov);
            fprintf(stderr,
                    "[mp4] %s samples=%d %.1fs %dx%d fps %d/%d file=%lld bytes moov@%lld\n",
                    url, movie->nsamples, movie->duration_s, movie->width, movie->height,
                    movie->fps_num, movie->fps_den, (long long) movie->file_size, (long long) off);
            return movie;
        }
        off += (int64_t) a.size;
    }
    set_err(err, err_len, "no moov");
    avc_movie_close(movie);
    return NULL;
}

void avc_movie_close(avc_movie *movie) {
    if (movie == NULL) {
        return;
    }
    free(movie->url);
    free(movie->sps);
    free(movie->pps);
    free(movie->samples);
    free(movie->window);
    free(movie->au);
    free(movie);
}

void avc_movie_detach_http(avc_movie *movie) {
    if (movie != NULL) {
        movie->http = NULL;
    }
}

void avc_movie_attach_http(avc_movie *movie, http_client *http) {
    if (movie != NULL) {
        movie->http = http;
    }
}

int avc_movie_samples(const avc_movie *movie) { return movie != NULL ? movie->nsamples : 0; }
int avc_movie_width(const avc_movie *movie) { return movie != NULL ? movie->width : 0; }
int avc_movie_height(const avc_movie *movie) { return movie != NULL ? movie->height : 0; }
int avc_movie_fps_num(const avc_movie *movie) { return movie != NULL ? movie->fps_num : 0; }
int avc_movie_fps_den(const avc_movie *movie) { return movie != NULL ? movie->fps_den : 1; }
int64_t avc_movie_frame_ns(const avc_movie *movie) { return movie != NULL ? movie->frame_ns : 0; }
double avc_movie_duration_s(const avc_movie *movie) { return movie != NULL ? movie->duration_s : 0; }
const char *avc_movie_url(const avc_movie *movie) { return movie != NULL ? movie->url : ""; }

static int ensure_window(avc_movie *movie, int index, http_pump pump, void *ctx, char *err,
                         size_t err_len) {
    struct sample *s = &movie->samples[index];
    int64_t start;
    int64_t end;
    int j;
    size_t len;
    const size_t cap = 4 * 1024 * 1024;

    if ((int64_t) s->size <= movie->window_len && s->offset >= movie->window_off &&
        s->offset + (int64_t) s->size <= movie->window_off + (int64_t) movie->window_len) {
        return 0;
    }
    if (movie->http == NULL) {
        set_err(err, err_len, "no connection for sample");
        return -1;
    }
    start = s->offset;
    end = start + (int64_t) s->size;
    for (j = index + 1; j < movie->nsamples; j++) {
        struct sample *n = &movie->samples[j];
        if (n->offset != end) {
            break;
        }
        if ((uint64_t) (end - start) + n->size > cap && j > index) {
            break;
        }
        end += n->size;
    }
    len = (size_t) (end - start);
    if (movie->window_cap < len) {
        uint8_t *grown = realloc(movie->window, len);
        if (grown == NULL) {
            set_err(err, err_len, "out of memory");
            return -1;
        }
        movie->window = grown;
        movie->window_cap = len;
    }
    if (download(movie->http, movie->url, start, len, movie->window, NULL, pump, ctx, err, err_len) !=
        0) {
        movie->window_len = 0;
        return -1;
    }
    movie->window_off = start;
    movie->window_len = len;
    return 0;
}

static void put_start(uint8_t *d, size_t *o, const uint8_t *nal, size_t n) {
    d[(*o)++] = 0;
    d[(*o)++] = 0;
    d[(*o)++] = 0;
    d[(*o)++] = 1;
    memcpy(d + *o, nal, n);
    *o += n;
}

bool avc_movie_sample(avc_movie *movie, int index, avc_au *out, http_pump pump, void *pump_ctx,
                      char *err, size_t err_len) {
    struct sample *s;
    const uint8_t *src;
    size_t i;
    size_t need;
    size_t o = 0;
    int key;
    if (movie == NULL || out == NULL || index < 0 || index >= movie->nsamples) {
        set_err(err, err_len, "bad sample index");
        return false;
    }
    if (ensure_window(movie, index, pump, pump_ctx, err, err_len) != 0) {
        return false;
    }
    s = &movie->samples[index];
    src = movie->window + (size_t) (s->offset - movie->window_off);
    key = s->key;
    need = (size_t) s->size + movie->sps_len + movie->pps_len + 64 + (size_t) s->size / 4;
    if (movie->au_cap < need) {
        uint8_t *grown = realloc(movie->au, need);
        if (grown == NULL) {
            set_err(err, err_len, "out of memory");
            return false;
        }
        movie->au = grown;
        movie->au_cap = need;
    }
    if (key) {
        put_start(movie->au, &o, movie->sps, movie->sps_len);
        put_start(movie->au, &o, movie->pps, movie->pps_len);
    }
    i = 0;
    while (i + (size_t) movie->nal_len <= s->size) {
        uint32_t nl = 0;
        int k;
        for (k = 0; k < movie->nal_len; k++) {
            nl = (nl << 8) | src[i + (size_t) k];
        }
        i += (size_t) movie->nal_len;
        if (nl == 0 || i + nl > s->size) {
            set_err(err, err_len, "bad nal length");
            return false;
        }
        if (o + 4 + nl > movie->au_cap) {
            set_err(err, err_len, "au overflow");
            return false;
        }
        movie->au[o++] = 0;
        movie->au[o++] = 0;
        movie->au[o++] = 0;
        movie->au[o++] = 1;
        memcpy(movie->au + o, src + i, nl);
        o += nl;
        i += nl;
    }
    if (i != s->size) {
        set_err(err, err_len, "nal trailer");
        return false;
    }
    movie->au_len = o;
    out->data = movie->au;
    out->size = o;
    out->pts_ns = s->pts_ns;
    out->keyframe = key;
    return true;
}
