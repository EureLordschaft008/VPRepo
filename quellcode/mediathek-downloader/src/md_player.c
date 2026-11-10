/*
 * Mediathek Downloader – eingebauter Player
 * =========================================
 * playbin von GStreamer spielt die Datei, gtksink liefert das Bild als GTK-Widget. Der Player ist
 * eine Seite im Stack des Hauptfensters; im Vollbild verschwindet dessen Titelleiste. Über dem Bild
 * liegt eine Leiste mit Zeitleiste und Knöpfen; sie blendet sich bei laufendem Video nach kurzer
 * Zeit ohne Mausbewegung aus. Der Bus von GStreamer meldet sich über die GLib-Hauptschleife,
 * darum läuft alles im GTK-Hauptthread.
 *
 * Tasten: Leertaste/K Pause, ←/→ (J/L) 10 Sekunden, ↑/↓ Lautstärke, M stumm, F/F11 Vollbild,
 * N nächste Folge, A Tonspur, S Untertitel, Z schwarze Balken aus/ein, +/− Zoom, 0 ganzes Bild,
 * Esc Vollbild verlassen oder Player schließen.
 *
 * Bild im Bild: Wählt man bei laufendem Video einen anderen Reiter, zieht die Seite in ein kleines
 * Fenster über allen anderen und spielt dort weiter. Doppelklick oder der Knopf in der Leiste
 * holt sie zurück ins Hauptfenster.
 *
 * Priorität: Damit das Bild nicht ruckelt, wenn andere Programme den Rechner auslasten, laufen
 * die Threads von GStreamer (Lesen, Entpacken, Dekodieren) bevorzugt. Unter Linux vergibt das
 * rtkit über D-Bus (wie bei PipeWire, ohne Administratorrechte), unter Windows steigt die
 * Priorität des Programms, solange ein Video läuft.
 *
 * Untertitel: playbin gibt sie an einen appsink statt sie ins Bild zu rechnen; ein Label über
 * dem Bild zeigt sie. So lassen sie sich ohne Umbau der Pipeline umschalten, bleiben beim Zoom
 * lesbar und brauchen kein Pango-Plugin von GStreamer.
 *
 * Zoom: Das Bild-Widget von gtksink passt das Video immer ganz in seine Fläche ein. Für den
 * Zoom bekommt es darum eine größere Fläche als das Fenster; die Bildfläche (ZoomBox) hat ein
 * eigenes GdkWindow und schneidet ab, was übersteht.
 */
#include <gst/gst.h>
#include <string.h>
#ifdef G_OS_WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include "md_core.h"
#include "md_player.h"

#define PLAYER_TICK_MS 250
#define REPORT_SECONDS 5        /* so oft den Stand melden */
#define HIDE_MS 2500            /* Leiste ausblenden nach so langer Ruhe der Maus */
#define MOTION_MIN_PX 8.0       /* erst so weite Mausbewegung zeigt die Leiste */
#define SEEK_STEP 10.0
#define SKIP_TOLERANCE 0.6      /* Sprung zum Schlüsselbild darf so weit (Anteil) danebenliegen */
#define VOLUME_STEP 0.05
#define PLAY_FLAG_TEXT (1 << 2) /* GstPlayFlags von playbin: Untertitel-Spuren nutzen */
#define SUBTITLE_MAX_SECONDS 10.0       /* Untertitel ohne Dauer so lange zeigen */
#define RESUME_MIN 5.0          /* kürzere Stände gelten als Anfang */
#define SEEK_WAIT_MS 3000        /* länger dauert kein Sprung; sonst gilt wieder die echte Stelle */
#define OSD_MS 1500             /* Hinweis oben links so lange zeigen */
#define KEY_HELP_MS 10000       /* Tastenübersicht ("?") so lange */
#define ZOOM_STEP 1.1
#define ZOOM_MAX 4.0
#define PIP_WIDTH 480           /* Breite des Bild-im-Bild-Fensters */
#define PIP_MARGIN 24           /* Abstand zur Bildschirmecke */
#define PIP_TITLEBAR 48         /* Platz für die Titelleiste, die die Fenstergröße nicht enthält */

static const char *const PLAYER_CSS =
    ".zd-player { background-color: black; }\n"
    ".zd-player-bar { padding: 6px 10px; border-radius: 0; }\n"
    ".zd-player-bar button { min-height: 0; padding: 4px 8px; }\n"
    ".zd-player-pip { border-radius: 8px; }\n"
    ".zd-player-title { font-weight: bold; }\n"
    ".zd-player-osd { padding: 8px 14px; border-radius: 8px; font-weight: bold; }\n"
    ".zd-player-subtitle { color: white; background-color: rgba(0, 0, 0, 0.65);"
    " padding: 4px 12px; border-radius: 6px; }\n"
    ".zd-player-error { padding: 18px; border-radius: 10px; }\n"
    ".zd-player-skip { padding: 8px 16px; font-weight: bold; }\n";

typedef struct {
    GtkWidget *root, *toplevel, *video, *zoom_box, *revealer, *scale, *time_label, *title_label;
    GtkWidget *play, *next, *audio_button, *text_button, *fullscreen, *volume;
    GtkWidget *error_box, *error_label, *osd, *subtitle, *intro_button;
    GtkWidget *pip_window, *pip_revealer, *pip_play;  /* pip_window: NULL = im Hauptfenster */
    GtkStack *stack;
    GstElement *playbin, *sink;         /* sink: gtksink oder der Kasten mit vapostproc davor */
    GstElement *vpp, *vpp_filter, *gtk_sink;    /* vpp: NULL = Umrechnung im Prozessor */
    char *path, *title, *next_path, *next_title;
    double start, duration;
    double video_width, video_height;   /* Bildgröße samt Pixelseitenverhältnis, 0 = unbekannt */
    int want_audio, want_text;  /* gewählte Spuren, -1 = Vorgabe bzw. keine Untertitel */
    int audio_count, text_count;
    bool prerolled, playing, ended, is_fullscreen, updating, closing;
    bool seeking;               /* Sprung bei laufendem Video: kurzes Anhalten nicht melden */
    double seek_target;         /* >= 0: Sprung läuft, Zeitleiste zeigt schon das Ziel */
    gint64 seek_started;
    double skip_from, skip_delta;       /* laufender Sprung um ±10 s, skip_delta 0 = keiner */
    double subtitle_start, subtitle_end;    /* Stelle im Video, in der der Text gilt */
    int subtitle_size;          /* Schriftgröße in Pixeln, folgt der Bildhöhe */
    bool fill;                  /* Balken ausblenden: Inhalt füllt die Fläche */
    double zoom;                /* sonst: Vergrößerung gegenüber dem eingepassten Bild */
    bool bars_known, bars_found, bars_busy;
    MdCrop bars;                /* Bereich mit Inhalt, wenn Balken fest im Bild sind */
    bool intro_known, intro_found, intro_was_in;   /* intro_was_in: letzter Takt im Intro */
    double intro_start, intro_end;
    bool pointer_known;         /* pointer_x/y: Mausstelle, ab der eine Bewegung zählt */
    double pointer_x, pointer_y;
    guint tick, hide_timer, osd_timer, inhibit;
    gint64 last_report;
    MdPlayerHooks hooks;
} Player;

static Player *player;          /* NULL = kein Player offen */
static double last_volume = 1.0;
static bool auto_skip_intro;
static bool gpu_convert = true;     /* Einstellung */
static bool gpu_convert_failed;     /* hat in diesem Lauf einen Fehler ausgelöst */

static void load_file(Player *p, const char *path, const char *title, double start);
static void build_track_menu(Player *p, bool text);
static void hide_subtitle(Player *p);
static void pip_leave(Player *p);
static void drop_gpu_convert(Player *p);

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

/* Während eines Sprungs meldet GStreamer noch die alte Stelle: dann gilt das Ziel, sonst
 * springt die Zeitleiste hin und her, und ein zweites → zählte von der alten Stelle aus */
static double current_position(const Player *p)
{
    if (p->ended) return p->duration;
    if (p->seek_target >= 0 &&
        g_get_monotonic_time() - p->seek_started < SEEK_WAIT_MS * G_TIME_SPAN_MILLISECOND)
        return p->seek_target;
    return query_seconds(p->playbin, false);
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
    GtkWidget *buttons[] = {p->play, p->pip_play};
    for (size_t i = 0; i < G_N_ELEMENTS(buttons); i++) {
        set_icon(buttons[i], p->ended ? "view-refresh-symbolic"
                             : p->playing ? "media-playback-pause-symbolic"
                             : "media-playback-start-symbolic");
        gtk_widget_set_tooltip_text(buttons[i], p->ended ? "Noch einmal (Leertaste)"
                                                : p->playing ? "Pause (Leertaste)"
                                                : "Abspielen (Leertaste)");
    }
}

/* Bildschirmschoner nur während der Wiedergabe sperren */
/* Nice-Wert der Player-Threads unter Linux; PipeWire nutzt denselben, rtkit erlaubt bis -15 */
#define PLAYER_NICE (-11)

/* Den aufrufenden Thread bevorzugen oder wieder normal laufen lassen. Unter Linux erben
 * Threads, die er startet (etwa die des Dekoders), seinen Wert. */
static void boost_thread(bool on)
{
#ifdef G_OS_WIN32
    SetThreadPriority(GetCurrentThread(), on ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_NORMAL);
#else
    id_t tid = (id_t)syscall(SYS_gettid);
    /* einen höheren Nice-Wert darf jeder setzen, einen niedrigeren nur mit Rechten oder rtkit */
    if (!on || setpriority(PRIO_PROCESS, tid, PLAYER_NICE) == 0) {
        if (!on) setpriority(PRIO_PROCESS, tid, 0);
        return;
    }
    /* Verbindung bleibt offen: GLib hält sie nur schwach, ohne eigene Referenz schließt sie,
     * bevor rtkit den Absender nachschlagen kann */
    static GDBusConnection *bus;
    static gsize once;
    if (g_once_init_enter(&once)) {
        bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
        g_once_init_leave(&once, 1);
    }
    if (!bus) return;
    /* ohne Rückruf: der Streaming-Thread wartet nicht auf die Antwort */
    g_dbus_connection_call(bus, "org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1",
                           "org.freedesktop.RealtimeKit1", "MakeThreadHighPriority",
                           g_variant_new("(ti)", (guint64)tid, PLAYER_NICE), NULL,
                           G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
#endif
}

/* Läuft im Thread, der die Nachricht schickt: GStreamer meldet so Start und Ende jedes
 * Streaming-Threads */
static GstBusSyncReply on_bus_sync(GstBus *bus, GstMessage *message, gpointer data)
{
    (void)bus;
    (void)data;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STREAM_STATUS) {
        GstStreamStatusType type;
        GstElement *owner = NULL;
        gst_message_parse_stream_status(message, &type, &owner);
        if (type == GST_STREAM_STATUS_TYPE_ENTER) boost_thread(true);
        else if (type == GST_STREAM_STATUS_TYPE_LEAVE) boost_thread(false);
    }
    return GST_BUS_PASS;
}

/* Windows: das ganze Programm bevorzugen, solange ein Video läuft */
static void boost_process(bool on)
{
#ifdef G_OS_WIN32
    SetPriorityClass(GetCurrentProcess(), on ? ABOVE_NORMAL_PRIORITY_CLASS
                                             : NORMAL_PRIORITY_CLASS);
#else
    (void)on;
#endif
}

static void update_inhibit(Player *p)
{
    boost_process(p->playing);
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
    GdkWindow *window = gtk_widget_get_window(p->zoom_box);
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
        gtk_revealer_set_reveal_child(GTK_REVEALER(p->pip_revealer), FALSE);
        set_cursor_visible(p, false);
    }
    return G_SOURCE_REMOVE;
}

/* Leiste zeigen und das Ausblenden neu ansetzen; im kleinen Fenster die kleine Leiste */
static void show_bar(Player *p)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(p->pip_window ? p->pip_revealer : p->revealer),
                                  TRUE);
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
    gtk_revealer_set_reveal_child(GTK_REVEALER(p->pip_window ? p->pip_revealer : p->revealer),
                                  TRUE);
    set_cursor_visible(p, true);
}

static gboolean on_osd_timeout(gpointer data)
{
    Player *p = data;
    p->osd_timer = 0;
    gtk_widget_hide(p->osd);
    return G_SOURCE_REMOVE;
}

static void show_osd_for(Player *p, const char *text, guint ms)
{
    gtk_label_set_text(GTK_LABEL(p->osd), text);
    gtk_widget_show(p->osd);
    if (p->osd_timer) g_source_remove(p->osd_timer);
    p->osd_timer = g_timeout_add(ms, on_osd_timeout, p);
}

/* kurzer Hinweis oben links, ohne die Leiste über das Bild zu legen */
static void show_osd(Player *p, const char *text)
{
    show_osd_for(p, text, OSD_MS);
}

const char *md_player_keys_text(void)
{
    return "Leertaste oder K\tAbspielen / Pause\n"
           "← → oder J L\t10 Sekunden zurück / vor\n"
           "↑ ↓\tLautstärke\n"
           "M\tStumm\n"
           "A\tNächste Tonspur\n"
           "S\tUntertitel wechseln\n"
           "Z\tSchwarze Balken aus / ein\n"
           "+ −\tZoom\n"
           "0\tGanzes Bild\n"
           "F, F11 oder Doppelklick\tVollbild\n"
           "N\tNächste Folge\n"
           "I\tIntro überspringen\n"
           "Esc\tZurück\n"
           "? oder F1\tDiese Übersicht";
}

/* "?": alle Tasten oben links, ein zweites "?" blendet sie aus */
static void toggle_key_help(Player *p)
{
    static char *text;
    if (!text) {                                    /* Tabulatoren richten im Hinweis nichts aus */
        char **lines = g_strsplit(md_player_keys_text(), "\t", -1);
        text = g_strjoinv(":  ", lines);
        g_strfreev(lines);
    }
    if (gtk_widget_get_visible(p->osd) && !g_strcmp0(gtk_label_get_text(GTK_LABEL(p->osd)), text)) {
        if (p->osd_timer) g_source_remove(p->osd_timer);
        on_osd_timeout(p);
        return;
    }
    show_osd_for(p, text, KEY_HELP_MS);
}

/* ---------------------------------------------------------------------------
 * Zoom
 * ------------------------------------------------------------------------- */
/* Lage des ganzen Bildes in der Bildfläche width × height: eingepasst, gezoomt oder so groß,
 * dass der Inhalt ohne die schwarzen Balken ganz in die Fläche passt. Vom Inhalt geht dabei
 * nichts verloren, auch wenn Fläche und Inhalt verschiedene Seitenverhältnisse haben; nur die
 * Balken stehen über und werden abgeschnitten. */
static void video_rect(const Player *p, int width, int height, GdkRectangle *r)
{
    *r = (GdkRectangle){0, 0, width, height};
    double vw = p->video_width, vh = p->video_height;
    if (vw <= 0 || vh <= 0 || width <= 0 || height <= 0 || (!p->fill && p->zoom <= 1.0)) return;
    double fit = MIN(width / vw, height / vh);
    /* Inhalt in Bildkoordinaten; ohne erkannte Balken das ganze Bild */
    double cx = 0, cy = 0, cw = vw, ch = vh;
    if (p->fill && p->bars_found) {
        double sx = vw / p->bars.frame_width, sy = vh / p->bars.frame_height;
        cx = p->bars.x * sx;
        cy = p->bars.y * sy;
        cw = p->bars.width * sx;
        ch = p->bars.height * sy;
    }
    double scale = p->fill ? MIN(width / cw, height / ch) : fit * p->zoom;
    scale = CLAMP(scale, fit, fit * ZOOM_MAX);
    double rw = vw * scale, rh = vh * scale;
    double rx = width / 2.0 - (cx + cw / 2) * scale, ry = height / 2.0 - (cy + ch / 2) * scale;
    /* kein leerer Rand, solange das Bild breiter bzw. höher ist als die Fläche */
    rx = rw >= width ? CLAMP(rx, width - rw, 0) : (width - rw) / 2;
    ry = rh >= height ? CLAMP(ry, height - rh, 0) : (height - rh) / 2;
    /* abgerundete Lage, aufgerundete Größe: lieber ein Pixel zu viel Bild als ein Spalt */
    int x = (int)rx - (rx < (int)rx), y = (int)ry - (ry < (int)ry);
    *r = (GdkRectangle){x, y, (int)(rw + rx - x + 0.999), (int)(rh + ry - y + 0.999)};
}

typedef struct {
    GtkEventBox parent;
    Player *player;
} ZoomBox;

typedef struct {
    GtkEventBoxClass parent_class;
} ZoomBoxClass;

G_DEFINE_TYPE(ZoomBox, zoom_box, GTK_TYPE_EVENT_BOX)

static void zoom_box_size_allocate(GtkWidget *w, GtkAllocation *a)
{
    GTK_WIDGET_CLASS(zoom_box_parent_class)->size_allocate(w, a);
    Player *p = ((ZoomBox *)w)->player;
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(w));
    if (!p || !child || !gtk_widget_get_visible(child)) return;
    GdkRectangle r;
    video_rect(p, a->width, a->height, &r);
    if (!gtk_widget_get_has_window(w)) {
        r.x += a->x;
        r.y += a->y;
    }
    gtk_widget_size_allocate(child, &r);
}

static void zoom_box_class_init(ZoomBoxClass *klass)
{
    GTK_WIDGET_CLASS(klass)->size_allocate = zoom_box_size_allocate;
}

static void zoom_box_init(ZoomBox *box)
{
    (void)box;
}

/* Bildgröße aus den Caps von gtksink */
static void read_video_size(Player *p)
{
    GstPad *pad = gst_element_get_static_pad(p->sink, "sink");
    GstCaps *caps = pad ? gst_pad_get_current_caps(pad) : NULL;
    p->video_width = p->video_height = 0;
    if (caps && gst_caps_get_size(caps) > 0) {
        const GstStructure *st = gst_caps_get_structure(caps, 0);
        int width = 0, height = 0, par_n = 1, par_d = 1;
        gst_structure_get_int(st, "width", &width);
        gst_structure_get_int(st, "height", &height);
        if (!gst_structure_get_fraction(st, "pixel-aspect-ratio", &par_n, &par_d) || par_d <= 0)
            par_n = par_d = 1;
        p->video_width = (double)width * par_n / par_d;
        p->video_height = height;
    }
    if (caps) gst_caps_unref(caps);
    if (pad) gst_object_unref(pad);
}

static void apply_zoom(Player *p)
{
    gtk_widget_queue_resize(p->zoom_box);
}

typedef struct {
    char *path;
    MdCrop crop;
    bool found;
} BarsJob;

static gboolean on_bars_done(gpointer data)
{
    BarsJob *job = data;
    Player *p = player;
    if (p && p->path && !strcmp(p->path, job->path)) {
        p->bars_busy = false;
        p->bars_known = true;
        p->bars_found = job->found;
        p->bars = job->crop;
        if (p->fill) {
            apply_zoom(p);
            show_osd(p, job->found ? "Schwarze Balken ausgeblendet"
                                   : "Keine Balken im Bild – ganzes Bild");
        }
    }
    g_free(job->path);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer bars_thread(gpointer data)
{
    BarsJob *job = data;
    job->found = md_detect_bars(job->path, &job->crop);
    g_timeout_add_full(G_PRIORITY_DEFAULT, 0, on_bars_done, job, NULL);
    return NULL;
}

/* Balken im Bild suchen (ffmpeg, einige Sekunden); bis dahin bleibt das ganze Bild eingepasst */
static void find_bars(Player *p)
{
    if (p->bars_known || p->bars_busy || !p->path) return;
    p->bars_busy = true;
    BarsJob *job = g_new0(BarsJob, 1);
    job->path = g_strdup(p->path);
    g_thread_unref(g_thread_new("balken", bars_thread, job));
}

typedef struct {
    char *path;
    double start, end;
    bool found;
} IntroJob;

static gboolean on_intro_done(gpointer data)
{
    IntroJob *job = data;
    Player *p = player;
    if (p && p->path && !strcmp(p->path, job->path)) {
        p->intro_known = true;
        p->intro_found = job->found;
        p->intro_start = job->start;
        p->intro_end = job->end;
    }
    g_free(job->path);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer intro_thread(gpointer data)
{
    IntroJob *job = data;
    job->found = md_find_intro(job->path, &job->start, &job->end);
    g_timeout_add_full(G_PRIORITY_DEFAULT, 0, on_intro_done, job, NULL);
    return NULL;
}

/* Intro suchen (Ton benachbarter Folgen, beim ersten Mal einige Sekunden) */
static void find_intro(Player *p)
{
    p->intro_known = p->intro_found = p->intro_was_in = false;
    if (p->intro_button) gtk_widget_hide(p->intro_button);
    IntroJob *job = g_new0(IntroJob, 1);
    job->path = g_strdup(p->path);
    g_thread_unref(g_thread_new("intro", intro_thread, job));
}

static void set_fill(Player *p, bool fill)
{
    p->fill = fill;
    p->zoom = 1;
    if (fill) find_bars(p);
    apply_zoom(p);
    show_osd(p, !fill ? "Ganzes Bild"
                : p->bars_busy ? "Schwarze Balken werden gesucht …"
                : p->bars_found ? "Schwarze Balken ausgeblendet"
                : "Keine Balken im Bild – ganzes Bild");
}

/* Zoom in Schritten, ausgehend von der jetzigen Größe */
static void zoom_by(Player *p, double factor)
{
    int width = gtk_widget_get_allocated_width(p->zoom_box);
    int height = gtk_widget_get_allocated_height(p->zoom_box);
    double current = p->zoom;
    if (p->fill && p->video_width > 0 && width > 0 && height > 0) {
        GdkRectangle r;
        video_rect(p, width, height, &r);
        current = r.width / (p->video_width * MIN(width / p->video_width,
                                                   height / p->video_height));
    }
    p->fill = false;
    p->zoom = CLAMP(current * factor, 1.0, ZOOM_MAX);
    if (p->zoom < 1.01) p->zoom = 1.0;
    apply_zoom(p);
    char *text = g_strdup_printf("Zoom %d %%", (int)(p->zoom * 100 + 0.5));
    show_osd(p, p->zoom == 1.0 ? "Ganzes Bild" : text);
    g_free(text);
}

static void on_zoom_fit(GtkMenuItem *item, gpointer data)
{
    (void)item;
    set_fill(data, false);
}

static void on_zoom_fill(GtkMenuItem *item, gpointer data)
{
    (void)item;
    set_fill(data, true);
}

static void on_zoom_in(GtkMenuItem *item, gpointer data)
{
    (void)item;
    zoom_by(data, ZOOM_STEP);
}

static void on_zoom_out(GtkMenuItem *item, gpointer data)
{
    (void)item;
    zoom_by(data, 1 / ZOOM_STEP);
}

/* ---------------------------------------------------------------------------
 * Steuerung
 * ------------------------------------------------------------------------- */
/* mode: GST_SEEK_FLAG_ACCURATE (bildgenau, muss ab dem letzten Schlüsselbild dekodieren) oder
 * GST_SEEK_FLAG_KEY_UNIT samt SNAP-Flags (sofort, landet auf einem Schlüsselbild) */
static void seek_to(Player *p, double seconds, GstSeekFlags mode)
{
    if (!p->prerolled) return;
    if (p->duration > 0) seconds = MIN(seconds, p->duration - 1);
    seconds = MAX(seconds, 0);
    GstSeekFlags flags = GST_SEEK_FLAG_FLUSH | mode;
    /* GStreamer meldet beim Springen kurz "pausiert"; das soll die Leiste nicht zeigen */
    if (p->playing) p->seeking = true;
    p->seek_target = seconds;
    hide_subtitle(p);
    p->seek_started = g_get_monotonic_time();
    if (!gst_element_seek_simple(p->playbin, GST_FORMAT_TIME, flags,
                                 (gint64)(seconds * GST_SECOND)))
        p->seek_target = -1;
    if (p->ended) {
        p->ended = false;
        gst_element_set_state(p->playbin, GST_STATE_PLAYING);
    }
    update_time(p);
}

/* Springen um delta Sekunden ohne Wartezeit: zum nächstgelegenen Schlüsselbild. Bildgenau
 * dauerte es bei AV1 mit einem Schlüsselbild alle 10 s bis zu anderthalb Sekunden. Liegt das
 * Schlüsselbild nicht in der gewünschten Richtung, folgt in on_bus ein genauer Sprung. */
static void skip(Player *p, double delta)
{
    if (!p->prerolled) return;
    double from = current_position(p);
    p->skip_from = from;
    p->skip_delta = delta;
    seek_to(p, from + delta, GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_NEAREST);
}

static void toggle_play(Player *p)
{
    if (!p->prerolled) return;
    if (p->ended) {
        seek_to(p, 0, GST_SEEK_FLAG_KEY_UNIT);
        return;
    }
    p->seeking = false;
    gst_element_set_state(p->playbin, p->playing ? GST_STATE_PAUSED : GST_STATE_PLAYING);
    if (p->playing) report(p, false);
}

static void toggle_fullscreen(Player *p)
{
    if (p->pip_window) return;
    if (p->is_fullscreen) gtk_window_unfullscreen(GTK_WINDOW(p->toplevel));
    else gtk_window_fullscreen(GTK_WINDOW(p->toplevel));
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
    double value = CLAMP(gtk_scale_button_get_value(GTK_SCALE_BUTTON(p->volume)) + delta, 0, 1);
    gtk_scale_button_set_value(GTK_SCALE_BUTTON(p->volume), value);
    char *text = g_strdup_printf("Lautstärke %d %%", (int)(value * 100 + 0.5));
    show_osd(p, text);
    g_free(text);
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
    skip(p, -SEEK_STEP);
}

static void on_forward_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    Player *p = data;
    skip(p, SEEK_STEP);
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
    if (!p->updating) seek_to(p, value, GST_SEEK_FLAG_KEY_UNIT);
    return FALSE;
}

static void on_open_external_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    Player *p = data;
    char *uri = g_filename_to_uri(p->path, NULL, NULL);
    if (uri) gtk_show_uri_on_window(GTK_WINDOW(p->toplevel), uri, GDK_CURRENT_TIME, NULL);
    g_free(uri);
}

/* ---------------------------------------------------------------------------
 * Ton und Untertitel
 * ------------------------------------------------------------------------- */
static void hide_subtitle(Player *p)
{
    gtk_widget_hide(p->subtitle);
    p->subtitle_end = -1;
}

/* Untertitel umschalten: playbin wählt die Spur, gezeigt wird nur bei gewählter Spur */
static void apply_text(Player *p)
{
    if (p->want_text >= 0) g_object_set(p->playbin, "current-text", p->want_text, NULL);
    else hide_subtitle(p);
}

typedef struct {
    Player *player;
    char *text;
    bool markup;
    double start, duration;
} SubtitleCue;

/* Text eines Untertitels zeigen (Hauptthread) */
static gboolean on_subtitle(gpointer data)
{
    SubtitleCue *cue = data;
    Player *p = cue->player;
    if (p == player && !p->closing && p->want_text >= 0 && p->prerolled) {
        char *clean = NULL;
        /* Markup nur, wenn es gültig ist; sonst als reiner Text */
        if (cue->markup && pango_parse_markup(cue->text, -1, 0, NULL, &clean, NULL, NULL)) {
            gtk_label_set_markup(GTK_LABEL(p->subtitle), cue->text);
        } else {
            gtk_label_set_text(GTK_LABEL(p->subtitle), cue->text);
        }
        g_free(clean);
        g_strstrip(cue->text);
        gtk_widget_set_visible(p->subtitle, *cue->text != 0);
        p->subtitle_start = cue->start;
        p->subtitle_end = cue->start + (cue->duration > 0 ? cue->duration : SUBTITLE_MAX_SECONDS);
    }
    g_free(cue->text);
    g_free(cue);
    return G_SOURCE_REMOVE;
}

/* appsink "new-sample", im Streaming-Thread: zur Spielzeit des Untertitels */
static GstFlowReturn on_text_sample(GstElement *sink, gpointer data)
{
    GstSample *sample = NULL;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (!sample) return GST_FLOW_OK;
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    const GstSegment *segment = gst_sample_get_segment(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    GstMapInfo map;
    if (buffer && GST_BUFFER_PTS_IS_VALID(buffer) && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        SubtitleCue *cue = g_new0(SubtitleCue, 1);
        cue->player = data;
        cue->text = g_utf8_make_valid((const char *)map.data, (gssize)map.size);
        const char *format = caps && gst_caps_get_size(caps)
                             ? gst_structure_get_string(gst_caps_get_structure(caps, 0), "format")
                             : NULL;
        cue->markup = !g_strcmp0(format, "pango-markup");
        guint64 pts = GST_BUFFER_PTS(buffer);
        if (segment) pts = gst_segment_to_stream_time(segment, GST_FORMAT_TIME, pts);
        cue->start = (double)pts / GST_SECOND;
        cue->duration = GST_BUFFER_DURATION_IS_VALID(buffer)
                        ? (double)GST_BUFFER_DURATION(buffer) / GST_SECOND : 0;
        gst_buffer_unmap(buffer, &map);
        if (GST_CLOCK_TIME_IS_VALID(pts)) {
            /* nicht als Idle: bei laufendem Video käme der Text sonst nie an die Reihe */
            g_timeout_add_full(G_PRIORITY_DEFAULT, 0, on_subtitle, cue, NULL);
        } else {                                    /* vor dem Anfang des Abschnitts */
            g_free(cue->text);
            g_free(cue);
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* Untertitel abgelaufen oder weggesprungen? Schrift passend zur Bildhöhe */
static void update_subtitle(Player *p)
{
    if (p->subtitle_end >= 0) {
        double now = current_position(p);
        if (now >= p->subtitle_end || now < p->subtitle_start - 1) hide_subtitle(p);
    }
    int size = MAX(gtk_widget_get_allocated_height(p->root) / 22, 14);
    if (size != p->subtitle_size) {
        p->subtitle_size = size;
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_size_new_absolute(size * PANGO_SCALE));
        gtk_label_set_attributes(GTK_LABEL(p->subtitle), attrs);
        pango_attr_list_unref(attrs);
        gtk_widget_set_margin_bottom(p->subtitle, size * 2);
    }
}

/* Taste A: nächste Tonspur */
static void next_audio(Player *p)
{
    if (!p->prerolled) return;
    if (p->audio_count < 2) {
        show_osd(p, p->audio_count ? "Nur eine Tonspur" : "Keine Tonspur");
        return;
    }
    int current = 0;
    g_object_get(p->playbin, "current-audio", &current, NULL);
    p->want_audio = (current + 1) % p->audio_count;
    g_object_set(p->playbin, "current-audio", p->want_audio, NULL);
    GstTagList *tags = NULL;
    g_signal_emit_by_name(p->playbin, "get-audio-tags", p->want_audio, &tags);
    char *name = track_name(tags, p->want_audio), *text = g_strdup_printf("Ton: %s", name);
    show_osd(p, text);
    g_free(name);
    g_free(text);
    if (tags) gst_tag_list_unref(tags);
    build_track_menu(p, false);
}

/* Taste S: Untertitel aus → erste Spur → … → aus */
static void next_text(Player *p)
{
    if (!p->prerolled) return;
    if (!p->text_count) {
        show_osd(p, "Keine Untertitel");
        return;
    }
    p->want_text = p->want_text + 1 >= p->text_count ? -1 : p->want_text + 1;
    if (p->want_text < 0) {
        show_osd(p, "Untertitel aus");
    } else {
        GstTagList *tags = NULL;
        g_signal_emit_by_name(p->playbin, "get-text-tags", p->want_text, &tags);
        char *name = track_name(tags, p->want_text);
        char *text = g_strdup_printf("Untertitel: %s", name);
        show_osd(p, text);
        g_free(name);
        g_free(text);
        if (tags) gst_tag_list_unref(tags);
    }
    apply_text(p);
    build_track_menu(p, true);
}

static void on_track_chosen(GtkCheckMenuItem *item, gpointer data)
{
    if (!gtk_check_menu_item_get_active(item)) return;
    Player *p = data;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "index"));
    if (g_object_get_data(G_OBJECT(item), "text")) {
        if (index == p->want_text) return;
        p->want_text = index;
        apply_text(p);
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
    bool shown = text ? p->want_text >= 0 : true;
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
    if (text) p->text_count = count;
    else p->audio_count = count;
    /* immer sichtbar, damit man weiß, wo es die Wahl gibt; ohne Wahl nur grau */
    bool choice = text ? count > 0 : count > 1;
    if (!choice) {
        GtkWidget *none = gtk_menu_item_new_with_label(text ? "Keine Untertitel in dieser Datei"
                                                            : "Nur eine Tonspur");
        gtk_widget_set_sensitive(none, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), none);
    }
    gtk_widget_show_all(menu);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(button), menu);
    gtk_widget_set_sensitive(button, choice);
    gtk_widget_set_tooltip_text(button, text ? (choice ? "Untertitel (S)" : "Keine Untertitel")
                                             : (choice ? "Tonspur (A)" : "Nur eine Tonspur"));
}

/* ---------------------------------------------------------------------------
 * GStreamer
 * ------------------------------------------------------------------------- */
static void finish_file(Player *p)
{
    p->ended = true;
    p->playing = false;
    p->seeking = false;
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
    if (p->is_fullscreen) gtk_window_unfullscreen(GTK_WINDOW(p->toplevel));
}

static gboolean on_bus(GstBus *bus, GstMessage *message, gpointer data)
{
    (void)bus;
    Player *p = data;
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ASYNC_DONE:
        p->seek_target = -1;                        /* Sprung fertig */
        if (p->skip_delta != 0) {
            /* das Schlüsselbild lag zu weit weg oder in der falschen Richtung: genau springen */
            double delta = p->skip_delta, landed = query_seconds(p->playbin, false);
            p->skip_delta = 0;
            bool backwards = delta > 0 ? landed <= p->skip_from + 1 : landed >= p->skip_from - 1;
            if (backwards || ABS(landed - (p->skip_from + delta)) > ABS(delta) * SKIP_TOLERANCE)
                seek_to(p, p->skip_from + delta, GST_SEEK_FLAG_ACCURATE);
        }
        if (!p->prerolled) {
            p->prerolled = true;
            p->duration = query_seconds(p->playbin, true);
            read_video_size(p);
            apply_zoom(p);
            if (p->want_audio >= 0) g_object_set(p->playbin, "current-audio", p->want_audio, NULL);
            apply_text(p);
            if (p->start > 0 && (p->duration <= 0 || p->start < p->duration - 1))
                gst_element_seek_simple(p->playbin, GST_FORMAT_TIME,
                                        GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                                        (gint64)(p->start * GST_SECOND));
            build_track_menu(p, false);
            build_track_menu(p, true);
            gst_element_set_state(p->playbin, GST_STATE_PLAYING);
            update_time(p);
        }
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(p->playbin)) {
            GstState state, pending;
            gst_message_parse_state_changed(message, NULL, &state, &pending);
            /* beim Springen hält die Pipeline kurz an und läuft gleich weiter */
            if (pending == GST_STATE_PLAYING) break;
            if (p->seeking && state != GST_STATE_PLAYING) break;
            p->seeking = false;
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
        if (p->vpp && p->path) {
            /* vielleicht liegt es an der Umrechnung in der Grafikkarte: einmal ohne versuchen */
            md_log("Player: Fehler mit Umrechnung in der Grafikkarte (%s), weiter ohne",
                   err ? err->message : "?");
            g_clear_error(&err);
            double at = p->prerolled ? current_position(p) : p->start;
            char *path = g_strdup(p->path), *title = g_strdup(p->title);
            gst_element_set_state(p->playbin, GST_STATE_NULL);
            drop_gpu_convert(p);
            load_file(p, path, title, at);
            g_free(path);
            g_free(title);
            break;
        }
        /* erst was zu tun ist, der GStreamer-Text nur als Detail */
        char *text = g_strdup_printf("Die Datei lässt sich hier nicht abspielen. Vielleicht ist "
                                     "sie unvollständig – mit dem Standard-Player versuchen.\n\n"
                                     "Details: %s", err ? err->message : "unbekannter Fehler");
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

static void skip_intro(Player *p)
{
    if (!p->intro_found) return;
    gtk_widget_hide(p->intro_button);
    seek_to(p, p->intro_end, GST_SEEK_FLAG_ACCURATE);
}

static void on_intro_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    skip_intro(data);
}

/* Läuft das Intro: Knopf zeigen oder, wenn eingestellt, gleich überspringen. Wer
 * zurückspult, ins Intro hinein, sieht es: übersprungen wird nur beim Hineinlaufen */
static void update_intro(Player *p)
{
    if (!p->intro_found || !p->prerolled) return;
    double pos = current_position(p);
    bool in = !p->ended && pos >= p->intro_start - 0.2 && pos < p->intro_end - 1.0;
    bool entered = in && !p->intro_was_in;
    /* vor dem Start zählt das Hineinlaufen erst, wenn das Video läuft */
    if (p->playing || !in) p->intro_was_in = in;
    if (entered && auto_skip_intro && p->playing && p->seek_target < 0) {
        skip_intro(p);
        show_osd(p, "Intro übersprungen");
        p->intro_was_in = false;
        return;
    }
    gtk_widget_set_visible(p->intro_button, in && !p->pip_window);
}

static gboolean on_tick(gpointer data)
{
    Player *p = data;
    update_time(p);
    update_subtitle(p);
    update_intro(p);
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
    hide_subtitle(p);
    p->audio_count = p->text_count = 0;
    p->seeking = false;
    p->seek_target = -1;
    p->skip_delta = 0;
    p->duration = 0;
    p->prerolled = p->ended = p->playing = false;
    /* Zoom gilt weiter, die Balken gehören zur Datei */
    p->bars_known = p->bars_found = p->bars_busy = false;
    if (p->fill) find_bars(p);
    find_intro(p);
    gtk_widget_hide(p->error_box);
    gtk_label_set_text(GTK_LABEL(p->title_label), p->title);
    if (p->pip_window) gtk_window_set_title(GTK_WINDOW(p->pip_window), p->title);
    gtk_label_set_text(GTK_LABEL(p->time_label), "");
    if (p->hooks.next) p->next_path = p->hooks.next(path, &p->next_title, p->hooks.data);
    gtk_widget_set_visible(p->next, p->next_path != NULL);
    if (p->next_title) {
        char *tip = g_strdup_printf("Nächste Folge (N): %s", p->next_title);
        gtk_widget_set_tooltip_text(p->next, tip);
        g_free(tip);
    }
    gtk_widget_set_sensitive(p->audio_button, FALSE);
    gtk_widget_set_sensitive(p->text_button, FALSE);
    char *uri = g_filename_to_uri(path, NULL, NULL);
    guint flags = 0;
    g_object_get(p->playbin, "flags", &flags, NULL);
    g_object_set(p->playbin, "uri", uri, "flags", flags | PLAY_FLAG_TEXT, NULL);
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
    Player *p = data;
    /* im kleinen Fenster gehören die Tasten des Hauptfensters wieder den anderen Seiten */
    if (w != (p->pip_window ? p->pip_window : p->toplevel)) return FALSE;
    guint key = gdk_keyval_to_lower(e->keyval);
    /* Springen, Zoom und Spurwechsel sollen das Bild nicht mit der Leiste verdecken; sie zeigen
     * höchstens einen kurzen Hinweis */
    static const guint QUIET[] = {GDK_KEY_Left, GDK_KEY_Right, GDK_KEY_j, GDK_KEY_l, GDK_KEY_z,
                                  GDK_KEY_plus, GDK_KEY_KP_Add, GDK_KEY_minus,
                                  GDK_KEY_KP_Subtract, GDK_KEY_0, GDK_KEY_KP_0, GDK_KEY_a,
                                  GDK_KEY_s, GDK_KEY_Up, GDK_KEY_Down, GDK_KEY_question,
                                  GDK_KEY_F1};
    bool quiet = false;
    for (size_t i = 0; i < G_N_ELEMENTS(QUIET); i++) quiet |= key == QUIET[i];
    /* die Hand am Touchpad oder ein vom System gemeldetes Mausereignis während des Tippens soll
     * die Leiste nicht zeigen: Bewegung zählt erst wieder ab der jetzigen Mausstelle */
    p->pointer_known = false;
    if (!quiet) show_bar(p);
    switch (key) {
    case GDK_KEY_space: case GDK_KEY_k: toggle_play(p); break;
    case GDK_KEY_Left: case GDK_KEY_j: skip(p, -SEEK_STEP); break;
    case GDK_KEY_Right: case GDK_KEY_l: skip(p, SEEK_STEP); break;
    case GDK_KEY_Up: change_volume(p, VOLUME_STEP); break;
    case GDK_KEY_Down: change_volume(p, -VOLUME_STEP); break;
    case GDK_KEY_f: case GDK_KEY_F11: toggle_fullscreen(p); break;
    case GDK_KEY_n: play_next(p); break;
    case GDK_KEY_i:
        if (p->intro_was_in) skip_intro(p);
        break;
    case GDK_KEY_a: next_audio(p); break;
    case GDK_KEY_s: next_text(p); break;
    case GDK_KEY_z: set_fill(p, !p->fill); break;
    case GDK_KEY_plus: case GDK_KEY_KP_Add: zoom_by(p, ZOOM_STEP); break;
    case GDK_KEY_minus: case GDK_KEY_KP_Subtract: zoom_by(p, 1 / ZOOM_STEP); break;
    case GDK_KEY_0: case GDK_KEY_KP_0: set_fill(p, false); break;
    case GDK_KEY_question: case GDK_KEY_F1: toggle_key_help(p); break;
    case GDK_KEY_m: {
        gboolean mute = FALSE;
        g_object_get(p->playbin, "mute", &mute, NULL);
        g_object_set(p->playbin, "mute", !mute, NULL);
        break;
    }
    case GDK_KEY_Escape:
        if (p->is_fullscreen) gtk_window_unfullscreen(GTK_WINDOW(p->toplevel));
        else md_player_close();
        break;
    default:
        return FALSE;
    }
    return TRUE;
}

/* Leiste nur bei echter Bewegung: Wackler um ein paar Pixel und Ereignisse ohne Bewegung, die
 * Desktop oder Touchpad auch beim Springen per Taste schicken, zeigten sie sonst */
static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer data)
{
    (void)w;
    Player *p = data;
    if (!p->pointer_known) {
        p->pointer_known = true;
        p->pointer_x = e->x_root;
        p->pointer_y = e->y_root;
        return FALSE;
    }
    double dx = e->x_root - p->pointer_x, dy = e->y_root - p->pointer_y;
    if (dx * dx + dy * dy < MOTION_MIN_PX * MOTION_MIN_PX) return FALSE;
    p->pointer_x = e->x_root;
    p->pointer_y = e->y_root;
    show_bar(p);
    return FALSE;
}

/* zurück ins Hauptfenster */
static void pip_restore(Player *p)
{
    pip_leave(p);
    gtk_stack_set_visible_child(p->stack, p->root);
    gtk_window_present(GTK_WINDOW(p->toplevel));
}

/* Klick aufs Bild: Pause, Doppelklick: Vollbild bzw. zurück ins Hauptfenster */
static gboolean on_video_press(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    (void)w;
    Player *p = data;
    if (e->button != GDK_BUTTON_PRIMARY) return FALSE;
    if (e->type == GDK_2BUTTON_PRESS && p->pip_window) pip_restore(p);
    else if (e->type == GDK_2BUTTON_PRESS) toggle_fullscreen(p);
    else if (e->type == GDK_BUTTON_PRESS) toggle_play(p);
    return TRUE;
}

/* Titelleiste samt Reitern nur außerhalb des Vollbilds */
static void show_titlebar(Player *p, bool shown)
{
    GtkWidget *titlebar = gtk_window_get_titlebar(GTK_WINDOW(p->toplevel));
    if (titlebar) gtk_widget_set_visible(titlebar, shown);
}

static gboolean on_window_state(GtkWidget *w, GdkEventWindowState *e, gpointer data)
{
    (void)w;
    Player *p = data;
    p->is_fullscreen = (e->new_window_state & GDK_WINDOW_STATE_FULLSCREEN) != 0;
    show_titlebar(p, !p->is_fullscreen);
    set_icon(p->fullscreen, p->is_fullscreen ? "view-restore-symbolic"
                                             : "view-fullscreen-symbolic");
    return FALSE;
}

static void on_close_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    md_player_close();
}

/* Hauptfenster zu (Programmende): auch ein kleines Fenster geht mit und meldet den Stand */
static void on_toplevel_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    Player *p = data;
    if (p->closing) return;
    p->closing = true;
    gtk_widget_destroy(p->root);
}

/* ---------------------------------------------------------------------------
 * Bild im Bild
 * ------------------------------------------------------------------------- */
/* Seite in einen anderen Behälter umhängen, ohne sie zu zerstören */
static void move_page(Player *p, GtkWidget *to)
{
    g_object_ref(p->root);
    gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(p->root)), p->root);
    if (GTK_IS_STACK(to)) gtk_stack_add_named(GTK_STACK(to), p->root, "player");
    else gtk_container_add(GTK_CONTAINER(to), p->root);
    g_object_unref(p->root);
}

/* Seite zurück in den Stack, das kleine Fenster zu */
static void pip_leave(Player *p)
{
    GtkWidget *window = p->pip_window;
    if (!window) return;
    p->pip_window = NULL;
    g_signal_handlers_disconnect_by_data(window, p);
    move_page(p, GTK_WIDGET(p->stack));
    gtk_widget_hide(p->pip_revealer);
    gtk_widget_show(p->revealer);
    gtk_widget_destroy(window);
    show_bar(p);
}

static gboolean on_pip_delete(GtkWidget *w, GdkEvent *e, gpointer data)
{
    (void)w;
    (void)e;
    (void)data;
    md_player_close();
    return TRUE;
}

static void on_pip_restore_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    pip_restore(data);
}

/* kleine Leiste des Bild-im-Bild-Fensters: Pause, zurück, schließen */
static GtkWidget *build_pip_bar(Player *p)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    GtkStyleContext *style = gtk_widget_get_style_context(bar);
    gtk_style_context_add_class(style, "osd");
    gtk_style_context_add_class(style, "zd-player-bar");
    gtk_style_context_add_class(style, "zd-player-pip");
    p->pip_play = new_bar_button("media-playback-start-symbolic", "Abspielen");
    g_signal_connect(p->pip_play, "clicked", G_CALLBACK(on_play_clicked), p);
    GtkWidget *restore = new_bar_button("window-maximize-symbolic", "Im Hauptfenster zeigen");
    g_signal_connect(restore, "clicked", G_CALLBACK(on_pip_restore_clicked), p);
    GtkWidget *close = new_bar_button("window-close-symbolic", "Schließen (Esc)");
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), p);
    gtk_box_pack_start(GTK_BOX(bar), p->pip_play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), restore, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), close, FALSE, FALSE, 0);
    p->pip_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(p->pip_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_halign(p->pip_revealer, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(p->pip_revealer, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(p->pip_revealer, 10);
    gtk_container_add(GTK_CONTAINER(p->pip_revealer), bar);
    gtk_widget_show_all(bar);
    /* erst im kleinen Fenster sichtbar */
    gtk_widget_set_no_show_all(p->pip_revealer, TRUE);
    return p->pip_revealer;
}

static void on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    Player *p = data;
    report(p, true);
    g_signal_handlers_disconnect_by_data(p->toplevel, p);
    if (p->pip_window) {
        GtkWidget *window = p->pip_window;
        p->pip_window = NULL;
        g_signal_handlers_disconnect_by_data(window, p);
        if (!gtk_widget_in_destruction(window)) gtk_widget_destroy(window);
    }
    if (p->is_fullscreen) gtk_window_unfullscreen(GTK_WINDOW(p->toplevel));
    show_titlebar(p, true);
    set_cursor_visible(p, true);
    if (p->tick) g_source_remove(p->tick);
    if (p->hide_timer) g_source_remove(p->hide_timer);
    if (p->osd_timer) g_source_remove(p->osd_timer);
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
    /* das Mausrad verstellt die Zeitleiste nicht: zu leicht aus Versehen */
    g_signal_connect(p->scale, "scroll-event", G_CALLBACK(gtk_true), NULL);
    gtk_box_pack_start(GTK_BOX(bar), p->scale, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    GtkWidget *close = new_bar_button("go-previous-symbolic", "Zurück (Esc)");
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), p);
    gtk_box_pack_start(GTK_BOX(row), close, FALSE, FALSE, 0);
    p->play = new_bar_button("media-playback-start-symbolic", "Abspielen (Leertaste)");
    g_signal_connect(p->play, "clicked", G_CALLBACK(on_play_clicked), p);
    GtkWidget *back = new_bar_button("media-seek-backward-symbolic", "10 Sekunden zurück (←)");
    g_signal_connect(back, "clicked", G_CALLBACK(on_back_clicked), p);
    GtkWidget *forward = new_bar_button("media-seek-forward-symbolic", "10 Sekunden vor (→)");
    g_signal_connect(forward, "clicked", G_CALLBACK(on_forward_clicked), p);
    p->next = new_bar_button("media-skip-forward-symbolic", "Nächste Folge (N)");
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
    gtk_widget_set_tooltip_text(p->audio_button, "Tonspur (A)");
    p->text_button = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(p->text_button),
                         gtk_image_new_from_icon_name("media-view-subtitles-symbolic",
                                                      GTK_ICON_SIZE_LARGE_TOOLBAR));
    gtk_button_set_relief(GTK_BUTTON(p->text_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(p->text_button, "Untertitel (S)");
    gtk_widget_set_can_focus(p->audio_button, FALSE);
    gtk_widget_set_can_focus(p->text_button, FALSE);
    GtkWidget *zoom = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(zoom),
                         gtk_image_new_from_icon_name("zoom-fit-best-symbolic",
                                                      GTK_ICON_SIZE_LARGE_TOOLBAR));
    gtk_button_set_relief(GTK_BUTTON(zoom), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(zoom, "Zoom und schwarze Balken (Z, +, −, 0)");
    gtk_widget_set_can_focus(zoom, FALSE);
    GtkWidget *zoom_menu = gtk_menu_new();
    static const struct {
        const char *label;
        GCallback handler;
    } ZOOM_ITEMS[] = {{"Ganzes Bild (0)", G_CALLBACK(on_zoom_fit)},
                      {"Schwarze Balken ausblenden (Z)", G_CALLBACK(on_zoom_fill)},
                      {"Vergrößern (+)", G_CALLBACK(on_zoom_in)},
                      {"Verkleinern (−)", G_CALLBACK(on_zoom_out)}};
    for (size_t i = 0; i < G_N_ELEMENTS(ZOOM_ITEMS); i++) {
        GtkWidget *item = gtk_menu_item_new_with_label(ZOOM_ITEMS[i].label);
        g_signal_connect(item, "activate", ZOOM_ITEMS[i].handler, p);
        gtk_menu_shell_append(GTK_MENU_SHELL(zoom_menu), item);
    }
    gtk_widget_show_all(zoom_menu);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(zoom), zoom_menu);
    p->volume = gtk_volume_button_new();
    gtk_scale_button_set_value(GTK_SCALE_BUTTON(p->volume), last_volume);
    gtk_widget_set_can_focus(p->volume, FALSE);
    /* GtkVolumeButton zeigt sonst nur den Prozentwert */
    gtk_widget_set_tooltip_text(p->volume, "Lautstärke (↑/↓, M schaltet stumm)");
    g_signal_connect(p->volume, "value-changed", G_CALLBACK(on_volume_changed), p);
    p->fullscreen = new_bar_button("view-fullscreen-symbolic", "Vollbild (F)");
    g_signal_connect(p->fullscreen, "clicked", G_CALLBACK(on_fullscreen_clicked), p);
    gtk_box_pack_end(GTK_BOX(row), p->fullscreen, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), p->volume, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), zoom, FALSE, FALSE, 0);
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

/* gtksink nimmt nur BGRx/BGRA im Hauptspeicher. Die Umrechnung dorthin macht playbin sonst mit
 * videoconvert auf einem einzigen Prozessorkern: Bei 1080p50 mit 10 Bit schafft der unter Last
 * keine 30 Bilder je Sekunde, das Bild ruckelt. Mit VA-API rechnet die Grafikkarte um
 * (vapostproc), gemessen gut dreimal so schnell. Rückgabe: was playbin als Bildausgabe bekommt. */
static GstElement *wrap_gpu_convert(Player *p, GstElement *sink)
{
    p->gtk_sink = sink;
    if (!gpu_convert || gpu_convert_failed) return sink;
    GstElement *vpp = gst_element_factory_make("vapostproc", NULL);
    if (!vpp) return sink;
    /* lässt sich die Grafikkarte öffnen? */
    bool usable = gst_element_set_state(vpp, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE;
    gst_element_set_state(vpp, GST_STATE_NULL);
    GstElement *filter = usable ? gst_element_factory_make("capsfilter", NULL) : NULL;
    if (!filter) {
        gst_object_unref(vpp);
        return sink;
    }
    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRx", NULL);
    g_object_set(filter, "caps", caps, NULL);
    gst_caps_unref(caps);
    GstElement *bin = gst_bin_new("bild");
    gst_bin_add_many(GST_BIN(bin), vpp, filter, sink, NULL);
    gst_element_link_many(vpp, filter, sink, NULL);
    GstPad *pad = gst_element_get_static_pad(vpp, "sink");
    gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
    gst_object_unref(pad);
    p->vpp = vpp;
    p->vpp_filter = filter;
    md_log("Player: Farbumrechnung in der Grafikkarte (VA-API)");
    return bin;
}

/* Nach einem Fehler: Umrechnung wieder im Prozessor, wie ohne VA-API. playbin steht auf NULL */
static void drop_gpu_convert(Player *p)
{
    gpu_convert_failed = true;
    gst_element_set_state(p->sink, GST_STATE_NULL);
    GstPad *ghost = gst_element_get_static_pad(p->sink, "sink");
    GstPad *pad = gst_element_get_static_pad(p->gtk_sink, "sink");
    gst_ghost_pad_set_target(GST_GHOST_PAD(ghost), NULL);
    gst_element_unlink_many(p->vpp, p->vpp_filter, p->gtk_sink, NULL);
    gst_bin_remove_many(GST_BIN(p->sink), p->vpp, p->vpp_filter, NULL);
    gst_ghost_pad_set_target(GST_GHOST_PAD(ghost), pad);
    gst_object_unref(pad);
    gst_object_unref(ghost);
    p->vpp = p->vpp_filter = NULL;
}

/* Seite samt Pipeline anlegen; NULL = GStreamer fehlt */
static Player *player_new(GtkStack *stack)
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
    p->sink = p->gtk_sink = sink;
    p->zoom = 1;
    p->seek_target = -1;
    g_object_get(sink, "widget", &p->video, NULL);
    GstElement *text_sink = gst_element_factory_make("appsink", NULL);
    if (text_sink) {
        /* async aus: Untertitel kommen selten, der Start darf nicht auf sie warten */
        g_object_set(text_sink, "emit-signals", TRUE, "sync", TRUE, "async", FALSE, NULL);
        g_signal_connect(text_sink, "new-sample", G_CALLBACK(on_text_sample), p);
        g_object_set(playbin, "text-sink", text_sink, NULL);
        /* nur mit eigenem Untertitel-Ziel: sonst blendet playbin Untertitel selbst ins Bild
         * ein, und das kann mit Bildern in der Grafikkarte nichts anfangen */
        p->sink = wrap_gpu_convert(p, sink);
    }
    g_object_set(playbin, "video-sink", p->sink, "volume", last_volume, NULL);
    GstBus *bus = gst_element_get_bus(playbin);
    gst_bus_set_sync_handler(bus, on_bus_sync, NULL, NULL);
    gst_bus_add_watch(bus, on_bus, p);
    gst_object_unref(bus);

    /* eigenes Fenster der Seite: schwarzer Grund und Mausbewegungen über Bild und Leiste */
    p->root = gtk_event_box_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(p->root), "zd-player");
    gtk_widget_add_events(p->root, GDK_POINTER_MOTION_MASK);

    GtkWidget *overlay = gtk_overlay_new();
    p->zoom_box = g_object_new(zoom_box_get_type(), NULL);
    ((ZoomBox *)p->zoom_box)->player = p;
    gtk_container_add(GTK_CONTAINER(p->zoom_box), p->video);
    g_object_unref(p->video);       /* gehört jetzt der Seite */
    g_signal_connect(p->zoom_box, "button-press-event", G_CALLBACK(on_video_press), p);
    gtk_container_add(GTK_CONTAINER(overlay), p->zoom_box);
    p->revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(p->revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_valign(p->revealer, GTK_ALIGN_END);
    gtk_container_add(GTK_CONTAINER(p->revealer), build_bar(p));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), p->revealer);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), build_pip_bar(p));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), build_error_box(p));
    p->osd = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->osd), "osd");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->osd), "zd-player-osd");
    gtk_widget_set_halign(p->osd, GTK_ALIGN_START);
    gtk_widget_set_valign(p->osd, GTK_ALIGN_START);
    gtk_widget_set_margin_start(p->osd, 20);
    gtk_widget_set_margin_top(p->osd, 20);
    gtk_widget_set_no_show_all(p->osd, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), p->osd);
    p->subtitle = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->subtitle), "zd-player-subtitle");
    gtk_label_set_justify(GTK_LABEL(p->subtitle), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(p->subtitle), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(p->subtitle), 60);
    gtk_widget_set_halign(p->subtitle, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(p->subtitle, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(p->subtitle, TRUE);
    p->subtitle_end = -1;
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), p->subtitle);
    p->intro_button = gtk_button_new_with_label("Intro überspringen");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->intro_button), "osd");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->intro_button), "zd-player-skip");
    gtk_widget_set_can_focus(p->intro_button, FALSE);
    gtk_widget_set_halign(p->intro_button, GTK_ALIGN_END);
    gtk_widget_set_valign(p->intro_button, GTK_ALIGN_END);
    gtk_widget_set_margin_end(p->intro_button, 24);
    gtk_widget_set_margin_bottom(p->intro_button, 90);
    gtk_widget_set_no_show_all(p->intro_button, TRUE);
    g_signal_connect(p->intro_button, "clicked", G_CALLBACK(on_intro_clicked), p);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), p->intro_button);
    /* Klicks gehen durch den Text hindurch ans Bild */
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), p->subtitle, TRUE);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), p->osd, TRUE);
    gtk_container_add(GTK_CONTAINER(p->root), overlay);

    p->stack = stack;
    p->toplevel = gtk_widget_get_toplevel(GTK_WIDGET(stack));
    g_signal_connect(p->toplevel, "destroy", G_CALLBACK(on_toplevel_destroy), p);
    g_signal_connect(p->toplevel, "key-press-event", G_CALLBACK(on_key), p);
    g_signal_connect(p->toplevel, "window-state-event", G_CALLBACK(on_window_state), p);
    g_signal_connect(p->root, "motion-notify-event", G_CALLBACK(on_motion), p);
    g_signal_connect(p->root, "destroy", G_CALLBACK(on_destroy), p);
    p->tick = g_timeout_add(PLAYER_TICK_MS, on_tick, p);
    gtk_widget_show_all(p->root);
    gtk_stack_add_named(stack, p->root, "player");
    return p;
}

/* ---------------------------------------------------------------------------
 * Schnittstelle
 * ------------------------------------------------------------------------- */
void md_player_set_auto_skip_intro(bool on)
{
    auto_skip_intro = on;
}

void md_player_set_gpu_convert(bool on)
{
    gpu_convert = on;
}

bool md_player_gpu_convert_possible(void)
{
    if (!gst_init_check(NULL, NULL, NULL)) return false;
    GstElementFactory *factory = gst_element_factory_find("vapostproc");
    if (factory) gst_object_unref(factory);
    return factory != NULL;
}

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

void md_player_close(void)
{
    Player *p = player;
    if (!p || p->closing) return;
    p->closing = true;
    if (p->hooks.closed) p->hooks.closed(p->hooks.data);
    gtk_widget_destroy(p->root);                    /* räumt in on_destroy auf */
}

bool md_player_pip(void)
{
    Player *p = player;
    if (!p || p->closing) return false;
    if (p->pip_window) return true;
    if (!p->playing) return false;
    if (p->is_fullscreen) gtk_window_unfullscreen(GTK_WINDOW(p->toplevel));
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), p->title);
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    int height = p->video_width > 0 && p->video_height > 0
                     ? (int)(PIP_WIDTH * p->video_height / p->video_width + 0.5)
                     : PIP_WIDTH * 9 / 16;
    gtk_window_set_default_size(GTK_WINDOW(window), PIP_WIDTH, height);
    /* unten rechts auf dem Bildschirm des Hauptfensters; unter Wayland bestimmt das der
     * Fenstermanager */
    GdkWindow *main_window = gtk_widget_get_window(p->toplevel);
    GdkMonitor *monitor = main_window ? gdk_display_get_monitor_at_window(
                                            gdk_window_get_display(main_window), main_window)
                                      : NULL;
    if (monitor) {
        GdkRectangle area;
        gdk_monitor_get_workarea(monitor, &area);
        gtk_window_move(GTK_WINDOW(window), area.x + area.width - PIP_WIDTH - PIP_MARGIN,
                        area.y + area.height - height - PIP_MARGIN - PIP_TITLEBAR);
    }
    p->pip_window = window;
    gtk_widget_hide(p->revealer);       /* die große Leiste passt nicht ins kleine Fenster */
    gtk_widget_show(p->pip_revealer);
    move_page(p, window);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key), p);
    g_signal_connect(window, "delete-event", G_CALLBACK(on_pip_delete), p);
    gtk_widget_show(window);
    show_bar(p);
    return true;
}

bool md_player_play(GtkStack *stack, const char *path, const char *title, double start,
                    const MdPlayerHooks *hooks)
{
    if (!player) {
        char *why = NULL;
        if (!md_player_available(&why)) {
            g_free(why);
            return false;
        }
        player = player_new(stack);
        if (!player) return false;
    }
    /* der alte Stand geht noch an die bisherigen Empfänger */
    if (player->path) report(player, true);
    g_clear_pointer(&player->path, g_free);
    pip_leave(player);
    player->hooks = *hooks;
    load_file(player, path, title, start);
    gtk_stack_set_visible_child(stack, player->root);
    return true;
}
