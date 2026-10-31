/*
 * Duplikat-Finder – grafische Oberfläche (GTK 3)
 * ==============================================
 * Aufbau und Optik wie beim Intro-Cutter und AV1-Converter. Ablauf:
 * Einstellungen -> Suchlauf -> Trefferliste (df_gui_results.c).
 * Ordner lassen sich per Drag & Drop aus dem Dateimanager hinzufügen.
 * Die Engine (df_*.c) läuft in einem eigenen Thread; Meldungen und Fortschritt
 * gelangen über g_idle_add() in den GTK-Hauptthread. Der Zustand der
 * Oberfläche liegt in der Struktur "ui" (Typ UiState, df_gui.h).
 *
 * Aufbau dieser Datei: kleine Helfer – Engine -> Oberfläche – Einstellungen
 * (Zustand, Speichern) – Seite Einstellungen – Seite Suchlauf – Ablauf –
 * Dialoge (Datenbank, Ignorierte, Zeitplan, Kriterien) – Fenster, Menü, Start
 *
 * Aufruf:
 *     duplikat-finder [Ergebnis.json]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "df_gui.h"

UiState ui;

static const char *const FOLDER_MODE_TEXT[3] = {"Alle Duplikate", "Nur im selben Ordner",
                                                "Nur aus verschiedenen Ordnern"};
static const char *const THUMB_SIZE_TEXT[3] = {"Klein", "Mittel", "Groß"};
static const int THUMB_SIZE_PX[3] = {96, 136, 200};

static void update_start_button(void);
static void on_search_clicked(GtkButton *b, gpointer d);

/* ---------------------------------------------------------------------------
 * Kleine Helfer
 * ------------------------------------------------------------------------- */
void add_css_class(GtkWidget *w, const char *cls)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

/* Label mit linksbündigem Text und optionaler CSS-Klasse */
GtkWidget *new_label(const char *text, const char *cls)
{
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    if (cls) add_css_class(widget, cls);
    return widget;
}

/* umbrechender Text, der die Inhaltsspalte nicht verbreitert */
GtkWidget *set_wrapping(GtkWidget *widget, int max_chars)
{
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(widget), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), max_chars);
    return widget;
}

/* Text eines Labels setzen (ungültiges UTF-8 wird ersetzt) */
void set_label_text(GtkWidget *widget, const char *text)
{
    char *valid = g_utf8_make_valid(text ? text : "", -1);
    gtk_label_set_text(GTK_LABEL(widget), valid);
    g_free(valid);
}

/* Pfad für die Anzeige (Dateinamen in beliebiger Kodierung) */
char *path_for_display(const char *path)
{
    return g_filename_display_name(path ? path : "");
}

/* Pfad im Standard-Programm öffnen (Ordner im Dateimanager, Video im Player) */
void open_in_default_app(const char *path)
{
    GError *err = NULL;
    char *uri = g_filename_to_uri(path, NULL, &err);
    if (uri && gtk_show_uri_on_window(GTK_WINDOW(ui.win), uri, GDK_CURRENT_TIME, &err)) {
        g_free(uri);
        return;
    }
    char *msg = g_strdup_printf("Konnte nicht geöffnet werden: %s", err ? err->message : path);
    show_info_bar(GTK_MESSAGE_ERROR, msg);
    g_free(msg);
    g_clear_error(&err);
    g_free(uri);
}

/* Datei im Dateimanager zeigen (markiert, falls der Dateimanager das kann) */
void show_in_file_manager(const char *path)
{
    GError *err = NULL;
    char *uri = g_filename_to_uri(path, NULL, NULL);
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    bool ok = false;
    if (bus && uri) {
        const char *uris[] = {uri, NULL};
        GVariant *v = g_dbus_connection_call_sync(
            bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
            "org.freedesktop.FileManager1", "ShowItems", g_variant_new("(^ass)", uris, ""), NULL,
            G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
        if (v) {
            ok = true;
            g_variant_unref(v);
        }
        g_clear_error(&err);
    }
    if (bus) g_object_unref(bus);
    g_free(uri);
    if (!ok) {
        char *dir = g_path_get_dirname(path);
        open_in_default_app(dir);
        g_free(dir);
    }
}

/* Trennlinie zwischen den Zeilen einer Liste */
static void list_separator_header(GtkListBoxRow *this_row, GtkListBoxRow *before, gpointer data)
{
    (void)data;
    if (before && !gtk_list_box_row_get_header(this_row))
        gtk_list_box_row_set_header(this_row, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
}

/* umrandete Liste mit Überschrift (Einstellungsgruppe) */
GtkWidget *add_group(GtkWidget *box, const char *title)
{
    if (title) gtk_box_pack_start(GTK_BOX(box), new_label(title, "df-heading"), FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new(NULL);
    add_css_class(frame, "df-group");
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_list_box_set_header_func(GTK_LIST_BOX(list), list_separator_header, NULL, NULL);
    gtk_container_add(GTK_CONTAINER(frame), list);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    return list;
}

/* Zeile: [prefix]  Titel / Untertitel  [suffix] */
GtkWidget *add_row(GtkWidget *list, const char *title, const char *subtitle, GtkWidget *prefix,
                   GtkWidget *suffix, GtkWidget **title_out, GtkWidget **sub_out)
{
    GtkWidget *r = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r), FALSE);
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(h, 10);
    gtk_widget_set_margin_bottom(h, 10);
    gtk_widget_set_margin_start(h, 14);
    gtk_widget_set_margin_end(h, 12);
    if (prefix) {
        gtk_widget_set_valign(prefix, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(h), prefix, FALSE, FALSE, 0);
    }
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    GtkWidget *t = new_label(title, "df-title");
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(t), 50);
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    GtkWidget *s = NULL;
    if (subtitle) {
        s = new_label(subtitle, "df-sub");
        add_css_class(s, "dim-label");
        gtk_label_set_line_wrap(GTK_LABEL(s), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(s), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_width_chars(GTK_LABEL(s), 20);
        gtk_label_set_max_width_chars(GTK_LABEL(s), 60);
        gtk_box_pack_start(GTK_BOX(v), s, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
    if (suffix) {
        gtk_widget_set_valign(suffix, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(h), suffix, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(r), h);
    gtk_container_add(GTK_CONTAINER(list), r);
    if (title_out) *title_out = t;
    if (sub_out) *sub_out = s;
    return r;
}

/* Seitenrahmen: zentrierte Spalte (höchstens max_width breit) in einem Scrollbereich */
static void on_column_size_allocate(GtkWidget *w, GdkRectangle *a, gpointer d)
{
    GtkWidget *box = d;
    int max_width = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(box), "max-width"));
    int margin = a->width > max_width + 48 ? (a->width - max_width) / 2 : 24;
    if (gtk_widget_get_margin_start(box) != margin) {
        gtk_widget_set_margin_start(box, margin);
        gtk_widget_set_margin_end(box, margin);
    }
    (void)w;
}

GtkWidget *new_page_column(GtkWidget **scroller, int max_width)
{
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    g_object_set_data(G_OBJECT(box), "max-width", GINT_TO_POINTER(max_width));
    gtk_container_add(GTK_CONTAINER(scrolled), box);
    g_signal_connect(scrolled, "size-allocate", G_CALLBACK(on_column_size_allocate), box);
    *scroller = scrolled;
    return box;
}

/* runder Hauptknopf (z. B. "Duplikate suchen") */
GtkWidget *new_pill_button(const char *text, const char *cls)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    add_css_class(b, "df-pill");
    if (cls) add_css_class(b, cls);
    return b;
}

/* Zahlenfeld mit Bereich */
GtkWidget *new_spin(double lo, double hi, double step, double value)
{
    GtkWidget *s = gtk_spin_button_new_with_range(lo, hi, step);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), value);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(s), TRUE);
    return s;
}

/* Ja/Nein-Rückfrage (modal) -> true bei "yes_label" */
bool ask_confirmation(const char *title, const char *text, const char *no_label,
                      const char *yes_label, bool destructive)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(ui.win), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", title);
    if (text) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text);
    gtk_dialog_add_button(GTK_DIALOG(d), no_label, GTK_RESPONSE_CANCEL);
    GtkWidget *yes = gtk_dialog_add_button(GTK_DIALOG(d), yes_label, GTK_RESPONSE_YES);
    add_css_class(yes, destructive ? "destructive-action" : "suggested-action");
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    bool r = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_YES;
    gtk_widget_destroy(d);
    return r;
}

/* Meldungsleiste oben im Fenster (NULL schließt sie) */
void show_info_bar(GtkMessageType type, const char *text)
{
    show_info_bar_action(type, text, NULL, NULL);
}

void show_info_bar_action(GtkMessageType type, const char *text, const char *button,
                          const char *kind)
{
    if (!text) {
        gtk_info_bar_set_revealed(GTK_INFO_BAR(ui.infobar), FALSE);
        return;
    }
    gtk_info_bar_set_message_type(GTK_INFO_BAR(ui.infobar), type);
    set_label_text(ui.info_label, text);
    g_free(ui.info_action_kind);
    ui.info_action_kind = g_strdup(kind);
    if (button) {
        gtk_button_set_label(GTK_BUTTON(ui.info_action), button);
        gtk_widget_show(ui.info_action);
    } else {
        gtk_widget_hide(ui.info_action);
    }
    gtk_widget_show(ui.infobar);
    gtk_info_bar_set_revealed(GTK_INFO_BAR(ui.infobar), TRUE);
}

char *choose_folder(const char *title, const char *current)
{
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(
        title, GTK_WINDOW(ui.win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Auswählen", "Abbrechen");
    if (current && g_file_test(current, G_FILE_TEST_IS_DIR))
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), current);
    char *result = NULL;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
        result = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    g_object_unref(dialog);
    return result;
}

char *choose_file(const char *title, bool save, const char *suggest, const char *filter_name,
                  const char *pattern)
{
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(
        title, GTK_WINDOW(ui.win), save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN,
        save ? "Speichern" : "Öffnen", "Abbrechen");
    if (save) {
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
        if (suggest) gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), suggest);
    }
    if (pattern) {
        GtkFileFilter *f = gtk_file_filter_new();
        gtk_file_filter_set_name(f, filter_name ? filter_name : pattern);
        gtk_file_filter_add_pattern(f, pattern);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), f);
        GtkFileFilter *all = gtk_file_filter_new();
        gtk_file_filter_set_name(all, "Alle Dateien");
        gtk_file_filter_add_pattern(all, "*");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all);
    }
    char *result = NULL;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
        result = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    g_object_unref(dialog);
    return result;
}

static const char CSS[] =
    ".df-heading { font-weight: bold; margin-top: 18px; margin-bottom: 4px; }\n"
    "frame.df-group > border { border-radius: 10px; background-color: @theme_base_color; }\n"
    "frame.df-group list { background-color: transparent; }\n"
    "frame.df-group list > row:first-child { border-top-left-radius: 10px;"
    " border-top-right-radius: 10px; }\n"
    "frame.df-group list > row:last-child { border-bottom-left-radius: 10px;"
    " border-bottom-right-radius: 10px; }\n"
    ".df-sub { font-size: 90%; }\n"
    ".df-hero { font-size: 170%; font-weight: 800; }\n"
    ".df-lead { font-size: 105%; }\n"
    "frame.df-group list > row:drop(active) { box-shadow: inset 0 0 0 2px"
    " @theme_selected_bg_color; background-color: alpha(@theme_selected_bg_color, 0.14); }\n"
    ".df-status { margin-top: 18px; margin-bottom: 4px; font-size: 90%; }\n"
    ".df-pill { border-radius: 999px; padding: 8px 30px; font-weight: bold; }\n"
    ".df-badge { border-radius: 999px; padding: 1px 9px; font-size: 85%; font-weight: bold; }\n"
    ".df-badge.ok { color: @success_color; background-color: alpha(@success_color, 0.16); }\n"
    ".df-badge.near { color: @warning_color; background-color: alpha(@warning_color, 0.16); }\n"
    ".df-badge.info { color: @theme_selected_bg_color;"
    " background-color: alpha(@theme_selected_bg_color, 0.14); }\n"
    ".df-thumb { border-radius: 6px; background-color: alpha(@theme_fg_color, 0.06); }\n"
    ".df-best { color: @success_color; font-weight: bold; }\n"
    ".df-checked { background-color: alpha(@error_color, 0.10); }\n"
    ".df-gtitle { font-weight: bold; }\n"
    ".df-eta { font-size: 125%; font-weight: bold; margin-top: 8px; }\n"
    ".df-log { font-family: monospace; font-size: 90%; }\n"
    ".df-mono { font-family: monospace; }\n"
    ".df-toolbar { padding: 6px 12px; border-bottom: 1px solid alpha(@theme_fg_color, 0.12); }\n";

/* ---------------------------------------------------------------------------
 * Engine -> Oberfläche (die Callbacks laufen in Worker-Threads)
 * ------------------------------------------------------------------------- */
/* Zeile ans Protokoll anhängen (Fehler rot, Hinweise gelb) */
static void protocol_append(const char *text)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui.protocol_view));
    const char *tag = g_str_has_prefix(text, "Nicht lesbar") || g_str_has_prefix(text, "Fehler")
                          ? "warn" : NULL;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    if (gtk_text_buffer_get_char_count(buf)) gtk_text_buffer_insert(buf, &end, "\n", 1);
    if (tag) gtk_text_buffer_insert_with_tags_by_name(buf, &end, text, -1, tag, NULL);
    else gtk_text_buffer_insert(buf, &end, text, -1);
    int lines = gtk_text_buffer_get_line_count(buf);
    if (lines > PROTOCOL_MAX_LINES + 500) {
        GtkTextIter start, cut;
        gtk_text_buffer_get_start_iter(buf, &start);
        gtk_text_buffer_get_iter_at_line(buf, &cut, lines - PROTOCOL_MAX_LINES);
        gtk_text_buffer_delete(buf, &start, &cut);
        gtk_text_buffer_get_start_iter(buf, &start);
        gtk_text_buffer_insert(buf, &start, "… ältere Zeilen stehen im Lauf-Protokoll (Menü)\n", -1);
    }
    GtkTextMark *mark = gtk_text_buffer_get_insert(buf);
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_place_cursor(buf, &end);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(ui.protocol_view), mark);
}

static gboolean on_idle_protocol(gpointer p)
{
    if (ui.win) protocol_append(p);
    g_free(p);
    return G_SOURCE_REMOVE;
}

static void frontend_log(const char *msg)
{
    g_idle_add(on_idle_protocol, g_utf8_make_valid(msg, -1));
}

typedef struct {
    DfPhase phase;
    int a, b;
} PhaseMsg;

static const char *phase_title(DfPhase p)
{
    switch (p) {
    case DF_PHASE_WALK: return "Ordner werden gelesen";
    case DF_PHASE_SCAN: return "Videos werden gelesen";
    case DF_PHASE_AUDIO: return "Ton-Fingerabdrücke für Teilclips";
    case DF_PHASE_COMPARE: return "Videos werden verglichen";
    case DF_PHASE_PARTIAL: return "Teilclips werden gesucht";
    }
    return "";
}

static gboolean on_idle_phase(gpointer p)
{
    PhaseMsg *m = p;
    if (ui.win) {
        ui.phase = m->phase;
        ui.phase_total = m->a;
        set_label_text(ui.scan_title, phase_title(m->phase));
        static const char *const sub[] = {
            "Alle Ordner und Unterordner werden nach Videos durchsucht.",
            "Von jedem neuen oder geänderten Video werden einige Vergleichsbilder gelesen – "
            "von CPU und GPU gleichzeitig. Bekannte Videos kommen aus der Datenbank.",
            "Für die Teilclip-Suche wird der Ton jedes Videos einmal ganz gelesen (dauert, "
            "bleibt aber in der Datenbank).",
            "Jedes Video wird mit jedem verglichen.",
            "Gekürzte Fassungen werden über den Ton gesucht und an den Bildern geprüft.",
        };
        set_label_text(ui.scan_sub, sub[m->phase]);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.scan_progress), 0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.scan_progress), "");
        set_label_text(ui.scan_eta, "");
        gtk_widget_set_visible(ui.pause_button, m->phase == DF_PHASE_SCAN || m->phase == DF_PHASE_AUDIO);
    }
    g_free(m);
    return G_SOURCE_REMOVE;
}

static void frontend_phase(DfPhase phase, int total)
{
    PhaseMsg *m = g_new0(PhaseMsg, 1);
    *m = (PhaseMsg){phase, total, 0};
    g_idle_add(on_idle_phase, m);
}

static gboolean on_idle_progress(gpointer p)
{
    PhaseMsg *m = p;
    if (ui.win && m->phase == ui.phase) {
        char *t = NULL;
        if (m->phase == DF_PHASE_WALK) {
            t = g_strdup_printf("%d Dateien gefunden …", m->a);
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(ui.scan_progress));
        } else if (m->b > 0) {
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.scan_progress), (double)m->a / m->b);
            t = m->phase == DF_PHASE_COMPARE || m->phase == DF_PHASE_PARTIAL
                    ? g_strdup_printf("%d %%", (int)(100.0 * m->a / m->b))
                    : g_strdup_printf("%d von %d", m->a, m->b);
        }
        if (t) gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.scan_progress), t);
        g_free(t);
    }
    g_free(m);
    return G_SOURCE_REMOVE;
}

static void frontend_progress(DfPhase phase, int done, int total)
{
    PhaseMsg *m = g_new0(PhaseMsg, 1);
    *m = (PhaseMsg){phase, done, total};
    g_idle_add(on_idle_progress, m);
}

/* ---------------------------------------------------------------------------
 * Einstellungen: Zustand
 * ------------------------------------------------------------------------- */
static void split_entry(GtkWidget *entry, DfStrList *out)
{
    const char *t = gtk_entry_get_text(GTK_ENTRY(entry));
    char **parts = g_strsplit(t, ";", -1);
    for (int i = 0; parts[i]; i++) {
        char *s = g_strstrip(parts[i]);
        if (*s) df_strlist_add(out, s);
    }
    g_strfreev(parts);
}

void options_from_ui(DfOptions *o)
{
    df_options_default(o);
    df_strlist_copy(&o->includes, &ui.includes);
    df_strlist_copy(&o->excludes, &ui.excludes);
    split_entry(ui.contains_entry, &o->path_contains);
    split_entry(ui.not_contains_entry, &o->path_excludes);
    o->subdirs = gtk_switch_get_active(GTK_SWITCH(ui.subdirs_sw));
    o->images = gtk_switch_get_active(GTK_SWITCH(ui.images_sw));
    o->skip_hidden = gtk_switch_get_active(GTK_SWITCH(ui.hidden_sw));
    o->skip_symlinks = gtk_switch_get_active(GTK_SWITCH(ui.symlinks_sw));
    o->skip_hardlinks = gtk_switch_get_active(GTK_SWITCH(ui.hardlinks_sw));
    o->min_size = (long long)(gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui.min_mb_spin)) * 1e6);
    o->max_size = (long long)(gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui.max_mb_spin)) * 1e6);
    o->similarity = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui.similarity_spin));
    o->frames = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.frames_spin));
    o->flipped = gtk_switch_get_active(GTK_SWITCH(ui.flipped_sw));
    o->duration_diff = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.duration_spin));
    o->folder_mode = (DfFolderMode)MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(ui.folder_combo)));
    o->against_db = gtk_switch_get_active(GTK_SWITCH(ui.againstdb_sw));
    o->white_borders = gtk_switch_get_active(GTK_SWITCH(ui.white_sw));
    o->retry_failed = gtk_switch_get_active(GTK_SWITCH(ui.retry_sw));
    o->partial = gtk_switch_get_active(GTK_SWITCH(ui.partial_sw));
    o->partial_min_ratio = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.partial_ratio_spin));
    o->partial_audio = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.partial_audio_spin));
    o->partial_visual = gtk_switch_get_active(GTK_SWITCH(ui.partial_visual_sw));
    o->partial_visual_pct = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.partial_visual_spin));
    o->cpu_workers = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.cpu_spin));
    o->gpu_workers = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.gpu_spin));
    o->considerate = gtk_switch_get_active(GTK_SWITCH(ui.considerate_sw));
}

int thumb_count_setting(void)
{
    return ui.thumbs_spin ? gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.thumbs_spin)) : 4;
}

int thumb_width_setting(void)
{
    int i = ui.thumb_size_combo ? gtk_combo_box_get_active(GTK_COMBO_BOX(ui.thumb_size_combo)) : 1;
    return THUMB_SIZE_PX[CLAMP(i, 0, 2)];
}

static char *settings_path(void)
{
    return g_build_filename(DF_APP_DIR, "gui.ini", NULL);
}

static void set_strlist_key(GKeyFile *kf, const char *key, const DfStrList *l)
{
    g_key_file_set_string_list(kf, SETTINGS_GROUP, key, (const char *const *)l->v, (gsize)l->n);
}

static void settings_load(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = settings_path();
    ui.loading_settings = true;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        const char *G = SETTINGS_GROUP;
        gsize n = 0;
        char **v = g_key_file_get_string_list(kf, G, "ordner", &n, NULL);
        for (gsize i = 0; v && i < n; i++) df_strlist_add(&ui.includes, v[i]);
        g_strfreev(v);
        v = g_key_file_get_string_list(kf, G, "ausgeschlossen", &n, NULL);
        for (gsize i = 0; v && i < n; i++) df_strlist_add(&ui.excludes, v[i]);
        g_strfreev(v);
        v = g_key_file_get_string_list(kf, G, "pfad_enthaelt", &n, NULL);
        if (v) {
            char *j = g_strjoinv("; ", v);
            gtk_entry_set_text(GTK_ENTRY(ui.contains_entry), j);
            g_free(j);
        }
        g_strfreev(v);
        v = g_key_file_get_string_list(kf, G, "pfad_ohne", &n, NULL);
        if (v) {
            char *j = g_strjoinv("; ", v);
            gtk_entry_set_text(GTK_ENTRY(ui.not_contains_entry), j);
            g_free(j);
        }
        g_strfreev(v);
#define SW(key, w) if (g_key_file_has_key(kf, G, key, NULL)) \
        gtk_switch_set_active(GTK_SWITCH(w), g_key_file_get_boolean(kf, G, key, NULL))
#define SP(key, w) if (g_key_file_has_key(kf, G, key, NULL)) \
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), g_key_file_get_double(kf, G, key, NULL))
#define CB(key, w) if (g_key_file_has_key(kf, G, key, NULL)) \
        gtk_combo_box_set_active(GTK_COMBO_BOX(w), g_key_file_get_integer(kf, G, key, NULL))
        SW("unterordner", ui.subdirs_sw);
        SW("fotos", ui.images_sw);
        SW("versteckte_ueberspringen", ui.hidden_sw);
        SW("symlinks_ueberspringen", ui.symlinks_sw);
        SW("hardlinks_einmal", ui.hardlinks_sw);
        SP("min_mb", ui.min_mb_spin);
        SP("max_mb", ui.max_mb_spin);
        SP("aehnlichkeit", ui.similarity_spin);
        SP("vergleichsbilder", ui.frames_spin);
        SW("gespiegelt", ui.flipped_sw);
        SP("laufzeit", ui.duration_spin);
        CB("ordner_modus", ui.folder_combo);
        SW("ganze_datenbank", ui.againstdb_sw);
        SW("weisse_balken", ui.white_sw);
        SW("fehler_neu", ui.retry_sw);
        SW("teilclips", ui.partial_sw);
        SP("teilclip_anteil", ui.partial_ratio_spin);
        SP("teilclip_ton", ui.partial_audio_spin);
        SW("teilclip_bild", ui.partial_visual_sw);
        SP("teilclip_bild_prozent", ui.partial_visual_spin);
        SP("cpu_prozesse", ui.cpu_spin);
        SP("gpu_prozesse", ui.gpu_spin);
        SW("ruecksicht", ui.considerate_sw);
        SP("vorschaubilder", ui.thumbs_spin);
        CB("vorschau_groesse", ui.thumb_size_combo);
#undef SW
#undef SP
#undef CB
        v = g_key_file_get_string_list(kf, G, "kriterien", &n, NULL);
        if (v && n) {
            ui.ncriteria = 0;
            for (gsize i = 0; i < n && ui.ncriteria < DF_CRIT_COUNT; i++)
                for (int c = 0; c < DF_CRIT_COUNT; c++)
                    if (!strcmp(v[i], DF_CRIT_ID[c])) ui.criteria[ui.ncriteria++] = c;
        }
        g_strfreev(v);
        ui.window_width = g_key_file_get_integer(kf, G, "fenster_breite", NULL);
        ui.window_height = g_key_file_get_integer(kf, G, "fenster_hoehe", NULL);
        ui.window_maximized = g_key_file_get_boolean(kf, G, "fenster_maximiert", NULL);
    }
    g_free(path);
    g_key_file_unref(kf);
    ui.loading_settings = false;
}

static void settings_save(void)
{
    GKeyFile *kf = g_key_file_new();
    const char *G = SETTINGS_GROUP;
    char *path = settings_path();
    DfOptions o;
    options_from_ui(&o);
    set_strlist_key(kf, "ordner", &o.includes);
    set_strlist_key(kf, "ausgeschlossen", &o.excludes);
    set_strlist_key(kf, "pfad_enthaelt", &o.path_contains);
    set_strlist_key(kf, "pfad_ohne", &o.path_excludes);
    g_key_file_set_boolean(kf, G, "unterordner", o.subdirs);
    g_key_file_set_boolean(kf, G, "fotos", o.images);
    g_key_file_set_boolean(kf, G, "versteckte_ueberspringen", o.skip_hidden);
    g_key_file_set_boolean(kf, G, "symlinks_ueberspringen", o.skip_symlinks);
    g_key_file_set_boolean(kf, G, "hardlinks_einmal", o.skip_hardlinks);
    g_key_file_set_double(kf, G, "min_mb", gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui.min_mb_spin)));
    g_key_file_set_double(kf, G, "max_mb", gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui.max_mb_spin)));
    g_key_file_set_double(kf, G, "aehnlichkeit", o.similarity);
    g_key_file_set_integer(kf, G, "vergleichsbilder", o.frames);
    g_key_file_set_boolean(kf, G, "gespiegelt", o.flipped);
    g_key_file_set_integer(kf, G, "laufzeit", o.duration_diff);
    g_key_file_set_integer(kf, G, "ordner_modus", o.folder_mode);
    g_key_file_set_boolean(kf, G, "ganze_datenbank", o.against_db);
    g_key_file_set_boolean(kf, G, "weisse_balken", o.white_borders);
    g_key_file_set_boolean(kf, G, "fehler_neu", o.retry_failed);
    g_key_file_set_boolean(kf, G, "teilclips", o.partial);
    g_key_file_set_integer(kf, G, "teilclip_anteil", o.partial_min_ratio);
    g_key_file_set_integer(kf, G, "teilclip_ton", o.partial_audio);
    g_key_file_set_boolean(kf, G, "teilclip_bild", o.partial_visual);
    g_key_file_set_integer(kf, G, "teilclip_bild_prozent", o.partial_visual_pct);
    g_key_file_set_integer(kf, G, "cpu_prozesse", o.cpu_workers);
    g_key_file_set_integer(kf, G, "gpu_prozesse", o.gpu_workers);
    g_key_file_set_boolean(kf, G, "ruecksicht", o.considerate);
    g_key_file_set_integer(kf, G, "vorschaubilder", thumb_count_setting());
    g_key_file_set_integer(kf, G, "vorschau_groesse",
                           MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(ui.thumb_size_combo))));
    const char *crit[DF_CRIT_COUNT];
    for (int i = 0; i < ui.ncriteria; i++) crit[i] = DF_CRIT_ID[ui.criteria[i]];
    g_key_file_set_string_list(kf, G, "kriterien", crit, (gsize)ui.ncriteria);
    if (ui.window_width > 0 && ui.window_height > 0) {
        g_key_file_set_integer(kf, G, "fenster_breite", ui.window_width);
        g_key_file_set_integer(kf, G, "fenster_hoehe", ui.window_height);
    }
    g_key_file_set_boolean(kf, G, "fenster_maximiert", ui.window_maximized);
    g_mkdir_with_parents(DF_APP_DIR, 0755);
    g_key_file_save_to_file(kf, path, NULL);
    df_options_free(&o);
    g_free(path);
    g_key_file_unref(kf);
}

static gboolean on_save_timer(gpointer d)
{
    (void)d;
    ui.save_timer = 0;
    settings_save();
    return G_SOURCE_REMOVE;
}

/* nach jeder Änderung (gesammelt) speichern */
void settings_changed(void)
{
    if (ui.loading_settings || !ui.win) return;
    if (ui.save_timer) g_source_remove(ui.save_timer);
    ui.save_timer = g_timeout_add(400, on_save_timer, NULL);
}

/* ---------------------------------------------------------------------------
 * Seite Einstellungen: Ordnerlisten
 * ------------------------------------------------------------------------- */
static void rebuild_folder_list(bool exclude);

typedef struct {
    bool exclude;
    char *path;
} FolderRef;

static void folder_ref_free(gpointer p, GClosure *c)
{
    (void)c;
    FolderRef *r = p;
    g_free(r->path);
    g_free(r);
}

static void strlist_remove(DfStrList *l, const char *s)
{
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->v[i], s)) {
            g_free(l->v[i]);
            memmove(&l->v[i], &l->v[i + 1], sizeof *l->v * (size_t)(l->n - i));
            l->n--;
            return;
        }
}

static bool strlist_has(const DfStrList *l, const char *s)
{
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->v[i], s)) return true;
    return false;
}

static void on_folder_remove(GtkButton *b, gpointer d)
{
    (void)b;
    FolderRef *r = d;
    strlist_remove(r->exclude ? &ui.excludes : &ui.includes, r->path);
    bool ex = r->exclude;
    rebuild_folder_list(ex);                    /* zerstört auch r */
    settings_changed();
    update_start_button();
}

static void add_folder(bool exclude, const char *path)
{
    char *canon = g_canonicalize_filename(path, NULL);
    size_t n = strlen(canon);
    while (n > 1 && canon[n - 1] == '/') canon[--n] = 0;
    DfStrList *l = exclude ? &ui.excludes : &ui.includes;
    if (!strlist_has(l, canon)) df_strlist_add(l, canon);
    g_free(canon);
    rebuild_folder_list(exclude);
    settings_changed();
    update_start_button();
}

static void on_add_folder_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    bool exclude = GPOINTER_TO_INT(d);
    DfStrList *l = exclude ? &ui.excludes : &ui.includes;
    char *f = choose_folder(exclude ? "Ordner überspringen" : "Ordner mit Videos",
                            l->n ? l->v[l->n - 1] : NULL);
    if (!f) return;
    add_folder(exclude, f);
    g_free(f);
}

static void rebuild_folder_list(bool exclude)
{
    GtkWidget *list = exclude ? ui.exclude_list : ui.include_list;
    GtkWidget *keep = exclude ? ui.exclude_add_row : ui.include_add_row;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = rows; l; l = l->next)
        if (l->data != keep) gtk_widget_destroy(l->data);
    g_list_free(rows);
    DfStrList *sl = exclude ? &ui.excludes : &ui.includes;
    for (int i = 0; i < sl->n; i++) {
        GtkWidget *rm = gtk_button_new_from_icon_name("list-remove-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_button_set_relief(GTK_BUTTON(rm), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(rm, "Aus der Liste nehmen");
        FolderRef *r = g_new0(FolderRef, 1);
        r->exclude = exclude;
        r->path = g_strdup(sl->v[i]);
        g_signal_connect_data(rm, "clicked", G_CALLBACK(on_folder_remove), r, folder_ref_free, 0);
        GtkWidget *icon = gtk_image_new_from_icon_name(exclude ? "action-unavailable-symbolic"
                                                               : "folder-videos-symbolic",
                                                       GTK_ICON_SIZE_BUTTON);
        char *disp = path_for_display(sl->v[i]);
        char *base = g_path_get_basename(disp);
        GtkWidget *sub = NULL;
        GtkWidget *row = add_row(list, base, disp, icon, rm, NULL, &sub);
        gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_line_wrap(GTK_LABEL(sub), FALSE);
        if (!g_file_test(sl->v[i], G_FILE_TEST_IS_DIR)) {
            char *t = g_strdup_printf("%s – nicht gefunden", disp);
            set_label_text(sub, t);
            g_free(t);
        }
        g_free(disp);
        g_free(base);
        /* Ordnerzeilen vor der "hinzufügen"-Zeile einsortieren */
        g_object_ref(row);
        gtk_container_remove(GTK_CONTAINER(list), row);
        gtk_list_box_insert(GTK_LIST_BOX(list), row, i);
        g_object_unref(row);
        gtk_widget_show_all(row);
    }
}

/* ---------------------------------------------------------------------------
 * Drag & Drop aus dem Dateimanager (mehrere Ordner auf einmal)
 * ------------------------------------------------------------------------- */
static char *dropped_item_path(const char *item)
{
    char *t = g_strstrip(g_strdup(item)), *path = NULL;
    if (*t == '/') {
        path = g_strdup(t);
    } else if (!strncmp(t, "~/", 2)) {
        path = g_build_filename(g_get_home_dir(), t + 2, NULL);
    } else if (*t != '#' && g_uri_peek_scheme(t)) {
        GFile *f = g_file_new_for_uri(t);
        path = g_file_get_path(f);
        g_object_unref(f);
    }
    g_free(t);
    return path;
}

static void on_drop(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *selection,
                    guint info, guint time, gpointer data)
{
    (void)w;
    (void)ctx;
    (void)x;
    (void)y;
    (void)info;
    (void)time;
    char **items = gtk_selection_data_get_uris(selection);
    if (!items) {
        char *text = (char *)gtk_selection_data_get_text(selection);
        items = text ? g_strsplit(text, "\n", -1) : NULL;
        g_free(text);
    }
    int added = 0;
    for (int i = 0; items && items[i]; i++) {
        char *path = dropped_item_path(items[i]);
        if (path && g_file_test(path, G_FILE_TEST_EXISTS)) {
            char *dir = g_file_test(path, G_FILE_TEST_IS_DIR) ? g_strdup(path) : g_path_get_dirname(path);
            add_folder(GPOINTER_TO_INT(data), dir);
            g_free(dir);
            added++;
        }
        g_free(path);
    }
    g_strfreev(items);
    if (!added)
        show_info_bar(GTK_MESSAGE_WARNING, "Bitte Ordner aus dem Dateimanager ziehen – nur lokale "
                      "bzw. eingehängte Pfade sind möglich.");
}

static void make_drop_target(GtkWidget *w, bool exclude)
{
    gtk_drag_dest_set(w, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY | GDK_ACTION_LINK);
    gtk_drag_dest_add_uri_targets(w);
    gtk_target_list_add_text_targets(gtk_drag_dest_get_target_list(w), 1);
    g_signal_connect(w, "drag-data-received", G_CALLBACK(on_drop), GINT_TO_POINTER(exclude));
}

/* ---------------------------------------------------------------------------
 * Seite Einstellungen
 * ------------------------------------------------------------------------- */
static void on_setting_changed(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    settings_changed();
}

static void update_partial_rows(void)
{
    bool on = gtk_switch_get_active(GTK_SWITCH(ui.partial_sw));
    for (int i = 0; i < 4; i++) gtk_widget_set_sensitive(ui.partial_rows[i], on);
    gtk_widget_set_sensitive(ui.partial_visual_spin,
                             on && gtk_switch_get_active(GTK_SWITCH(ui.partial_visual_sw)));
}

static void on_partial_changed(GtkWidget *w, GParamSpec *p, gpointer d)
{
    (void)w;
    (void)p;
    (void)d;
    update_partial_rows();
    settings_changed();
}

static void on_switch_changed(GtkWidget *w, GParamSpec *p, gpointer d)
{
    (void)w;
    (void)p;
    (void)d;
    settings_changed();
}

static GtkWidget *new_switch(void)
{
    GtkWidget *s = gtk_switch_new();
    g_signal_connect(s, "notify::active", G_CALLBACK(on_switch_changed), NULL);
    return s;
}

static GtkWidget *new_setting_spin(double lo, double hi, double step, double value)
{
    GtkWidget *s = new_spin(lo, hi, step, value);
    g_signal_connect(s, "value-changed", G_CALLBACK(on_setting_changed), NULL);
    return s;
}

static GtkWidget *new_combo(const char *const *texts, int n)
{
    GtkWidget *c = gtk_combo_box_text_new();
    for (int i = 0; i < n; i++) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), NULL, texts[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(c), 0);
    g_signal_connect(c, "changed", G_CALLBACK(on_setting_changed), NULL);
    return c;
}

/* Datenbank-Zeile: Anzahl der gemerkten Dateien */
void update_db_row(void)
{
    DfDbInfo i;
    if (!ui.db_sub || !df_db_info(&i)) return;
    char sz[32];
    GString *t = g_string_new(NULL);
    if (i.files || i.failed) {
        g_string_printf(t, "%d Dateien gemerkt (%s)", i.files,
                        df_format_bytes((double)i.bytes, sz, sizeof sz));
        if (i.failed) g_string_append_printf(t, ", %d nicht lesbar", i.failed);
        if (i.audio) g_string_append_printf(t, ", %d Ton-Fingerabdrücke", i.audio);
        g_string_append(t, " – sie werden nicht erneut gelesen, solange sie sich nicht ändern.");
    } else {
        g_string_assign(t, "Noch leer – gelesene Videos werden hier gemerkt, damit spätere "
                           "Suchläufe in Sekunden fertig sind.");
    }
    set_label_text(ui.db_sub, t->str);
    g_string_free(t, TRUE);
}

static void update_frames_sub(void)
{
    int k = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.frames_spin));
    char *t = g_strdup_printf("Je Video werden %d Bilder gleichmäßig über die Laufzeit verteilt "
                              "verglichen (bei %d %%, … der Laufzeit); ein abweichendes Bild ist "
                              "erlaubt. Mehr Bilder = sicherer, aber langsamer. Eine Änderung "
                              "liest alle Videos neu ein.", k, 100 / (k + 1));
    set_label_text(ui.frames_sub, t);
    g_free(t);
}

static void on_frames_changed(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    update_frames_sub();
    settings_changed();
}

static void update_gpu_row(void)
{
    char *t;
    if (!ui.gpu_ready) t = g_strdup("Grafikkarte wird geprüft …");
    else if (ui.gpu.available)
        t = g_strdup_printf("%s dekodiert per VAAPI (H.264, HEVC, VP9, AV1) – gemessen etwa 4- bis "
                            "5-mal so schnell wie ein CPU-Prozess. Formate, die die GPU nicht kann, "
                            "übernimmt die CPU.", ui.gpu.name);
    else t = g_strdup("Keine Grafikkarte mit VAAPI-Dekodierung gefunden – es arbeitet nur die CPU.");
    set_label_text(ui.gpu_row_sub, t);
    g_free(t);
    if (ui.gpu_ready) gtk_widget_hide(ui.gpu_spinner);
    gtk_widget_set_sensitive(ui.gpu_spin, !ui.gpu_ready || ui.gpu.available);
}

static void update_start_button(void)
{
    const char *hint = !ui.includes.n ? "Ordner hinzufügen" : !df_worker_path()
                       ? "Hilfsprozess fehlt – bitte neu installieren" : "";
    if (ui.setup_hint) set_label_text(ui.setup_hint, hint);
    if (ui.search_button)
        gtk_widget_set_sensitive(ui.search_button, ui.includes.n && !ui.busy && df_worker_path());
    if (ui.open_last_button)
        gtk_widget_set_visible(ui.open_last_button,
                               ui.have_result || g_file_test(DF_RESULT_PATH, G_FILE_TEST_EXISTS));
}

static void on_open_last_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (ui.have_result) show_page("results");
    else results_load_file(DF_RESULT_PATH, false);
}

static void on_criteria_clicked(GtkButton *b, gpointer d);
static void on_db_clean_clicked(GtkButton *b, gpointer d);

static GtkWidget *build_setup_page(void)
{
    GtkWidget *scroller, *box = new_page_column(&scroller, 860), *list, *button, *sub;
    GtkWidget *lead = new_label("Findet doppelte Videos – auch wenn sie anders kodiert, verkleinert, "
                                "gespiegelt oder mit Balken versehen sind – sowie auf Wunsch "
                                "gekürzte Fassungen und doppelte Bilder.", "df-lead");
    set_wrapping(lead, 80);
    add_css_class(lead, "dim-label");
    gtk_box_pack_start(GTK_BOX(box), lead, FALSE, FALSE, 6);

    /* Ordner */
    ui.include_list = add_group(box, "Ordner");
    button = gtk_button_new_with_label("Hinzufügen …");
    g_signal_connect(button, "clicked", G_CALLBACK(on_add_folder_clicked), GINT_TO_POINTER(0));
    ui.include_add_row = add_row(ui.include_list, "Ordner hinzufügen",
                                 "Auswählen oder Ordner aus dem Dateimanager hierher ziehen "
                                 "(auch mehrere).", gtk_image_new_from_icon_name(
                                     "list-add-symbolic", GTK_ICON_SIZE_BUTTON), button, NULL, NULL);
    make_drop_target(ui.include_list, false);
    ui.exclude_list = add_group(box, "Ordner überspringen");
    button = gtk_button_new_with_label("Hinzufügen …");
    g_signal_connect(button, "clicked", G_CALLBACK(on_add_folder_clicked), GINT_TO_POINTER(1));
    ui.exclude_add_row = add_row(ui.exclude_list, "Ordner überspringen",
                                 "Diese Ordner (mit Unterordnern) werden nicht durchsucht.",
                                 gtk_image_new_from_icon_name("list-add-symbolic",
                                                              GTK_ICON_SIZE_BUTTON),
                                 button, NULL, NULL);
    make_drop_target(ui.exclude_list, true);

    /* Dateien */
    list = add_group(box, "Dateien");
    ui.subdirs_sw = new_switch();
    gtk_switch_set_active(GTK_SWITCH(ui.subdirs_sw), TRUE);
    add_row(list, "Unterordner einbeziehen", NULL, NULL, ui.subdirs_sw, NULL, NULL);
    ui.images_sw = new_switch();
    add_row(list, "Auch Bilder vergleichen", "JPEG, PNG, WebP, GIF … – Bilder werden nur mit "
            "Bildern verglichen.", NULL, ui.images_sw, NULL, NULL);
    ui.hidden_sw = new_switch();
    gtk_switch_set_active(GTK_SWITCH(ui.hidden_sw), TRUE);
    add_row(list, "Versteckte Ordner und Dateien überspringen", "Namen, die mit einem Punkt "
            "beginnen (z. B. .Trash, .thumbnails).", NULL, ui.hidden_sw, NULL, NULL);
    ui.symlinks_sw = new_switch();
    gtk_switch_set_active(GTK_SWITCH(ui.symlinks_sw), TRUE);
    add_row(list, "Symbolische Links nicht verfolgen", "Sonst werden auch die Ziele von Links "
            "durchsucht und unter ihrem echten Pfad gezeigt.", NULL, ui.symlinks_sw, NULL, NULL);
    ui.hardlinks_sw = new_switch();
    gtk_switch_set_active(GTK_SWITCH(ui.hardlinks_sw), TRUE);
    add_row(list, "Hardlinks nur einmal zählen", "Mehrere Namen derselben Datei belegen den Platz "
            "nur einmal – sie sind keine Duplikate.", NULL, ui.hardlinks_sw, NULL, NULL);
    ui.min_mb_spin = new_setting_spin(0, 1000000, 1, 0);
    add_row(list, "Mindestgröße (MB)", "Kleinere Dateien überspringen (0 = alle).", NULL,
            ui.min_mb_spin, NULL, NULL);
    ui.max_mb_spin = new_setting_spin(0, 10000000, 100, 0);
    add_row(list, "Höchstgröße (MB)", "Größere Dateien überspringen (0 = keine Grenze).", NULL,
            ui.max_mb_spin, NULL, NULL);
    ui.contains_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.contains_entry), "z. B. 2025; *.webm");
    gtk_entry_set_width_chars(GTK_ENTRY(ui.contains_entry), 22);
    g_signal_connect(ui.contains_entry, "changed", G_CALLBACK(on_setting_changed), NULL);
    add_row(list, "Nur Pfade mit …", "Nur Dateien, deren Pfad einen dieser Texte enthält (durch ; "
            "getrennt; * und ? als Platzhalter). Leer = alle.", NULL, ui.contains_entry, NULL, NULL);
    ui.not_contains_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.not_contains_entry), "z. B. Sample; *.part");
    gtk_entry_set_width_chars(GTK_ENTRY(ui.not_contains_entry), 22);
    g_signal_connect(ui.not_contains_entry, "changed", G_CALLBACK(on_setting_changed), NULL);
    add_row(list, "Pfade überspringen mit …", "Dateien, deren Pfad einen dieser Texte enthält, "
            "werden übersprungen.", NULL, ui.not_contains_entry, NULL, NULL);

    /* Vergleich */
    list = add_group(box, "Vergleich");
    ui.similarity_spin = new_setting_spin(50, 100, 1, DEFAULT_SIMILARITY);
    add_row(list, "Mindestähnlichkeit (%)", "100 % = Bild für Bild gleich. Neu kodierte Kopien "
            "liegen meist bei 93–100 %, verschiedene Videos um 50 %. 90 % findet zuverlässig "
            "Duplikate; darunter steigen Fehltreffer.", NULL, ui.similarity_spin, NULL, NULL);
    ui.frames_spin = new_spin(1, DF_MAX_FRAMES, 1, DEFAULT_FRAMES);
    g_signal_connect(ui.frames_spin, "value-changed", G_CALLBACK(on_frames_changed), NULL);
    add_row(list, "Vergleichsbilder je Video", "", NULL, ui.frames_spin, NULL, &ui.frames_sub);
    ui.duration_spin = new_setting_spin(0, 100, 1, DEFAULT_DURATION_DIFF);
    add_row(list, "Laufzeit darf abweichen um (%)", "Videos, deren Laufzeit stärker abweicht, "
            "gelten nie als Duplikat (100 = egal). Gekürzte Fassungen findet die Teilclip-Suche.",
            NULL, ui.duration_spin, NULL, NULL);
    ui.flipped_sw = new_switch();
    gtk_switch_set_active(GTK_SWITCH(ui.flipped_sw), TRUE);
    add_row(list, "Gespiegelte Videos erkennen", "Auch seitenverkehrte Kopien gelten als Duplikat.",
            NULL, ui.flipped_sw, NULL, NULL);
    ui.white_sw = new_switch();
    add_row(list, "Auch weiße Balken abschneiden", "Schwarze Balken (Letterbox) werden immer "
            "ignoriert. Eine Änderung liest alle Videos neu ein.", NULL, ui.white_sw, NULL, NULL);
    ui.folder_combo = new_combo(FOLDER_MODE_TEXT, 3);
    add_row(list, "Ordner", "Nur Duplikate innerhalb desselben Ordners – oder nur solche aus "
            "verschiedenen Ordnern – anzeigen.", NULL, ui.folder_combo, NULL, NULL);
    ui.againstdb_sw = new_switch();
    add_row(list, "Mit der ganzen Datenbank vergleichen", "Auch Videos aus früheren Suchläufen in "
            "anderen Ordnern einbeziehen (wenn es sie noch gibt).", NULL, ui.againstdb_sw, NULL,
            NULL);
    ui.retry_sw = new_switch();
    add_row(list, "Nicht lesbare Dateien erneut versuchen", "Sonst werden sie bis zu einer Änderung "
            "übersprungen.", NULL, ui.retry_sw, NULL, NULL);
    button = gtk_button_new_with_label("Reihenfolge …");
    g_signal_connect(button, "clicked", G_CALLBACK(on_criteria_clicked), NULL);
    add_row(list, "Beste Datei einer Gruppe", "Nach welchen Merkmalen die zu behaltende Datei "
            "bestimmt wird (z. B. erst Laufzeit, dann Auflösung, dann Bitrate).", NULL, button,
            NULL, NULL);

    /* Teilclips */
    list = add_group(box, "Teilclips (gekürzte Fassungen)");
    ui.partial_sw = gtk_switch_new();
    g_signal_connect(ui.partial_sw, "notify::active", G_CALLBACK(on_partial_changed), NULL);
    add_row(list, "Teilclips suchen", "Findet Videos, die ein Ausschnitt eines anderen sind, über "
            "einen Ton-Fingerabdruck. Der Ton jedes Videos wird dazu einmal ganz gelesen – das "
            "dauert beim ersten Mal (bleibt dann in der Datenbank). Videos ohne Ton werden nicht "
            "erkannt.", NULL, ui.partial_sw, NULL, NULL);
    ui.partial_ratio_spin = new_setting_spin(1, 95, 1, 10);
    ui.partial_rows[0] = add_row(list, "Mindestlänge des Clips (% der Quelle)", NULL, NULL,
                                 ui.partial_ratio_spin, NULL, NULL);
    ui.partial_audio_spin = new_setting_spin(50, 100, 1, 80);
    ui.partial_rows[1] = add_row(list, "Ton-Ähnlichkeit (%)", "Neu kodierter Ton liegt meist bei "
                                 "85–98 %, fremder Ton bei 50 %.", NULL, ui.partial_audio_spin,
                                 NULL, NULL);
    ui.partial_visual_sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(ui.partial_visual_sw), TRUE);
    g_signal_connect(ui.partial_visual_sw, "notify::active", G_CALLBACK(on_partial_changed), NULL);
    ui.partial_visual_spin = new_setting_spin(50, 100, 1, 85);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), ui.partial_visual_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), ui.partial_visual_sw, FALSE, FALSE, 0);
    gtk_widget_set_valign(ui.partial_visual_sw, GTK_ALIGN_CENTER);
    ui.partial_rows[2] = add_row(list, "Bilder zusätzlich prüfen (%)", "Gleicher Ton allein reicht "
                                 "nicht (z. B. gleiche Musik): an der gefundenen Stelle werden "
                                 "auch die Bilder verglichen.", NULL, vbox, NULL, NULL);
    ui.partial_rows[3] = ui.partial_rows[2];

    /* Hardware */
    list = add_group(box, "Hardware");
    ui.gpu_spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(ui.gpu_spinner));
    ui.gpu_spin = new_setting_spin(0, MAX_GPU_WORKERS, 1, DEFAULT_GPU_WORKERS);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(hb), ui.gpu_spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), ui.gpu_spin, FALSE, FALSE, 0);
    add_row(list, "GPU-Prozesse", "", NULL, hb, NULL, &ui.gpu_row_sub);
    char *cpu_sub = g_strdup_printf("Dieser PC hat %d Threads. Jeder CPU-Prozess dekodiert ein Video "
                                    "mit einem Thread; Standard: alle bis auf 2.", df_cpu_count());
    ui.cpu_spin = new_setting_spin(0, MAX_CPU_WORKERS, 1, df_default_cpu_workers());
    add_row(list, "CPU-Prozesse", cpu_sub, NULL, ui.cpu_spin, NULL, NULL);
    g_free(cpu_sub);
    ui.considerate_sw = new_switch();
    gtk_switch_set_active(GTK_SWITCH(ui.considerate_sw), TRUE);
    add_row(list, "Anderen Programmen Vorrang lassen", "Alle Prozesse laufen mit niedriger "
            "Priorität. Braucht ein anderes Programm die CPU, pausieren einzelne CPU-Prozesse, "
            "bis wieder Luft ist; bei knappem Arbeitsspeicher starten keine neuen Dateien. Jede "
            "Datei hat eine Zeit- und Speichergrenze – hängt oder stürzt eine Datei ab, trifft "
            "das nur sie.", NULL, ui.considerate_sw, NULL, NULL);

    /* Anzeige */
    list = add_group(box, "Trefferliste");
    ui.thumbs_spin = new_setting_spin(1, DF_MAX_FRAMES, 1, 4);
    add_row(list, "Vorschaubilder je Datei", "Werden beim Anzeigen gelesen und zwischengespeichert.",
            NULL, ui.thumbs_spin, NULL, NULL);
    ui.thumb_size_combo = new_combo(THUMB_SIZE_TEXT, 3);
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.thumb_size_combo), 1);
    add_row(list, "Größe der Vorschaubilder", NULL, NULL, ui.thumb_size_combo, NULL, NULL);

    /* Datenbank */
    list = add_group(box, "Datenbank");
    button = gtk_button_new_with_label("Aufräumen");
    gtk_widget_set_tooltip_text(button, "Einträge von Dateien entfernen, die es nicht mehr gibt");
    g_signal_connect(button, "clicked", G_CALLBACK(on_db_clean_clicked), NULL);
    add_row(list, "Gelesene Dateien", "", NULL, button, NULL, &ui.db_sub);

    /* Start */
    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(bottom, 24);
    gtk_widget_set_halign(bottom, GTK_ALIGN_CENTER);
    ui.open_last_button = gtk_button_new_with_label("Letztes Ergebnis öffnen");
    add_css_class(ui.open_last_button, "df-pill");
    g_signal_connect(ui.open_last_button, "clicked", G_CALLBACK(on_open_last_clicked), NULL);
    ui.search_button = new_pill_button("Duplikate suchen", "suggested-action");
    g_signal_connect(ui.search_button, "clicked", G_CALLBACK(on_search_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bottom), ui.open_last_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), ui.search_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bottom, FALSE, FALSE, 0);
    ui.setup_hint = new_label("", "df-sub");
    add_css_class(ui.setup_hint, "dim-label");
    gtk_widget_set_halign(ui.setup_hint, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(box), ui.setup_hint, FALSE, FALSE, 4);
    (void)sub;
    return scroller;
}

/* ---------------------------------------------------------------------------
 * Seite Suchlauf
 * ------------------------------------------------------------------------- */
static void on_pause_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    ui.paused = !ui.paused;
    df_set_paused(ui.paused);
    gtk_button_set_label(b, ui.paused ? "Fortsetzen" : "Pausieren");
    show_info_bar(GTK_MESSAGE_INFO, ui.paused ? "Pausiert – laufende Dateien werden noch fertig "
                  "gelesen, neue nicht begonnen." : NULL);
}

static void on_cancel_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (!ui.busy) return;
    if (!ask_confirmation("Suchlauf abbrechen?", "Bereits gelesene Videos bleiben in der Datenbank – "
                          "der nächste Suchlauf macht dort weiter.", "Weiterlaufen lassen",
                          "Abbrechen", true))
        return;
    df_cancel = 1;
    df_set_paused(false);
    gtk_widget_set_sensitive(ui.cancel_button, FALSE);
    show_info_bar(GTK_MESSAGE_INFO, "Wird abgebrochen …");
}

static GtkWidget *build_scan_page(void)
{
    GtkWidget *scroller, *box = new_page_column(&scroller, 900);
    ui.scan_title = new_label("", "df-hero");
    gtk_widget_set_margin_top(ui.scan_title, 18);
    gtk_box_pack_start(GTK_BOX(box), ui.scan_title, FALSE, FALSE, 0);
    ui.scan_sub = set_wrapping(new_label("", "df-lead"), 90);
    add_css_class(ui.scan_sub, "dim-label");
    gtk_box_pack_start(GTK_BOX(box), ui.scan_sub, FALSE, FALSE, 4);
    ui.scan_progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(ui.scan_progress), TRUE);
    gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(ui.scan_progress), 0.05);
    gtk_widget_set_margin_top(ui.scan_progress, 14);
    gtk_box_pack_start(GTK_BOX(box), ui.scan_progress, FALSE, FALSE, 0);
    ui.scan_eta = new_label("", "df-eta");
    gtk_box_pack_start(GTK_BOX(box), ui.scan_eta, FALSE, FALSE, 0);
    ui.scan_rates = set_wrapping(new_label("", "df-sub"), 100);
    add_css_class(ui.scan_rates, "dim-label");
    gtk_box_pack_start(GTK_BOX(box), ui.scan_rates, FALSE, FALSE, 2);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(buttons, 14);
    ui.pause_button = gtk_button_new_with_label("Pausieren");
    add_css_class(ui.pause_button, "df-pill");
    g_signal_connect(ui.pause_button, "clicked", G_CALLBACK(on_pause_clicked), NULL);
    ui.cancel_button = new_pill_button("Abbrechen", "destructive-action");
    g_signal_connect(ui.cancel_button, "clicked", G_CALLBACK(on_cancel_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons), ui.pause_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), ui.cancel_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);

    /* laufende Dateien: je Datei eine schmale Zeile, bei breitem Fenster zweispaltig */
    ui.scan_jobs_heading = new_label("Laufende Dateien", "df-heading");
    gtk_box_pack_start(GTK_BOX(box), ui.scan_jobs_heading, FALSE, FALSE, 0);
    GtkWidget *jobs_frame = gtk_frame_new(NULL);
    add_css_class(jobs_frame, "df-group");
    ui.scan_jobs_list = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(ui.scan_jobs_list), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(ui.scan_jobs_list), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(ui.scan_jobs_list), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(ui.scan_jobs_list), 2);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(ui.scan_jobs_list), 16);
    gtk_widget_set_margin_top(ui.scan_jobs_list, 4);
    gtk_widget_set_margin_bottom(ui.scan_jobs_list, 4);
    gtk_widget_set_margin_start(ui.scan_jobs_list, 10);
    gtk_widget_set_margin_end(ui.scan_jobs_list, 10);
    for (int i = 0; i < MAX_CPU_WORKERS + MAX_GPU_WORKERS; i++) {
        GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *icon = gtk_image_new_from_icon_name("computer-symbolic", GTK_ICON_SIZE_MENU);
        ui.scan_jobs_titles[i] = new_label("", "df-sub");
        gtk_label_set_ellipsize(GTK_LABEL(ui.scan_jobs_titles[i]), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_width_chars(GTK_LABEL(ui.scan_jobs_titles[i]), 10);
        ui.scan_jobs_subs[i] = new_label("", "df-sub");
        add_css_class(ui.scan_jobs_subs[i], "dim-label");
        gtk_box_pack_start(GTK_BOX(h), icon, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(h), ui.scan_jobs_titles[i], TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(h), ui.scan_jobs_subs[i], FALSE, FALSE, 0);
        gtk_widget_set_margin_top(h, 2);
        gtk_widget_set_margin_bottom(h, 2);
        gtk_container_add(GTK_CONTAINER(ui.scan_jobs_list), h);
        ui.scan_jobs_rows[i] = gtk_widget_get_parent(h);    /* GtkFlowBoxChild */
        g_object_set_data(G_OBJECT(ui.scan_jobs_rows[i]), "icon", icon);
    }
    gtk_container_add(GTK_CONTAINER(jobs_frame), ui.scan_jobs_list);
    gtk_box_pack_start(GTK_BOX(box), jobs_frame, FALSE, FALSE, 0);
    ui.scan_jobs_frame = jobs_frame;

    gtk_box_pack_start(GTK_BOX(box), new_label("Protokoll", "df-heading"), FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new(NULL);
    add_css_class(frame, "df-group");
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(sw, -1, 200);
    ui.protocol_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(ui.protocol_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ui.protocol_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(ui.protocol_view), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(ui.protocol_view), 6);
    add_css_class(ui.protocol_view, "df-log");
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui.protocol_view));
    gtk_text_buffer_create_tag(buf, "warn", "foreground", "#c88800", NULL);
    gtk_container_add(GTK_CONTAINER(sw), ui.protocol_view);
    gtk_container_add(GTK_CONTAINER(frame), sw);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    return scroller;
}

/* alle 0,5 s: Tempo, Restzeit, laufende Dateien */
static gboolean on_progress_timer(gpointer d)
{
    (void)d;
    if (!ui.busy) {
        ui.progress_timer = 0;
        return G_SOURCE_REMOVE;
    }
    DfProgress p;
    bool running = df_progress(&p);
    if (running && (ui.phase == DF_PHASE_SCAN || ui.phase == DF_PHASE_AUDIO)) {
        char eta[64], *t;
        if (ui.paused) t = g_strdup("Pausiert");
        else if (p.eta >= 0) t = g_strdup_printf("Noch %s", df_format_eta(p.eta, eta, sizeof eta));
        else t = g_strdup("Restzeit wird gemessen …");
        set_label_text(ui.scan_eta, t);
        g_free(t);
        GString *r = g_string_new(NULL);
        if (ui.phase == DF_PHASE_SCAN)
            g_string_append_printf(r, "%.1f Dateien/s – GPU %.1f/s, CPU %.1f/s", p.files_per_sec,
                                   p.gpu_per_sec, p.cpu_per_sec);
        else
            g_string_append_printf(r, "%.1f Dateien/s", p.files_per_sec);
        g_string_append_printf(r, " · CPU-Last %.0f %%", p.cpu_load);
        if (p.held_cpu > 0)
            g_string_append_printf(r, " · %d CPU-Prozess%s pausiert (andere Programme haben Vorrang)",
                                   p.held_cpu, p.held_cpu == 1 ? "" : "e");
        if (p.failed) g_string_append_printf(r, " · %d nicht lesbar", p.failed);
        set_label_text(ui.scan_rates, r->str);
        g_string_free(r, TRUE);
    } else if (!running) {
        set_label_text(ui.scan_rates, "");
    }
    DfJobInfo jobs[MAX_CPU_WORKERS + MAX_GPU_WORKERS];
    int n = running ? df_jobs_snapshot(jobs, G_N_ELEMENTS(jobs)) : 0;
    /* GPU zuerst, dann CPU; pausierte stehen schon in der Statuszeile – keine eigene Zeile */
    int row = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            if (jobs[i].gpu != (pass == 0) || !jobs[i].busy) continue;
            char *base = g_path_get_basename(jobs[i].path);
            char *disp = g_filename_display_name(base);
            char *sub = g_strdup_printf("%s · %.0f s", jobs[i].gpu ? "GPU" : "CPU", jobs[i].secs);
            set_label_text(ui.scan_jobs_titles[row], disp);
            gtk_widget_set_tooltip_text(ui.scan_jobs_titles[row], disp);
            set_label_text(ui.scan_jobs_subs[row], sub);
            gtk_image_set_from_icon_name(
                GTK_IMAGE(g_object_get_data(G_OBJECT(ui.scan_jobs_rows[row]), "icon")),
                jobs[i].gpu ? "video-display-symbolic" : "computer-symbolic", GTK_ICON_SIZE_MENU);
            gtk_widget_show_all(ui.scan_jobs_rows[row]);
            row++;
            g_free(base);
            g_free(disp);
            g_free(sub);
        }
    char *head = row ? g_strdup_printf("Laufende Dateien (%d)", row) : g_strdup("Laufende Dateien");
    set_label_text(ui.scan_jobs_heading, head);
    g_free(head);
    gtk_widget_set_visible(ui.scan_jobs_frame, row > 0);
    for (int i = row; i < MAX_CPU_WORKERS + MAX_GPU_WORKERS; i++) gtk_widget_hide(ui.scan_jobs_rows[i]);
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------------------
 * Ablauf
 * ------------------------------------------------------------------------- */
typedef struct {
    int rc;
    const char *err;
    DfResult res;
    DfStats st;
} ScanDone;

static void quit_application(void);

/* Standby und Abmelden sperren, solange gesucht oder mit Dateien gearbeitet wird – über die
 * Sitzung (Energieverwaltung des Desktops) und über systemd-logind */
void session_inhibit(bool on, const char *reason)
{
    if (on) {
        if (ui.inhibit_count++ == 0) {
            ui.inhibit_cookie = gtk_application_inhibit(
                ui.app, GTK_WINDOW(ui.win),
                GTK_APPLICATION_INHIBIT_SUSPEND | GTK_APPLICATION_INHIBIT_LOGOUT, reason);
            df_inhibit_sleep(true, reason);
        }
    } else if (ui.inhibit_count > 0 && --ui.inhibit_count == 0) {
        if (ui.inhibit_cookie) gtk_application_uninhibit(ui.app, ui.inhibit_cookie);
        ui.inhibit_cookie = 0;
        df_inhibit_sleep(false, NULL);
    }
}

static gboolean on_scan_done(gpointer p)
{
    ScanDone *d = p;
    g_thread_join(ui.worker);
    ui.worker = NULL;
    ui.busy = false;
    session_inhibit(false, NULL);
    ui.paused = false;
    df_set_paused(false);
    gtk_button_set_label(GTK_BUTTON(ui.pause_button), "Pausieren");
    if (ui.quit_when_idle) {
        df_result_free(&d->res);
        g_free(d);
        quit_application();
        return G_SOURCE_REMOVE;
    }
    gtk_widget_set_sensitive(ui.back_btn, TRUE);
    update_db_row();
    if (d->rc < 0) {
        show_info_bar(GTK_MESSAGE_ERROR, d->err);
        show_page("setup");
    } else if (d->rc == 1) {
        show_info_bar(GTK_MESSAGE_INFO, "Suchlauf abgebrochen – bereits gelesene Videos sind "
                      "gemerkt.");
        show_page(ui.have_result ? "results" : "setup");
    } else {
        char sz[32];
        long long bytes = 0;
        for (int g = 0; g < d->res.ngroups; g++) {
            const DfGroup *gr = &d->res.groups[g];
            long long mx = 0, sum = 0;
            for (int i = 0; i < gr->n; i++) {
                long long s = d->res.files[d->res.members[gr->first + i].file].size;
                sum += s;
                mx = MAX(mx, s);
            }
            bytes += sum - mx;
        }
        char failed[64] = "";
        if (d->st.failed) snprintf(failed, sizeof failed, ", %d nicht lesbar", d->st.failed);
        char *msg = g_strdup_printf("%d Dateien geprüft (%d neu gelesen%s) – %d Gruppen mit %d "
                                    "Duplikaten, zusammen bis zu %s.", d->st.found, d->st.scanned,
                                    failed, d->st.groups, d->st.duplicates,
                                    df_format_bytes((double)bytes, sz, sizeof sz));
        show_info_bar(GTK_MESSAGE_INFO, msg);
        g_free(msg);
        DfResult *r = g_new(DfResult, 1);
        *r = d->res;
        results_set(r, NULL, NULL);
        g_free(r);
        show_page("results");
        if (!gtk_window_is_active(GTK_WINDOW(ui.win))) {
            GNotification *n = g_notification_new("Suchlauf fertig");
            char *body = g_strdup_printf("%d Gruppen mit %d Duplikaten", d->st.groups,
                                         d->st.duplicates);
            g_notification_set_body(n, body);
            g_application_send_notification(G_APPLICATION(ui.app), "scan-done", n);
            g_object_unref(n);
            g_free(body);
        }
    }
    if (d->rc != 0) df_result_free(&d->res);
    update_start_button();
    g_free(d);
    return G_SOURCE_REMOVE;
}

static gpointer scan_thread(gpointer p)
{
    (void)p;
    ScanDone *d = g_new0(ScanDone, 1);
    d->rc = df_scan(&ui.run_opts, &ui.gpu, &d->res, &d->st, &d->err);
    g_idle_add(on_scan_done, d);
    return NULL;
}

static void start_scan(void)
{
    if (ui.busy) return;
    if (!ui.gpu_ready) {
        show_info_bar(GTK_MESSAGE_INFO, "Einen Moment – die Grafikkarte wird noch geprüft.");
        return;
    }
    df_options_free(&ui.run_opts);
    options_from_ui(&ui.run_opts);
    if (!ui.run_opts.includes.n) return;
    if (ui.run_opts.cpu_workers == 0 && (!ui.gpu.available || ui.run_opts.gpu_workers == 0))
        ui.run_opts.cpu_workers = 1;
    if (ui.have_result) results_save_now();
    ui.busy = true;
    df_cancel = 0;
    gtk_widget_set_sensitive(ui.cancel_button, TRUE);
    gtk_widget_set_sensitive(ui.back_btn, FALSE);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui.protocol_view));
    gtk_text_buffer_set_text(buf, "", -1);
    show_info_bar(GTK_MESSAGE_INFO, NULL);
    set_label_text(ui.scan_title, phase_title(DF_PHASE_WALK));
    set_label_text(ui.scan_sub, "");
    set_label_text(ui.scan_eta, "");
    set_label_text(ui.scan_rates, "");
    for (int i = 0; i < MAX_CPU_WORKERS + MAX_GPU_WORKERS; i++) gtk_widget_hide(ui.scan_jobs_rows[i]);
    show_page("scan");
    update_start_button();
    session_inhibit(true, "Duplikat-Finder sucht doppelte Videos");
    ui.worker = g_thread_new("scan", scan_thread, NULL);
    if (!ui.progress_timer) ui.progress_timer = g_timeout_add(500, on_progress_timer, NULL);
}

static void on_search_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    start_scan();
}

void show_page(const char *name)
{
    gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), name);
    bool setup = !strcmp(name, "setup");
    gtk_widget_set_visible(ui.back_btn, !setup);
    gtk_widget_set_tooltip_text(ui.back_btn, "Zu den Einstellungen");
    if (setup) {
        update_db_row();
        update_start_button();
    }
}

static void on_back_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (ui.busy) return;
    show_page("setup");
}

/* ---------------------------------------------------------------------------
 * Dialoge: Qualitätskriterien, Datenbank, Ignorierte Dateien, Zeitplan
 * ------------------------------------------------------------------------- */
typedef struct {
    GtkWidget *list;
    int order[DF_CRIT_COUNT];
    bool on[DF_CRIT_COUNT];
} CritDialog;

static void crit_fill(CritDialog *c);

static void on_crit_move(GtkButton *b, gpointer d)
{
    CritDialog *c = d;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "index"));
    int dir = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "dir"));
    int j = i + dir;
    if (j < 0 || j >= DF_CRIT_COUNT) return;
    int t = c->order[i];
    c->order[i] = c->order[j];
    c->order[j] = t;
    crit_fill(c);
}

static void on_crit_toggle(GtkToggleButton *t, gpointer d)
{
    CritDialog *c = d;
    int crit = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "crit"));
    c->on[crit] = gtk_toggle_button_get_active(t);
}

static void crit_fill(CritDialog *c)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(c->list));
    for (GList *l = rows; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(rows);
    for (int i = 0; i < DF_CRIT_COUNT; i++) {
        int crit = c->order[i];
        GtkWidget *check = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), c->on[crit]);
        g_object_set_data(G_OBJECT(check), "crit", GINT_TO_POINTER(crit));
        g_signal_connect(check, "toggled", G_CALLBACK(on_crit_toggle), c);
        GtkWidget *bx = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *up = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_BUTTON);
        GtkWidget *down = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_BUTTON);
        g_object_set_data(G_OBJECT(up), "index", GINT_TO_POINTER(i));
        g_object_set_data(G_OBJECT(up), "dir", GINT_TO_POINTER(-1));
        g_object_set_data(G_OBJECT(down), "index", GINT_TO_POINTER(i));
        g_object_set_data(G_OBJECT(down), "dir", GINT_TO_POINTER(1));
        g_signal_connect(up, "clicked", G_CALLBACK(on_crit_move), c);
        g_signal_connect(down, "clicked", G_CALLBACK(on_crit_move), c);
        gtk_widget_set_sensitive(up, i > 0);
        gtk_widget_set_sensitive(down, i < DF_CRIT_COUNT - 1);
        gtk_box_pack_start(GTK_BOX(bx), up, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bx), down, FALSE, FALSE, 0);
        static const char *const hint[DF_CRIT_COUNT] = {
            "längere Laufzeit ist besser", "mehr Pixel", "mehr Bilder je Sekunde",
            "höhere Video-Bitrate", "höhere Ton-Bitrate", "größere Datei",
            "zuletzt geändert", "am längsten vorhanden", "kürzester Pfad (meist der Hauptordner)"};
        add_row(c->list, DF_CRIT_NAME[crit], hint[crit], check, bx, NULL, NULL);
    }
    gtk_widget_show_all(c->list);
}

static void on_criteria_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Beste Datei einer Gruppe", GTK_WINDOW(ui.win),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_USE_HEADER_BAR,
                                                 "Abbrechen", GTK_RESPONSE_CANCEL, "Übernehmen",
                                                 GTK_RESPONSE_OK, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 520, 560);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    GtkWidget *lead = set_wrapping(new_label("Die Merkmale werden von oben nach unten geprüft: "
                                             "das erste, in dem sich die Dateien unterscheiden, "
                                             "entscheidet. Diese Datei bleibt bei „Niedrigere "
                                             "Qualität markieren“ unmarkiert.", "df-sub"), 60);
    gtk_box_pack_start(GTK_BOX(box), lead, FALSE, FALSE, 0);
    CritDialog c;
    memset(&c, 0, sizeof c);
    int k = 0;
    for (int i = 0; i < ui.ncriteria; i++) {
        c.order[k++] = ui.criteria[i];
        c.on[ui.criteria[i]] = true;
    }
    for (int crit = 0; crit < DF_CRIT_COUNT; crit++) {
        bool used = false;
        for (int i = 0; i < k; i++) used |= c.order[i] == crit;
        if (!used) c.order[k++] = crit;
    }
    c.list = add_group(box, NULL);
    crit_fill(&c);
    gtk_container_add(GTK_CONTAINER(area), box);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        ui.ncriteria = 0;
        for (int i = 0; i < DF_CRIT_COUNT; i++)
            if (c.on[c.order[i]]) ui.criteria[ui.ncriteria++] = c.order[i];
        settings_changed();
    }
    gtk_widget_destroy(dlg);
}

static void on_db_clean_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (ui.busy) return;
    GdkWindow *w = gtk_widget_get_window(ui.win);
    GdkCursor *c = gdk_cursor_new_from_name(gdk_display_get_default(), "wait");
    gdk_window_set_cursor(w, c);
    while (gtk_events_pending()) gtk_main_iteration();
    int n = df_db_prune_missing();
    gdk_window_set_cursor(w, NULL);
    g_object_unref(c);
    char *msg = g_strdup_printf("%d Einträge von Dateien entfernt, die es nicht mehr gibt.", n);
    show_info_bar(GTK_MESSAGE_INFO, msg);
    g_free(msg);
    update_db_row();
}

/* Liste mit Suchfeld (Datenbank bearbeiten, ignorierte Dateien) */
typedef struct {
    GtkWidget *dlg, *entry, *list, *count;
    bool ignored;
} ListDialog;

static void list_dialog_fill(ListDialog *l);

static void on_list_remove(GtkButton *b, gpointer d)
{
    ListDialog *l = d;
    const char *path = g_object_get_data(G_OBJECT(b), "path");
    if (l->ignored) df_ignore_remove(path);
    else df_db_remove_path(path);
    list_dialog_fill(l);
}

static void list_dialog_fill(ListDialog *l)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(l->list));
    for (GList *r = rows; r; r = r->next) gtk_widget_destroy(r->data);
    g_list_free(rows);
    DfStrList items;
    const char *q = gtk_entry_get_text(GTK_ENTRY(l->entry));
    if (l->ignored) {
        DfStrList all;
        df_ignore_list(&all);
        memset(&items, 0, sizeof items);
        for (int i = 0; i < all.n && items.n < 500; i++)
            if (!*q || strcasestr(all.v[i], q)) df_strlist_add(&items, all.v[i]);
        df_strlist_free(&all);
    } else {
        df_db_search(q, 500, &items);
    }
    for (int i = 0; i < items.n; i++) {
        char **f = g_strsplit(items.v[i], "\t", 3);
        GtkWidget *rm = gtk_button_new_from_icon_name("user-trash-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_button_set_relief(GTK_BUTTON(rm), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(rm, l->ignored ? "Nicht mehr ignorieren"
                                                   : "Eintrag löschen (Datei wird neu gelesen)");
        g_object_set_data_full(G_OBJECT(rm), "path", g_strdup(f[0]), g_free);
        g_signal_connect(rm, "clicked", G_CALLBACK(on_list_remove), l);
        char *disp = path_for_display(f[0]);
        char *base = g_path_get_basename(disp);
        char *sub = !l->ignored && f[1] && atoi(f[1]) != 0
                        ? g_strdup_printf("%s – nicht lesbar: %s", disp, f[2] ? f[2] : "")
                        : g_strdup(disp);
        GtkWidget *s = NULL;
        add_row(l->list, base, sub, NULL, rm, NULL, &s);
        gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_line_wrap(GTK_LABEL(s), FALSE);
        g_free(disp);
        g_free(base);
        g_free(sub);
        g_strfreev(f);
    }
    char *t = g_strdup_printf(items.n >= 500 ? "Die ersten %d Treffer" : "%d Treffer", items.n);
    set_label_text(l->count, t);
    g_free(t);
    gtk_widget_show_all(l->list);
    df_strlist_free(&items);
}

static void on_list_search(GtkSearchEntry *e, gpointer d)
{
    (void)e;
    list_dialog_fill(d);
}

static void list_dialog(bool ignored)
{
    ListDialog l = {.ignored = ignored};
    l.dlg = gtk_dialog_new_with_buttons(ignored ? "Ignorierte Dateien" : "Datenbank bearbeiten",
                                        GTK_WINDOW(ui.win), GTK_DIALOG_MODAL |
                                        GTK_DIALOG_USE_HEADER_BAR, "Schließen",
                                        GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(l.dlg), 760, 640);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(l.dlg));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    GtkWidget *lead = set_wrapping(new_label(ignored ? "Diese Dateien tauchen in keinem Ergebnis "
                                             "mehr auf." : "Gemerkte Dateien suchen. Ein gelöschter "
                                             "Eintrag wird beim nächsten Suchlauf neu gelesen.",
                                             "df-sub"), 80);
    add_css_class(lead, "dim-label");
    gtk_box_pack_start(GTK_BOX(box), lead, FALSE, FALSE, 0);
    l.entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(l.entry), "Teil des Pfads");
    g_signal_connect(l.entry, "search-changed", G_CALLBACK(on_list_search), &l);
    gtk_box_pack_start(GTK_BOX(box), l.entry, FALSE, FALSE, 0);
    l.count = new_label("", "df-sub");
    gtk_box_pack_start(GTK_BOX(box), l.count, FALSE, FALSE, 0);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(sw, TRUE);
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    l.list = add_group(inner, NULL);
    gtk_container_add(GTK_CONTAINER(sw), inner);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(area), box);
    list_dialog_fill(&l);
    gtk_widget_show_all(l.dlg);
    gtk_dialog_run(GTK_DIALOG(l.dlg));
    gtk_widget_destroy(l.dlg);
    update_db_row();
}

/* Datenbank-Pfade verschieben (Laufwerk umbenannt) */
static void relocate_dialog(void)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Pfade in der Datenbank ändern", GTK_WINDOW(ui.win),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_USE_HEADER_BAR,
                                                 "Abbrechen", GTK_RESPONSE_CANCEL, "Ändern",
                                                 GTK_RESPONSE_OK, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_box_pack_start(GTK_BOX(box), set_wrapping(new_label("Wurde ein Laufwerk umbenannt oder ein "
                       "Ordner verschoben, behalten die Dateien so ihre gelesenen Vergleichsbilder.",
                       "df-sub"), 60), FALSE, FALSE, 0);
    GtkWidget *list = add_group(box, NULL);
    GtkWidget *old_e = gtk_entry_new(), *new_e = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(old_e), 36);
    gtk_entry_set_width_chars(GTK_ENTRY(new_e), 36);
    gtk_entry_set_placeholder_text(GTK_ENTRY(old_e), "/mnt/alt/Videos");
    gtk_entry_set_placeholder_text(GTK_ENTRY(new_e), "/mnt/neu/Videos");
    add_row(list, "Bisheriger Anfang", NULL, NULL, old_e, NULL, NULL);
    add_row(list, "Neuer Anfang", NULL, NULL, new_e, NULL, NULL);
    gtk_container_add(GTK_CONTAINER(area), box);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        int n = df_db_relocate(gtk_entry_get_text(GTK_ENTRY(old_e)),
                               gtk_entry_get_text(GTK_ENTRY(new_e)));
        char *msg = g_strdup_printf("%d Pfade geändert.", n);
        show_info_bar(GTK_MESSAGE_INFO, msg);
        g_free(msg);
    }
    gtk_widget_destroy(dlg);
}

/* Zeitplan: systemd-Benutzer-Timer, der die Terminal-Version mit den Einstellungen startet */
static char *unit_path(const char *name)
{
    return g_build_filename(g_get_user_config_dir(), "systemd", "user", name, NULL);
}

static bool run_systemctl(const char *a, const char *b, const char *c)
{
    char *argv[] = {"systemctl", "--user", (char *)a, (char *)b, (char *)c, NULL};
    int status = -1;
    bool ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                           G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, NULL);
    return ok && status == 0;
}

static void schedule_dialog(void)
{
    char *timer = unit_path("duplikat-finder.timer"), *service = unit_path("duplikat-finder.service");
    char *content = NULL;
    int hh = 3, mm = 0;
    bool active = g_file_get_contents(timer, &content, NULL, NULL);
    if (content) {
        char *p = strstr(content, "OnCalendar=*-*-* ");
        if (p) sscanf(p + 17, "%d:%d", &hh, &mm);
        g_free(content);
    }
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Geplanter Suchlauf", GTK_WINDOW(ui.win),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_USE_HEADER_BAR,
                                                 "Abbrechen", GTK_RESPONSE_CANCEL, "Übernehmen",
                                                 GTK_RESPONSE_OK, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_box_pack_start(GTK_BOX(box), set_wrapping(new_label("Sucht täglich zur gewählten Uhrzeit mit "
                       "den aktuellen Einstellungen (auch ohne geöffnetes Fenster) und meldet sich "
                       "danach. Das Ergebnis öffnet „Letztes Ergebnis öffnen“. War der PC zur "
                       "Uhrzeit aus, wird der Lauf nachgeholt.", "df-sub"), 60), FALSE, FALSE, 0);
    GtkWidget *list = add_group(box, NULL);
    GtkWidget *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), active);
    add_row(list, "Täglich suchen", NULL, NULL, sw, NULL, NULL);
    GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *h = new_spin(0, 23, 1, hh), *m = new_spin(0, 59, 5, mm);
    gtk_box_pack_start(GTK_BOX(tb), h, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), gtk_label_new(":"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tb), m, FALSE, FALSE, 0);
    add_row(list, "Uhrzeit", NULL, NULL, tb, NULL, NULL);
    gtk_container_add(GTK_CONTAINER(area), box);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        settings_save();
        bool on = gtk_switch_get_active(GTK_SWITCH(sw));
        if (on) {
            char *dir = g_path_get_dirname(timer);
            g_mkdir_with_parents(dir, 0755);
            g_free(dir);
            char *cli = g_find_program_in_path("duplikat-finder-cli");
            if (!cli) {
                char *exe = g_file_read_link("/proc/self/exe", NULL);
                char *d2 = exe ? g_path_get_dirname(exe) : g_strdup("/usr/bin");
                cli = g_build_filename(d2, "duplikat-finder-cli", NULL);
                g_free(exe);
                g_free(d2);
            }
            char *svc = g_strdup_printf("[Unit]\nDescription=Duplikat-Finder: geplanter Suchlauf\n\n"
                                        "[Service]\nType=oneshot\nNice=15\nIOSchedulingClass=idle\n"
                                        "ExecStart=\"%s\" --zeitplan\n", cli);
            char *tim = g_strdup_printf("[Unit]\nDescription=Duplikat-Finder: täglicher Suchlauf\n\n"
                                        "[Timer]\nOnCalendar=*-*-* %02d:%02d:00\nPersistent=true\n\n"
                                        "[Install]\nWantedBy=timers.target\n",
                                        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(h)),
                                        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(m)));
            bool ok = g_file_set_contents(service, svc, -1, NULL) &&
                      g_file_set_contents(timer, tim, -1, NULL) &&
                      run_systemctl("daemon-reload", NULL, NULL) &&
                      run_systemctl("enable", "--now", "duplikat-finder.timer");
            show_info_bar(ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR,
                          ok ? "Geplanter Suchlauf eingerichtet." :
                          "Der Zeitplan konnte nicht eingerichtet werden (systemd).");
            g_free(cli);
            g_free(svc);
            g_free(tim);
        } else if (active) {
            run_systemctl("disable", "--now", "duplikat-finder.timer");
            g_unlink(timer);
            g_unlink(service);
            run_systemctl("daemon-reload", NULL, NULL);
            show_info_bar(GTK_MESSAGE_INFO, "Geplanter Suchlauf entfernt.");
        }
    }
    gtk_widget_destroy(dlg);
    g_free(timer);
    g_free(service);
}

/* ---------------------------------------------------------------------------
 * Fenster, Menü, Programmstart
 * ------------------------------------------------------------------------- */
static void quit_application(void)
{
    if (ui.save_timer) {
        g_source_remove(ui.save_timer);
        ui.save_timer = 0;
    }
    settings_save();
    while (ui.inhibit_count > 0) session_inhibit(false, NULL);
    results_shutdown();
    if (ui.win) gtk_widget_destroy(ui.win);
    ui.win = NULL;
}

static gboolean on_window_configure(GtkWidget *w, GdkEventConfigure *e, gpointer d)
{
    (void)e;
    (void)d;
    if (!ui.window_maximized) {
        int width, height;
        gtk_window_get_size(GTK_WINDOW(w), &width, &height);
        if (width != ui.window_width || height != ui.window_height) {
            ui.window_width = width;
            ui.window_height = height;
            settings_changed();
        }
    }
    return FALSE;
}

static gboolean on_window_state(GtkWidget *w, GdkEventWindowState *e, gpointer d)
{
    (void)w;
    (void)d;
    bool max = (e->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    if (max != ui.window_maximized) {
        ui.window_maximized = max;
        settings_changed();
    }
    return FALSE;
}

static gboolean on_window_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w;
    (void)e;
    (void)d;
    if (!ui.busy) {
        quit_application();
        return TRUE;
    }
    if (ui.quit_when_idle) return TRUE;
    if (!ask_confirmation("Der Suchlauf läuft noch", "Beim Beenden wird er abgebrochen. Bereits "
                          "gelesene Videos bleiben gemerkt; der nächste Suchlauf macht dort weiter.",
                          "Weiterlaufen lassen", "Abbrechen und beenden", true))
        return TRUE;
    ui.quit_when_idle = true;
    df_cancel = 1;
    df_set_paused(false);
    show_info_bar(GTK_MESSAGE_INFO, "Wird abgebrochen …");
    return TRUE;
}

static gboolean on_unix_signal(gpointer d)
{
    (void)d;
    if (!ui.win) return G_SOURCE_CONTINUE;
    if (ui.busy) {
        ui.quit_when_idle = true;
        df_cancel = 1;
    } else {
        quit_application();
    }
    return G_SOURCE_CONTINUE;
}

static void on_info_bar_response(GtkInfoBar *info_bar, gint response, gpointer d)
{
    (void)d;
    if (response == 1 && ui.info_action_kind) {
        if (!strcmp(ui.info_action_kind, "open-scheduled")) results_load_file(DF_SCHEDULED_PATH, false);
        else if (!strcmp(ui.info_action_kind, "open-log")) open_in_default_app(DF_RUN_LOG);
    }
    gtk_info_bar_set_revealed(info_bar, FALSE);
}

#define ACTION(name) static void name(GSimpleAction *a, GVariant *p, gpointer d)
#define UNUSED (void)a; (void)p; (void)d

ACTION(on_action_open_log)
{
    UNUSED;
    if (g_file_test(DF_RUN_LOG, G_FILE_TEST_IS_REGULAR)) open_in_default_app(DF_RUN_LOG);
    else show_info_bar(GTK_MESSAGE_INFO, "Es gibt noch kein Lauf-Protokoll.");
}

ACTION(on_action_open_data)
{
    UNUSED;
    open_in_default_app(DF_APP_DIR);
}

ACTION(on_action_open_result)
{
    UNUSED;
    if (ui.busy) return;
    char *f = choose_file("Ergebnis öffnen", false, NULL, "Ergebnisse (*.json)", "*.json");
    if (f) results_load_file(f, false);
    g_free(f);
}

ACTION(on_action_db_edit)
{
    UNUSED;
    if (!ui.busy) list_dialog(false);
}

ACTION(on_action_ignored)
{
    UNUSED;
    if (!ui.busy) list_dialog(true);
}

ACTION(on_action_db_relocate)
{
    UNUSED;
    if (!ui.busy) relocate_dialog();
}

ACTION(on_action_db_clean)
{
    UNUSED;
    on_db_clean_clicked(NULL, NULL);
}

ACTION(on_action_db_failed)
{
    UNUSED;
    if (ui.busy) return;
    char *msg = g_strdup_printf("%d nicht lesbare Dateien vergessen – sie werden beim nächsten "
                                "Suchlauf erneut versucht.", df_db_forget_failed());
    show_info_bar(GTK_MESSAGE_INFO, msg);
    g_free(msg);
    update_db_row();
}

ACTION(on_action_db_clear)
{
    UNUSED;
    if (ui.busy) return;
    if (!ask_confirmation("Datenbank leeren?", "Alle gelesenen Vergleichsbilder, Ton-Fingerabdrücke "
                          "und „kein Duplikat“-Markierungen werden gelöscht. Der nächste Suchlauf "
                          "liest alle Videos neu.", "Abbrechen", "Leeren", true))
        return;
    df_db_clear();
    show_info_bar(GTK_MESSAGE_INFO, "Datenbank geleert.");
    update_db_row();
}

ACTION(on_action_db_export)
{
    UNUSED;
    if (ui.busy) return;
    char *f = choose_file("Datenbank exportieren", true, "duplikat-finder-datenbank.json",
                          "JSON (*.json)", "*.json");
    if (!f) return;
    char err[512];
    bool ok = df_db_export_json(f, err, sizeof err);
    show_info_bar(ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR, ok ? "Datenbank exportiert." : err);
    g_free(f);
}

ACTION(on_action_db_import)
{
    UNUSED;
    if (ui.busy) return;
    char *f = choose_file("Datenbank importieren", false, NULL, "JSON (*.json)", "*.json");
    if (!f) return;
    char err[512];
    int n = df_db_import_json(f, err, sizeof err);
    char *msg = n >= 0 ? g_strdup_printf("%d Einträge importiert.", n) : g_strdup(err);
    show_info_bar(n >= 0 ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR, msg);
    g_free(msg);
    g_free(f);
    update_db_row();
}

ACTION(on_action_notmatch_clear)
{
    UNUSED;
    if (!ask_confirmation("„Kein Duplikat“-Markierungen löschen?", "Als „kein Duplikat“ markierte "
                          "Paare werden beim nächsten Suchlauf wieder gemeldet.", "Abbrechen",
                          "Löschen", true))
        return;
    char *msg = g_strdup_printf("%d Paare gelöscht.", df_notmatch_clear());
    show_info_bar(GTK_MESSAGE_INFO, msg);
    g_free(msg);
}

ACTION(on_action_pair_test)
{
    UNUSED;
    pair_test_dialog();
}

ACTION(on_action_schedule)
{
    UNUSED;
    schedule_dialog();
}

ACTION(on_action_about)
{
    UNUSED;
    gtk_show_about_dialog(GTK_WINDOW(ui.win), "program-name", DF_NAME, "version", DF_VERSION,
                          "logo-icon-name", "duplikat-finder", "comments",
                          "Findet doppelte Videos und Bilder anhand ihres Inhalts – per CPU und "
                          "GPU (VAAPI), auch bei 100 000 Dateien und mehr.\n"
                          "Einstellungen, Datenbank und Protokolle: ~/.duplikat-finder",
                          NULL);
}

ACTION(on_action_quit)
{
    UNUSED;
    if (ui.win) gtk_window_close(GTK_WINDOW(ui.win));
}

static void on_startup(GApplication *app, gpointer d)
{
    (void)d;
    setlocale(LC_NUMERIC, "C");
    static const GActionEntry actions[] = {
        {.name = "open-log", .activate = on_action_open_log},
        {.name = "open-data", .activate = on_action_open_data},
        {.name = "open-result", .activate = on_action_open_result},
        {.name = "db-edit", .activate = on_action_db_edit},
        {.name = "db-clean", .activate = on_action_db_clean},
        {.name = "db-failed", .activate = on_action_db_failed},
        {.name = "db-relocate", .activate = on_action_db_relocate},
        {.name = "db-export", .activate = on_action_db_export},
        {.name = "db-import", .activate = on_action_db_import},
        {.name = "db-clear", .activate = on_action_db_clear},
        {.name = "ignored", .activate = on_action_ignored},
        {.name = "notmatch-clear", .activate = on_action_notmatch_clear},
        {.name = "pair-test", .activate = on_action_pair_test},
        {.name = "schedule", .activate = on_action_schedule},
        {.name = "about", .activate = on_action_about},
        {.name = "quit", .activate = on_action_quit},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), NULL);
    const char *quit_accels[] = {"<Control>q", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);
    const char *open_accels[] = {"<Control>o", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.open-result", open_accels);
}

static GtkWidget *build_menu_button(void)
{
    GMenu *menu = g_menu_new(), *sec1 = g_menu_new(), *db = g_menu_new(), *sec3 = g_menu_new(),
          *about = g_menu_new();
    g_menu_append(sec1, "Ergebnis öffnen …", "app.open-result");
    g_menu_append(sec1, "Zwei Dateien vergleichen …", "app.pair-test");
    g_menu_append(sec1, "Geplanter Suchlauf …", "app.schedule");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec1));
    g_menu_append(db, "Bearbeiten …", "app.db-edit");
    g_menu_append(db, "Aufräumen (fehlende Dateien)", "app.db-clean");
    g_menu_append(db, "Nicht lesbare Dateien erneut versuchen", "app.db-failed");
    g_menu_append(db, "Pfade ändern (Laufwerk umbenannt) …", "app.db-relocate");
    g_menu_append(db, "Exportieren …", "app.db-export");
    g_menu_append(db, "Importieren …", "app.db-import");
    g_menu_append(db, "Leeren …", "app.db-clear");
    g_menu_append_submenu(menu, "Datenbank", G_MENU_MODEL(db));
    g_menu_append(sec3, "Ignorierte Dateien …", "app.ignored");
    g_menu_append(sec3, "„Kein Duplikat“-Markierungen löschen …", "app.notmatch-clear");
    g_menu_append(sec3, "Lauf-Protokoll öffnen", "app.open-log");
    g_menu_append(sec3, "Datenordner öffnen", "app.open-data");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec3));
    g_menu_append(about, "Über Duplikat-Finder", "app.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(about));
    GtkWidget *menu_button = gtk_menu_button_new();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
    gtk_button_set_image(GTK_BUTTON(menu_button),
                         gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(menu_button, "Menü");
    g_object_unref(menu);
    g_object_unref(sec1);
    g_object_unref(db);
    g_object_unref(sec3);
    g_object_unref(about);
    return menu_button;
}

static gboolean on_gpu_done(gpointer p)
{
    (void)p;
    ui.gpu_ready = true;
    df_thumbs_set_gpu(&ui.gpu);
    if (ui.win) update_gpu_row();
    /* zum Testen: DUPLIKAT_FINDER_AUTOSTART=1 startet den Suchlauf gleich nach dem Start */
    if (ui.win && g_getenv("DUPLIKAT_FINDER_AUTOSTART") && ui.includes.n) start_scan();
    return G_SOURCE_REMOVE;
}

static gpointer gpu_check_thread(gpointer p)
{
    (void)p;
    df_gpu_detect(&ui.gpu);
    g_idle_add(on_gpu_done, NULL);
    return NULL;
}

static char *g_open_file;                       /* Ergebnis von der Befehlszeile */

static void on_activate(GtkApplication *app, gpointer d)
{
    (void)d;
    if (ui.win) {
        gtk_window_present(GTK_WINDOW(ui.win));
        return;
    }
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(css_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);
    static const int default_order[] = {DF_CRIT_DURATION, DF_CRIT_RESOLUTION, DF_CRIT_FPS,
                                        DF_CRIT_BITRATE, DF_CRIT_AUDIO_BITRATE, DF_CRIT_SIZE};
    ui.ncriteria = G_N_ELEMENTS(default_order);
    memcpy(ui.criteria, default_order, sizeof default_order);

    ui.win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ui.win), DF_NAME);
    gtk_window_set_default_size(GTK_WINDOW(ui.win), 1100, 860);
    gtk_window_set_icon_name(GTK_WINDOW(ui.win), "duplikat-finder");
    ui.header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(ui.header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(ui.header), DF_NAME);
    gtk_window_set_titlebar(GTK_WINDOW(ui.win), ui.header);
    ui.back_btn = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
    g_signal_connect(ui.back_btn, "clicked", G_CALLBACK(on_back_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(ui.header), ui.back_btn);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(ui.header), build_menu_button());

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ui.infobar = gtk_info_bar_new();
    gtk_info_bar_set_show_close_button(GTK_INFO_BAR(ui.infobar), TRUE);
    g_signal_connect(ui.infobar, "response", G_CALLBACK(on_info_bar_response), NULL);
    ui.info_label = new_label("", NULL);
    set_wrapping(ui.info_label, 100);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(ui.infobar))),
                      ui.info_label);
    ui.info_action = gtk_info_bar_add_button(GTK_INFO_BAR(ui.infobar), "", 1);
    gtk_box_pack_start(GTK_BOX(vbox), ui.infobar, FALSE, FALSE, 0);
    ui.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(ui.stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(ui.stack), 180);
    gtk_stack_add_named(GTK_STACK(ui.stack), build_setup_page(), "setup");
    gtk_stack_add_named(GTK_STACK(ui.stack), build_scan_page(), "scan");
    gtk_stack_add_named(GTK_STACK(ui.stack), build_results_page(), "results");
    gtk_box_pack_start(GTK_BOX(vbox), ui.stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(ui.win), vbox);
    g_signal_connect(ui.win, "delete-event", G_CALLBACK(on_window_delete), NULL);

    settings_load();
    rebuild_folder_list(false);
    rebuild_folder_list(true);
    if (ui.window_width > 0 && ui.window_height > 0)
        gtk_window_set_default_size(GTK_WINDOW(ui.win), ui.window_width, ui.window_height);
    if (ui.window_maximized) gtk_window_maximize(GTK_WINDOW(ui.win));
    g_signal_connect(ui.win, "configure-event", G_CALLBACK(on_window_configure), NULL);
    g_signal_connect(ui.win, "window-state-event", G_CALLBACK(on_window_state), NULL);
    gtk_widget_show_all(ui.win);
    gtk_widget_hide(ui.infobar);
    for (int i = 0; i < MAX_CPU_WORKERS + MAX_GPU_WORKERS; i++) gtk_widget_hide(ui.scan_jobs_rows[i]);
    gtk_widget_hide(ui.res_more_button);
    gtk_widget_hide(ui.res_empty);
    update_partial_rows();
    update_frames_sub();
    update_gpu_row();
    update_db_row();
    show_page("setup");
    if (!df_worker_path())
        show_info_bar(GTK_MESSAGE_ERROR, "Der Hilfsprozess duplikat-finder-worker fehlt – bitte das "
                      "Paket neu installieren.");
    g_thread_unref(g_thread_new("gpu", gpu_check_thread, NULL));
    if (g_open_file) {
        results_load_file(g_open_file, false);
    } else {
        /* neues Ergebnis eines geplanten Suchlaufs (neuer als das zuletzt angezeigte)? */
        GStatBuf sched, last;
        if (g_stat(DF_SCHEDULED_PATH, &sched) == 0 &&
            (g_stat(DF_RESULT_PATH, &last) != 0 || sched.st_mtime > last.st_mtime))
            show_info_bar_action(GTK_MESSAGE_INFO, "Es gibt ein neues Ergebnis vom geplanten "
                                 "Suchlauf.", "Öffnen", "open-scheduled");
    }
}

static int on_command_line(GApplication *app, GApplicationCommandLine *cl, gpointer d)
{
    (void)d;
    int argc;
    char **argv = g_application_command_line_get_arguments(cl, &argc);
    if (argc > 1) {
        GFile *f = g_application_command_line_create_file_for_arg(cl, argv[1]);
        g_free(g_open_file);
        g_open_file = g_file_get_path(f);
        g_object_unref(f);
    }
    g_strfreev(argv);
    if (ui.win && g_open_file && !ui.busy) results_load_file(g_open_file, false);
    g_application_activate(app);
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    df_init();
    df_fe.log = frontend_log;
    df_fe.phase = frontend_phase;
    df_fe.progress = frontend_progress;
    g_set_prgname("duplikat-finder");
    g_set_application_name(DF_NAME);
    ui.app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(ui.app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(ui.app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(ui.app, "command-line", G_CALLBACK(on_command_line), NULL);
    g_unix_signal_add(SIGINT, on_unix_signal, NULL);
    g_unix_signal_add(SIGTERM, on_unix_signal, NULL);
    g_unix_signal_add(SIGHUP, on_unix_signal, NULL);
    int rc = g_application_run(G_APPLICATION(ui.app), argc, argv);
    df_shutdown();
    g_object_unref(ui.app);
    return rc;
}
