#ifndef FILE_APP_H
#define FILE_APP_H

#include <dirent.h>
#include <strings.h>

/* One directory at a time. Never recurse, follow links, or open file data. */
#define FILE_MAX_ENTRIES 512
#define FILE_VISIBLE_ROWS 5
struct file_entry {
    char name[256];
    mode_t mode;
    off_t size;
    time_t mtime;
};
static struct file_entry file_entries[FILE_MAX_ENTRIES];
static char file_path[384] = "/";
static int file_count, file_scroll, file_overflow, file_errno;
static int file_info_open, file_info_index;

static int file_sort(const void *aa, const void *bb)
{
    const struct file_entry *a = aa, *b = bb;
    int ad = S_ISDIR(a->mode), bd = S_ISDIR(b->mode);
    if (ad != bd) return bd - ad;
    return strcasecmp(a->name, b->name);
}

static void file_scan(void)
{
    file_count = file_scroll = file_overflow = file_errno = 0;
    file_info_open = 0;
    DIR *dir = opendir(file_path);
    if (!dir) { file_errno = errno; dirty = 1; return; }
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (strlen(de->d_name) >= sizeof file_entries[0].name) continue;
        if (file_count >= FILE_MAX_ENTRIES) { file_overflow = 1; break; }
        struct file_entry *e = &file_entries[file_count];
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        char full[sizeof file_path + sizeof e->name + 2];
        snprintf(full, sizeof full, "%s%s%s", file_path,
                 !strcmp(file_path, "/") ? "" : "/", e->name);
        struct stat st;
        if (lstat(full, &st) == 0) {
            e->mode = st.st_mode; e->size = st.st_size; e->mtime = st.st_mtime;
        } else { e->mode = 0; e->size = 0; e->mtime = 0; }
        file_count++;
    }
    closedir(dir);
    qsort(file_entries, file_count, sizeof file_entries[0], file_sort);
    dirty = 1;
}

static void file_reset(void)
{
    snprintf(file_path, sizeof file_path, "/");
    file_scan();
}

static void file_back(void)
{
    if (file_info_open) { file_info_open = 0; dirty = 1; return; }
    if (!strcmp(file_path, "/")) { app_open = -1; dirty = 1; return; }
    char *last = strrchr(file_path, '/');
    if (!last || last == file_path) snprintf(file_path, sizeof file_path, "/");
    else *last = 0;
    file_scan();
}

static const char *file_kind(mode_t mode)
{
    if (S_ISDIR(mode)) return "Folder";
    if (S_ISREG(mode)) return "File";
    if (S_ISLNK(mode)) return "Link";
    return "System";
}

static void file_size(char *dst, size_t n, off_t bytes)
{
    if (bytes < 0) { snprintf(dst, n, "--"); return; }
    if (bytes < 1024) snprintf(dst, n, "%lld B", (long long)bytes);
    else if (bytes < 1048576) snprintf(dst, n, "%.1f KiB", (double)bytes / 1024);
    else if (bytes < 1073741824) snprintf(dst, n, "%.1f MiB", (double)bytes / 1048576);
    else snprintf(dst, n, "%.1f GiB", (double)bytes / 1073741824);
}

static void file_elide(char *dst, size_t n, const char *src, int maxw, int face)
{
    size_t len = strlen(src), cut = len;
    if (len >= n) cut = n - 1;
    for (;;) {
        memcpy(dst, src, cut);
        dst[cut] = 0;
        if (cut < len && cut + 3 < n) strcat(dst, "...");
        if (textw(dst, face) <= maxw || cut == 0) return;
        do { cut--; } while (cut && ((unsigned char)src[cut] & 0xC0) == 0x80);
    }
}

static void file_draw_info(void)
{
    if (!file_info_open || file_info_index < 0 || file_info_index >= file_count) return;
    const struct file_entry *e = &file_entries[file_info_index];
    fill(0, STATUS_H, LW, LH, TH->scrim, 160);
    int x = 132, y = 104, w = 536, h = 300;
    rrect(x, y, x + w, y + h, 24, TH->card, 255, -1);
    char name[256], path[384], full[sizeof file_path + 258];
    char size[32], stamp[32] = "--";
    file_elide(name, sizeof name, e->name, w - 52, FACE(FS20));
    snprintf(full, sizeof full, "%s%s%s", file_path,
             !strcmp(file_path, "/") ? "" : "/", e->name);
    file_elide(path, sizeof path, full, w - 52, FACE(FS16));
    file_size(size, sizeof size, e->size);
    if (e->mtime > 0) {
        struct tm tm;
        localtime_r(&e->mtime, &tm);
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", &tm);
    }
    text(x + 26, vbase(y + 38, name, FACE(FS20)), name,
         FACE(FS20), TH->on, 255);
    text(x + 26, vbase(y + 82, path, FACE(FS16)), path,
         FACE(FS16), TH->on3, 255);
    const char *labels[3] = {"Type", "Size", "Modified"};
    const char *vals[3] = {file_kind(e->mode), S_ISREG(e->mode) ? size : "--", stamp};
    for (int i = 0; i < 3; i++) {
        int yy = y + 134 + i * 42;
        text(x + 26, vbase(yy, labels[i], FACE(FS16)), labels[i],
             FACE(FS16), TH->on3, 255);
        text(x + 230, vbase(yy, vals[i], FACE(FS16)), vals[i],
             FACE(FS16), TH->on, 255);
    }
    textcenter(LW / 2, vbase(y + h - 23, "Tap to close", FACE(FS16)),
               "Tap to close", FACE(FS16), TH->on3, 255);
}

static void file_draw(void)
{
    const int y0 = 152, row_h = 50, row_step = 54;
    char shown[384], count[48];
    file_elide(shown, sizeof shown, file_path, 570, FACE(FS18));
    text(MARGIN, vbase(118, shown, FACE(FS18)), shown,
         FACE(FS18), TH->on2, 255);
    rrect(674, 100, 776, 139, 19, TH->chip, 255, -1);
    textcenter(725, vbase(119, "Refresh", FACE(FS16)), "Refresh",
               FACE(FS16), TH->chip_ink, 255);
    if (file_errno) {
        textcenter(LW / 2, vbase(265, "Cannot open folder", FACE(FS20)),
                   "Cannot open folder", FACE(FS20), TH->on2, 255);
    } else if (!file_count) {
        textcenter(LW / 2, vbase(265, "Empty folder", FACE(FS20)),
                   "Empty folder", FACE(FS20), TH->on2, 255);
    }
    for (int i = 0; i < FILE_VISIBLE_ROWS && i + file_scroll < file_count; i++) {
        int idx = i + file_scroll, y = y0 + i * row_step;
        const struct file_entry *e = &file_entries[idx];
        rrect(MARGIN, y, LW - MARGIN, y + row_h, 15,
              TH->card, TH->card_alpha, -1);
        uint32_t ic = S_ISDIR(e->mode) ? TH->accent : TH->on3;
        rrect(MARGIN + 14, y + 15, MARGIN + 33, y + 34, 4, ic,
              S_ISDIR(e->mode) ? 190 : 100, -1);
        char name[256], meta[48];
        file_elide(name, sizeof name, e->name, 475, FACE(FS16));
        text(MARGIN + 46, vbase(y + row_h / 2, name, FACE(FS16)),
             name, FACE(FS16), TH->on, 255);
        if (S_ISREG(e->mode)) file_size(meta, sizeof meta, e->size);
        else snprintf(meta, sizeof meta, "%s", file_kind(e->mode));
        text(LW - MARGIN - 18 - textw(meta, FACE(FS16)),
             vbase(y + row_h / 2, meta, FACE(FS16)),
             meta, FACE(FS16), TH->on3, 255);
    }
    if (file_count > FILE_VISIBLE_ROWS) {
        int track = FILE_VISIBLE_ROWS * row_step - 4;
        int thumb = track * FILE_VISIBLE_ROWS / file_count;
        if (thumb < 28) thumb = 28;
        int max = file_count - FILE_VISIBLE_ROWS;
        int top = y0 + (track - thumb) * file_scroll / max;
        rrect(786, y0, 790, y0 + track, 2, TH->on3, 40, -1);
        rrect(785, top, 791, top + thumb, 3, TH->accent, 190, -1);
    }
    snprintf(count, sizeof count, "%d-%d / %d%s", file_count ? file_scroll + 1 : 0,
             file_count < file_scroll + FILE_VISIBLE_ROWS ? file_count : file_scroll + FILE_VISIBLE_ROWS,
             file_count, file_overflow ? "+" : "");
    text(MARGIN, vbase(450, count, FACE(FS16)), count,
         FACE(FS16), TH->on3, 255);
    file_draw_info();
}

static int file_hit(int x, int y)
{
    if (file_info_open) { file_info_open = 0; dirty = 1; return 1; }
    if (x >= 674 && x < 776 && y >= 100 && y < 139) {
        file_scan(); return 1;
    }
    if (x < MARGIN || x > LW - MARGIN || y < 152 || y >= 422) return 0;
    int row = (y - 152) / 54;
    if (row < 0 || row >= FILE_VISIBLE_ROWS || (y - 152) % 54 >= 50) return 0;
    int idx = row + file_scroll;
    if (idx >= file_count) return 0;
    const struct file_entry *e = &file_entries[idx];
    if (S_ISDIR(e->mode)) {
        char next[sizeof file_path];
        int n = snprintf(next, sizeof next, "%s%s%s", file_path,
                         !strcmp(file_path, "/") ? "" : "/", e->name);
        if (n > 0 && n < (int)sizeof next) {
            snprintf(file_path, sizeof file_path, "%s", next);
            file_scan();
        } else {
            file_info_open = 1; file_info_index = idx; dirty = 1;
        }
    } else { file_info_open = 1; file_info_index = idx; dirty = 1; }
    return 1;
}

static void file_swipe(int dy)
{
    int max = file_count - FILE_VISIBLE_ROWS;
    if (max < 0) max = 0;
    file_scroll += dy < 0 ? 4 : -4;
    if (file_scroll < 0) file_scroll = 0;
    if (file_scroll > max) file_scroll = max;
    dirty = 1;
}

#endif
