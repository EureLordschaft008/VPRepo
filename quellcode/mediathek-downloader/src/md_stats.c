/*
 * Mediathek Downloader – Sehzeit und Statistik
 * ============================================
 * sehzeit.tsv: je Zeile Tag, Stunde, F (Film) oder S (Serie), Name, Sekunden, Starts und wie oft
 * bis zum Ende. Gleiche Tage, Stunden und Titel werden zusammengezählt, so bleibt die Datei klein.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "md_core.h"
#include "md_stats.h"

#define STATS_FILE "sehzeit.tsv"
#define STATS_FIELDS 7
#define CHART_GAP 0.25          /* Lücke zwischen Balken, Anteil der Balkenbreite */
#define CHART_LABEL_HEIGHT 16

/* Eine Stunde eines Titels */
typedef struct {
    char *day;                  /* "2026-11-05", Ortszeit */
    int hour;
    bool film;
    char *name;
    double seconds;
    int starts, finished;
} Row;

static GHashTable *rows;        /* "Tag\tStunde\tF|S\tName" -> Row* */

static void row_free(gpointer data)
{
    Row *r = data;
    g_free(r->day);
    g_free(r->name);
    g_free(r);
}

static char *stats_path(void)
{
    return g_build_filename(md_app_dir(), STATS_FILE, NULL);
}

/* Zeile anlegen oder die vorhandene liefern */
static Row *row_get(const char *day, int hour, bool film, const char *name)
{
    char *key = g_strdup_printf("%s\t%d\t%c\t%s", day, hour, film ? 'F' : 'S', name);
    Row *r = g_hash_table_lookup(rows, key);
    if (r) {
        g_free(key);
        return r;
    }
    r = g_new0(Row, 1);
    r->day = g_strdup(day);
    r->hour = hour;
    r->film = film;
    r->name = g_strdup(name);
    g_hash_table_insert(rows, key, r);
    return r;
}

bool md_stats_load(void)
{
    if (!rows) rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, row_free);
    char *path = stats_path(), *text = NULL;
    bool found = g_file_get_contents(path, &text, NULL, NULL);
    g_free(path);
    if (!found) return false;
    char **lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char **f = g_strsplit(lines[i], "\t", -1);
        if (g_strv_length(f) == STATS_FIELDS && strlen(f[0]) == 10) {
            char *name = g_strcompress(f[3]);
            Row *r = row_get(f[0], CLAMP(atoi(f[1]), 0, 23), f[2][0] == 'F', name);
            r->seconds += MAX(g_ascii_strtod(f[4], NULL), 0);
            r->starts += MAX(atoi(f[5]), 0);
            r->finished += MAX(atoi(f[6]), 0);
            g_free(name);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(text);
    return true;
}

void md_stats_save(void)
{
    if (!rows) return;
    GString *out = g_string_new(NULL);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, rows);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const Row *r = value;
        char *escaped = g_strescape(r->name, NULL);
        char seconds[G_ASCII_DTOSTR_BUF_SIZE];
        g_string_append_printf(out, "%s\t%d\t%c\t%s\t%s\t%d\t%d\n", r->day, r->hour,
                               r->film ? 'F' : 'S', escaped,
                               g_ascii_formatd(seconds, sizeof seconds, "%.0f", r->seconds),
                               r->starts, r->finished);
        g_free(escaped);
    }
    char *path = stats_path();
    g_file_set_contents(path, out->str, (gssize)out->len, NULL);
    g_free(path);
    g_string_free(out, TRUE);
}

void md_stats_add(const char *name, bool film, gint64 when, double seconds, int starts,
                  int finished)
{
    if (!rows) md_stats_load();
    if (!name || !*name || strchr(name, '\n')) return;
    GDateTime *t = g_date_time_new_from_unix_local(when);
    if (!t) return;
    char *day = g_date_time_format(t, "%Y-%m-%d");
    Row *r = row_get(day, g_date_time_get_hour(t), film, name);
    r->seconds += MAX(seconds, 0);
    r->starts += starts;
    r->finished += finished;
    g_free(day);
    g_date_time_unref(t);
}

/* Unix-Zeit zum Ende der Stunde einer Zeile (für "zuletzt geschaut") */
static gint64 row_time(const Row *r)
{
    int y = 0, m = 0, d = 0;
    if (sscanf(r->day, "%d-%d-%d", &y, &m, &d) != 3) return 0;
    GDateTime *t = g_date_time_new_local(y, m, d, r->hour, 59, 59);
    gint64 seconds = t ? g_date_time_to_unix(t) : 0;
    if (t) g_date_time_unref(t);
    return seconds;
}

static gint compare_titles(gconstpointer a, gconstpointer b)
{
    const MdStatsTitle *x = *(MdStatsTitle *const *)a, *y = *(MdStatsTitle *const *)b;
    return (x->seconds < y->seconds) - (x->seconds > y->seconds);
}

static void title_free(gpointer data)
{
    MdStatsTitle *t = data;
    g_free(t->name);
    g_free(t);
}

GPtrArray *md_stats_titles(void)
{
    if (!rows) md_stats_load();
    GHashTable *by_name = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *titles = g_ptr_array_new_with_free_func(title_free);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, rows);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const Row *r = value;
        char *key = g_strdup_printf("%c%s", r->film ? 'F' : 'S', r->name);
        MdStatsTitle *t = g_hash_table_lookup(by_name, key);
        if (!t) {
            t = g_new0(MdStatsTitle, 1);
            t->name = g_strdup(r->name);
            t->film = r->film;
            g_ptr_array_add(titles, t);
            g_hash_table_insert(by_name, key, t);
        } else {
            g_free(key);
        }
        t->seconds += r->seconds;
        t->starts += r->starts;
        t->finished += r->finished;
        t->last = MAX(t->last, row_time(r));
    }
    GList *keys = g_hash_table_get_keys(by_name);
    g_list_free_full(keys, g_free);
    g_hash_table_destroy(by_name);
    g_ptr_array_sort(titles, compare_titles);
    return titles;
}

/* Tag als "JJJJ-MM-TT", offset Tage vor heute */
static char *day_before(int offset)
{
    GDateTime *now = g_date_time_new_now_local(), *t = g_date_time_add_days(now, -offset);
    char *day = g_date_time_format(t, "%Y-%m-%d");
    g_date_time_unref(t);
    g_date_time_unref(now);
    return day;
}

/* Sekunden je Tag (Schlüssel "JJJJ-MM-TT" -> double*) */
static GHashTable *seconds_by_day(void)
{
    if (!rows) md_stats_load();
    GHashTable *days = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, rows);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const Row *r = value;
        double *sum = g_hash_table_lookup(days, r->day);
        if (!sum) {
            sum = g_new0(double, 1);
            g_hash_table_insert(days, r->day, sum);
        }
        *sum += r->seconds;
    }
    return days;
}

void md_stats_days(double *out, int days)
{
    GHashTable *by_day = seconds_by_day();
    for (int i = 0; i < days; i++) {
        char *day = day_before(days - 1 - i);
        const double *sum = g_hash_table_lookup(by_day, day);
        out[i] = sum ? *sum : 0;
        g_free(day);
    }
    g_hash_table_destroy(by_day);
}

void md_stats_weekdays(double out[7])
{
    memset(out, 0, 7 * sizeof *out);
    if (!rows) md_stats_load();
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, rows);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const Row *r = value;
        int y = 0, m = 0, d = 0;
        if (sscanf(r->day, "%d-%d-%d", &y, &m, &d) != 3 || !g_date_valid_dmy(d, m, y)) continue;
        GDate *date = g_date_new_dmy((GDateDay)d, (GDateMonth)m, (GDateYear)y);
        out[g_date_get_weekday(date) - 1] += r->seconds;      /* G_DATE_MONDAY = 1 */
        g_date_free(date);
    }
}

void md_stats_hours(double out[24])
{
    memset(out, 0, 24 * sizeof *out);
    if (!rows) md_stats_load();
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, rows);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const Row *r = value;
        out[r->hour] += r->seconds;
    }
}

double md_stats_total(void)
{
    double hours[24], total = 0;
    md_stats_hours(hours);
    for (int i = 0; i < 24; i++) total += hours[i];
    return total;
}

double md_stats_best_day(char **day)
{
    GHashTable *by_day = seconds_by_day();
    double best = 0;
    const char *best_day = "";
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, by_day);
    while (g_hash_table_iter_next(&iter, &key, &value))
        if (*(double *)value > best) {
            best = *(double *)value;
            best_day = key;
        }
    *day = g_strdup(best_day);
    g_hash_table_destroy(by_day);
    return best;
}

int md_stats_streak(void)
{
    GHashTable *by_day = seconds_by_day();
    int streak = 0;
    /* heute zählt mit, fehlt aber nicht: der Tag ist noch nicht vorbei */
    for (int offset = 0;; offset++) {
        char *day = day_before(offset);
        const double *sum = g_hash_table_lookup(by_day, day);
        g_free(day);
        if (sum && *sum >= 60) streak++;
        else if (offset > 0) break;
    }
    g_hash_table_destroy(by_day);
    return streak;
}

/* --- Balkendiagramm ---------------------------------------------------------------------- */
typedef struct {
    double *values;
    char **labels, **tips;
    int count;
} Chart;

static void chart_free(gpointer data)
{
    Chart *c = data;
    g_free(c->values);
    g_strfreev(c->labels);
    g_strfreev(c->tips);
    g_free(c);
}

/* Breite eines Balkens samt Lücke */
static double chart_slot(GtkWidget *w, const Chart *c)
{
    return (double)gtk_widget_get_allocated_width(w) / MAX(c->count, 1);
}

static gboolean on_chart_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    const Chart *c = data;
    GtkStyleContext *style = gtk_widget_get_style_context(w);
    GdkRGBA bar, text;
    if (!gtk_style_context_lookup_color(style, "theme_selected_bg_color", &bar))
        gdk_rgba_parse(&bar, "#3584e4");
    gtk_style_context_get_color(style, gtk_style_context_get_state(style), &text);
    double height = gtk_widget_get_allocated_height(w) - CHART_LABEL_HEIGHT;
    double slot = chart_slot(w, c), max = 0;
    for (int i = 0; i < c->count; i++) max = MAX(max, c->values[i]);
    /* Grundlinie */
    cairo_set_source_rgba(cr, text.red, text.green, text.blue, 0.25);
    cairo_rectangle(cr, 0, height - 1, gtk_widget_get_allocated_width(w), 1);
    cairo_fill(cr);
    PangoLayout *layout = gtk_widget_create_pango_layout(w, NULL);
    PangoFontDescription *font = pango_font_description_copy(
        pango_context_get_font_description(pango_layout_get_context(layout)));
    pango_font_description_set_size(font, pango_font_description_get_size(font) * 4 / 5);
    pango_layout_set_font_description(layout, font);
    for (int i = 0; i < c->count; i++) {
        double h = max > 0 ? (height - 4) * c->values[i] / max : 0;
        if (c->values[i] > 0) h = MAX(h, 2);
        double gap = slot * CHART_GAP / 2;
        cairo_set_source_rgba(cr, bar.red, bar.green, bar.blue, 0.9);
        cairo_rectangle(cr, i * slot + gap, height - 1 - h, slot - 2 * gap, h);
        cairo_fill(cr);
        if (!c->labels[i] || !*c->labels[i]) continue;
        int lw = 0, lh = 0;
        pango_layout_set_text(layout, c->labels[i], -1);
        pango_layout_get_pixel_size(layout, &lw, &lh);
        cairo_set_source_rgba(cr, text.red, text.green, text.blue, 0.7);
        double x = CLAMP(i * slot + (slot - lw) / 2, 0,
                         gtk_widget_get_allocated_width(w) - lw);     /* Ränder nicht abschneiden */
        cairo_move_to(cr, x, height + 1);
        pango_cairo_show_layout(cr, layout);
    }
    pango_font_description_free(font);
    g_object_unref(layout);
    return FALSE;
}

static gboolean on_chart_tooltip(GtkWidget *w, gint x, gint y, gboolean keyboard,
                                 GtkTooltip *tooltip, gpointer data)
{
    (void)y;
    (void)keyboard;
    const Chart *c = data;
    int i = (int)floor(x / chart_slot(w, c));
    if (i < 0 || i >= c->count || !c->tips[i]) return FALSE;
    gtk_tooltip_set_text(tooltip, c->tips[i]);
    return TRUE;
}

/* Kopie einer Textliste mit count Einträgen, NULL-Einträge werden "" */
static char **copy_texts(const char *const *texts, int count)
{
    char **copy = g_new0(char *, count + 1);
    for (int i = 0; i < count; i++) copy[i] = g_strdup(texts && texts[i] ? texts[i] : "");
    return copy;
}

GtkWidget *md_stats_chart(const double *values, const char *const *labels,
                          const char *const *tips, int count, int height)
{
    Chart *c = g_new0(Chart, 1);
    c->values = g_memdup2(values, (gsize)count * sizeof *values);
    c->labels = copy_texts(labels, count);
    c->tips = copy_texts(tips, count);
    c->count = count;
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, count * 6, height + CHART_LABEL_HEIGHT);
    gtk_widget_set_has_tooltip(area, TRUE);
    g_object_set_data_full(G_OBJECT(area), "chart", c, chart_free);
    g_signal_connect(area, "draw", G_CALLBACK(on_chart_draw), c);
    g_signal_connect(area, "query-tooltip", G_CALLBACK(on_chart_tooltip), c);
    return area;
}
