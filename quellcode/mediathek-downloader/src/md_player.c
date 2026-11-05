/*
 * Mediathek Downloader – eingebauter Player
 * =========================================
 * playbin von GStreamer spielt die Datei, gtksink liefert das Bild als GTK-Widget. Über dem Bild
 * liegt eine Leiste mit Zeitleiste und Knöpfen; sie blendet sich bei laufendem Video nach kurzer
 * Zeit ohne Mausbewegung aus. Der Bus von GStreamer meldet sich über die GLib-Hauptschleife,
 * darum läuft alles im GTK-Hauptthread.
 *
 * Tasten: Leertaste/K Pause, ←/→ (J/L) 10 Sekunden, ↑/↓ Lautstärke, M stumm, F/F11 Vollbild,
 * N nächste Folge, Esc Vollbild verlassen oder Fenster schließen.
 */
#include <gst/gst.h>
#include <string.h>
#include "md_player.h"

#define PLAYER_TICK_MS 250
#define REPORT_SECONDS 5        /* so oft den Stand melden */
#define HIDE_MS 2500            /* Leiste ausblenden nach so langer Ruhe der Maus */
#define SEEK_STEP 10.0
#define VOLUME_STEP 0.05
#define PLAY_FLAG_TEXT (1 << 2) /* GstPlayFlags von playbin: Untertitel zeigen */
#define RESUME_MIN 5.0          /* kürzere Stände gelten als Anfang */
#define DEFAULT_WIDTH 1280

static const char *const PLAYER_CSS =
    ".zd-player { background-color: black; }\n"
    ".zd-player-bar { padding: 6px 10px; border-radius: 0; }\n"
    ".zd-player-bar button { min-height: 0; padding: 4px 8px; }\n"
    ".zd-player-title { font-weight: bold; }\n"
    ".zd-player-error { padding: 18px; border-radius: 10px; }\n";

typedef struct {
    GtkWidget *win, *video, *revealer, *scale, *time_label, *title_label;
    GtkWidget *play, *next, *audio_button, *text_button, *fullscreen, *volume;
    GtkWidget *error_box, *error_label;
    GstElement *playbin;
    char *path, *title, *next_path, *next_title;
    double start, duration;
    int want_audio, want_text;  /* gewählte Spuren, -1 = Vorgabe bzw. keine Untertitel */
    bool prerolled, playing, ended, is_fullscreen, updating, stay_paused;
    guint tick, hide_timer, inhibit;
    gint64 last_report;
    MdPlayerHooks hooks;
} Player;

static Player *player;          /* NULL = kein Fenster offen */
static double last_volume = 1.0;

static void load_file(Player *p, const char *path, const char *title, double start);

/* ---------------------------------------------------------------------------
 * Kleine Helfer
 * ------------------------------------------------------------------------- */
/* "1:02:03" oder "12:03" */
static char *format_clock(double seconds)
{
    int s = seconds > 0 ? (int)seconds : 0;
    if (s >= 3600) return g_strdup_printf("%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    return g_strdup_printf("%d:%02d", s / 60, s % 60);
}

static double query_seconds(GstElement *e, bool duration)
{
    gint64 value = 0;
    bool ok = duration ? gst_element_query_duration(e, GST_FORMAT_TIME, &value)
                       : gst_element_query_position(e, GST_FORMAT_TIME, &value);
    return ok && value > 0 ? (double)value / GST_SECOND : 0;
}

static double current_position(const Player *p)
{
    return p->ended ? p->duration : query_seconds(p->playbin, false);
}

/* Stand an die Oberfläche melden */
static void report(Player *p, bool left)
{
    if (!p->path || !p->hooks.progress) return;
    double position = p->prerolled ? current_position(p) : p->start;
    p->hooks.progress(p->path, position, p->duration, left, p->hooks.data);
    p->last_report = g_get_monotonic_time();
}

static void set_icon(GtkWidget *button, const char *icon)
{
    gtk_button_set_image(GTK_BUTTON(button),
                         gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR));
}

/* Knopf ohne Tastaturfokus: Leertaste und Pfeile gehören dem Player */
static GtkWidget *new_bar_button(const char *icon, const char *tooltip)
{
    GtkWidget *b = gtk_button_new();
    set_icon(b, icon);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(b, tooltip);
    gtk_widget_set_can_focus(b, FALSE);
    return b;
}

/* Name einer Spur aus ihren Angaben */
static char *track_name(GstTagList *tags, int index)
{
    char *title = NULL, *code = NULL, *name = NULL;
    if (tags) {
        gst_tag_list_get_string(tags, GST_TAG_TITLE, &title);
        gst_tag_list_get_string(tags, GST_TAG_LANGUAGE_CODE, &code);
    }
    if (title && *title) {
        name = g_strdup(title);
    } else if (code) {
        static const char *const LANGUAGES[][2] = {
            {"de", "Deutsch"}, {"deu", "Deutsch"}, {"ger", "Deutsch"}, {"en", "Englisch"},
            {"eng", "Englisch"}, {"fr", "Französisch"}, {"fra", "Französisch"},
            {"fre", "Französisch"}, {"it", "Italienisch"}, {"ita", "Italienisch"},
            {"es", "Spanisch"}, {"spa", "Spanisch"}, {"tr", "Türkisch"}, {"tur", "Türkisch"}};
        for (size_t i = 0; i < G_N_ELEMENTS(LANGUAGES) && !name; i++)
            if (!g_ascii_strcasecmp(code, LANGUAGES[i][0])) name = g_strdup(LANGUAGES[i][1]);
        if (!name) name = g_strdup(code);
    } else {
        name = g_strdup_printf("Spur %d", index + 1);
    }
    g_free(title);
    g_free(code);
    return name;
}

/* ---------------------------------------------------------------------------
 * Anzeige
 * ------------------------------------------------------------------------- */
static void update_time(Player *p)
{
    if (!p->prerolled) return;
    if (p->duration <= 0) p->duration = query_seconds(p->playbin, true);
    double position = current_position(p);
    char *now = format_clock(position), *total = format_clock(p->duration);
    char *text = g_strdup_printf("%s / %s", now, total);
    gtk_label_set_text(GTK_LABEL(p->time_label), text);
    p->updating = true;
    gtk_range_set_range(GTK_RANGE(p->scale), 0, MAX(p->duration, 1));
    gtk_range_set_value(GTK_RANGE(p->scale), position);
    p->updating = false;
    g_free(now);
    g_free(total);
    g_free(text);
}

static void update_play_button(Player *p)
{
    set_icon(p->play, p->ended ? "view-refresh-symbolic"
                      : p->playing ? "media-playback-pause-symbolic"
                      : "media-playback-start-symbolic");
    gtk_widget_set_tooltip_text(p->play, p->ended ? "Noch einmal" : p->playing ? "Pause"
                                                                   : "Abspielen");
}

/* Bildschirmschoner nur während der Wiedergabe sperren */
static void update_inhibit(Player *p)
{
    GtkApplication *app = GTK_APPLICATION(g_application_get_default());
    if (!app) return;
    if (p->playing && !p->inhibit)
        p->inhibit = gtk_application_inhibit(app, NULL, GTK_APPLICATION_INHIBIT_IDLE,
                                             "Video läuft");
    else if (!p->playing && p->inhibit) {
        gtk_application_uninhibit(app, p->inhibit);
        p->inhibit = 0;
    }
}

static void set_cursor_visible(Player *p, bool visible)
{
    GdkWindow *window = gtk_widget_get_window(p->win);
    if (!window) return;
    GdkCursor *cursor = visible ? NULL
                        : gdk_cursor_new_from_name(gdk_window_get_display(window), "none");
    gdk_window_set_cursor(window, cursor);
    if (cursor) g_object_unref(cursor);
}

static gboolean on_hide_timeout(gpointer data)
{
    Player *p = data;
    p->hide_timer = 0;
    if (p->playing) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(p->revealer), FALSE);
        set_cursor_visible(p, false);
    }
    return G_SOURCE_REMOVE;
}

/* Leiste zeigen und das Ausblenden neu ansetzen */
static void show_bar(Player *p)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(p->revealer), TRUE);
    set_cursor_visible(p, true);
    if (p->hide_timer) g_source_remove(p->hide_timer);
    p->hide_timer = g_timeout_add(HIDE_MS, on_hide_timeout, p);
}

static void show_error(Player *p, const char *text)
{
    gtk_label_set_text(GTK_LABEL(p->error_label), text);
    gtk_widget_show_all(p->error_box);
    p->playing = false;
    update_play_button(p);
    update_inhibit(p);
    gtk_revealer_set_reveal_child(GTK_REVEALER(p->revealer), TRUE);
    set_cursor_visible(p, true);
}

/* ---------------------------------------------------------------------------
 * Steuerung
 * ------------------------------------------------------------------------- */
static void seek_to(Player *p, double seconds, bool accurate)
{
    if (!p->prerolled) return;
    if (p->duration > 0) seconds = MIN(seconds, p->duration - 1);
    seconds = MAX(seconds, 0);
    GstSeekFlags flags = GST_SEEK_FLAG_FLUSH | (accurate ? GST_SEEK_FLAG_ACCURATE
                                                         : GST_SEEK_FLAG_KEY_UNIT);
    gst_element_seek_simple(p->playbin, GST_FORMAT_TIME, flags,
                            (gint64)(seconds * GST_SECOND));
    if (p->ended) {
        p->ended = false;
        gst_element_set_state(p->playbin, GST_STATE_PLAYING);
    }
    update_time(p);
}

static void toggle_play(Player *p)
{
    if (!p->prerolled) return;
    if (p->ended) {
        seek_to(p, 0, false);
        return;
    }
    gst_element_set_state(p->playbin, p->playing ? GST_STATE_PAUSED : GST_STATE_PLAYING);
    if (p->playing) report(p, false);
}

static void toggle_fullscreen(Player *p)
{
    if (p->is_fullscreen) gtk_window_unfullscreen(GTK_WINDOW(p->win));
    else gtk_window_fullscreen(GTK_WINDOW(p->win));
}

static void play_next(Player *p)
{
    if (!p->next_path) return;
    char *path = g_strdup(p->next_path), *title = g_strdup(p->next_title);
    load_file(p, path, title, 0);
    g_free(path);
    g_free(title);
}

static void change_volume(Player *p, double delta)
{
    double value = gtk_scale_button_get_value(GTK_SCALE_BUTTON(p->volume));
    gtk_scale_button_set_value(GTK_SCALE_BUTTON(p->volume), CLAMP(value + delta, 0, 1));
}

static void on_play_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    toggle_play(data);
}

static void on_back_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    Player *p = data;
    seek_to(p, current_position(p) - SEEK_STEP, true);
}

static void on_forward_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    Player *p = data;
    seek_to(p, current_position(p) + SEEK_STEP, true);
}

static void on_next_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    play_next(data);
}

static void on_fullscreen_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    toggle_fullscreen(data);
}

static void on_volume_changed(GtkScaleButton *b, double value, gpointer data)
{
    (void)b;
    Player *p = data;
    last_volume = value;
    g_object_set(p->playbin, "volume", value, "mute", FALSE, NULL);
}

/* Ziehen an der Zeitleiste */
static gboolean on_scale_change_value(GtkRange *range, GtkScrollType scroll, double value,
                                      gpointer data)
{
    (void)range;
    (void)scroll;
    Player *p = data;
    if (!p->updating) seek_to(p, value, false);
    return FALSE;
}

static void on_open_external_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    Player *p = data;
    char *uri = g_filename_to_uri(p->path, NULL, NULL);
    if (uri) gtk_show_uri_on_window(GTK_WINDOW(p->win), uri, GDK_CURRENT_TIME, NULL);
    g_free(uri);
}

/* ---------------------------------------------------------------------------
 * Ton und Untertitel
 * ------------------------------------------------------------------------- */
/* Untertitel lassen sich in playbin nur beim Start ein- und ausschalten: Datei an derselben
 * Stelle neu öffnen. Läuft verzögert, weil das Menü noch sein Signal ausliefert. */
static gboolean restart_pipeline(gpointer data)
{
    Player *p = data;
    if (!p->prerolled) return G_SOURCE_REMOVE;
    p->start = current_position(p);
    p->stay_paused = !p->playing && !p->ended;
    p->ended = false;
    gst_element_set_state(p->playbin, GST_STATE_NULL);
    guint flags = 0;
    g_object_get(p->playbin, "flags", &flags, NULL);
    flags = p->want_text < 0 ? flags & ~PLAY_FLAG_TEXT : flags | PLAY_FLAG_TEXT;
    g_object_set(p->playbin, "flags", flags, NULL);
    p->prerolled = false;
    gst_element_set_state(p->playbin, GST_STATE_PAUSED);
    return G_SOURCE_REMOVE;
}

static void on_track_chosen(GtkCheckMenuItem *item, gpointer data)
{
    if (!gtk_check_menu_item_get_active(item)) return;
    Player *p = data;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "index"));
    if (g_object_get_data(G_OBJECT(item), "text")) {
        if (index == p->want_text) return;
        p->want_text = index;
        g_idle_add(restart_pipeline, p);
    } else {
        p->want_audio = index;
        g_object_set(p->playbin, "current-audio", index, NULL);
    }
}

static GtkWidget *add_track_item(GtkWidget *menu, GSList **group, const char *label, int index,
                                 bool text, bool active, Player *p)
{
    GtkWidget *item = gtk_radio_menu_item_new_with_label(*group, label);
    *group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
    g_object_set_data(G_OBJECT(item), "index", GINT_TO_POINTER(index));
    if (text) g_object_set_data(G_OBJECT(item), "text", GINT_TO_POINTER(1));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), active);
    g_signal_connect(item, "toggled", G_CALLBACK(on_track_chosen), p);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

/* Auswahl für Ton (text = false) oder Untertitel; ohne Wahlmöglichkeit unsichtbar */
static void build_track_menu(Player *p, bool text)
{
    GtkWidget *button = text ? p->text_button : p->audio_button;
    int count = 0, current = 0;
    guint flags = 0;
    g_object_get(p->playbin, text ? "n-text" : "n-audio", &count,
                 text ? "current-text" : "current-audio", &current, "flags", &flags, NULL);
    bool shown = text ? (flags & PLAY_FLAG_TEXT) != 0 : true;
    GtkWidget *menu = gtk_menu_new();
    GSList *group = NULL;
    if (text) add_track_item(menu, &group, "Aus", -1, true, !shown, p);
    for (int i = 0; i < count; i++) {
        GstTagList *tags = NULL;
        g_signal_emit_by_name(p->playbin, text ? "get-text-tags" : "get-audio-tags", i, &tags);
        char *name = track_name(tags, i);
        add_track_item(menu, &group, name, i, text, shown && i == current, p);
        g_free(name);
        if (tags) gst_tag_list_unref(tags);
    }
    gtk_widget_show_all(menu);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(button), menu);
    gtk_widget_set_visible(button, text ? count > 0 : count > 1);
}

/* ---------------------------------------------------------------------------
 * GStreamer
 * ------------------------------------------------------------------------- */
static void finish_file(Player *p)
{
    p->ended = true;
    p->playing = false;
    report(p, false);
    if (p->next_path) {
        play_next(p);
        return;
    }
    gst_element_set_state(p->playbin, GST_STATE_PAUSED);
    update_play_button(p);
    update_inhibit(p);
    update_time(p);
    show_bar(p);
    if (p->is_fullscreen) gtk_window_unfullscreen(GTK_WINDOW(p->win));
}

static gboolean on_bus(GstBus *bus, GstMessage *message, gpointer data)
{
    (void)bus;
    Player *p = data;
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ASYNC_DONE:
        if (!p->prerolled) {
            p->prerolled = true;
            p->duration = query_seconds(p->playbin, true);
            if (p->want_audio >= 0) g_object_set(p->playbin, "current-audio", p->want_audio, NULL);
            if (p->want_text >= 0) g_object_set(p->playbin, "current-text", p->want_text, NULL);
            if (p->start > 0 && (p->duration <= 0 || p->start < p->duration - 1))
                gst_element_seek_simple(p->playbin, GST_FORMAT_TIME,
                                        GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                                        (gint64)(p->start * GST_SECOND));
            build_track_menu(p, false);
            build_track_menu(p, true);
            if (!p->stay_paused) gst_element_set_state(p->playbin, GST_STATE_PLAYING);
            p->stay_paused = false;
            update_time(p);
        }
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(p->playbin)) {
            GstState state, pending;
            gst_message_parse_state_changed(message, NULL, &state, &pending);
            /* beim Springen hält die Pipeline kurz an und läuft gleich weiter */
            if (pending == GST_STATE_PLAYING) break;
            p->playing = state == GST_STATE_PLAYING;
            update_play_button(p);
            update_inhibit(p);
            if (!p->playing) show_bar(p);
        }
        break;
    case GST_MESSAGE_EOS:
        finish_file(p);
        break;
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gst_message_parse_error(message, &err, NULL);
        char *text = g_strdup_printf("Die Datei lässt sich nicht abspielen:\n%s",
                                     err ? err->message : "unbekannter Fehler");
        gst_element_set_state(p->playbin, GST_STATE_NULL);
        show_error(p, text);
        g_free(text);
        g_clear_error(&err);
        break;
    }
    default:
        break;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_tick(gpointer data)
{
    Player *p = data;
    update_time(p);
    if (p->playing && g_get_monotonic_time() - p->last_report >= REPORT_SECONDS * G_USEC_PER_SEC)
        report(p, false);
    return G_SOURCE_CONTINUE;
}

/* Datei laden; die bisherige meldet vorher ihren Stand */
static void load_file(Player *p, const char *path, const char *title, double start)
{
    if (p->path) report(p, true);
    gst_element_set_state(p->playbin, GST_STATE_NULL);
    g_free(p->path);
    g_free(p->title);
    g_clear_pointer(&p->next_path, g_free);
    g_clear_pointer(&p->next_title, g_free);
    p->path = g_strdup(path);
    p->title = g_strdup(title ? title : "");
    p->start = start >= RESUME_MIN ? start : 0;
    p->want_audio = p->want_text = -1;
    p->stay_paused = false;
    p->duration = 0;
    p->prerolled = p->ended = p->playing = false;
    gtk_widget_hide(p->error_box);
    gtk_window_set_title(GTK_WINDOW(p->win), *p->title ? p->title : "Player");
    gtk_label_set_text(GTK_LABEL(p->title_label), p->title);
    gtk_label_set_text(GTK_LABEL(p->time_label), "");
    if (p->hooks.next) p->next_path = p->hooks.next(path, &p->next_title, p->hooks.data);
    gtk_widget_set_visible(p->next, p->next_path != NULL);
    if (p->next_title) {
        char *tip = g_strdup_printf("Nächste Folge: %s", p->next_title);
        gtk_widget_set_tooltip_text(p->next, tip);
        g_free(tip);
    }
    gtk_widget_hide(p->audio_button);
    gtk_widget_hide(p->text_button);
    char *uri = g_filename_to_uri(path, NULL, NULL);
    guint flags = 0;
    g_object_get(p->playbin, "flags", &flags, NULL);
    g_object_set(p->playbin, "uri", uri, "flags", flags & ~PLAY_FLAG_TEXT, NULL);
    g_free(uri);
    gst_element_set_state(p->playbin, GST_STATE_PAUSED);
    update_play_button(p);
    show_bar(p);
    p->last_report = g_get_monotonic_time();
}

/* ---------------------------------------------------------------------------
 * Fenster
 * ------------------------------------------------------------------------- */
static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer data)
{
    (void)w;
    Player *p = data;
    guint key = gdk_keyval_to_lower(e->keyval);
    /* Springen soll das Bild nicht mit der Leiste verdecken */
    if (key != GDK_KEY_Left && key != GDK_KEY_Right && key != GDK_KEY_j && key != GDK_KEY_l)
        show_bar(p);
    switch (key) {
    case GDK_KEY_space: case GDK_KEY_k: toggle_play(p); break;
    case GDK_KEY_Left: case GDK_KEY_j: seek_to(p, current_position(p) - SEEK_STEP, true); break;
    case GDK_KEY_Right: case GDK_KEY_l: seek_to(p, current_position(p) + SEEK_STEP, true); break;
    case GDK_KEY_Up: change_volume(p, VOLUME_STEP); break;
    case GDK_KEY_Down: change_volume(p, -VOLUME_STEP); break;
    case GDK_KEY_f: case GDK_KEY_F11: toggle_fullscreen(p); break;
    case GDK_KEY_n: play_next(p); break;
    case GDK_KEY_m: {
        gboolean mute = FALSE;
        g_object_get(p->playbin, "mute", &mute, NULL);
        g_object_set(p->playbin, "mute", !mute, NULL);
        break;
    }
    case GDK_KEY_Escape:
        if (p->is_fullscreen) gtk_window_unfullscreen(GTK_WINDOW(p->win));
        else gtk_widget_destroy(p->win);
        break;
    default:
        return FALSE;
    }
    return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer data)
{
    (void)w;
    (void)e;
    show_bar(data);
    return FALSE;
}

/* Klick aufs Bild: Pause, Doppelklick: Vollbild */
static gboolean on_video_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w;
    Player *p = data;
    if (e->button != GDK_BUTTON_PRIMARY) return FALSE;
    if (e->type == GDK_2BUTTON_PRESS) toggle_fullscreen(p);
    else if (e->type == GDK_BUTTON_PRESS) toggle_play(p);
    return TRUE;
}

static gboolean on_window_state(GtkWidget *w, GdkEventWindowState *e, gpointer data)
{
    (void)w;
    Player *p = data;
    p->is_fullscreen = (e->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
    set_icon(p->fullscreen, p->is_fullscreen ? "view-restore-symbolic"
                                             : "view-fullscreen-symbolic");
    return FALSE;
}

static void on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    Player *p = data;
    report(p, true);
    if (p->tick) g_source_remove(p->tick);
    if (p->hide_timer) g_source_remove(p->hide_timer);
    p->playing = false;
    update_inhibit(p);
    gst_element_set_state(p->playbin, GST_STATE_NULL);
    GstBus *bus = gst_element_get_bus(p->playbin);
    gst_bus_remove_watch(bus);
    gst_object_unref(bus);
    gst_object_unref(p->playbin);
    g_free(p->path);
    g_free(p->title);
    g_free(p->next_path);
    g_free(p->next_title);
    g_free(p);
    player = NULL;
}

static GtkWidget *build_bar(Player *p)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "osd");
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "zd-player-bar");
    p->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
    gtk_scale_set_draw_value(GTK_SCALE(p->scale), FALSE);
    gtk_widget_set_can_focus(p->scale, FALSE);
    g_signal_connect(p->scale, "change-value", G_CALLBACK(on_scale_change_value), p);
    gtk_box_pack_start(GTK_BOX(bar), p->scale, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    p->play = new_bar_button("media-playback-start-symbolic", "Abspielen");
    g_signal_connect(p->play, "clicked", G_CALLBACK(on_play_clicked), p);
    GtkWidget *back = new_bar_button("media-seek-backward-symbolic", "10 Sekunden zurück");
    g_signal_connect(back, "clicked", G_CALLBACK(on_back_clicked), p);
    GtkWidget *forward = new_bar_button("media-seek-forward-symbolic", "10 Sekunden vor");
    g_signal_connect(forward, "clicked", G_CALLBACK(on_forward_clicked), p);
    p->next = new_bar_button("media-skip-forward-symbolic", "Nächste Folge");
    gtk_widget_set_no_show_all(p->next, TRUE);
    g_signal_connect(p->next, "clicked", G_CALLBACK(on_next_clicked), p);
    p->time_label = gtk_label_new("");
    gtk_widget_set_margin_start(p->time_label, 8);
    p->title_label = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->title_label),
                                "zd-player-title");
    gtk_label_set_ellipsize(GTK_LABEL(p->title_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(p->title_label, 12);
    gtk_widget_set_margin_end(p->title_label, 12);
    gtk_box_pack_start(GTK_BOX(row), p->play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), forward, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->next, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->time_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->title_label, TRUE, TRUE, 0);

    p->audio_button = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(p->audio_button),
                         gtk_image_new_from_icon_name("audio-x-generic-symbolic",
                                                      GTK_ICON_SIZE_LARGE_TOOLBAR));
    gtk_button_set_relief(GTK_BUTTON(p->audio_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(p->audio_button, "Tonspur");
    p->text_button = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(p->text_button),
                         gtk_image_new_from_icon_name("media-view-subtitles-symbolic",
                                                      GTK_ICON_SIZE_LARGE_TOOLBAR));
    gtk_button_set_relief(GTK_BUTTON(p->text_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(p->text_button, "Untertitel");
    gtk_widget_set_can_focus(p->audio_button, FALSE);
    gtk_widget_set_can_focus(p->text_button, FALSE);
    gtk_widget_set_no_show_all(p->audio_button, TRUE);
    gtk_widget_set_no_show_all(p->text_button, TRUE);
    p->volume = gtk_volume_button_new();
    gtk_scale_button_set_value(GTK_SCALE_BUTTON(p->volume), last_volume);
    gtk_widget_set_can_focus(p->volume, FALSE);
    g_signal_connect(p->volume, "value-changed", G_CALLBACK(on_volume_changed), p);
    p->fullscreen = new_bar_button("view-fullscreen-symbolic", "Vollbild (F)");
    g_signal_connect(p->fullscreen, "clicked", G_CALLBACK(on_fullscreen_clicked), p);
    gtk_box_pack_end(GTK_BOX(row), p->fullscreen, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), p->volume, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), p->text_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), p->audio_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), row, FALSE, FALSE, 0);
    return bar;
}

static GtkWidget *build_error_box(Player *p)
{
    p->error_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(p->error_box), "osd");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->error_box), "zd-player-error");
    gtk_widget_set_halign(p->error_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(p->error_box, GTK_ALIGN_CENTER);
    p->error_label = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(p->error_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(p->error_label), 60);
    gtk_label_set_justify(GTK_LABEL(p->error_label), GTK_JUSTIFY_CENTER);
    GtkWidget *external = gtk_button_new_with_label("Mit dem Standard-Player öffnen");
    gtk_widget_set_halign(external, GTK_ALIGN_CENTER);
    g_signal_connect(external, "clicked", G_CALLBACK(on_open_external_clicked), p);
    gtk_box_pack_start(GTK_BOX(p->error_box), p->error_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->error_box), external, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(p->error_box, TRUE);
    return p->error_box;
}

static void load_css(void)
{
    static bool loaded;
    if (loaded) return;
    loaded = true;
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, PLAYER_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* 16:9, höchstens 1280 breit und vier Fünftel des Bildschirms */
static void set_default_size(GtkWidget *win, GtkWindow *parent)
{
    int width = DEFAULT_WIDTH, height = DEFAULT_WIDTH * 9 / 16;
    GdkWindow *window = parent ? gtk_widget_get_window(GTK_WIDGET(parent)) : NULL;
    GdkMonitor *monitor = window ? gdk_display_get_monitor_at_window(
                                       gdk_window_get_display(window), window) : NULL;
    if (monitor) {
        GdkRectangle area;
        gdk_monitor_get_workarea(monitor, &area);
        width = MIN(width, area.width * 4 / 5);
        height = MIN(width * 9 / 16, area.height * 4 / 5);
        width = MIN(width, height * 16 / 9);
    }
    gtk_window_set_default_size(GTK_WINDOW(win), width, height);
}

/* Fenster samt Pipeline anlegen; NULL = GStreamer fehlt */
static Player *player_new(GtkWindow *parent)
{
    GstElement *playbin = gst_element_factory_make("playbin", NULL);
    GstElement *sink = gst_element_factory_make("gtksink", NULL);
    if (!playbin || !sink) {
        if (playbin) gst_object_unref(playbin);
        if (sink) gst_object_unref(sink);
        return NULL;
    }
    load_css();
    Player *p = g_new0(Player, 1);
    p->playbin = playbin;
    g_object_get(sink, "widget", &p->video, NULL);
    g_object_set(playbin, "video-sink", sink, "volume", last_volume, NULL);
    GstBus *bus = gst_element_get_bus(playbin);
    gst_bus_add_watch(bus, on_bus, p);
    gst_object_unref(bus);

    p->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    set_default_size(p->win, parent);
    gtk_window_set_icon_name(GTK_WINDOW(p->win), "mediathek-downloader");
    if (parent) gtk_window_set_transient_for(GTK_WINDOW(p->win), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(p->win), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(p->win), "zd-player");
    gtk_widget_add_events(p->win, GDK_POINTER_MOTION_MASK);

    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *events = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(events), p->video);
    g_object_unref(p->video);       /* gehört jetzt dem Fenster */
    g_signal_connect(events, "button-press-event", G_CALLBACK(on_video_press), p);
    gtk_container_add(GTK_CONTAINER(overlay), events);
    p->revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(p->revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_valign(p->revealer, GTK_ALIGN_END);
    gtk_container_add(GTK_CONTAINER(p->revealer), build_bar(p));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), p->revealer);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), build_error_box(p));
    gtk_container_add(GTK_CONTAINER(p->win), overlay);

    g_signal_connect(p->win, "key-press-event", G_CALLBACK(on_key), p);
    g_signal_connect(p->win, "motion-notify-event", G_CALLBACK(on_motion), p);
    g_signal_connect(p->win, "window-state-event", G_CALLBACK(on_window_state), p);
    g_signal_connect(p->win, "destroy", G_CALLBACK(on_destroy), p);
    p->tick = g_timeout_add(PLAYER_TICK_MS, on_tick, p);
    gtk_widget_show_all(p->win);
    return p;
}

/* ---------------------------------------------------------------------------
 * Schnittstelle
 * ------------------------------------------------------------------------- */
bool md_player_available(char **why)
{
    GError *err = NULL;
    if (!gst_init_check(NULL, NULL, &err)) {
        *why = g_strdup_printf("GStreamer startet nicht: %s", err ? err->message : "?");
        g_clear_error(&err);
        return false;
    }
    const char *const needed[][2] = {{"playbin", "gstreamer1.0-plugins-base"},
                                     {"gtksink", "gstreamer1.0-gtk3"}};
    for (size_t i = 0; i < G_N_ELEMENTS(needed); i++) {
        GstElementFactory *factory = gst_element_factory_find(needed[i][0]);
        if (!factory) {
            *why = g_strdup_printf("Das Paket %s fehlt.", needed[i][1]);
            return false;
        }
        gst_object_unref(factory);
    }
    return true;
}

const char *md_player_current(void)
{
    return player ? player->path : NULL;
}

bool md_player_play(GtkWindow *parent, const char *path, const char *title, double start,
                    const MdPlayerHooks *hooks)
{
    if (!player) {
        char *why = NULL;
        if (!md_player_available(&why)) {
            g_free(why);
            return false;
        }
        player = player_new(parent);
        if (!player) return false;
    }
    /* der alte Stand geht noch an die bisherigen Empfänger */
    if (player->path) report(player, true);
    g_clear_pointer(&player->path, g_free);
    player->hooks = *hooks;
    load_file(player, path, title, start);
    gtk_window_present(GTK_WINDOW(player->win));
    return true;
}
