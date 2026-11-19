#ifndef LAUNCHER_HISTORY_DATA_H
#define LAUNCHER_HISTORY_DATA_H

/* UTC half-hour buckets. Sensor reads are shared with the existing dashboard;
   this module never opens a sensor or writes to the root filesystem. */
#define HIST_CAP 2048
#define HIST_METRICS 5
#define HIST_MAGIC 0x31545348u
#define HIST_FILE "/mnt/UDISK/launcher/history-v1.bin"

struct hist_stat {
    double sum;
    float low, high;
    uint32_t count;
};
struct hist_record {
    uint32_t magic, bytes;
    int64_t slot;
    struct hist_stat metric[HIST_METRICS];
    uint32_t check;
};
static struct hist_record hist_ring[HIST_CAP];
static int64_t hist_live_slot = -1;
static int hist_runtime, hist_save_state;
static int hist_save_pipe = -1;
static const char *hist_path = HIST_FILE;
static const char *hist_temp_path = "/mnt/UDISK/launcher/history-v1.tmp";
static struct hist_record hist_pending;
static int hist_pending_valid;

static uint32_t hist_hash(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static int hist_valid(const struct hist_record *r)
{
    if (r->magic != HIST_MAGIC || r->bytes != sizeof *r ||
        r->slot < 960000 || r->slot > 2400000 ||
        r->check != hist_hash(r, offsetof(struct hist_record, check))) return 0;
    for (int i = 0; i < HIST_METRICS; i++) {
        const struct hist_stat *s = &r->metric[i];
        if (s->count > 1800 || (s->count &&
            (!isfinite(s->sum) || !isfinite(s->low) || !isfinite(s->high) ||
             s->low > s->high))) return 0;
    }
    return 1;
}

static struct hist_record *hist_get(int64_t slot)
{
    struct hist_record *r = &hist_ring[(uint64_t)slot % HIST_CAP];
    return r->slot == slot && r->magic == HIST_MAGIC ? r : NULL;
}

static void hist_load(void)
{
    hist_runtime = 1;
    FILE *f = fopen(hist_path, "rb");
    if (!f) return;
    struct hist_record r;
    long valid_end = 0;
    int loaded = 0;
    while (fread(&r, sizeof r, 1, f) == 1) {
        if (!hist_valid(&r)) break;
        hist_ring[(uint64_t)r.slot % HIST_CAP] = r;
        valid_end += sizeof r;
        loaded++;
    }
    fclose(f);
    struct stat st;
    if (stat(hist_path, &st) == 0 && st.st_size > valid_end) {
        int fd = open(hist_path, O_WRONLY);
        if (fd >= 0) { (void)ftruncate(fd, valid_end); close(fd); }
        printf("history: discarded truncated tail at %ld\n", valid_end);
    }
    printf("history: restored %d buckets\n", loaded);
}

static int hist_write_all(int fd, const void *buf, size_t size)
{
    const uint8_t *p = buf;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static int hist_compact(int64_t latest)
{
    int fd = open(hist_temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int ok = 0;
    for (int64_t slot = latest - 30 * 48 - 2; slot <= latest; slot++) {
        struct hist_record *r = hist_get(slot);
        if (r) {
            struct hist_record clean = *r;
            clean.check = hist_hash(&clean, offsetof(struct hist_record, check));
            if (hist_write_all(fd, &clean, sizeof clean)) { ok = -1; break; }
        }
    }
    if (!ok && fsync(fd)) ok = -1;
    if (close(fd)) ok = -1;
    if (!ok && rename(hist_temp_path, hist_path)) ok = -1;
    if (ok) unlink(hist_temp_path);
    return ok;
}

static void hist_save(const struct hist_record *src)
{
    if (!hist_runtime) return;
    if (hist_save_pipe >= 0) {
        hist_pending = *src;
        hist_pending_valid = 1;
        return;
    }
    int pipefd[2];
    if (pipe(pipefd)) { hist_save_state = 3; return; }
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); hist_save_state = 3; return; }
    if (pid == 0) {
        close(pipefd[0]);
        struct hist_record r = *src;
        r.check = hist_hash(&r, offsetof(struct hist_record, check));
        int fd = open(hist_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        int ok = fd < 0 ? -1 : hist_write_all(fd, &r, sizeof r);
        if (fd >= 0 && !ok && fsync(fd)) ok = -1;
        if (fd >= 0 && close(fd)) ok = -1;
        struct stat st;
        if (!ok && stat(hist_path, &st) == 0 && st.st_size > 300000)
            ok = hist_compact(r.slot);
        char result = ok ? 'E' : 'S';
        (void)write(pipefd[1], &result, 1);
        close(pipefd[1]);
        _exit(ok ? 1 : 0);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    hist_save_pipe = pipefd[0];
    hist_save_state = 1;
}

static void hist_poll(void)
{
    if (hist_save_pipe < 0) return;
    char result;
    ssize_t n = read(hist_save_pipe, &result, 1);
    if (n == 1 || n == 0) {
        hist_save_state = n == 1 && result == 'S' ? 2 : 3;
        if (hist_save_state == 3) printf("history: save failed\n");
        close(hist_save_pipe);
        hist_save_pipe = -1;
        if (hist_pending_valid) {
            struct hist_record pending = hist_pending;
            hist_pending_valid = 0;
            hist_save(&pending);
        }
    }
}

static void hist_tick(void)
{
    if (!hist_runtime) return;
    time_t now = time(NULL);
    if (now < 1735689600 || now > 4102444800LL) return;
    int64_t slot = (int64_t)now / 1800;
    if (hist_live_slot != slot) {
        if (hist_live_slot >= 0) {
            struct hist_record *old = hist_get(hist_live_slot);
            int nonempty = 0;
            if (old) for (int i = 0; i < HIST_METRICS; i++)
                nonempty |= old->metric[i].count > 0;
            /* A large clock correction invalidates the old wall-clock slot. */
            if (nonempty && slot > hist_live_slot && slot - hist_live_slot <= 2)
                hist_save(old);
        }
        hist_live_slot = slot;
        struct hist_record *fresh = &hist_ring[(uint64_t)slot % HIST_CAP];
        memset(fresh, 0, sizeof *fresh);
        fresh->magic = HIST_MAGIC;
        fresh->bytes = sizeof *fresh;
        fresh->slot = slot;
    }
}

static void hist_sample(int metric, double value)
{
    if (!hist_runtime || metric < 0 || metric >= HIST_METRICS ||
        !isfinite(value)) return;
    hist_tick();
    if (hist_live_slot < 0) return;
    struct hist_stat *s = &hist_ring[(uint64_t)hist_live_slot % HIST_CAP].metric[metric];
    if (s->count == 0) s->low = s->high = (float)value;
    if (value < s->low) s->low = (float)value;
    if (value > s->high) s->high = (float)value;
    s->sum += value;
    s->count++;
}

static void hist_demo_fill(void)
{
    int64_t latest = (int64_t)time(NULL) / 1800;
    for (int i = 0; i < 30 * 48; i++) {
        int64_t slot = latest - (30 * 48 - 1) + i;
        if ((i % 97) == 0) continue;
        struct hist_record *r = &hist_ring[(uint64_t)slot % HIST_CAP];
        memset(r, 0, sizeof *r);
        r->magic = HIST_MAGIC; r->bytes = sizeof *r; r->slot = slot;
        double phase = (double)i * .16;
        double values[HIST_METRICS] = {
            18 + 6 * sin(phase) + 3 * sin(phase * .17),
            25 + 2 * sin(phase * .53),
            61 + 9 * sin(phase * .31),
            150 + 80 * sin(phase * .25) + 20 * sin(phase),
            180 + 130 * sin(phase * .47)
        };
        for (int m = 0; m < HIST_METRICS; m++) {
            struct hist_stat *s = &r->metric[m];
            s->count = 5;
            s->sum = values[m] * 5;
            s->low = (float)(values[m] - (m == 4 ? 25 : m == 3 ? 20 : 1.5));
            s->high = (float)(values[m] + (m == 4 ? 25 : m == 3 ? 20 : 1.5));
            if (s->low < 0) s->low = 0;
        }
    }
    if (getenv("HISTORY_PREVIEW_EXTREME")) {
        struct hist_record *r = hist_get(latest - 2);
        if (r) {
            r->metric[0].sum = 998.5 * 5;
            r->metric[0].low = 998;
            r->metric[0].high = 999;
            r->metric[1].sum = -35.0 * 5;
            r->metric[1].low = -36;
            r->metric[1].high = -34;
            r->metric[3].sum = 60000.0 * 5;
            r->metric[3].low = 200;
            r->metric[3].high = 60000;
            r->metric[4].sum = 199999.0 * 5;
            r->metric[4].low = 100;
            r->metric[4].high = 199999;
        }
    }
}

#endif
