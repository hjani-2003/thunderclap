/* Manifest parsing/layout, journal and compression. */
#include "manifest.h"
#include "journal.h"
#include "compress.h"
#include "check.h"

#include <sodium.h>

#include <sys/stat.h>

static void add(manifest_t *m, uint8_t type, const char *path, uint64_t size)
{
    m->ents = xrealloc(m->ents, (m->n_ents + 1) * sizeof *m->ents);
    entry_t *e = &m->ents[m->n_ents++];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->path = xstrdup(path);
    e->size = size;
    e->mode = 0644;
    if (type == ENT_LINK)
        e->target = xstrdup("elsewhere");
}

static int roundtrip(manifest_t *src, manifest_t *dst)
{
    buf_t b = { 0 };
    manifest_serialize(src, &b);
    char err[128];
    int rc = manifest_parse(dst, b.data, b.len, err, sizeof err);
    buf_free(&b);
    return rc;
}

static void test_manifest_safety(void)
{
    const char *bad[] = { "/etc/passwd", "../x", "a/../../x", "a//b", "a/./b", "a/", "" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        manifest_t m = { 0 }, out = { 0 };
        add(&m, ENT_FILE, bad[i], 1);
        CHECK(roundtrip(&m, &out) < 0);
        manifest_free(&m);
        manifest_free(&out);
    }
    /* nothing may be written through a symlink from the same manifest */
    manifest_t m = { 0 }, out = { 0 };
    add(&m, ENT_DIR, "top", 0);
    add(&m, ENT_LINK, "top/l", 0);
    add(&m, ENT_FILE, "top/l/evil", 5);
    CHECK(roundtrip(&m, &out) < 0);
    manifest_free(&m);
    manifest_free(&out);

    /* a symlink next to normal files is fine */
    add(&m, ENT_DIR, "top", 0);
    add(&m, ENT_LINK, "top/l", 0);
    add(&m, ENT_FILE, "top/file", 5);
    CHECK(roundtrip(&m, &out) == 0);
    CHECK(out.n_ents == 3 && out.total_bytes == 5 && !strcmp(out.ents[1].target, "elsewhere"));
    manifest_free(&m);
    manifest_free(&out);
}

static void test_layout(void)
{
    manifest_t m = { 0 };
    uint32_t cs = 1 << 20;   /* 1 MiB chunks for the test */
    add(&m, ENT_DIR, "t", 0);
    add(&m, ENT_FILE, "t/big", 3 * cs + 10);   /* 4 chunks */
    for (int i = 0; i < 100; i++) {
        char p[32];
        snprintf(p, sizeof p, "t/s%d", i);
        add(&m, ENT_FILE, p, 30000);           /* 100 × 30 KB ⇒ bundles */
    }
    add(&m, ENT_FILE, "t/empty", 0);
    manifest_layout(&m, cs);

    uint32_t file_chunks = 0, bundles = 0, bundled = 0;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < m.n_chunks; i++) {
        const chunk_t *c = &m.chunks[i];
        CHECK(c->len <= cs);
        bytes += c->len;
        if (c->kind == CHUNK_FILE)
            file_chunks++;
        else {
            bundles++;
            bundled += c->count;
        }
    }
    CHECK(file_chunks == 4);
    CHECK(bundled == 100);
    CHECK(bundles == 3);   /* 34 files fit in 1 MiB */
    CHECK(bytes == 3ull * cs + 10 + 100 * 30000);
    CHECK(m.ents[1].n_chunks == 4 && m.ents[2].n_chunks == 1 && m.ents[102].n_chunks == 0);
    manifest_free(&m);
}

static void test_journal(void)
{
    char dir[] = "/tmp/thunderclap-test-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[128];
    snprintf(path, sizeof path, "%s/j", dir);

    journal_t j;
    buf_t meta = { 0 };
    buf_put(&meta, "hello", 5);
    bool resumed;
    CHECK(journal_open(&j, path, 100, &meta, &resumed) == 0 && !resumed);
    CHECK(journal_set(&j, 3) && !journal_set(&j, 3));
    journal_set(&j, 99);
    CHECK(journal_done(&j) == 2);
    CHECK(journal_flush(&j, -1) == 0);
    journal_close(&j, false);

    buf_t meta2 = { 0 };
    buf_put(&meta2, "other", 5);
    CHECK(journal_open(&j, path, 100, &meta2, &resumed) == 0 && resumed);
    CHECK(meta2.len == 5 && !memcmp(meta2.data, "hello", 5));   /* stored meta wins */
    CHECK(journal_test(&j, 3) && journal_test(&j, 99) && !journal_test(&j, 4));
    journal_clear(&j, 3);
    CHECK(!journal_test(&j, 3) && journal_done(&j) == 1);
    journal_close(&j, false);

    /* different chunk count ⇒ start over */
    CHECK(journal_open(&j, path, 50, &meta, &resumed) == 0 && !resumed && journal_done(&j) == 0);
    journal_close(&j, true);
    struct stat st;
    CHECK(stat(path, &st) < 0);
    rmdir(dir);
    buf_free(&meta);
    buf_free(&meta2);
}

static void test_compress(void)
{
    size_t n = 1 << 20;
    uint8_t *text = malloc(n), *rnd = malloc(n), *out = malloc(compress_bound(n)), *back = malloc(n);
    for (size_t i = 0; i < n; i++)
        text[i] = "the quick brown fox jumps over the lazy dog\n"[i % 44];
    randombytes_buf(rnd, n);

    compressor_t *c = compressor_new(COMPRESS_AUTO);
    size_t z = compressor_run(c, text, n, out, compress_bound(n));
    CHECK(z > 0 && z < n / 10);
    void *d = decompressor_new();
    CHECK(decompress_chunk(d, out, z, back, n) == 0 && !memcmp(back, text, n));
    CHECK(decompress_chunk(d, out, z, back, n - 1) < 0);   /* wrong expected size */
    CHECK(compressor_run(c, rnd, n, out, compress_bound(n)) == 0);   /* random: send raw */
    compressor_free(c);

    c = compressor_new(COMPRESS_OFF);
    CHECK(compressor_run(c, text, n, out, compress_bound(n)) == 0);
    compressor_free(c);
    decompressor_free(d);
    free(text);
    free(rnd);
    free(out);
    free(back);
}

int main(void)
{
    if (sodium_init() < 0)
        return 1;
    g_log_level = LOG_ERR;
    test_manifest_safety();
    test_layout();
    test_journal();
    test_compress();
    DONE();
}
