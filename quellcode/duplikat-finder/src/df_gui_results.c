/*
 * Duplikat-Finder – Trefferliste, Vorschaubilder, Vergleichsfenster
 * ==================================================================
 * Die Gruppen erscheinen in Portionen (GROUPS_BATCH, weitere per Knopf), damit
 * auch Zehntausende Gruppen die Oberfläche nicht ausbremsen. Vorschaubilder
 * liest eine kleine Gruppe eigener Hilfsprozesse im Hintergrund; sie werden
 * als JPEG zwischengespeichert (~/.cache/duplikat-finder/vorschau).
 *
 * Markierungen und entfernte Einträge werden laufend gespeichert
 * (~/.duplikat-finder/letztes-ergebnis.json) – nach einem Neustart geht es
 * genau dort weiter.
 *
 * Dateiaktionen (Papierkorb, Löschen, Verschieben, Kopieren, Hard-/Symlinks)
 * laufen in einem eigenen Thread mit Fortschrittsfenster.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#include "df_gui.h"

struct MemberRow {
    int m;                      /* Mitglied */
    GtkWidget *row, *check, *image, *title, *sub, *info, *badge;
};

struct GroupBox {
    int g;
    GtkWidget *frame, *list, *header, *title;
};

enum { FILTER_ALL, FILTER_VIDEOS, FILTER_IMAGES, FILTER_PARTIAL, FILTER_FLIPPED, FILTER_IDENTICAL,
       FILTER_SIMILAR, FILTER_CHECKED };
static const char *const FILTER_TEXT[] = {"Alle Gruppen", "Nur Videos", "Nur Bilder",
                                          "Mit Teilclips", "Mit gespiegelten", "Nur 100 % gleiche",
                                          "Nur unter 100 %", "Mit markierten Dateien"};
enum { SORT_SIM_DESC, SORT_SIM_ASC, SORT_SPACE, SORT_COUNT, SORT_DURATION, SORT_PATH };
static const char *const SORT_TEXT[] = {"Ähnlichste zuerst", "Unähnlichste zuerst",
                                        "Meiste Platzersparnis", "Meiste Dateien",
                                        "Längste Videos", "Nach Pfad"};

static void update_selection_label(void);
static void show_more_groups(void);
static void schedule_result_save(void);

/* ---------------------------------------------------------------------------
 * Hilfen
 * ------------------------------------------------------------------------- */
static int alive_count(int g)
{
    const DfGroup *gr = &ui.res.groups[g];
    int n = 0;
    for (int i = 0; i < gr->n; i++) n += !ui.removed[gr->first + i];
    return n;
}

static const DfFile *member_file(int m)
{
    return &ui.res.files[ui.res.members[m].file];
}

static int group_of_member(int m)
{
    /* Gruppen sind lückenlos hintereinander: binäre Suche über first */
    int lo = 0, hi = ui.res.ngroups - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (ui.res.groups[mid].first <= m) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

/* Platz, der frei würde, wenn je Gruppe nur die größte Datei bliebe */
static long long group_dup_bytes(int g)
{
    const DfGroup *gr = &ui.res.groups[g];
    long long mx = 0, sum = 0;
    for (int i = 0; i < gr->n; i++) {
        if (ui.removed[gr->first + i]) continue;
        long long s = member_file(gr->first + i)->size;
        sum += s;
        mx = MAX(mx, s);
    }
    return sum - mx;
}

/* ---------------------------------------------------------------------------
 * Vorschaubilder (Hintergrund-Threads, Cache im Speicher und auf der Platte)
 * ------------------------------------------------------------------------- */
typedef struct {
    int m, generation, k, width;
    char *path, *key;
    long long size, mtime;
    GdkPixbuf *pix;
} ThumbJob;

static char *thumb_key(const DfFile *f, int k, int w)
{
    char *raw = g_strdup_printf("%s|%lld|%lld|%d|%d|2", f->path, f->size, f->mtime, k, w);
    char *md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, raw, -1);
    g_free(raw);
    return md5;
}

static void free_rgb(guchar *pixels, gpointer d)
{
    (void)d;
    g_free(pixels);
}

/* Vorschaubilder nebeneinander zu einem Streifen */
static GdkPixbuf *compose_strip(DfThumbs *t, int width)
{
    int h = 0, total = 0, n = 0;
    for (int i = 0; i < t->n; i++)
        if (t->rgb[i]) {
            h = MAX(h, t->h[i]);
            total += t->w[i];
            n++;
        }
    if (!n) return NULL;
    (void)width;
    int gap = 4;
    total += gap * (n - 1);
    GdkPixbuf *strip = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, total, h);
    gdk_pixbuf_fill(strip, 0x00000000);
    int x = 0;
    for (int i = 0; i < t->n; i++) {
        if (!t->rgb[i]) continue;
        GdkPixbuf *p = gdk_pixbuf_new_from_data(t->rgb[i], GDK_COLORSPACE_RGB, FALSE, 8, t->w[i],
                                                t->h[i], t->w[i] * 3, free_rgb, NULL);
        t->rgb[i] = NULL;
        gdk_pixbuf_copy_area(p, 0, 0, t->w[i], t->h[i], strip, x, (h - t->h[i]) / 2);
        g_object_unref(p);
        x += t->w[i] + gap;
    }
    return strip;
}

static gboolean on_thumb_ready(gpointer p)
{
    ThumbJob *j = p;
    if (ui.win && j->pix) {
        if (g_hash_table_size(ui.thumb_cache) > 3000) g_hash_table_remove_all(ui.thumb_cache);
        g_hash_table_replace(ui.thumb_cache, g_strdup(j->key), g_object_ref(j->pix));
        if (j->generation == ui.thumb_generation && ui.mrows && j->m < ui.res.nmembers &&
            ui.mrows[j->m])
            gtk_image_set_from_pixbuf(GTK_IMAGE(ui.mrows[j->m]->image), j->pix);
    }
    if (j->pix) g_object_unref(j->pix);
    g_free(j->path);
    g_free(j->key);
    g_free(j);
    return G_SOURCE_REMOVE;
}

static void thumb_worker(gpointer data, gpointer user)
{
    (void)user;
    ThumbJob *j = data;
    if (j->generation != g_atomic_int_get(&ui.thumb_generation)) {
        g_idle_add(on_thumb_ready, j);
        return;
    }
    char *file = g_build_filename(DF_THUMB_DIR, j->key, NULL);
    char *jpg = g_strconcat(file, ".jpg", NULL);
    g_free(file);
    j->pix = gdk_pixbuf_new_from_file(jpg, NULL);
    if (!j->pix) {
        DfThumbs t;
        if (df_thumbs_get(j->path, j->k, j->width, true, &t)) {
            j->pix = compose_strip(&t, j->width);
            if (j->pix) {
                g_mkdir_with_parents(DF_THUMB_DIR, 0755);
                gdk_pixbuf_save(j->pix, jpg, "jpeg", NULL, "quality", "82", NULL);
            }
        }
        df_thumbs_free(&t);
    }
    g_free(jpg);
    g_idle_add(on_thumb_ready, j);
}

static void request_thumb(MemberRow *mr)
{
    const DfFile *f = member_file(mr->m);
    int k = f->image ? 1 : thumb_count_setting(), w = thumb_width_setting();
    char *key = thumb_key(f, k, w);
    GdkPixbuf *cached = g_hash_table_lookup(ui.thumb_cache, key);
    if (cached) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(mr->image), cached);
        g_free(key);
        return;
    }
    /* Platzhalter in der richtigen Größe, damit die Liste nicht springt */
    int ph = w * 9 / 16;
    gtk_widget_set_size_request(mr->image, k * w + (k - 1) * 4, ph);
    ThumbJob *j = g_new0(ThumbJob, 1);
    j->m = mr->m;
    j->generation = ui.thumb_generation;
    j->k = k;
    j->width = w;
    j->path = g_strdup(f->path);
    j->key = key;
    if (!ui.thumb_pool) ui.thumb_pool = g_thread_pool_new(thumb_worker, NULL, 3, FALSE, NULL);
    g_thread_pool_push(ui.thumb_pool, j, NULL);
}

/* ---------------------------------------------------------------------------
 * Zeilen
 * ------------------------------------------------------------------------- */
static void on_member_check(GtkToggleButton *t, gpointer d)
{
    MemberRow *mr = d;
    ui.checked[mr->m] = gtk_toggle_button_get_active(t);
    GtkStyleContext *sc = gtk_widget_get_style_context(mr->row);
    if (ui.checked[mr->m]) gtk_style_context_add_class(sc, "df-checked");
    else gtk_style_context_remove_class(sc, "df-checked");
    update_selection_label();
    schedule_result_save();
}

static void sync_checks(void)
{
    for (int m = 0; m < ui.res.nmembers; m++) {
        MemberRow *mr = ui.mrows[m];
        if (!mr) continue;
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mr->check)) != ui.checked[m])
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mr->check), ui.checked[m]);
    }
    update_selection_label();
    schedule_result_save();
}

/* Info-Zeile: beste Werte der Gruppe grün */
static char *member_info_markup(int m, int g)
{
    const DfGroup *gr = &ui.res.groups[g];
    const DfFile *f = member_file(m);
    static const DfCriterion crit[] = {DF_CRIT_DURATION, DF_CRIT_RESOLUTION, DF_CRIT_FPS,
                                       DF_CRIT_BITRATE, DF_CRIT_AUDIO_BITRATE, DF_CRIT_SIZE};
    bool best[6] = {false};
    for (int c = 0; c < 6; c++) {
        double mine = df_criterion_value(f, crit[c]), mx = -INFINITY, mn = INFINITY;
        for (int i = 0; i < gr->n; i++) {
            if (ui.removed[gr->first + i]) continue;
            double v = df_criterion_value(member_file(gr->first + i), crit[c]);
            mx = MAX(mx, v);
            mn = MIN(mn, v);
        }
        best[c] = mx > mn && mine >= mx - (crit[c] == DF_CRIT_DURATION ? 0.5 : 1e-9);
    }
    GString *s = g_string_new(NULL);
#define PART(i, ...) do { \
        char *t_ = g_markup_printf_escaped(__VA_ARGS__); \
        if (s->len) g_string_append(s, "  ·  "); \
        if ((i) >= 0 && best[i]) g_string_append_printf(s, "<span foreground=\"#26a269\" " \
                                                           "weight=\"bold\">%s</span>", t_); \
        else g_string_append(s, t_); \
        g_free(t_); } while (0)
    char buf[64];
    if (!f->image) PART(0, "%s", df_format_duration(f->duration, buf, sizeof buf));
    PART(1, "%d×%d", f->width, f->height);
    if (!f->image && f->fps > 0) PART(2, "%.3g fps", f->fps);
    if (!f->image && f->bitrate > 0) {
        if (f->bitrate >= 1000) {
            snprintf(buf, sizeof buf, "%.1f Mbit/s", f->bitrate / 1000.0);
            for (char *c = buf; *c; c++) if (*c == '.') *c = ',';
            PART(3, "%s", buf);
        } else {
            PART(3, "%d kbit/s", f->bitrate);
        }
    }
    PART(-1, "%s", df_codec_name(f->vcodec));
    if (*f->acodec) {
        if (f->abitrate > 0) PART(4, "%s %d kbit/s", df_codec_name(f->acodec), f->abitrate);
        else PART(4, "%s", df_codec_name(f->acodec));
    } else if (!f->image) {
        PART(-1, "kein Ton");
    }
    PART(5, "%s", df_format_bytes((double)f->size, buf, sizeof buf));
    PART(-1, "%s", df_format_date(f->mtime, buf, sizeof buf));
#undef PART
    return g_string_free(s, FALSE);
}

static void row_menu_action(GtkMenuItem *item, gpointer d);

static void add_menu_item(GtkWidget *menu, const char *label, const char *action, int m)
{
    GtkWidget *it = gtk_menu_item_new_with_label(label);
    g_object_set_data(G_OBJECT(it), "action", (gpointer)action);
    g_signal_connect(it, "activate", G_CALLBACK(row_menu_action), GINT_TO_POINTER(m));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
}

static GtkWidget *member_menu(int m)
{
    GtkWidget *menu = gtk_menu_new();
    add_menu_item(menu, "Öffnen", "open", m);
    add_menu_item(menu, "Im Ordner zeigen", "folder", m);
    add_menu_item(menu, "Umbenennen …", "rename", m);
    add_menu_item(menu, "Pfad kopieren", "copy", m);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    add_menu_item(menu, "Diese behalten, Rest der Gruppe markieren", "keep", m);
    add_menu_item(menu, "Gruppe vergleichen …", "compare", m);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    add_menu_item(menu, "Aus der Liste entfernen", "remove", m);
    add_menu_item(menu, "Aus der Liste entfernen und künftig ignorieren", "ignore", m);
    gtk_widget_show_all(menu);
    return menu;
}

static gboolean on_row_button(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)w;
    MemberRow *mr = d;
    if (e->type == GDK_2BUTTON_PRESS && e->button == 1) {
        open_in_default_app(member_file(mr->m)->path);
        return TRUE;
    }
    if (e->type == GDK_BUTTON_PRESS && e->button == 3) {
        GtkWidget *menu = member_menu(mr->m);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)e);
        return TRUE;
    }
    return FALSE;
}

static void on_member_row_destroy(GtkWidget *w, gpointer d)
{
    (void)w;
    MemberRow *mr = d;
    if (ui.mrows && mr->m < ui.res.nmembers && ui.mrows[mr->m] == mr) ui.mrows[mr->m] = NULL;
    g_free(mr);
}

static void add_member_row(GroupBox *gb, int m)
{
    MemberRow *mr = g_new0(MemberRow, 1);
    mr->m = m;
    const DfFile *f = member_file(m);
    const DfMember *mem = &ui.res.members[m];
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget *ev = gtk_event_box_new();
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(h, 8);
    gtk_widget_set_margin_bottom(h, 8);
    gtk_widget_set_margin_start(h, 12);
    gtk_widget_set_margin_end(h, 8);
    mr->check = gtk_check_button_new();
    gtk_widget_set_valign(mr->check, GTK_ALIGN_CENTER);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mr->check), ui.checked[m]);
    gtk_widget_set_tooltip_text(mr->check, "Markieren (für Papierkorb, Verschieben …)");
    g_signal_connect(mr->check, "toggled", G_CALLBACK(on_member_check), mr);
    gtk_box_pack_start(GTK_BOX(h), mr->check, FALSE, FALSE, 0);
    mr->image = gtk_image_new();
    add_css_class(mr->image, "df-thumb");
    gtk_widget_set_valign(mr->image, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(h), mr->image, FALSE, FALSE, 0);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    char *disp = path_for_display(f->path);
    char *base = g_path_get_basename(disp), *dir = g_path_get_dirname(disp);
    mr->title = new_label(base, "df-title");
    gtk_label_set_ellipsize(GTK_LABEL(mr->title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_selectable(GTK_LABEL(mr->title), FALSE);
    gtk_widget_set_tooltip_text(mr->title, disp);
    mr->sub = new_label(dir, "df-sub");
    add_css_class(mr->sub, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(mr->sub), PANGO_ELLIPSIZE_MIDDLE);
    char *markup = member_info_markup(m, gb->g);
    mr->info = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(mr->info), markup);
    gtk_label_set_xalign(GTK_LABEL(mr->info), 0);
    gtk_label_set_line_wrap(GTK_LABEL(mr->info), TRUE);
    add_css_class(mr->info, "df-sub");
    g_free(markup);
    gtk_box_pack_start(GTK_BOX(v), mr->title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), mr->sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), mr->info, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
    /* Kennzeichen */
    GtkWidget *badges = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(badges, GTK_ALIGN_CENTER);
    char *bt = g_strdup_printf("%.0f %%", floor(mem->similarity));
    mr->badge = new_label(bt, "df-badge");
    gtk_widget_set_halign(mr->badge, GTK_ALIGN_END);
    add_css_class(mr->badge, mem->similarity >= 99.99f ? "ok" : mem->similarity >= 95 ? "info" : "near");
    gtk_widget_set_tooltip_text(mr->badge, "Ähnlichkeit zur ähnlichsten Datei der Gruppe");
    g_free(bt);
    gtk_box_pack_start(GTK_BOX(badges), mr->badge, FALSE, FALSE, 0);
    if (mem->flipped) {
        GtkWidget *b = new_label("gespiegelt", "df-badge");
        add_css_class(b, "near");
        gtk_widget_set_halign(b, GTK_ALIGN_END);
        gtk_box_pack_start(GTK_BOX(badges), b, FALSE, FALSE, 0);
    }
    if (mem->partial) {
        char d2[32];
        char *t = g_strdup_printf("Teilclip ab %s", df_format_duration(mem->offset, d2, sizeof d2));
        GtkWidget *b = new_label(t, "df-badge");
        add_css_class(b, "info");
        gtk_widget_set_halign(b, GTK_ALIGN_END);
        if (mem->source >= 0 && mem->source < ui.res.nfiles) {
            char *src = path_for_display(ui.res.files[mem->source].path);
            char *tt = g_strdup_printf("Ausschnitt aus %s, beginnt dort bei %s", src, d2);
            gtk_widget_set_tooltip_text(b, tt);
            g_free(src);
            g_free(tt);
        }
        gtk_box_pack_start(GTK_BOX(badges), b, FALSE, FALSE, 0);
        g_free(t);
    }
    gtk_box_pack_start(GTK_BOX(h), badges, FALSE, FALSE, 0);
    GtkWidget *mb = gtk_menu_button_new();
    gtk_button_set_relief(GTK_BUTTON(mb), GTK_RELIEF_NONE);
    gtk_button_set_image(GTK_BUTTON(mb), gtk_image_new_from_icon_name("view-more-symbolic",
                                                                      GTK_ICON_SIZE_BUTTON));
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(mb), member_menu(m));
    gtk_widget_set_valign(mb, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(h), mb, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ev), h);
    gtk_container_add(GTK_CONTAINER(row), ev);
    g_signal_connect(ev, "button-press-event", G_CALLBACK(on_row_button), mr);
    gtk_container_add(GTK_CONTAINER(gb->list), row);
    mr->row = row;
    if (ui.checked[m]) add_css_class(row, "df-checked");
    g_signal_connect(row, "destroy", G_CALLBACK(on_member_row_destroy), mr);
    ui.mrows[m] = mr;
    g_free(disp);
    g_free(base);
    g_free(dir);
    request_thumb(mr);
}

static void update_group_title(GroupBox *gb)
{
    const DfGroup *gr = &ui.res.groups[gb->g];
    char sz[32];
    float mn = 100;
    for (int i = 0; i < gr->n; i++)
        if (!ui.removed[gr->first + i]) mn = MIN(mn, ui.res.members[gr->first + i].similarity);
    char *t = g_strdup_printf("%d Dateien · ab %.0f %% ähnlich%s · %s Duplikate", alive_count(gb->g),
                              floor(mn), gr->partial ? " · mit Teilclips" : "",
                              df_format_bytes((double)group_dup_bytes(gb->g), sz, sizeof sz));
    set_label_text(gb->title, t);
    g_free(t);
}

static void on_group_compare(GtkButton *b, gpointer d)
{
    (void)b;
    compare_window_open(GPOINTER_TO_INT(d));
}

static void remove_members(const int *ms, int n, bool rebuild);

static void on_group_notmatch(GtkButton *b, gpointer d)
{
    (void)b;
    int g = GPOINTER_TO_INT(d);
    const DfGroup *gr = &ui.res.groups[g];
    int ms[gr->n], n = 0;
    for (int i = 0; i < gr->n; i++) {
        int a = gr->first + i;
        if (ui.removed[a]) continue;
        ms[n++] = a;
        for (int j = i + 1; j < gr->n; j++) {
            int bb = gr->first + j;
            if (!ui.removed[bb]) df_notmatch_add(member_file(a)->id, member_file(bb)->id);
        }
    }
    remove_members(ms, n, false);
    show_info_bar(GTK_MESSAGE_INFO, "Als „kein Duplikat“ gemerkt – diese Dateien werden nicht mehr "
                  "zusammen gemeldet.");
}

static void on_group_keep_best(GtkButton *b, gpointer d)
{
    (void)b;
    int g = GPOINTER_TO_INT(d);
    const DfGroup *gr = &ui.res.groups[g];
    int best = df_best_member(&ui.res, g, ui.criteria, ui.ncriteria, ui.removed);
    for (int i = 0; i < gr->n; i++) {
        int m = gr->first + i;
        if (!ui.removed[m]) ui.checked[m] = m != best;
    }
    sync_checks();
}

static void on_group_destroy(GtkWidget *w, gpointer d)
{
    (void)w;
    GroupBox *gb = d;
    if (ui.gboxes && gb->g < ui.res.ngroups && ui.gboxes[gb->g] == gb) ui.gboxes[gb->g] = NULL;
    g_free(gb);
}

static GtkWidget *small_button(const char *label, const char *tooltip)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_set_tooltip_text(b, tooltip);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    return b;
}

static void add_group_box(int g, int number)
{
    GroupBox *gb = g_new0(GroupBox, 1);
    gb->g = g;
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(head, 16);
    char *num = g_strdup_printf("Gruppe %d", number);
    GtkWidget *nl = new_label(num, "df-gtitle");
    g_free(num);
    gb->title = new_label("", "df-sub");
    add_css_class(gb->title, "dim-label");
    gtk_box_pack_start(GTK_BOX(head), nl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), gb->title, TRUE, TRUE, 0);
    GtkWidget *b1 = small_button("Beste behalten", "Alle außer der besten Datei markieren "
                                 "(nach der eingestellten Reihenfolge der Merkmale)");
    g_signal_connect(b1, "clicked", G_CALLBACK(on_group_keep_best), GINT_TO_POINTER(g));
    GtkWidget *b2 = small_button("Vergleichen", "Alle Vergleichsbilder nebeneinander ansehen");
    g_signal_connect(b2, "clicked", G_CALLBACK(on_group_compare), GINT_TO_POINTER(g));
    GtkWidget *b3 = small_button("Kein Duplikat", "Diese Dateien nie wieder zusammen melden");
    g_signal_connect(b3, "clicked", G_CALLBACK(on_group_notmatch), GINT_TO_POINTER(g));
    gtk_box_pack_end(GTK_BOX(head), b3, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(head), b2, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(head), b1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), head, FALSE, FALSE, 0);
    gb->list = add_group(outer, NULL);
    gb->frame = outer;
    gb->header = head;
    const DfGroup *gr = &ui.res.groups[g];
    for (int i = 0; i < gr->n; i++)
        if (!ui.removed[gr->first + i]) add_member_row(gb, gr->first + i);
    update_group_title(gb);
    g_signal_connect(outer, "destroy", G_CALLBACK(on_group_destroy), gb);
    ui.gboxes[g] = gb;
    gtk_box_pack_start(GTK_BOX(ui.res_list_box), outer, FALSE, FALSE, 0);
    gtk_widget_show_all(outer);
}

/* ---------------------------------------------------------------------------
 * Ansicht: Filter und Sortierung
 * ------------------------------------------------------------------------- */
static bool group_passes(int g, const char *q, int filter)
{
    const DfGroup *gr = &ui.res.groups[g];
    if (alive_count(g) < 2) return false;
    bool any = !q || !*q;
    bool has_img = false, has_vid = false, flipped = false, checked = false;
    float mn = 100;
    for (int i = 0; i < gr->n; i++) {
        int m = gr->first + i;
        if (ui.removed[m]) continue;
        const DfFile *f = member_file(m);
        if (!any && strcasestr(f->path, q)) any = true;
        if (f->image) has_img = true;
        else has_vid = true;
        if (ui.res.members[m].flipped) flipped = true;
        if (ui.checked[m]) checked = true;
        mn = MIN(mn, ui.res.members[m].similarity);
    }
    if (!any) return false;
    switch (filter) {
    case FILTER_VIDEOS: return has_vid;
    case FILTER_IMAGES: return has_img;
    case FILTER_PARTIAL: return gr->partial;
    case FILTER_FLIPPED: return flipped;
    case FILTER_IDENTICAL: return mn >= 99.99f;
    case FILTER_SIMILAR: return mn < 99.99f;
    case FILTER_CHECKED: return checked;
    default: return true;
    }
}

static int g_sort_mode;

static double group_sort_key(int g)
{
    const DfGroup *gr = &ui.res.groups[g];
    switch (g_sort_mode) {
    case SORT_SIM_DESC: return -gr->similarity;
    case SORT_SIM_ASC: return gr->similarity;
    case SORT_SPACE: return -(double)group_dup_bytes(g);
    case SORT_COUNT: return -alive_count(g);
    case SORT_DURATION: return -member_file(gr->first)->duration;
    default: return 0;
    }
}

static int cmp_view(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    if (g_sort_mode == SORT_PATH) {
        int c = strcmp(member_file(ui.res.groups[a].first)->path,
                       member_file(ui.res.groups[b].first)->path);
        return c ? c : a - b;
    }
    double ka = group_sort_key(a), kb = group_sort_key(b);
    if (ka != kb) return ka < kb ? -1 : 1;
    return a - b;
}

void results_rebuild_view(void)
{
    if (!ui.have_result) return;
    g_atomic_int_inc(&ui.thumb_generation);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(ui.res_list_box));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(kids);
    const char *q = gtk_entry_get_text(GTK_ENTRY(ui.res_search));
    int filter = MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(ui.res_filter_combo)));
    g_sort_mode = MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(ui.res_sort_combo)));
    g_free(ui.view);
    ui.view = g_new(int, MAX(ui.res.ngroups, 1));
    ui.nview = 0;
    for (int g = 0; g < ui.res.ngroups; g++)
        if (group_passes(g, q, filter)) ui.view[ui.nview++] = g;
    qsort(ui.view, ui.nview, sizeof *ui.view, cmp_view);
    ui.shown = 0;
    show_more_groups();
    gtk_widget_set_visible(ui.res_empty, ui.nview == 0);
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ui.res_scroller)),
                             0);
    update_selection_label();
}

static void show_more_groups(void)
{
    int end = MIN(ui.shown + GROUPS_BATCH, ui.nview);
    for (int i = ui.shown; i < end; i++) add_group_box(ui.view[i], i + 1);
    ui.shown = end;
    int rest = ui.nview - ui.shown;
    if (rest > 0) {
        char *t = g_strdup_printf("Weitere %d Gruppen anzeigen (%d übrig)", MIN(rest, GROUPS_BATCH),
                                  rest);
        gtk_button_set_label(GTK_BUTTON(ui.res_more_button), t);
        g_free(t);
    }
    gtk_widget_set_visible(ui.res_more_button, rest > 0);
}

static void on_more_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    show_more_groups();
}

/* beim Scrollen ans Ende automatisch nachladen */
static void on_edge_reached(GtkScrolledWindow *sw, GtkPositionType pos, gpointer d)
{
    (void)sw;
    (void)d;
    if (pos == GTK_POS_BOTTOM && ui.shown < ui.nview) show_more_groups();
}

static gboolean on_filter_timer(gpointer d)
{
    (void)d;
    ui.filter_timer = 0;
    results_rebuild_view();
    return G_SOURCE_REMOVE;
}

static void on_filter_changed(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    if (ui.filter_timer) g_source_remove(ui.filter_timer);
    ui.filter_timer = g_timeout_add(250, on_filter_timer, NULL);
}

static void update_summary(void)
{
    int groups = 0, dups = 0;
    long long bytes = 0;
    for (int g = 0; g < ui.res.ngroups; g++) {
        int a = alive_count(g);
        if (a < 2) continue;
        groups++;
        dups += a - 1;
        bytes += group_dup_bytes(g);
    }
    char sz[32];
    char *t = g_strdup_printf("%d Gruppen · %d Duplikate · bis zu %s frei, wenn je Gruppe nur eine "
                              "Datei bleibt", groups, dups, df_format_bytes((double)bytes, sz, sizeof sz));
    set_label_text(ui.res_summary, t);
    g_free(t);
}

static void update_selection_label(void)
{
    int n = 0;
    long long bytes = 0;
    for (int m = 0; m < ui.res.nmembers; m++)
        if (ui.checked[m] && !ui.removed[m]) {
            n++;
            bytes += member_file(m)->size;
        }
    char sz[32];
    char *t = n ? g_strdup_printf("%d markiert · %s", n, df_format_bytes((double)bytes, sz, sizeof sz))
                : g_strdup("Nichts markiert");
    set_label_text(ui.res_sel_label, t);
    g_free(t);
    update_summary();
}

/* ---------------------------------------------------------------------------
 * Speichern (laufend)
 * ------------------------------------------------------------------------- */
void results_save_now(void)
{
    if (ui.result_save_timer) {
        g_source_remove(ui.result_save_timer);
        ui.result_save_timer = 0;
    }
    if (!ui.have_result) return;
    char err[512];
    if (!df_result_save(&ui.res, ui.removed, ui.checked, DF_RESULT_PATH, err, sizeof err))
        df_log(false, "Ergebnis nicht gespeichert: %s", err);
}

static gboolean on_result_save_timer(gpointer d)
{
    (void)d;
    ui.result_save_timer = 0;
    results_save_now();
    return G_SOURCE_REMOVE;
}

static void schedule_result_save(void)
{
    if (ui.result_save_timer) g_source_remove(ui.result_save_timer);
    ui.result_save_timer = g_timeout_add(2000, on_result_save_timer, NULL);
}

/* ---------------------------------------------------------------------------
 * Ergebnis setzen / laden
 * ------------------------------------------------------------------------- */
void results_clear(void)
{
    if (!ui.have_result) return;
    g_atomic_int_inc(&ui.thumb_generation);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(ui.res_list_box));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(kids);
    df_result_free(&ui.res);
    g_free(ui.removed);
    g_free(ui.checked);
    g_free(ui.mrows);
    g_free(ui.gboxes);
    g_free(ui.view);
    ui.removed = ui.checked = NULL;
    ui.mrows = NULL;
    ui.gboxes = NULL;
    ui.view = NULL;
    ui.nview = ui.shown = 0;
    ui.have_result = false;
}

void results_set(DfResult *r, bool *removed, bool *checked)
{
    results_clear();
    ui.res = *r;
    memset(r, 0, sizeof *r);
    int nm = MAX(ui.res.nmembers, 1);
    ui.removed = removed ? removed : g_new0(bool, nm);
    ui.checked = checked ? checked : g_new0(bool, nm);
    ui.mrows = g_new0(MemberRow *, nm);
    ui.gboxes = g_new0(GroupBox *, MAX(ui.res.ngroups, 1));
    ui.have_result = true;
    results_rebuild_view();
    results_save_now();
}

bool results_load_file(const char *path, bool quiet)
{
    DfResult r;
    bool *removed, *checked;
    char err[512];
    if (!df_result_load(&r, &removed, &checked, path, err, sizeof err)) {
        if (!quiet) show_info_bar(GTK_MESSAGE_ERROR, err);
        return false;
    }
    results_set(&r, removed, checked);
    show_page("results");
    return true;
}

void results_shutdown(void)
{
    results_save_now();
    if (ui.thumb_pool) {
        g_atomic_int_inc(&ui.thumb_generation);
        g_thread_pool_free(ui.thumb_pool, TRUE, FALSE);
        ui.thumb_pool = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Entfernen aus der Liste (nach Aktionen oder von Hand)
 * ------------------------------------------------------------------------- */
static void remove_members(const int *ms, int n, bool rebuild)
{
    for (int i = 0; i < n; i++) {
        int m = ms[i];
        ui.removed[m] = true;
        ui.checked[m] = false;
        if (ui.mrows[m]) gtk_widget_destroy(ui.mrows[m]->row);
    }
    if (rebuild) {
        results_rebuild_view();
    } else {
        /* betroffene Gruppen: Titel neu oder ganz ausblenden */
        for (int i = 0; i < n; i++) {
            int g = group_of_member(ms[i]);
            GroupBox *gb = ui.gboxes[g];
            if (!gb) continue;
            if (alive_count(g) < 2) gtk_widget_destroy(gb->frame);
            else update_group_title(gb);
        }
    }
    update_selection_label();
    schedule_result_save();
}

/* ---------------------------------------------------------------------------
 * Menü einer Zeile
 * ------------------------------------------------------------------------- */
static void rename_member(int m)
{
    const DfFile *f = member_file(m);
    char *base = g_path_get_basename(f->path);
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Umbenennen", GTK_WINDOW(ui.win),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_USE_HEADER_BAR,
                                                 "Abbrechen", GTK_RESPONSE_CANCEL, "Umbenennen",
                                                 GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget *e = gtk_entry_new();
    char *disp = g_filename_display_name(base);
    gtk_entry_set_text(GTK_ENTRY(e), disp);
    gtk_entry_set_width_chars(GTK_ENTRY(e), 60);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_widget_set_margin_start(e, 18);
    gtk_widget_set_margin_end(e, 18);
    gtk_widget_set_margin_bottom(e, 18);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), e);
    gtk_widget_show_all(dlg);
    const char *dot = strrchr(disp, '.');
    gtk_editable_select_region(GTK_EDITABLE(e), 0, dot ? (int)g_utf8_pointer_to_offset(disp, dot) : -1);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        char *newname = g_filename_from_utf8(gtk_entry_get_text(GTK_ENTRY(e)), -1, NULL, NULL, NULL);
        char *np = NULL, err[256];
        if (newname && df_rename_file(f->path, newname, &np, err, sizeof err)) {
            DfFile *fm = &ui.res.files[ui.res.members[m].file];
            g_free(fm->path);
            fm->path = np;
            MemberRow *mr = ui.mrows[m];
            if (mr) {
                char *d2 = path_for_display(np);
                char *b2 = g_path_get_basename(d2);
                set_label_text(mr->title, b2);
                gtk_widget_set_tooltip_text(mr->title, d2);
                g_free(d2);
                g_free(b2);
            }
            schedule_result_save();
        } else {
            char *msg = g_strdup_printf("Umbenennen fehlgeschlagen: %s", newname ? err : "ungültiger Name");
            show_info_bar(GTK_MESSAGE_ERROR, msg);
            g_free(msg);
        }
        g_free(newname);
    }
    gtk_widget_destroy(dlg);
    g_free(disp);
    g_free(base);
}

static void row_menu_action(GtkMenuItem *item, gpointer d)
{
    const char *a = g_object_get_data(G_OBJECT(item), "action");
    int m = GPOINTER_TO_INT(d);
    if (!ui.have_result || m >= ui.res.nmembers || ui.removed[m]) return;
    const DfFile *f = member_file(m);
    if (!strcmp(a, "open")) {
        open_in_default_app(f->path);
    } else if (!strcmp(a, "folder")) {
        show_in_file_manager(f->path);
    } else if (!strcmp(a, "rename")) {
        rename_member(m);
    } else if (!strcmp(a, "copy")) {
        char *disp = path_for_display(f->path);
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), disp, -1);
        g_free(disp);
    } else if (!strcmp(a, "keep")) {
        int g = group_of_member(m);
        const DfGroup *gr = &ui.res.groups[g];
        for (int i = 0; i < gr->n; i++) {
            int x = gr->first + i;
            if (!ui.removed[x]) ui.checked[x] = x != m;
        }
        sync_checks();
    } else if (!strcmp(a, "compare")) {
        compare_window_open(group_of_member(m));
    } else if (!strcmp(a, "remove") || !strcmp(a, "ignore")) {
        if (!strcmp(a, "ignore")) df_ignore_add(f->path);
        remove_members(&m, 1, false);
    }
}

/* ---------------------------------------------------------------------------
 * Auswahl (Menü "Auswählen")
 * ------------------------------------------------------------------------- */
static char *ask_text(const char *title, const char *text, const char *placeholder)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(title, GTK_WINDOW(ui.win),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_USE_HEADER_BAR,
                                                 "Abbrechen", GTK_RESPONSE_CANCEL, "Markieren",
                                                 GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_box_pack_start(GTK_BOX(box), set_wrapping(new_label(text, "df-sub"), 60), FALSE, FALSE, 0);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_box_pack_start(GTK_BOX(box), e, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box);
    gtk_widget_show_all(dlg);
    char *r = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK && *gtk_entry_get_text(GTK_ENTRY(e)))
        r = g_strdup(gtk_entry_get_text(GTK_ENTRY(e)));
    gtk_widget_destroy(dlg);
    return r;
}

static bool ask_range(double *from, double *to)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Nach Ähnlichkeit markieren", GTK_WINDOW(ui.win),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_USE_HEADER_BAR,
                                                 "Abbrechen", GTK_RESPONSE_CANCEL, "Markieren",
                                                 GTK_RESPONSE_OK, NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_box_pack_start(GTK_BOX(box), set_wrapping(new_label("Markiert in jeder Gruppe alle Dateien "
                       "mit einer Ähnlichkeit in diesem Bereich – außer der besten.", "df-sub"), 60),
                       FALSE, FALSE, 0);
    GtkWidget *list = add_group(box, NULL);
    GtkWidget *a = new_spin(0, 100, 1, 99), *b = new_spin(0, 100, 1, 100);
    add_row(list, "Von (%)", NULL, NULL, a, NULL, NULL);
    add_row(list, "Bis (%)", NULL, NULL, b, NULL, NULL);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box);
    gtk_widget_show_all(dlg);
    bool ok = gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK;
    *from = gtk_spin_button_get_value(GTK_SPIN_BUTTON(a));
    *to = gtk_spin_button_get_value(GTK_SPIN_BUTTON(b));
    gtk_widget_destroy(dlg);
    return ok;
}

static void on_select_action(GSimpleAction *act, GVariant *p, gpointer d)
{
    (void)p;
    (void)d;
    if (!ui.have_result) return;
    const char *name = g_action_get_name(G_ACTION(act));
    DfSelectSpec s = {.order = ui.criteria, .norder = ui.ncriteria};
    char *text = NULL;
    if (!strcmp(name, "sel-lowest")) s.rule = DF_SEL_LOWEST_QUALITY;
    else if (!strcmp(name, "sel-newest")) s.rule = DF_SEL_KEEP_NEWEST;
    else if (!strcmp(name, "sel-oldest")) s.rule = DF_SEL_KEEP_OLDEST;
    else if (!strcmp(name, "sel-identical")) s.rule = DF_SEL_IDENTICAL;
    else if (!strcmp(name, "sel-identical-size")) s.rule = DF_SEL_IDENTICAL_SIZE;
    else if (!strcmp(name, "sel-partial")) s.rule = DF_SEL_PARTIAL_CLIPS;
    else if (!strcmp(name, "sel-invert")) s.rule = DF_SEL_INVERT;
    else if (!strcmp(name, "sel-none")) s.rule = DF_SEL_NONE;
    else if (!strcmp(name, "sel-path") || !strcmp(name, "sel-notpath")) {
        bool inv = !strcmp(name, "sel-notpath");
        text = ask_text(inv ? "Markieren, wenn der Pfad NICHT enthält …" : "Markieren, wenn der Pfad "
                        "enthält …", inv ? "Markiert Dateien, deren Pfad den Text nicht enthält. "
                        "Treffen alle Dateien einer Gruppe zu, bleibt die beste unmarkiert."
                        : "Markiert Dateien, deren Pfad den Text enthält (* und ? als Platzhalter "
                        "möglich). Treffen alle Dateien einer Gruppe zu, bleibt die beste "
                        "unmarkiert.", "z. B. /Downloads/");
        if (!text) return;
        s.rule = inv ? DF_SEL_PATH_NOT_CONTAINS : DF_SEL_PATH_CONTAINS;
        s.text = text;
        s.add = true;
    } else if (!strcmp(name, "sel-range")) {
        if (!ask_range(&s.sim_from, &s.sim_to)) return;
        s.rule = DF_SEL_SIMILARITY_RANGE;
    } else {
        return;
    }
    /* Auswahl nur in den angezeigten (gefilterten) Gruppen */
    bool *vis_removed = g_new(bool, MAX(ui.res.nmembers, 1));
    memcpy(vis_removed, ui.removed, sizeof(bool) * (size_t)ui.res.nmembers);
    bool *in_view = g_new0(bool, MAX(ui.res.ngroups, 1));
    for (int i = 0; i < ui.nview; i++) in_view[ui.view[i]] = true;
    bool *backup = g_memdup2(ui.checked, sizeof(bool) * (size_t)MAX(ui.res.nmembers, 1));
    for (int g = 0; g < ui.res.ngroups; g++)
        if (!in_view[g])
            for (int i = 0; i < ui.res.groups[g].n; i++) vis_removed[ui.res.groups[g].first + i] = true;
    df_select(&ui.res, ui.checked, vis_removed, &s);
    for (int g = 0; g < ui.res.ngroups; g++)   /* ausgeblendete Gruppen behalten ihre Auswahl */
        if (!in_view[g])
            for (int i = 0; i < ui.res.groups[g].n; i++) {
                int m = ui.res.groups[g].first + i;
                ui.checked[m] = backup[m];
            }
    g_free(backup);
    g_free(in_view);
    g_free(vis_removed);
    g_free(text);
    sync_checks();
}

/* ---------------------------------------------------------------------------
 * Aktionen mit den markierten Dateien (Thread + Fortschrittsfenster)
 * ------------------------------------------------------------------------- */
typedef struct {
    DfAction act;
    char *target_dir;
    int *ms;                    /* Mitglieder */
    char **paths, **targets;    /* je Mitglied: Pfad und ggf. Link-Ziel */
    int n;
    volatile int done;
    volatile bool cancel;
    bool *ok;
    char **errors;
    char **new_paths;
    GtkWidget *dlg, *bar, *label;
    guint timer;
    bool finished;
    bool ignore_after;          /* entfernen + ignorieren */
} ActJob;

static gpointer action_thread(gpointer d)
{
    ActJob *j = d;
    for (int i = 0; i < j->n && !j->cancel; i++) {
        char err[512] = "";
        j->ok[i] = df_file_action(j->act, j->paths[i], j->act == DF_ACT_MOVE || j->act == DF_ACT_COPY
                                  ? j->target_dir : j->targets[i], &j->new_paths[i], err, sizeof err);
        if (!j->ok[i]) j->errors[i] = g_strdup(err);
        g_atomic_int_inc(&j->done);
    }
    j->finished = true;
    return NULL;
}

static void on_action_cancel(GtkDialog *dlg, gint resp, gpointer d)
{
    (void)dlg;
    (void)resp;
    ActJob *j = d;
    j->cancel = true;
}

static void action_finish(ActJob *j)
{
    int ok = 0, failed = 0;
    GString *errs = g_string_new(NULL);
    int *rm = g_new(int, MAX(j->n, 1)), nrm = 0;
    for (int i = 0; i < j->done; i++) {
        int m = j->ms[i];
        if (j->ok[i]) {
            ok++;
            if (j->act == DF_ACT_COPY) {
                ui.checked[m] = false;
            } else {
                rm[nrm++] = m;
                if (j->act == DF_ACT_MOVE && j->new_paths[i]) {
                    DfFile *f = &ui.res.files[ui.res.members[m].file];
                    g_free(f->path);
                    f->path = g_strdup(j->new_paths[i]);
                }
            }
        } else {
            failed++;
            if (failed <= 8) {
                char *disp = path_for_display(j->paths[i]);
                char *base = g_path_get_basename(disp);
                g_string_append_printf(errs, "\n%s: %s", base, j->errors[i] ? j->errors[i] : "?");
                g_free(disp);
                g_free(base);
            }
        }
    }
    remove_members(rm, nrm, false);
    if (j->act == DF_ACT_COPY) sync_checks();
    g_free(rm);
    static const char *const verb[] = {"in den Papierkorb gelegt", "gelöscht", "verschoben",
                                       "kopiert", "durch Hardlinks ersetzt",
                                       "durch symbolische Links ersetzt"};
    char fail[64] = ".";
    if (failed) snprintf(fail, sizeof fail, ", %d fehlgeschlagen:", failed);
    char *msg = g_strdup_printf("%d Datei%s %s%s%s%s", ok, ok == 1 ? "" : "en", verb[j->act], fail,
                                errs->str, failed > 8 ? "\n…" : "");
    show_info_bar(failed ? GTK_MESSAGE_WARNING : GTK_MESSAGE_INFO, msg);
    g_free(msg);
    g_string_free(errs, TRUE);
    update_db_row();
}

static void act_job_free(ActJob *j)
{
    for (int i = 0; i < j->n; i++) {
        g_free(j->paths[i]);
        g_free(j->targets[i]);
        g_free(j->errors[i]);
        g_free(j->new_paths[i]);
    }
    g_free(j->paths);
    g_free(j->targets);
    g_free(j->errors);
    g_free(j->new_paths);
    g_free(j->ok);
    g_free(j->ms);
    g_free(j->target_dir);
    g_free(j);
}

static gboolean on_action_timer(gpointer d)
{
    ActJob *j = d;
    int done = g_atomic_int_get(&j->done);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(j->bar), j->n ? (double)done / j->n : 1);
    char *t = g_strdup_printf("%d von %d", done, j->n);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(j->bar), t);
    g_free(t);
    if (!j->finished) return G_SOURCE_CONTINUE;
    gtk_widget_destroy(j->dlg);
    session_inhibit(false, NULL);
    action_finish(j);
    act_job_free(j);
    return G_SOURCE_REMOVE;
}

static void run_action(DfAction act)
{
    if (!ui.have_result) return;
    int n = 0;
    long long bytes = 0;
    for (int m = 0; m < ui.res.nmembers; m++)
        if (ui.checked[m] && !ui.removed[m]) {
            n++;
            bytes += member_file(m)->size;
        }
    if (!n) {
        show_info_bar(GTK_MESSAGE_INFO, "Es ist nichts markiert.");
        return;
    }
    /* Gruppen, in denen alles markiert ist */
    int all_groups = 0;
    for (int g = 0; g < ui.res.ngroups; g++) {
        const DfGroup *gr = &ui.res.groups[g];
        int alive = 0, chk = 0;
        for (int i = 0; i < gr->n; i++) {
            int m = gr->first + i;
            if (ui.removed[m]) continue;
            alive++;
            chk += ui.checked[m];
        }
        if (alive >= 2 && chk == alive) all_groups++;
    }
    char sz[32];
    df_format_bytes((double)bytes, sz, sizeof sz);
    char *warn = all_groups ? g_strdup_printf("\n\nAchtung: In %d Gruppe%s sind ALLE Dateien markiert "
                                              "– danach bliebe dort keine übrig.", all_groups,
                                              all_groups == 1 ? "" : "n")
                            : g_strdup("");
    char *target_dir = NULL;
    bool go = false;
    char *q;
    switch (act) {
    case DF_ACT_TRASH:
        q = g_strdup_printf("%d Dateien (%s) werden in den Papierkorb gelegt – von dort lassen sie "
                            "sich wiederherstellen.%s", n, sz, warn);
        go = ask_confirmation("In den Papierkorb legen?", q, "Abbrechen", "In den Papierkorb", true);
        g_free(q);
        break;
    case DF_ACT_DELETE:
        q = g_strdup_printf("%d Dateien (%s) werden endgültig gelöscht – ohne Papierkorb, nicht "
                            "rückgängig zu machen.%s", n, sz, warn);
        go = ask_confirmation("Endgültig löschen?", q, "Abbrechen", "Endgültig löschen", true);
        g_free(q);
        break;
    case DF_ACT_MOVE:
    case DF_ACT_COPY:
        target_dir = choose_folder(act == DF_ACT_MOVE ? "Markierte verschieben nach" :
                                   "Markierte kopieren nach", NULL);
        go = target_dir != NULL;
        break;
    case DF_ACT_HARDLINK:
    case DF_ACT_SYMLINK:
        q = g_strdup_printf("Jede markierte Datei wird durch einen %s auf die unmarkierte (beste) "
                            "Datei ihrer Gruppe ersetzt. %s%s",
                            act == DF_ACT_HARDLINK ? "Hardlink" : "symbolischen Link",
                            act == DF_ACT_HARDLINK ? "Das spart den Platz, der Dateiname bleibt; "
                            "beide Namen zeigen danach auf denselben Inhalt (nur auf demselben "
                            "Laufwerk möglich)." : "Wird das Ziel später gelöscht, ist der Link "
                            "kaputt.", warn);
        go = ask_confirmation(act == DF_ACT_HARDLINK ? "Durch Hardlinks ersetzen?"
                                                     : "Durch symbolische Links ersetzen?", q,
                              "Abbrechen", "Ersetzen", true);
        g_free(q);
        break;
    }
    g_free(warn);
    if (!go) {
        g_free(target_dir);
        return;
    }
    ActJob *j = g_new0(ActJob, 1);
    j->act = act;
    j->target_dir = target_dir;
    j->ms = g_new(int, n);
    j->paths = g_new0(char *, n);
    j->targets = g_new0(char *, n);
    j->errors = g_new0(char *, n);
    j->new_paths = g_new0(char *, n);
    j->ok = g_new0(bool, n);
    int skipped = 0;
    for (int g = 0; g < ui.res.ngroups; g++) {
        const DfGroup *gr = &ui.res.groups[g];
        int keep = -1;
        if (act == DF_ACT_HARDLINK || act == DF_ACT_SYMLINK) {
            /* Ziel: beste unmarkierte Datei der Gruppe */
            bool *excl = g_new(bool, ui.res.nmembers);
            for (int m = 0; m < ui.res.nmembers; m++) excl[m] = ui.removed[m] || ui.checked[m];
            keep = df_best_member(&ui.res, g, ui.criteria, ui.ncriteria, excl);
            g_free(excl);
        }
        for (int i = 0; i < gr->n; i++) {
            int m = gr->first + i;
            if (!ui.checked[m] || ui.removed[m]) continue;
            if ((act == DF_ACT_HARDLINK || act == DF_ACT_SYMLINK) && keep < 0) {
                skipped++;
                continue;
            }
            j->ms[j->n] = m;
            j->paths[j->n] = g_strdup(member_file(m)->path);
            if (keep >= 0) j->targets[j->n] = g_strdup(member_file(keep)->path);
            j->n++;
        }
    }
    if (skipped)
        show_info_bar(GTK_MESSAGE_WARNING, "In manchen Gruppen ist alles markiert – dort gibt es "
                      "keine Datei, auf die verlinkt werden kann; sie werden übersprungen.");
    j->dlg = gtk_dialog_new_with_buttons("Wird ausgeführt …", GTK_WINDOW(ui.win),
                                         GTK_DIALOG_MODAL | GTK_DIALOG_USE_HEADER_BAR,
                                         "Anhalten", GTK_RESPONSE_CANCEL, NULL);
    gtk_window_set_default_size(GTK_WINDOW(j->dlg), 420, -1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    j->bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(j->bar), TRUE);
    gtk_box_pack_start(GTK_BOX(box), j->bar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(j->dlg))), box);
    g_signal_connect(j->dlg, "response", G_CALLBACK(on_action_cancel), j);
    g_signal_connect(j->dlg, "delete-event", G_CALLBACK(gtk_true), NULL);
    gtk_widget_show_all(j->dlg);
    session_inhibit(true, "Duplikat-Finder verschiebt bzw. löscht Dateien");
    g_thread_unref(g_thread_new("action", action_thread, j));
    j->timer = g_timeout_add(150, on_action_timer, j);
}

static void on_file_action(GSimpleAction *act, GVariant *p, gpointer d)
{
    (void)p;
    (void)d;
    const char *name = g_action_get_name(G_ACTION(act));
    if (!strcmp(name, "act-trash")) run_action(DF_ACT_TRASH);
    else if (!strcmp(name, "act-delete")) run_action(DF_ACT_DELETE);
    else if (!strcmp(name, "act-move")) run_action(DF_ACT_MOVE);
    else if (!strcmp(name, "act-copy")) run_action(DF_ACT_COPY);
    else if (!strcmp(name, "act-hardlink")) run_action(DF_ACT_HARDLINK);
    else if (!strcmp(name, "act-symlink")) run_action(DF_ACT_SYMLINK);
    else if (!strcmp(name, "act-remove") || !strcmp(name, "act-ignore")) {
        bool ignore = !strcmp(name, "act-ignore");
        int *ms = g_new(int, MAX(ui.res.nmembers, 1)), n = 0;
        for (int m = 0; m < ui.res.nmembers; m++)
            if (ui.checked[m] && !ui.removed[m]) {
                ms[n++] = m;
                if (ignore) df_ignore_add(member_file(m)->path);
            }
        if (n) remove_members(ms, n, false);
        else show_info_bar(GTK_MESSAGE_INFO, "Es ist nichts markiert.");
        g_free(ms);
    } else if (!strcmp(name, "act-copy-paths")) {
        GString *s = g_string_new(NULL);
        for (int m = 0; m < ui.res.nmembers; m++)
            if (ui.checked[m] && !ui.removed[m]) {
                char *disp = path_for_display(member_file(m)->path);
                g_string_append_printf(s, "%s\n", disp);
                g_free(disp);
            }
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), s->str, -1);
        g_string_free(s, TRUE);
        show_info_bar(GTK_MESSAGE_INFO, "Pfade der markierten Dateien kopiert.");
    }
}

static void on_export_action(GSimpleAction *act, GVariant *p, gpointer d)
{
    (void)p;
    (void)d;
    if (!ui.have_result) return;
    const char *name = g_action_get_name(G_ACTION(act));
    char err[512];
    bool ok = true;
    char *f = NULL;
    if (!strcmp(name, "exp-json")) {
        f = choose_file("Ergebnis speichern", true, "duplikate.json", "Ergebnisse (*.json)", "*.json");
        if (f) ok = df_result_save(&ui.res, ui.removed, ui.checked, f, err, sizeof err);
    } else if (!strcmp(name, "exp-csv")) {
        f = choose_file("Als Tabelle exportieren", true, "duplikate.csv", "CSV (*.csv)", "*.csv");
        if (f) ok = df_result_export_csv(&ui.res, ui.removed, ui.checked, f, err, sizeof err);
    } else if (!strcmp(name, "exp-html")) {
        f = choose_file("Als Webseite exportieren", true, "duplikate.html", "HTML (*.html)", "*.html");
        if (f) ok = df_result_export_html(&ui.res, ui.removed, ui.checked, f, err, sizeof err);
    }
    if (f) show_info_bar(ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR, ok ? "Gespeichert." : err);
    g_free(f);
}

static void on_rescan_action(GSimpleAction *act, GVariant *p, gpointer d)
{
    (void)act;
    (void)p;
    (void)d;
    if (!ui.have_result) return;
    int n = 0;
    for (int m = 0; m < ui.res.nmembers; m++)
        if (ui.checked[m] && !ui.removed[m]) {
            df_db_remove_path(member_file(m)->path);
            n++;
        }
    char *msg = g_strdup_printf("%d Dateien werden beim nächsten Suchlauf neu gelesen.", n);
    show_info_bar(GTK_MESSAGE_INFO, msg);
    g_free(msg);
}

/* ---------------------------------------------------------------------------
 * Seite aufbauen
 * ------------------------------------------------------------------------- */
static GtkWidget *menu_button(const char *label, GMenu *menu)
{
    GtkWidget *b = gtk_menu_button_new();
    GtkWidget *bx = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(bx), gtk_label_new(label), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bx), gtk_image_new_from_icon_name("pan-down-symbolic",
                                                                 GTK_ICON_SIZE_BUTTON), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b), bx);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(b), G_MENU_MODEL(menu));
    g_object_unref(menu);
    return b;
}

static GtkWidget *combo_of(const char *const *t, int n)
{
    GtkWidget *c = gtk_combo_box_text_new();
    for (int i = 0; i < n; i++) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), NULL, t[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(c), 0);
    g_signal_connect(c, "changed", G_CALLBACK(on_filter_changed), NULL);
    return c;
}

static void on_key_trash(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
}

static gboolean on_results_key(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    (void)w;
    (void)d;
    if (strcmp(gtk_stack_get_visible_child_name(GTK_STACK(ui.stack)), "results") != 0) return FALSE;
    if (e->keyval == GDK_KEY_Delete && !gtk_widget_has_focus(ui.res_search)) {
        run_action((e->state & GDK_SHIFT_MASK) ? DF_ACT_DELETE : DF_ACT_TRASH);
        return TRUE;
    }
    if (e->keyval == GDK_KEY_f && (e->state & GDK_CONTROL_MASK)) {
        gtk_widget_grab_focus(ui.res_search);
        return TRUE;
    }
    return FALSE;
}

GtkWidget *build_results_page(void)
{
    (void)on_key_trash;
    ui.thumb_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    static const GActionEntry entries[] = {
        {.name = "sel-lowest", .activate = on_select_action},
        {.name = "sel-newest", .activate = on_select_action},
        {.name = "sel-oldest", .activate = on_select_action},
        {.name = "sel-identical", .activate = on_select_action},
        {.name = "sel-identical-size", .activate = on_select_action},
        {.name = "sel-partial", .activate = on_select_action},
        {.name = "sel-path", .activate = on_select_action},
        {.name = "sel-notpath", .activate = on_select_action},
        {.name = "sel-range", .activate = on_select_action},
        {.name = "sel-invert", .activate = on_select_action},
        {.name = "sel-none", .activate = on_select_action},
        {.name = "act-trash", .activate = on_file_action},
        {.name = "act-delete", .activate = on_file_action},
        {.name = "act-move", .activate = on_file_action},
        {.name = "act-copy", .activate = on_file_action},
        {.name = "act-hardlink", .activate = on_file_action},
        {.name = "act-symlink", .activate = on_file_action},
        {.name = "act-remove", .activate = on_file_action},
        {.name = "act-ignore", .activate = on_file_action},
        {.name = "act-copy-paths", .activate = on_file_action},
        {.name = "act-rescan", .activate = on_rescan_action},
        {.name = "exp-json", .activate = on_export_action},
        {.name = "exp-csv", .activate = on_export_action},
        {.name = "exp-html", .activate = on_export_action},
    };
    GSimpleActionGroup *group = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(group), entries, G_N_ELEMENTS(entries), NULL);
    gtk_widget_insert_action_group(page, "res", G_ACTION_GROUP(group));
    g_object_unref(group);

    /* Werkzeugleiste */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    add_css_class(bar, "df-toolbar");
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GMenu *sel = g_menu_new(), *s1 = g_menu_new(), *s2 = g_menu_new(), *s3 = g_menu_new();
    g_menu_append(s1, "Niedrigere Qualität markieren (beste behalten)", "res.sel-lowest");
    g_menu_append(s1, "Ältere markieren (neueste behalten)", "res.sel-newest");
    g_menu_append(s1, "Neuere markieren (älteste behalten)", "res.sel-oldest");
    g_menu_append(s1, "100 % gleiche markieren (beste behalten)", "res.sel-identical");
    g_menu_append(s1, "100 % gleiche mit gleicher Größe markieren", "res.sel-identical-size");
    g_menu_append(s1, "Teilclips markieren (Quelle behalten)", "res.sel-partial");
    g_menu_append_section(sel, NULL, G_MENU_MODEL(s1));
    g_menu_append(s2, "Pfad enthält … markieren", "res.sel-path");
    g_menu_append(s2, "Pfad enthält nicht … markieren", "res.sel-notpath");
    g_menu_append(s2, "Nach Ähnlichkeit markieren …", "res.sel-range");
    g_menu_append_section(sel, NULL, G_MENU_MODEL(s2));
    g_menu_append(s3, "Auswahl umkehren", "res.sel-invert");
    g_menu_append(s3, "Nichts markieren", "res.sel-none");
    g_menu_append_section(sel, NULL, G_MENU_MODEL(s3));
    g_object_unref(s1);
    g_object_unref(s2);
    g_object_unref(s3);
    gtk_box_pack_start(GTK_BOX(row1), menu_button("Auswählen", sel), FALSE, FALSE, 0);

    GMenu *act = g_menu_new(), *a1 = g_menu_new(), *a2 = g_menu_new(), *a3 = g_menu_new();
    g_menu_append(a1, "In den Papierkorb (Entf)", "res.act-trash");
    g_menu_append(a1, "Endgültig löschen (Umschalt+Entf)", "res.act-delete");
    g_menu_append(a1, "Verschieben nach …", "res.act-move");
    g_menu_append(a1, "Kopieren nach …", "res.act-copy");
    g_menu_append_section(act, NULL, G_MENU_MODEL(a1));
    g_menu_append(a2, "Durch Hardlinks auf die beste Datei ersetzen", "res.act-hardlink");
    g_menu_append(a2, "Durch symbolische Links ersetzen", "res.act-symlink");
    g_menu_append_section(act, NULL, G_MENU_MODEL(a2));
    g_menu_append(a3, "Aus der Liste entfernen", "res.act-remove");
    g_menu_append(a3, "Aus der Liste entfernen und künftig ignorieren", "res.act-ignore");
    g_menu_append(a3, "Beim nächsten Suchlauf neu lesen", "res.act-rescan");
    g_menu_append(a3, "Pfade kopieren", "res.act-copy-paths");
    g_menu_append_section(act, NULL, G_MENU_MODEL(a3));
    g_object_unref(a1);
    g_object_unref(a2);
    g_object_unref(a3);
    GtkWidget *actb = menu_button("Markierte", act);
    add_css_class(actb, "destructive-action");
    gtk_box_pack_start(GTK_BOX(row1), actb, FALSE, FALSE, 0);

    GMenu *exp = g_menu_new();
    g_menu_append(exp, "Ergebnis speichern unter …", "res.exp-json");
    g_menu_append(exp, "Als Tabelle (CSV) …", "res.exp-csv");
    g_menu_append(exp, "Als Webseite (HTML) …", "res.exp-html");
    gtk_box_pack_start(GTK_BOX(row1), menu_button("Exportieren", exp), FALSE, FALSE, 0);
    ui.res_sel_label = new_label("", "df-sub");
    gtk_widget_set_margin_start(ui.res_sel_label, 8);
    gtk_box_pack_start(GTK_BOX(row1), ui.res_sel_label, FALSE, FALSE, 0);
    ui.res_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.res_search), "In Pfaden suchen (Strg+F)");
    gtk_entry_set_width_chars(GTK_ENTRY(ui.res_search), 26);
    g_signal_connect(ui.res_search, "search-changed", G_CALLBACK(on_filter_changed), NULL);
    gtk_box_pack_end(GTK_BOX(row1), ui.res_search, FALSE, FALSE, 0);
    ui.res_sort_combo = combo_of(SORT_TEXT, G_N_ELEMENTS(SORT_TEXT));
    gtk_box_pack_end(GTK_BOX(row1), ui.res_sort_combo, FALSE, FALSE, 0);
    ui.res_filter_combo = combo_of(FILTER_TEXT, G_N_ELEMENTS(FILTER_TEXT));
    gtk_box_pack_end(GTK_BOX(row1), ui.res_filter_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), row1, FALSE, FALSE, 0);
    ui.res_summary = new_label("", "df-sub");
    add_css_class(ui.res_summary, "dim-label");
    gtk_box_pack_start(GTK_BOX(bar), ui.res_summary, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), bar, FALSE, FALSE, 0);

    /* Liste */
    GtkWidget *scroller;
    GtkWidget *col = new_page_column(&scroller, 1500);
    ui.res_scroller = scroller;
    g_signal_connect(scroller, "edge-reached", G_CALLBACK(on_edge_reached), NULL);
    ui.res_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(col), ui.res_list_box, FALSE, FALSE, 0);
    ui.res_empty = new_label("Keine Duplikate – oder keine, die zum Filter passen.", "df-lead");
    add_css_class(ui.res_empty, "dim-label");
    gtk_widget_set_halign(ui.res_empty, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(ui.res_empty, 60);
    gtk_box_pack_start(GTK_BOX(col), ui.res_empty, FALSE, FALSE, 0);
    ui.res_more_button = gtk_button_new_with_label("");
    add_css_class(ui.res_more_button, "df-pill");
    gtk_widget_set_halign(ui.res_more_button, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(ui.res_more_button, 18);
    g_signal_connect(ui.res_more_button, "clicked", G_CALLBACK(on_more_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(col), ui.res_more_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), scroller, TRUE, TRUE, 0);
    g_signal_connect(page, "realize", G_CALLBACK(gtk_widget_grab_focus), NULL);
    g_signal_connect_after(page, "map", G_CALLBACK(gtk_widget_grab_focus), NULL);
    g_signal_connect(page, "key-press-event", G_CALLBACK(on_results_key), NULL);
    gtk_widget_set_can_focus(page, TRUE);
    return page;
}

/* ---------------------------------------------------------------------------
 * Vergleichsfenster: alle Vergleichsbilder einer Gruppe nebeneinander
 * ------------------------------------------------------------------------- */
typedef struct {
    GtkWidget *win, *grid, *scale;
    int g;
    int *ms, n;
    int k;
    int width;
    int generation;
    GtkWidget **cells;          /* n*k Bilder */
    GtkWidget **checks;
} CompareWin;

typedef struct {
    CompareWin *cw;
    int col, generation;
    char *path;
    DfThumbs t;
    bool ok;
} CompareJob;

static GList *g_compare_windows;

static gboolean on_compare_thumbs(gpointer p)
{
    CompareJob *j = p;
    if (g_list_find(g_compare_windows, j->cw) && j->generation == j->cw->generation && j->ok) {
        for (int i = 0; i < j->t.n && i < j->cw->k; i++) {
            if (!j->t.rgb[i]) continue;
            GdkPixbuf *pb = gdk_pixbuf_new_from_data(j->t.rgb[i], GDK_COLORSPACE_RGB, FALSE, 8,
                                                     j->t.w[i], j->t.h[i], j->t.w[i] * 3, free_rgb,
                                                     NULL);
            j->t.rgb[i] = NULL;
            gtk_image_set_from_pixbuf(GTK_IMAGE(j->cw->cells[j->col * j->cw->k + i]), pb);
            g_object_unref(pb);
        }
    }
    df_thumbs_free(&j->t);
    g_free(j->path);
    g_free(j);
    return G_SOURCE_REMOVE;
}

static gpointer compare_thread(gpointer p)
{
    CompareJob *j = p;
    j->ok = df_thumbs_get(j->path, j->cw->k, j->cw->width, true, &j->t);
    g_idle_add(on_compare_thumbs, j);
    return NULL;
}

static void compare_load(CompareWin *cw)
{
    cw->generation++;
    for (int c = 0; c < cw->n; c++) {
        CompareJob *j = g_new0(CompareJob, 1);
        j->cw = cw;
        j->col = c;
        j->generation = cw->generation;
        j->path = g_strdup(member_file(cw->ms[c])->path);
        g_thread_unref(g_thread_new("compare", compare_thread, j));
    }
}

static void on_compare_check(GtkToggleButton *t, gpointer d)
{
    int m = GPOINTER_TO_INT(d);
    if (!ui.have_result || m >= ui.res.nmembers) return;
    ui.checked[m] = gtk_toggle_button_get_active(t);
    if (ui.mrows[m]) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.mrows[m]->check), ui.checked[m]);
    update_selection_label();
    schedule_result_save();
}

static void on_compare_open(GtkButton *b, gpointer d)
{
    (void)b;
    int m = GPOINTER_TO_INT(d);
    if (ui.have_result && m < ui.res.nmembers) open_in_default_app(member_file(m)->path);
}

static gboolean on_scale_release(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w;
    (void)e;
    CompareWin *cw = d;
    int nw = (int)gtk_range_get_value(GTK_RANGE(cw->scale));
    if (nw != cw->width) {
        cw->width = nw;
        compare_load(cw);
    }
    return FALSE;
}

static void on_compare_destroy(GtkWidget *w, gpointer d)
{
    (void)w;
    CompareWin *cw = d;
    g_compare_windows = g_list_remove(g_compare_windows, cw);
    g_free(cw->ms);
    g_free(cw->cells);
    g_free(cw->checks);
    g_free(cw);
}

void compare_window_open(int g)
{
    if (!ui.have_result || g < 0 || g >= ui.res.ngroups) return;
    const DfGroup *gr = &ui.res.groups[g];
    CompareWin *cw = g_new0(CompareWin, 1);
    cw->g = g;
    cw->ms = g_new(int, gr->n);
    for (int i = 0; i < gr->n; i++)
        if (!ui.removed[gr->first + i]) cw->ms[cw->n++] = gr->first + i;
    const DfFile *f0 = member_file(cw->ms[0]);
    cw->k = f0->image ? 1 : MAX(1, f0->nframes ? f0->nframes : ui.res.frames);
    cw->width = cw->n <= 2 ? 360 : cw->n == 3 ? 280 : 220;
    cw->cells = g_new0(GtkWidget *, cw->n * cw->k);
    cw->checks = g_new0(GtkWidget *, cw->n);
    cw->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_transient_for(GTK_WINDOW(cw->win), GTK_WINDOW(ui.win));
    gtk_window_set_default_size(GTK_WINDOW(cw->win), MIN(1700, 140 + cw->n * (cw->width + 30)), 900);
    GtkWidget *hb = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hb), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(hb), "Vergleichen");
    char *subtitle = g_strdup_printf("%d Dateien · Vergleichsbilder an denselben Stellen der Laufzeit",
                                     cw->n);
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(hb), subtitle);
    g_free(subtitle);
    cw->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 120, 640, 20);
    gtk_range_set_value(GTK_RANGE(cw->scale), cw->width);
    gtk_scale_set_draw_value(GTK_SCALE(cw->scale), FALSE);
    gtk_widget_set_size_request(cw->scale, 160, -1);
    gtk_widget_set_tooltip_text(cw->scale, "Bildgröße");
    g_signal_connect(cw->scale, "button-release-event", G_CALLBACK(on_scale_release), cw);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), cw->scale);
    gtk_window_set_titlebar(GTK_WINDOW(cw->win), hb);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    cw->grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(cw->grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(cw->grid), 8);
    gtk_widget_set_margin_start(cw->grid, 14);
    gtk_widget_set_margin_end(cw->grid, 14);
    gtk_widget_set_margin_top(cw->grid, 10);
    gtk_widget_set_margin_bottom(cw->grid, 14);
    /* Kopf je Datei */
    for (int c = 0; c < cw->n; c++) {
        int m = cw->ms[c];
        const DfFile *f = member_file(m);
        GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        char *disp = path_for_display(f->path);
        char *base = g_path_get_basename(disp);
        GtkWidget *t = new_label(base, "df-title");
        gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(t), 30);
        gtk_widget_set_tooltip_text(t, disp);
        char *markup = member_info_markup(m, g);
        GtkWidget *info = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(info), markup);
        gtk_label_set_xalign(GTK_LABEL(info), 0);
        gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(info), 36);
        add_css_class(info, "df-sub");
        g_free(markup);
        GtkWidget *bx = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        cw->checks[c] = gtk_check_button_new_with_label("Markiert");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cw->checks[c]), ui.checked[m]);
        g_signal_connect(cw->checks[c], "toggled", G_CALLBACK(on_compare_check), GINT_TO_POINTER(m));
        GtkWidget *ob = gtk_button_new_with_label("Abspielen");
        g_signal_connect(ob, "clicked", G_CALLBACK(on_compare_open), GINT_TO_POINTER(m));
        gtk_box_pack_start(GTK_BOX(bx), cw->checks[c], FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(bx), ob, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v), info, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(v), bx, FALSE, FALSE, 0);
        gtk_grid_attach(GTK_GRID(cw->grid), v, c + 1, 0, 1, 1);
        g_free(disp);
        g_free(base);
    }
    /* Zeilen: Stelle i – Bild je Datei, darunter der Abstand zur ersten Datei */
    for (int i = 0; i < cw->k; i++) {
        char *pos = g_strdup_printf("%d %%", f0->image ? 0 : 100 * (i + 1) / (cw->k + 1));
        GtkWidget *pl = new_label(pos, "df-sub");
        add_css_class(pl, "dim-label");
        gtk_widget_set_valign(pl, GTK_ALIGN_CENTER);
        gtk_grid_attach(GTK_GRID(cw->grid), pl, 0, 1 + i, 1, 1);
        g_free(pos);
        for (int c = 0; c < cw->n; c++) {
            GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            GtkWidget *img = gtk_image_new_from_icon_name("image-loading-symbolic", GTK_ICON_SIZE_DIALOG);
            add_css_class(img, "df-thumb");
            gtk_widget_set_size_request(img, cw->width, cw->width * 9 / 16);
            cw->cells[c * cw->k + i] = img;
            gtk_box_pack_start(GTK_BOX(v), img, FALSE, FALSE, 0);
            const DfFile *fa = member_file(cw->ms[0]), *fb = member_file(cw->ms[c]);
            if (c > 0 && i < fa->nframes && i < fb->nframes) {
                const DfFrame *x = &fa->frames[i], *y = &fb->frames[i];
                char *t;
                if (!(x->flags & DF_FRAME_OK) || !(y->flags & DF_FRAME_OK)) t = g_strdup("nicht lesbar");
                else if ((x->flags | y->flags) & DF_FRAME_FLAT) t = g_strdup("einfarbig – zählt nicht");
                else {
                    int dd = __builtin_popcountll(x->hash ^ (ui.res.members[cw->ms[c]].flipped
                                                             ? y->hash_flip : y->hash));
                    t = g_strdup_printf("%.0f %% gleich (Abstand %d von 64 Bit)", 100.0 * (1 - dd / 64.0), dd);
                }
                GtkWidget *dl = new_label(t, "df-sub");
                add_css_class(dl, "dim-label");
                gtk_box_pack_start(GTK_BOX(v), dl, FALSE, FALSE, 0);
                g_free(t);
            }
            gtk_grid_attach(GTK_GRID(cw->grid), v, c + 1, 1 + i, 1, 1);
        }
    }
    gtk_container_add(GTK_CONTAINER(sw), cw->grid);
    gtk_container_add(GTK_CONTAINER(cw->win), sw);
    g_signal_connect(cw->win, "destroy", G_CALLBACK(on_compare_destroy), cw);
    g_compare_windows = g_list_prepend(g_compare_windows, cw);
    gtk_widget_show_all(cw->win);
    compare_load(cw);
}

/* ---------------------------------------------------------------------------
 * Zwei Dateien vergleichen (Diagnose)
 * ------------------------------------------------------------------------- */
typedef struct {
    GtkWidget *dlg, *a, *b, *result, *button, *spinner;
    char *pa, *pb;
    DfPairTest t;
    bool alive;
} PairDlg;

static gboolean on_pair_done(gpointer p)
{
    PairDlg *d = p;
    if (!d->alive) {
        df_pair_test_free(&d->t);
        g_free(d->pa);
        g_free(d->pb);
        g_free(d);
        return G_SOURCE_REMOVE;
    }
    gtk_spinner_stop(GTK_SPINNER(d->spinner));
    gtk_widget_set_sensitive(d->button, TRUE);
    GString *s = g_string_new(NULL);
    if (!d->t.ok) {
        g_string_append_printf(s, "Fehler: %s", d->t.error);
    } else {
        char da[32], db[32];
        g_string_append_printf(s, "A: %s, %d×%d, %s\nB: %s, %d×%d, %s\n\n",
                               df_format_duration(d->t.a.duration, da, sizeof da), d->t.a.width,
                               d->t.a.height, df_codec_name(d->t.a.vcodec),
                               df_format_duration(d->t.b.duration, db, sizeof db), d->t.b.width,
                               d->t.b.height, df_codec_name(d->t.b.vcodec));
        for (int i = 0; i < d->t.nframes; i++) {
            if (d->t.dist[i] < 0)
                g_string_append_printf(s, "Bild %d: einfarbig oder nicht lesbar – zählt nicht\n", i + 1);
            else
                g_string_append_printf(s, "Bild %d: Abstand %2d Bit (%.0f %%), gespiegelt %2d Bit\n",
                                       i + 1, d->t.dist[i], 100.0 * (1 - d->t.dist[i] / 64.0),
                                       d->t.dist_flip[i]);
        }
        double dur_diff = fabs(d->t.a.duration - d->t.b.duration) /
                          fmax(1e-3, fmax(d->t.a.duration, d->t.b.duration)) * 100;
        g_string_append_printf(s, "\nÄhnlichkeit: %.1f %% (gespiegelt %.1f %%)\nLaufzeitunterschied: "
                               "%.1f %%", d->t.similarity, d->t.similarity_flip, dur_diff);
        DfOptions o;
        options_from_ui(&o);
        bool hit = (d->t.similarity >= o.similarity ||
                    (o.flipped && d->t.similarity_flip >= o.similarity)) &&
                   (o.duration_diff >= 100 || dur_diff <= o.duration_diff + 0.01);
        g_string_append_printf(s, "\n\nMit den aktuellen Einstellungen (ab %.0f %%, Laufzeit ±%d %%): "
                               "%s", o.similarity, o.duration_diff,
                               hit ? "Duplikat" : "kein Duplikat");
        df_options_free(&o);
    }
    set_label_text(d->result, s->str);
    g_string_free(s, TRUE);
    df_pair_test_free(&d->t);
    return G_SOURCE_REMOVE;
}

static gpointer pair_thread(gpointer p)
{
    PairDlg *d = p;
    DfOptions o;
    options_from_ui(&o);
    df_pair_test(d->pa, d->pb, o.frames, o.white_borders, &d->t);
    df_options_free(&o);
    g_idle_add(on_pair_done, d);
    return NULL;
}

static void on_pair_run(GtkButton *b, gpointer p)
{
    (void)b;
    PairDlg *d = p;
    g_free(d->pa);
    g_free(d->pb);
    d->pa = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d->a));
    d->pb = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d->b));
    if (!d->pa || !d->pb) {
        set_label_text(d->result, "Bitte zwei Dateien wählen.");
        return;
    }
    gtk_widget_set_sensitive(d->button, FALSE);
    gtk_spinner_start(GTK_SPINNER(d->spinner));
    set_label_text(d->result, "Wird gelesen …");
    g_thread_unref(g_thread_new("pair", pair_thread, d));
}

static void on_pair_destroy(GtkWidget *w, gpointer p)
{
    (void)w;
    PairDlg *d = p;
    d->alive = false;
    if (gtk_widget_get_sensitive(d->button)) {      /* kein Lauf mehr unterwegs */
        g_free(d->pa);
        g_free(d->pb);
        g_free(d);
    }
}

void pair_test_dialog(void)
{
    PairDlg *d = g_new0(PairDlg, 1);
    d->alive = true;
    d->dlg = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_transient_for(GTK_WINDOW(d->dlg), GTK_WINDOW(ui.win));
    gtk_window_set_title(GTK_WINDOW(d->dlg), "Zwei Dateien vergleichen");
    gtk_window_set_default_size(GTK_WINDOW(d->dlg), 640, 520);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_box_pack_start(GTK_BOX(box), set_wrapping(new_label("Zeigt, warum zwei Dateien als Duplikat "
                       "gelten oder nicht – Bild für Bild.", "df-sub"), 70), FALSE, FALSE, 0);
    GtkWidget *list = add_group(box, NULL);
    d->a = gtk_file_chooser_button_new("Datei A", GTK_FILE_CHOOSER_ACTION_OPEN);
    d->b = gtk_file_chooser_button_new("Datei B", GTK_FILE_CHOOSER_ACTION_OPEN);
    gtk_widget_set_size_request(d->a, 320, -1);
    gtk_widget_set_size_request(d->b, 320, -1);
    add_row(list, "Datei A", NULL, NULL, d->a, NULL, NULL);
    add_row(list, "Datei B", NULL, NULL, d->b, NULL, NULL);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(hb, 8);
    d->button = new_pill_button("Vergleichen", "suggested-action");
    g_signal_connect(d->button, "clicked", G_CALLBACK(on_pair_run), d);
    d->spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(hb), d->button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), d->spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), hb, FALSE, FALSE, 0);
    d->result = new_label("", "df-mono");
    gtk_label_set_selectable(GTK_LABEL(d->result), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(d->result), TRUE);
    gtk_widget_set_margin_top(d->result, 10);
    gtk_box_pack_start(GTK_BOX(box), d->result, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(d->dlg), box);
    g_signal_connect(d->dlg, "destroy", G_CALLBACK(on_pair_destroy), d);
    gtk_widget_show_all(d->dlg);
}
