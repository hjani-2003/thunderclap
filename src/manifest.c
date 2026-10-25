#include "manifest.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

static entry_t *add_entry(manifest_t *m)
{
    if (m->n_ents == m->cap_ents) {
        m->cap_ents = m->cap_ents ? m->cap_ents * 2 : 64;
        m->ents = xrealloc(m->ents, m->cap_ents * sizeof *m->ents);
    }
    entry_t *e = &m->ents[m->n_ents++];
    memset(e, 0, sizeof *e);
    return e;
}

static int add_one(manifest_t *m, const char *src, const char *rel, const struct stat *st)
{
    entry_t *e;
    if (S_ISREG(st->st_mode)) {
        e = add_entry(m);
        e->type = ENT_FILE;
        e->size = (uint64_t)st->st_size;
        m->total_bytes += e->size;
        m->n_files++;
    } else if (S_ISDIR(st->st_mode)) {
        e = add_entry(m);
        e->type = ENT_DIR;
    } else if (S_ISLNK(st->st_mode)) {
        char target[4096];
        ssize_t n = readlink(src, target, sizeof target - 1);
        if (n < 0)
            return -1;
        target[n] = 0;
        e = add_entry(m);
        e->type = ENT_LINK;
        e->target = xstrdup(target);
    } else {
        LOGW("skipping special file %s", src);
        return 0;
    }
    e->mode = st->st_mode & 07777;
    e->mtime_ns = (int64_t)st->st_mtim.tv_sec * 1000000000LL + st->st_mtim.tv_nsec;
    e->path = xstrdup(rel);
    e->src = xstrdup(src);
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int walk(manifest_t *m, const char *src, const char *rel)
{
    struct stat st;
    if (lstat(src, &st) < 0)
        return -1;
    if (add_one(m, src, rel, &st) < 0)
        return -1;
    if (!S_ISDIR(st.st_mode))
        return 0;

    DIR *d = opendir(src);
    if (!d)
        return -1;
    /* Sort names so the order (and therefore the transfer id) is stable. */
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = xrealloc(names, cap * sizeof *names);
        }
        names[n++] = xstrdup(de->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp_str);

    int rc = 0;
    for (size_t i = 0; i < n; i++) {
        if (rc == 0) {
            char *s, *r;
            if (asprintf(&s, "%s/%s", src, names[i]) < 0 || asprintf(&r, "%s/%s", rel, names[i]) < 0)
                abort();
            rc = walk(m, s, r);
            free(s);
            free(r);
        }
        free(names[i]);
    }
    free(names);
    return rc;
}

int manifest_add_path(manifest_t *m, const char *path)
{
    char *copy = xstrdup(path);
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/')
        copy[--len] = 0;
    const char *base = strrchr(copy, '/');
    base = base ? base + 1 : copy;
    int rc;
    if (!*base || !strcmp(base, ".") || !strcmp(base, "..")) {
        /* "." or "/" — use the directory's real name. */
        char *real = realpath(copy, NULL);
        const char *rb = real ? strrchr(real, '/') : NULL;
        rc = rb && rb[1] ? walk(m, copy, rb + 1) : (errno = EINVAL, -1);
        free(real);
    } else {
        rc = walk(m, copy, base);
    }
    free(copy);
    return rc;
}

void manifest_serialize(const manifest_t *m, buf_t *out)
{
    buf_u32(out, m->n_ents);
    for (uint32_t i = 0; i < m->n_ents; i++) {
        const entry_t *e = &m->ents[i];
        buf_u8(out, e->type);
        buf_u32(out, e->mode);
        buf_u64(out, (uint64_t)e->mtime_ns);
        buf_u64(out, e->size);
        buf_str(out, e->path);
        if (e->type == ENT_LINK)
            buf_str(out, e->target);
    }
}

static bool path_safe(const char *p)
{
    if (!*p || p[0] == '/')
        return false;
    const char *s = p;
    while (*s) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
            return false;
        if (!e)
            break;
        s = e + 1;
        if (!*s)
            return false;
    }
    return true;
}

int manifest_parse(manifest_t *m, const uint8_t *data, size_t len, char *err, size_t errn)
{
    rd_t r = { data, data + len, false };
    memset(m, 0, sizeof *m);
    uint32_t n = rd_u32(&r);
    if (n > 50000000) {
        snprintf(err, errn, "manifest too large");
        return -1;
    }
    char **links = NULL;
    size_t n_links = 0;
    for (uint32_t i = 0; i < n && !r.err; i++) {
        entry_t *e = add_entry(m);
        e->type = rd_u8(&r);
        e->mode = rd_u32(&r) & 07777;
        e->mtime_ns = (int64_t)rd_u64(&r);
        e->size = rd_u64(&r);
        e->path = rd_str(&r);
        if (e->type == ENT_LINK)
            e->target = rd_str(&r);
        if (r.err)
            break;
        if (e->type > ENT_LINK || !path_safe(e->path) || strlen(e->path) != strnlen(e->path, 65535)) {
            snprintf(err, errn, "unsafe path in manifest");
            free(links);
            return -1;
        }
        if (e->type == ENT_FILE) {
            m->total_bytes += e->size;
            m->n_files++;
        } else if (e->type == ENT_LINK) {
            links = xrealloc(links, (n_links + 1) * sizeof *links);
            links[n_links++] = e->path;
        }
    }
    if (r.err) {
        snprintf(err, errn, "truncated manifest");
        free(links);
        return -1;
    }
    /* No entry may live underneath a symlink from the same manifest,
     * otherwise a sender could write outside the destination. */
    if (n_links) {
        qsort(links, n_links, sizeof *links, cmp_str);
        char prefix[65536];
        for (uint32_t i = 0; i < m->n_ents; i++) {
            const char *p = m->ents[i].path;
            for (const char *s = strchr(p, '/'); s; s = strchr(s + 1, '/')) {
                /* Copy rather than cut in place: p may itself be in links[]. */
                memcpy(prefix, p, (size_t)(s - p));
                prefix[s - p] = 0;
                char *key = prefix;
                bool hit = bsearch(&key, links, n_links, sizeof *links, cmp_str) != NULL;
                if (hit) {
                    snprintf(err, errn, "path below symlink in manifest");
                    free(links);
                    return -1;
                }
            }
        }
        free(links);
    }
    return 0;
}

void manifest_layout(manifest_t *m, uint32_t chunk_size)
{
    m->chunk_size = chunk_size;
    free(m->chunks);
    free(m->bundle_list);
    uint32_t cap = 64, bcap = 64;
    m->chunks = xmalloc(cap * sizeof *m->chunks);
    m->bundle_list = xmalloc(bcap * sizeof *m->bundle_list);
    m->n_chunks = m->n_bundle_list = 0;

    uint32_t bstart = 0, bcount = 0;
    uint64_t bbytes = 0;

#define PUSH_CHUNK(c) do { \
        if (m->n_chunks == cap) { cap *= 2; m->chunks = xrealloc(m->chunks, cap * sizeof *m->chunks); } \
        m->chunks[m->n_chunks++] = (c); \
    } while (0)
#define FLUSH_BUNDLE() do { \
        if (bcount) { \
            PUSH_CHUNK(((chunk_t){ .kind = CHUNK_BUNDLE, .entry = bstart, .count = bcount, .len = (uint32_t)bbytes })); \
            bcount = 0; bbytes = 0; \
        } \
    } while (0)

    uint32_t small_max = MIN(SMALL_FILE_MAX, chunk_size);
    for (uint32_t i = 0; i < m->n_ents; i++) {
        entry_t *e = &m->ents[i];
        e->n_chunks = 0;
        if (e->type != ENT_FILE || e->size == 0)
            continue;
        if (e->size < small_max) {
            if (bbytes + e->size > chunk_size || bcount >= 8192)
                FLUSH_BUNDLE();
            if (!bcount)
                bstart = m->n_bundle_list;
            if (m->n_bundle_list == bcap) {
                bcap *= 2;
                m->bundle_list = xrealloc(m->bundle_list, bcap * sizeof *m->bundle_list);
            }
            m->bundle_list[m->n_bundle_list++] = i;
            bcount++;
            bbytes += e->size;
            e->n_chunks = 1;
        } else {
            for (uint64_t off = 0; off < e->size; off += chunk_size) {
                uint32_t len = (uint32_t)MIN((uint64_t)chunk_size, e->size - off);
                PUSH_CHUNK(((chunk_t){ .kind = CHUNK_FILE, .entry = i, .offset = off, .len = len }));
                e->n_chunks++;
            }
        }
    }
    FLUSH_BUNDLE();
#undef PUSH_CHUNK
#undef FLUSH_BUNDLE
}

void manifest_free(manifest_t *m)
{
    for (uint32_t i = 0; i < m->n_ents; i++) {
        free(m->ents[i].path);
        free(m->ents[i].target);
        free(m->ents[i].src);
    }
    free(m->ents);
    free(m->chunks);
    free(m->bundle_list);
    memset(m, 0, sizeof *m);
}

void manifest_title(const manifest_t *m, char *out, size_t n)
{
    uint32_t tops = 0;
    const char *first = NULL;
    for (uint32_t i = 0; i < m->n_ents; i++) {
        if (!strchr(m->ents[i].path, '/')) {
            if (!first)
                first = m->ents[i].path;
            tops++;
        }
    }
    if (!first)
        snprintf(out, n, "(empty)");
    else if (tops == 1)
        snprintf(out, n, "%s", first);
    else
        snprintf(out, n, "%s + %u more", first, tops - 1);
}
