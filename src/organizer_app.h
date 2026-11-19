#ifndef ORGANIZER_APP_H
#define ORGANIZER_APP_H

/* Small, bounded personal data store. Writes happen only after an edit. */
#define ORG_FILE BASELINE_DIR "/organizer.txt"
#define ORG_NOTES_MAX 24
#define ORG_EVENTS_MAX 96
#define ORG_TEXT_MAX 72
struct org_note { char text[ORG_TEXT_MAX + 1]; };
struct org_event { int date; char text[ORG_TEXT_MAX + 1]; };
static struct org_note org_notes[ORG_NOTES_MAX];
static struct org_event org_events[ORG_EVENTS_MAX];
static int org_note_count, org_event_count, org_note_selected = -1, org_note_scroll;
static int org_year, org_month, org_day, org_edit_kind, org_edit_index;
static unsigned long org_revision;
static int org_save_state; /* 0 idle, 1 saving, 2 saved, 3 failed */
static long org_save_started;

static int org_date_key(int year, int month, int day)
{
    return year * 10000 + month * 100 + day;
}

static int org_days(int year, int month)
{
    static const int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1 || month > 12) return 0;
    return days[month] + (month == 2 && year % 4 == 0 &&
           (year % 100 != 0 || year % 400 == 0));
}

static int org_event_for(int key)
{
    for (int i = 0; i < org_event_count; i++)
        if (org_events[i].date == key) return i;
    return -1;
}

static int org_safe_text(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > ORG_TEXT_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) return 0;
    return 1;
}

static void org_load(void)
{
    if (!config_mount_ready()) return;
    FILE *f = fopen(ORG_FILE, "r");
    if (!f) return;
    char line[160];
    int version = 0;
    if (fgets(line, sizeof line, f) && sscanf(line, "organizer=%d revision=%lu",
            &version, &org_revision) == 2 && version == 1) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == 'N' && line[1] == '\t' &&
                org_note_count < ORG_NOTES_MAX && org_safe_text(line + 2)) {
                snprintf(org_notes[org_note_count++].text, ORG_TEXT_MAX + 1,
                         "%s", line + 2);
            } else if (line[0] == 'E' && line[1] == '\t' &&
                       org_event_count < ORG_EVENTS_MAX) {
                char *sep = strchr(line + 2, '\t');
                if (!sep) continue;
                *sep++ = 0;
                int date = atoi(line + 2);
                int year = date / 10000, month = (date / 100) % 100, day = date % 100;
                if (year < 2020 || year > 2099 || day < 1 ||
                    day > org_days(year, month) || !org_safe_text(sep) ||
                    org_event_for(date) >= 0) continue;
                org_events[org_event_count].date = date;
                snprintf(org_events[org_event_count++].text, ORG_TEXT_MAX + 1,
                         "%s", sep);
            }
        }
    }
    fclose(f);
}

static void org_result(unsigned long rev, int ok)
{
    char path[80];
    snprintf(path, sizeof path, "/tmp/organizer_result.%lu", rev);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) { (void)write(fd, ok ? "ok\n" : "fail\n", ok ? 3 : 5); close(fd); }
}

static void org_save(void)
{
    if (!config_mount_ready()) { org_save_state = 3; dirty = 1; return; }
    unsigned long rev = ++org_revision;
    org_save_state = 1;
    org_save_started = now_ms();
    dirty = 1;
    pid_t pid = fork();
    if (pid < 0) { org_save_state = 3; return; }
    if (pid) return;
    int ok = 0;
    int lockfd = open(BASELINE_DIR "/organizer.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lockfd < 0 || flock(lockfd, LOCK_EX)) goto done;
    {
        FILE *old = fopen(ORG_FILE, "r");
        if (old) {
            char header[100]; unsigned long prior = 0; int version = 0;
            if (fgets(header, sizeof header, old) &&
                sscanf(header, "organizer=%d revision=%lu", &version, &prior) == 2 &&
                version == 1 && prior > rev) { fclose(old); ok = 1; goto done; }
            fclose(old);
        }
    }
    char temp[128];
    snprintf(temp, sizeof temp, BASELINE_DIR "/organizer.%ld.tmp", (long)getpid());
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) goto done;
    if (dprintf(fd, "organizer=1 revision=%lu\n", rev) < 0) goto close_fail;
    for (int i = 0; i < org_note_count; i++)
        if (dprintf(fd, "N\t%s\n", org_notes[i].text) < 0) goto close_fail;
    for (int i = 0; i < org_event_count; i++)
        if (dprintf(fd, "E\t%d\t%s\n", org_events[i].date,
                    org_events[i].text) < 0) goto close_fail;
    int synced = fsync(fd) == 0;
    if (close(fd)) synced = 0;
    if (!synced) { unlink(temp); goto done; }
    if (rename(temp, ORG_FILE)) { unlink(temp); goto done; }
    {
        int dir = open(BASELINE_DIR, O_RDONLY | O_DIRECTORY);
        if (dir >= 0) { fsync(dir); close(dir); }
    }
    ok = 1;
    goto done;
close_fail:
    close(fd); unlink(temp);
done:
    org_result(rev, ok);
    _exit(ok ? 0 : 1);
}

static void org_poll(void)
{
    if (org_save_state != 1) return;
    char path[80], result[8] = {0};
    snprintf(path, sizeof path, "/tmp/organizer_result.%lu", org_revision);
    FILE *f = fopen(path, "r");
    if (f) {
        (void)fgets(result, sizeof result, f);
        fclose(f); unlink(path);
        org_save_state = !strcmp(result, "ok\n") ? 2 : 3;
        dirty = 1;
    } else if (now_ms() - org_save_started > 8000) {
        org_save_state = 3; dirty = 1;
    }
}

static void org_open_editor(int kind, int index, const char *current)
{
    org_edit_kind = kind;
    org_edit_index = index;
    kb_target = org_input;
    snprintf(kb_text, sizeof kb_text, "%s", current ? current : "");
    kb_len = (int)strlen(kb_text);
    kb_shift = 0;
    kb_open = 1;
    dirty = 1;
}

static void org_commit(void)
{
    if (!org_safe_text(org_input)) return;
    if (org_edit_kind == 1) {
        if (org_edit_index >= 0 && org_edit_index < org_note_count)
            snprintf(org_notes[org_edit_index].text, ORG_TEXT_MAX + 1, "%s", org_input);
        else if (org_note_count < ORG_NOTES_MAX) {
            org_note_selected = org_note_count;
            snprintf(org_notes[org_note_count++].text, ORG_TEXT_MAX + 1, "%s", org_input);
            if (org_note_selected >= org_note_scroll + 5) org_note_scroll = org_note_selected - 4;
        } else return;
    } else if (org_edit_kind == 2) {
        int key = org_date_key(org_year, org_month, org_day);
        int i = org_event_for(key);
        if (i < 0) {
            if (org_event_count >= ORG_EVENTS_MAX) return;
            i = org_event_count++;
        }
        org_events[i].date = key;
        snprintf(org_events[i].text, ORG_TEXT_MAX + 1, "%s", org_input);
    } else return;
    org_save();
}

static void org_today(void)
{
    time_t now = time(NULL); struct tm t;
    localtime_r(&now, &t);
    org_year = t.tm_year + 1900;
    org_month = t.tm_mon + 1;
    org_day = t.tm_mday;
}

static void org_month_shift(int by)
{
    org_month += by;
    if (org_month < 1) { org_month = 12; org_year--; }
    if (org_month > 12) { org_month = 1; org_year++; }
    if (org_year < 2020) { org_year = 2020; org_month = 1; }
    if (org_year > 2099) { org_year = 2099; org_month = 12; }
    if (org_day > org_days(org_year, org_month)) org_day = org_days(org_year, org_month);
    dirty = 1;
}

static void org_button(int x0, int y0, int x1, int y1, const char *label, int primary)
{
    rrect(x0, y0, x1, y1, 14, primary ? TH->accent : TH->chip, 255, -1);
    textcenter((x0+x1)/2, vbase((y0+y1)/2, label, FACE(FS18)),
               label, FACE(FS18), primary ? TH->accent_ink : TH->chip_ink, 255);
}

static void org_save_label(void)
{
    const char *s = org_save_state == 1 ? "保存中…" : org_save_state == 2 ?
                    "已保存" : org_save_state == 3 ? "保存失败" : "";
    if (*s) text(565, vbase(447, s, FACE(FS16)), s, FACE(FS16),
                 org_save_state == 3 ? 0xE5484D : TH->on3, 255);
}

static void org_draw_calendar(void)
{
    char month[32], date[32];
    snprintf(month, sizeof month, "%04d / %02d", org_year, org_month);
    text(88, vbase(114, month, FACE(FS26)), month, FACE(FS26), TH->on, 255);
    org_button(355, 86, 405, 136, "<", 0);
    org_button(413, 86, 463, 136, ">", 0);
    org_button(471, 86, 527, 136, "今天", 0);
    static const char *wd_zh[7] = {"一","二","三","四","五","六","日"};
    static const char *wd_en[7] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
    for (int c = 0; c < 7; c++) {
        const char *name = ui_lang ? wd_en[c] : wd_zh[c];
        textcenter(111 + c*60, vbase(153, name, FACE(FS16)),
                   name, FACE(FS16), TH->on3, 255);
    }
    struct tm first = {.tm_year = org_year-1900, .tm_mon = org_month-1,
                       .tm_mday = 1, .tm_hour = 12};
    mktime(&first);
    int start = (first.tm_wday + 6) % 7;
    for (int day = 1; day <= org_days(org_year, org_month); day++) {
        int cell = start + day - 1, row = cell/7, col = cell%7;
        int x = 82 + col*60, y = 170 + row*48;
        int selected = day == org_day;
        int has = org_event_for(org_date_key(org_year, org_month, day)) >= 0;
        rrect(x, y, x+56, y+44, 11, selected ? TH->accent : TH->card,
              selected ? 255 : TH->card_alpha, -1);
        char n[8]; snprintf(n, sizeof n, "%d", day);
        textcenter(x+28, vbase(y+21, n, FACE(FS18)), n, FACE(FS18),
                   selected ? TH->accent_ink : TH->on, 255);
        if (has) rrect(x+25, y+35, x+31, y+39, 3,
                       selected ? TH->accent_ink : TH->accent, 255, -1);
    }
    rrect(552, 86, 776, 454, 22, TH->card, TH->card_alpha, -1);
    snprintf(date, sizeof date, "%04d-%02d-%02d", org_year, org_month, org_day);
    text(572, vbase(120, date, FACE(FS20)), date, FACE(FS20), TH->on, 255);
    int i = org_event_for(org_date_key(org_year, org_month, org_day));
    if (i >= 0) {
        const char *s = org_events[i].text;
        for (int row = 0; row < 4 && *s; row++) {
            char part[20]; snprintf(part, sizeof part, "%.19s", s);
            text(572, vbase(178+row*28, part, FACE(FS18)),
                 part, FACE(FS18), TH->on2, 255);
            s += strlen(part);
        }
        org_button(572, 336, 658, 388, "编辑", 0);
        org_button(668, 336, 754, 388, "删除", 0);
    } else {
        text(572, vbase(188, "暂无数据", FACE(FS18)),
             "暂无数据", FACE(FS18), TH->on3, 255);
        org_button(572, 336, 754, 388, "添加", 1);
    }
    org_save_label();
}

static int org_calendar_hit(int x, int y)
{
    if (y >= 86 && y <= 136) {
        if (x >= 355 && x < 405) { org_month_shift(-1); return 1; }
        if (x >= 413 && x < 463) { org_month_shift(1); return 1; }
        if (x >= 471 && x < 527) { org_today(); dirty = 1; return 1; }
    }
    if (x >= 82 && x < 502 && y >= 170 && y < 458) {
        int col = (x-82)/60, row = (y-170)/48;
        if (col >= 0 && col < 7) {
            struct tm first = {.tm_year = org_year-1900, .tm_mon = org_month-1,
                               .tm_mday = 1, .tm_hour = 12};
            mktime(&first);
            int day = row*7+col-(first.tm_wday+6)%7+1;
            if (day >= 1 && day <= org_days(org_year, org_month)) {
                org_day = day; dirty = 1;
            }
            return 1;
        }
    }
    if (y >= 336 && y < 388 && x >= 572 && x < 754) {
        int key = org_date_key(org_year, org_month, org_day);
        int i = org_event_for(key);
        if (i >= 0 && x >= 668) {
            memmove(&org_events[i], &org_events[i+1],
                    (size_t)(org_event_count-i-1)*sizeof org_events[0]);
            org_event_count--; org_save();
        } else org_open_editor(2, i, i >= 0 ? org_events[i].text : "");
        return 1;
    }
    return 0;
}

static void org_draw_notes(void)
{
    char count[32];
    snprintf(count, sizeof count, "%d / %d", org_note_count, ORG_NOTES_MAX);
    text(40, vbase(108, count, FACE(FS18)), count, FACE(FS18), TH->on3, 255);
    org_button(380, 84, 520, 134, "添加", 1);
    for (int row = 0; row < 5; row++) {
        int i = org_note_scroll + row;
        if (i >= org_note_count) break;
        int y = 146 + row*58, active = i == org_note_selected;
        rrect(24, y, 524, y+54, 14, active ? TH->accent : TH->card,
              active ? 100 : TH->card_alpha, -1);
        char shown[80];
        file_elide(shown, sizeof shown, org_notes[i].text, 460, FACE(FS18));
        text(42, vbase(y+27, shown, FACE(FS18)), shown, FACE(FS18), TH->on, 255);
    }
    rrect(548, 84, 776, 424, 22, TH->card, TH->card_alpha, -1);
    if (org_note_selected >= 0 && org_note_selected < org_note_count) {
        const char *s = org_notes[org_note_selected].text;
        for (int row = 0; row < 4 && *s; row++) {
            char part[20]; snprintf(part, sizeof part, "%.19s", s);
            text(568, vbase(124+row*36, part, FACE(FS18)), part,
                 FACE(FS18), TH->on, 255);
            s += strlen(part);
        }
        org_button(568, 336, 655, 388, "编辑", 0);
        org_button(666, 336, 754, 388, "删除", 0);
    } else {
        text(568, vbase(124, "暂无数据", FACE(FS18)),
             "暂无数据", FACE(FS18), TH->on3, 255);
    }
    org_save_label();
}

static int org_notes_hit(int x, int y)
{
    if (x >= 380 && x < 520 && y >= 84 && y < 134) {
        if (org_note_count < ORG_NOTES_MAX) org_open_editor(1, -1, "");
        return 1;
    }
    if (x >= 24 && x < 524 && y >= 146 && y < 436) {
        int row = (y-146)/58;
        if ((y-146)%58 < 54 && org_note_scroll+row < org_note_count) {
            org_note_selected = org_note_scroll+row; dirty = 1;
        }
        return 1;
    }
    if (org_note_selected >= 0 && org_note_selected < org_note_count &&
        y >= 336 && y < 388) {
        if (x >= 568 && x < 655) {
            org_open_editor(1, org_note_selected,
                            org_notes[org_note_selected].text); return 1;
        }
        if (x >= 666 && x < 754) {
            memmove(&org_notes[org_note_selected], &org_notes[org_note_selected+1],
                    (size_t)(org_note_count-org_note_selected-1)*sizeof org_notes[0]);
            org_note_count--;
            if (org_note_selected >= org_note_count) org_note_selected = org_note_count-1;
            if (org_note_scroll > org_note_count-5)
                org_note_scroll = org_note_count > 5 ? org_note_count-5 : 0;
            org_save(); return 1;
        }
    }
    return 0;
}

static void org_notes_swipe(int dy)
{
    int max = org_note_count > 5 ? org_note_count-5 : 0;
    org_note_scroll += dy < 0 ? 4 : -4;
    if (org_note_scroll < 0) org_note_scroll = 0;
    if (org_note_scroll > max) org_note_scroll = max;
    dirty = 1;
}

#endif
