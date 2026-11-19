#ifndef LAUNCHER_HISTORY_UI_H
#define LAUNCHER_HISTORY_UI_H

static int hist_metric_for_card(int card)
{
    return card == 3 ? 0 : card >= 4 ? card - 1 : card;
}

static double hist_display_value(int card, double v)
{
    return card == 3 ? pm25_to_aqi(v) : v;
}

static void hist_value_text(char *out, size_t cap, int card, double value)
{
    if (card == 0 || card == 1 || card == 2)
        snprintf(out, cap, "%.1f", value);
    else snprintf(out, cap, "%.0f", value);
}

static void hist_open_card(int card)
{
    history_idx = card;
    history_span = 0;
    history_page = 3;
    history_open = 1;
    detail_open = 0;
    dirty = 1;
}

static void hist_page_clamp(void)
{
    int max = history_span ? 2 : 3;
    if (history_page < 0) history_page = 0;
    if (history_page > max) history_page = max;
}

static int hist_hit(int x, int y)
{
    if (!history_open) return 0;
    if (x <= 70 && y >= STATUS_H && y <= STATUS_H + 56) {
        history_open = 0;
        detail_open = 1;
        detail_idx = history_idx;
        dirty = 1;
        return 1;
    }
    if (y >= STATUS_H + 8 && y <= STATUS_H + 48 && x >= 520 && x <= 776) {
        history_span = x < 648 ? 0 : 1;
        history_page = history_span ? 2 : 3;
        dirty = 1;
    }
    return 1;
}

static void hist_swipe(int dx)
{
    history_page += dx < 0 ? 1 : -1;
    hist_page_clamp();
    dirty = 1;
}

static int hist_plot_y(double val, double low, double high)
{
    double t = (val - low) / (high - low);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return 338 - (int)round(t * 132.0);
}

/* The atlas starts at 16 px. Scale only the two chart-bound labels to 14 px;
   the value labels on the points remain at the larger size requested. */
static int hist_small_width(const char *s)
{
    int width = 0;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        const struct glyph *g = find_glyph(FACE(FS16), cp);
        width += g ? (g->adv * 7 + 4) / 8 : 8;
    }
    return width;
}

static void hist_small_text(int x, int baseline, const char *s, uint32_t color, int opacity)
{
    int pen = x;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        const struct glyph *gp = find_glyph(FACE(FS16), cp);
        if (!gp) { pen += 8; continue; }
        struct glyph g = *gp;
        const uint8_t *src = atlas + blob_off + g.off;
        int dw = (g.w * 7 + 4) / 8, dh = (g.h * 7 + 4) / 8;
        int gx = pen + (g.bx * 7) / 8, gy = baseline + (g.by * 7) / 8;
        for (int yy = 0; yy < dh; yy++) {
            int py = gy + yy;
            if (py < 0 || py >= LH) continue;
            int sy = yy * 8 / 7;
            if (sy >= g.h) sy = g.h - 1;
            uint32_t *row = land + (size_t)py * LW;
            for (int xx = 0; xx < dw; xx++) {
                int px = gx + xx;
                if (px < 0 || px >= LW) continue;
                int sx = xx * 8 / 7;
                if (sx >= g.w) sx = g.w - 1;
                int a = src[(size_t)sy * g.w + sx] * opacity / 255;
                if (a) row[px] = blend(row[px], color, a);
            }
        }
        pen += (g.adv * 7 + 4) / 8;
    }
}

static void hist_axis(double low, double high)
{
    int opacity = TH->is_light ? 165 : 170;
    for (int i = 0; i < 3; i++) {
        int y = 206 + i * 66;
        fill(110, y, 748, y + 1, TH->on3, opacity);
    }
    char lo[32], hi[32], label[64];
    hist_value_text(lo, sizeof lo, history_idx, low);
    hist_value_text(hi, sizeof hi, history_idx, high);
    snprintf(label, sizeof label, "%s", hi);
    hist_small_text(104 - hist_small_width(label), 211, label, TH->on3, opacity);
    snprintf(label, sizeof label, "%s", lo);
    hist_small_text(104 - hist_small_width(label), 343, label, TH->on3, opacity);
}

static void hist_area_segment(int x0, int y0, int x1, int y1)
{
    if (x1 <= x0) return;
    for (int x = x0; x <= x1; x++) {
        double f = (double)(x - x0) / (x1 - x0);
        int y = (int)round(y0 + (y1 - y0) * f);
        if (y < 338)
            fill(x, y, x + 1, 339, TH->accent, TH->is_light ? 22 : 34);
    }
}

static void hist_24h(void)
{
    int64_t latest = (int64_t)time(NULL) / 1800;
    int64_t first = latest - 47;
    int metric = hist_metric_for_card(history_idx);
    double vals[48], minv = INFINITY, maxv = -INFINITY;
    int have[48] = {0}, present = 0;
    for (int i = 0; i < 48; i++) {
        struct hist_record *r = hist_get(first + i);
        if (!r || !r->metric[metric].count) continue;
        vals[i] = hist_display_value(history_idx,
                                     r->metric[metric].sum / r->metric[metric].count);
        have[i] = 1;
        present++;
        if (vals[i] < minv) minv = vals[i];
        if (vals[i] > maxv) maxv = vals[i];
    }
    if (!present) {
        textcenter(400, 264, ui_lang ? "No data yet" : "无数据",
                   FACE(FS20), TH->on3, 255);
        return;
    }
    double minspan = history_idx == 1 ? 2 : history_idx == 2 ? 5 :
                     history_idx == 4 ? 20 : history_idx == 5 ? 20 : 5;
    double pad = fmax((maxv - minv) * .2, minspan * .5);
    double low = minv - pad, high = maxv + pad;
    if (history_idx != 1 && low < 0) low = 0;
    if (high <= low) high = low + minspan;
    hist_axis(low, high);
    int start = history_page * 12;
    int last_x = 0, last_y = 0, last_valid = 0;
    for (int i = 0; i < 12; i++) {
        int j = start + i, x = 130 + i * 55;
        time_t at = (time_t)((first + j) * 1800);
        struct tm local;
        localtime_r(&at, &local);
        char label[24];
        snprintf(label, sizeof label, "%02d:%02d", local.tm_hour, local.tm_min);
        if (j == 47) strncat(label, "*", sizeof label - strlen(label) - 1);
        textcenter(x, 393, label, FACE(FS16), TH->on3, 255);
        if (!have[j]) { last_valid = 0; continue; }
        int y = hist_plot_y(vals[j], low, high);
        if (last_valid) {
            hist_area_segment(last_x, last_y, x, y);
            wx_line(last_x, last_y, x, y, TH->accent, 2);
        }
        rrect(x - 4, y - 4, x + 4, y + 4, 4, TH->accent, 255, -1);
        hist_value_text(label, sizeof label, history_idx, vals[j]);
        int value_face = textw(label, FACE(FS18)) <= 51 ? FACE(FS18) : FACE(FS16);
        textcenter(x, y - 15, label, value_face, TH->on, 255);
        last_x = x; last_y = y; last_valid = 1;
    }
}

struct hist_day { double low, high; int valid; time_t start, end; };

static void hist_30d(void)
{
    struct hist_day days[30] = {0};
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    local.tm_hour = local.tm_min = local.tm_sec = 0;
    local.tm_isdst = -1;
    for (int i = 0; i < 30; i++) {
        struct tm d = local;
        d.tm_mday -= 29 - i;
        days[i].start = mktime(&d);
        d.tm_mday++;
        days[i].end = mktime(&d);
    }
    int metric = hist_metric_for_card(history_idx);
    int64_t first = (int64_t)days[0].start / 1800 - 1;
    int64_t last = (int64_t)now / 1800;
    for (int64_t slot = first; slot <= last; slot++) {
        struct hist_record *r = hist_get(slot);
        if (!r || !r->metric[metric].count) continue;
        time_t sample_at = (time_t)(slot * 1800 + 900);
        for (int i = 0; i < 30; i++) {
            if (sample_at < days[i].start || sample_at >= days[i].end) continue;
            double lo = hist_display_value(history_idx, r->metric[metric].low);
            double hi = hist_display_value(history_idx, r->metric[metric].high);
            if (!days[i].valid || lo < days[i].low) days[i].low = lo;
            if (!days[i].valid || hi > days[i].high) days[i].high = hi;
            days[i].valid = 1;
            break;
        }
    }
    double minv = INFINITY, maxv = -INFINITY;
    int present = 0;
    for (int i = 0; i < 30; i++) if (days[i].valid) {
        present++;
        if (days[i].low < minv) minv = days[i].low;
        if (days[i].high > maxv) maxv = days[i].high;
    }
    if (!present) {
        textcenter(400, 264, ui_lang ? "No data yet" : "无数据",
                   FACE(FS20), TH->on3, 255);
        return;
    }
    double minspan = history_idx == 1 ? 2 : history_idx == 2 ? 5 :
                     history_idx == 4 ? 20 : history_idx == 5 ? 20 : 5;
    double pad = fmax((maxv - minv) * .2, minspan * .5);
    double low = minv - pad, high = maxv + pad;
    if (history_idx != 1 && low < 0) low = 0;
    if (high <= low) high = low + minspan;
    hist_axis(low, high);
    int start = history_page * 10;
    for (int i = 0; i < 10; i++) {
        int j = start + i, x = 130 + i * 67;
        time_t at = days[j].start;
        localtime_r(&at, &local);
        char label[24];
        snprintf(label, sizeof label, "%02d/%02d", local.tm_mon + 1, local.tm_mday);
        if (j == 29) strncat(label, "*", sizeof label - strlen(label) - 1);
        textcenter(x, 393, label, FACE(FS16), TH->on3, 255);
        if (!days[j].valid) continue;
        int top = hist_plot_y(days[j].high, low, high);
        int bot = hist_plot_y(days[j].low, low, high);
        if (bot - top < 10) {
            int cy = (top + bot) / 2;
            rrect(x - 5, cy - 5, x + 5, cy + 5, 5, TH->accent, 255, -1);
        } else {
            rrect(x - 5, top, x + 5, bot, 5, TH->accent, 255, -1);
        }
        hist_value_text(label, sizeof label, history_idx, days[j].high);
        int value_face = textw(label, FACE(FS18)) <= 63 ? FACE(FS18) : FACE(FS16);
        textcenter(x, top - 15, label, value_face, TH->on, 255);
        hist_value_text(label, sizeof label, history_idx, days[j].low);
        value_face = textw(label, FACE(FS18)) <= 63 ? FACE(FS18) : FACE(FS16);
        textcenter(x, bot + 20, label, value_face, TH->on2, 255);
    }
}

static void hist_draw(void)
{
    if (!history_open) return;
    draw_app_header(ui_lang ? "History" : "History");
    for (int i = 0; i < 2; i++) {
        int x = 520 + i * 128, y = STATUS_H + 8;
        const char *label = i ? (ui_lang ? "30 days" : "30天") :
                                (ui_lang ? "24 hours" : "24小时");
        rrect(x, y, x + 120, y + 40, 20,
              i == history_span ? TH->accent : TH->chip, 255, -1);
        textcenter(x + 60, vbase(y + 20, label, FACE(FS18)), label, FACE(FS18),
                   i == history_span ? TH->accent_ink : TH->chip_ink, 255);
    }
    rrect(24, 108, 776, 448, 22, TH->card, TH->card_alpha, -1);
    text(50, 146, SENS[history_idx].name, FACE(FS26), TH->on, 255);
    int nwidth = textw(SENS[history_idx].name, FACE(FS26));
    text(64 + nwidth, 144, SENS[history_idx].unit, FACE(FS16), TH->on3, 255);
    if (history_idx == 3)
        text(610, 143, "Estimated AQI", FACE(FS16), TH->on3, 255);
    if (history_span) hist_30d(); else hist_24h();
    char pages[20];
    snprintf(pages, sizeof pages, "%d / %d", history_page + 1, history_span ? 3 : 4);
    textcenter(400, 427, pages, FACE(FS16), TH->on3, 255);
    if (history_page == (history_span ? 2 : 3))
        text(53, 427, "* in progress", FACE(FS16), TH->on3, 255);
    if (hist_save_state == 3)
        text(53, 427, ui_lang ? "Save failed" : "保存失败", FACE(FS16), 0xE5484D, 255);
}

#endif
