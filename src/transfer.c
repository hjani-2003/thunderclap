#include "transfer.h"
#include "config.h"

#include <stdarg.h>

#define KEEP_FINISHED 30
#define HISTORY_MAX   200

pthread_mutex_t g_xfer_lock = PTHREAD_MUTEX_INITIALIZER;
static transfer_t *g_list;
static int g_next_id = 1;
static cJSON *g_history;

static const char *state_names[] = {
    "scanning", "connecting", "waiting", "active", "finishing", "done", "failed", "cancelled", "rejected",
};

const char *tstate_name(tstate_t s) { return state_names[s]; }

bool transfer_finished(const transfer_t *t) { return t->state >= TS_DONE; }

void transfers_init(void)
{
    char path[1200];
    config_path("history.json", path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (f) {
        char *s = NULL;
        size_t n = 0;
        FILE *mem = open_memstream(&s, &n);
        char tmp[8192];
        size_t r;
        while ((r = fread(tmp, 1, sizeof tmp, f)) > 0)
            fwrite(tmp, 1, r, mem);
        fclose(mem);
        fclose(f);
        g_history = cJSON_Parse(s);
        free(s);
    }
    if (!cJSON_IsArray(g_history)) {
        cJSON_Delete(g_history);
        g_history = cJSON_CreateArray();
    }
}

static cJSON *json_locked(transfer_t *t, bool with_hist);

static void history_add(transfer_t *t)
{
    cJSON *j = json_locked(t, false);
    cJSON_InsertItemInArray(g_history, 0, j);
    while (cJSON_GetArraySize(g_history) > HISTORY_MAX)
        cJSON_DeleteItemFromArray(g_history, HISTORY_MAX);
    char *s = cJSON_PrintUnformatted(g_history);
    char path[1200], tmp[1300];
    config_path("history.json", path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        fputs(s, f);
        fclose(f);
        rename(tmp, path);
    }
    free(s);
}

transfer_t *transfer_new(bool outgoing)
{
    transfer_t *t = xcalloc(1, sizeof *t);
    t->outgoing = outgoing;
    t->state = outgoing ? TS_SCANNING : TS_WAITING;
    t->started_ms = wall_ms();
    t->t_start = t->t_last = now_sec();
    t->refs = 1;   /* held by the engine thread */
    pthread_cond_init(&t->cond, NULL);

    pthread_mutex_lock(&g_xfer_lock);
    t->id = g_next_id++;
    t->next = g_list;
    g_list = t;
    /* Drop the oldest finished transfers nobody references any more. */
    int finished = 0;
    for (transfer_t **pp = &g_list; *pp;) {
        transfer_t *x = *pp;
        if (transfer_finished(x) && ++finished > KEEP_FINISHED && x->refs == 0) {
            *pp = x->next;
            pthread_cond_destroy(&x->cond);
            free(x);
        } else {
            pp = &x->next;
        }
    }
    pthread_mutex_unlock(&g_xfer_lock);
    return t;
}

transfer_t *transfer_get(int id)
{
    pthread_mutex_lock(&g_xfer_lock);
    transfer_t *t = g_list;
    while (t && t->id != id)
        t = t->next;
    if (t)
        t->refs++;
    pthread_mutex_unlock(&g_xfer_lock);
    return t;
}

void transfer_put(transfer_t *t)
{
    pthread_mutex_lock(&g_xfer_lock);
    t->refs--;
    pthread_mutex_unlock(&g_xfer_lock);
}

static void set_state_locked(transfer_t *t, tstate_t s)
{
    if (transfer_finished(t))
        return;
    if (s == TS_ACTIVE && t->state != TS_ACTIVE) {
        t->t_start = t->t_last = now_sec();
        t->last_bytes = atomic_load(&t->bytes_done);
    }
    t->state = s;
    if (transfer_finished(t)) {
        t->finished_ms = wall_ms();
        double secs = now_sec() - t->t_start;
        uint64_t moved = atomic_load(&t->bytes_done) - t->skipped_bytes;
        if (secs > 0 && s == TS_DONE)
            t->rate = moved / secs;   /* report the average once finished */
        history_add(t);
    }
    pthread_cond_broadcast(&t->cond);
}

void transfer_set_state(transfer_t *t, tstate_t s)
{
    pthread_mutex_lock(&g_xfer_lock);
    set_state_locked(t, s);
    pthread_mutex_unlock(&g_xfer_lock);
}

void transfer_fail(transfer_t *t, tstate_t s, const char *fmt, ...)
{
    pthread_mutex_lock(&g_xfer_lock);
    if (!transfer_finished(t)) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(t->error, sizeof t->error, fmt, ap);
        va_end(ap);
        LOGW("transfer %d %s: %s", t->id, state_names[s], t->error);
        set_state_locked(t, s);
    }
    pthread_mutex_unlock(&g_xfer_lock);
}

void transfers_tick(void)
{
    double now = now_sec();
    pthread_mutex_lock(&g_xfer_lock);
    for (transfer_t *t = g_list; t; t = t->next) {
        if (t->state != TS_ACTIVE && t->state != TS_FINISHING)
            continue;
        double dt = now - t->t_last;
        if (dt < 0.2)
            continue;
        uint64_t b = atomic_load(&t->bytes_done);
        double inst = (double)(b - t->last_bytes) / dt;
        t->rate = t->n_hist ? 0.6 * t->rate + 0.4 * inst : inst;
        t->last_bytes = b;
        t->t_last = now;
        if (t->n_hist == RATE_HIST) {
            memmove(t->hist, t->hist + 1, (RATE_HIST - 1) * sizeof t->hist[0]);
            t->n_hist--;
        }
        t->hist[t->n_hist++] = (float)inst;
    }
    pthread_mutex_unlock(&g_xfer_lock);
}

/* Caller holds g_xfer_lock. */
static cJSON *json_locked(transfer_t *t, bool with_hist)
{
    cJSON *j = cJSON_CreateObject();
    uint64_t done = atomic_load(&t->bytes_done), wire = atomic_load(&t->wire_bytes);
    cJSON_AddNumberToObject(j, "id", t->id);
    cJSON_AddStringToObject(j, "direction", t->outgoing ? "out" : "in");
    cJSON_AddBoolToObject(j, "bench", t->bench);
    cJSON_AddStringToObject(j, "peer", t->peer_name);
    cJSON_AddStringToObject(j, "peer_fp", t->peer_fp);
    cJSON_AddStringToObject(j, "title", t->title);
    cJSON_AddStringToObject(j, "dest", t->dest);
    cJSON_AddStringToObject(j, "state", state_names[t->state]);
    cJSON_AddStringToObject(j, "error", t->error);
    cJSON_AddNumberToObject(j, "total", (double)t->total_bytes);
    cJSON_AddNumberToObject(j, "done", (double)done);
    cJSON_AddNumberToObject(j, "skipped", (double)t->skipped_bytes);
    cJSON_AddNumberToObject(j, "wire", (double)wire);
    cJSON_AddNumberToObject(j, "files", t->n_files);
    cJSON_AddNumberToObject(j, "chunks", t->n_chunks);
    cJSON_AddNumberToObject(j, "streams", t->streams);
    cJSON_AddBoolToObject(j, "resumed", t->resumed);
    cJSON_AddNumberToObject(j, "rate", t->rate);
    cJSON_AddNumberToObject(j, "started", (double)t->started_ms);
    cJSON_AddNumberToObject(j, "finished", (double)t->finished_ms);
    double elapsed = (t->finished_ms ? t->finished_ms : wall_ms()) - t->started_ms;
    cJSON_AddNumberToObject(j, "elapsed", elapsed / 1000.0);
    /* Compression ratio over the bytes moved in this session. */
    uint64_t moved = done > t->skipped_bytes ? done - t->skipped_bytes : 0;
    cJSON_AddNumberToObject(j, "ratio", moved && wire ? (double)wire / moved : 1.0);
    double eta = -1;
    if (t->state == TS_ACTIVE && t->rate > 1 && t->total_bytes > done)
        eta = (t->total_bytes - done) / t->rate;
    if (t->bench && t->state == TS_ACTIVE)
        eta = t->bench_secs - (now_sec() - t->t_start);
    cJSON_AddNumberToObject(j, "eta", eta);
    if (with_hist) {
        cJSON *h = cJSON_AddArrayToObject(j, "hist");
        for (int i = 0; i < t->n_hist; i++)
            cJSON_AddItemToArray(h, cJSON_CreateNumber(t->hist[i]));
    }
    return j;
}

cJSON *transfer_json(transfer_t *t, bool with_hist)
{
    pthread_mutex_lock(&g_xfer_lock);
    cJSON *j = json_locked(t, with_hist);
    pthread_mutex_unlock(&g_xfer_lock);
    return j;
}

cJSON *transfers_json(bool with_hist)
{
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&g_xfer_lock);
    for (transfer_t *t = g_list; t; t = t->next)
        cJSON_AddItemToArray(arr, json_locked(t, with_hist && !transfer_finished(t)));
    pthread_mutex_unlock(&g_xfer_lock);
    return arr;
}

cJSON *history_json(void)
{
    pthread_mutex_lock(&g_xfer_lock);
    cJSON *c = cJSON_Duplicate(g_history, true);
    pthread_mutex_unlock(&g_xfer_lock);
    return c;
}
