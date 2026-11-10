/*
 * Mediathek Downloader – Regressionstests der Engine
 * ==================================================
 * Bindet md_core.c direkt ein, um an die statischen Funktionen zu kommen. Downloads laufen
 * über einen kleinen HTTP-Server auf 127.0.0.1 in diesem Programm, alle Dateien in einem
 * temporären Ordner (test_util.h). Aufruf: make test
 */
#include "../src/md_core.c"

#include <netinet/in.h>
#include <sys/socket.h>

#include "test_util.h"

#define BODY_SIZE (256 * 1024)
#define RESUME_AT 100000
#define CURL_TIMEOUT_MS 30000

static const char *const GOOD_URLS[] = {
    "https://example.org/film.mp4", "http://example.org/a/b.mp4?x=1&y=2",
};
static const char *const BAD_URLS[] = {
    "-K/pfad", "--output=/tmp/x", "file:///etc/passwd", "gopher://example.org/",
    "ftp://example.org/film.mp4", "javascript:alert(1)", "httpx://example.org/", "http:/example.org/",
    " https://example.org/",
};
static const struct {
    const char *key;
    size_t offset;
} URL_FIELDS[] = {
    {"url_video_hd", G_STRUCT_OFFSET(MdTitle, url_high)},
    {"url_video", G_STRUCT_OFFSET(MdTitle, url_medium)},
    {"url_video_low", G_STRUCT_OFFSET(MdTitle, url_low)},
    {"url_subtitle", G_STRUCT_OFFSET(MdTitle, url_subtitle)},
    {"url_website", G_STRUCT_OFFSET(MdTitle, website)},
};
#define URL_FIELD(t, i) G_STRUCT_MEMBER(char *, t, URL_FIELDS[i].offset)

/* ---------------------------------------------------------------------------
 * HTTP-Server: liefert body, auf Wunsch ab einer Byte-Position
 * ------------------------------------------------------------------------- */
static guint8 body[BODY_SIZE];
static int server_fd, server_port;
static gint last_range = -1;    /* Startposition der letzten Anfrage, -1 = ohne Range */

static void send_all(int fd, const void *data, size_t size)
{
    const char *p = data;
    while (size > 0) {
        ssize_t sent = send(fd, p, size, MSG_NOSIGNAL);
        if (sent <= 0) return;
        p += sent;
        size -= (size_t)sent;
    }
}

static void serve_client(int client)
{
    char request[4096];
    size_t have = 0;
    while (have < sizeof request - 1 && !g_strstr_len(request, (gssize)have, "\r\n\r\n")) {
        ssize_t got = recv(client, request + have, sizeof request - 1 - have, 0);
        if (got <= 0) return;
        have += (size_t)got;
    }
    request[have] = 0;
    const char *range = strcasestr(request, "\r\nRange: bytes=");
    int start = range ? atoi(range + strlen("\r\nRange: bytes=")) : 0;
    g_atomic_int_set(&last_range, range ? start : -1);
    char *head = range
        ? g_strdup_printf("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %d-%d/%d\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n",
                          start, BODY_SIZE - 1, BODY_SIZE, BODY_SIZE - start)
        : g_strdup_printf("HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                          BODY_SIZE);
    send_all(client, head, strlen(head));
    g_free(head);
    if (!strncmp(request, "GET ", 4)) send_all(client, body + start, (size_t)(BODY_SIZE - start));
}

static gpointer server_thread(gpointer data)
{
    (void)data;
    for (;;) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) return NULL;
        serve_client(client);
        close(client);
    }
}

static GThread *server_start(void)
{
    for (int i = 0; i < BODY_SIZE; i++) body[i] = (guint8)(i * 31 + (i >> 8));
    server_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof address;
    if (server_fd < 0 || bind(server_fd, (struct sockaddr *)&address, size) != 0 ||
        listen(server_fd, 8) != 0 || getsockname(server_fd, (struct sockaddr *)&address, &size) != 0)
        g_error("Testserver startet nicht: %s", g_strerror(errno));
    server_port = ntohs(address.sin_port);
    return g_thread_new("testserver", server_thread, NULL);
}

static void server_stop(GThread *thread)
{
    shutdown(server_fd, SHUT_RDWR);
    close(server_fd);
    g_thread_join(thread);
}

/* ---------------------------------------------------------------------------
 * Helfer
 * ------------------------------------------------------------------------- */
static char *test_path(const char *name)
{
    return g_build_filename(test_dir, name, NULL);
}

static char *server_url(const char *name)
{
    return g_strdup_printf("http://127.0.0.1:%d/%s", server_port, name);
}

/* Auf curl warten -> Exit-Code, -1 = lief zu lange oder wurde beendet */
static int wait_curl(pid_t pid)
{
    g_assert_cmpint(pid, >, 0);
    for (int waited = 0; waited < CURL_TIMEOUT_MS; waited += 10) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        g_usleep(10 * 1000);
    }
    stop_child(pid);
    return -1;
}

static void assert_file_is_body(const char *path)
{
    char *data = NULL;
    gsize size = 0;
    g_assert_true(g_file_get_contents(path, &data, &size, NULL));
    g_assert_cmpmem(data, size, body, BODY_SIZE);
    g_free(data);
}

/* Text in dieser Form so oft wiederholen, dass mindestens bytes Bytes zusammenkommen */
static char *repeated(const char *start, const char *unit, size_t bytes)
{
    GString *text = g_string_new(start);
    while (text->len < bytes) g_string_append(text, unit);
    return g_string_free(text, FALSE);
}

/* ---------------------------------------------------------------------------
 * Adressen aus der Antwort der Suche
 * ------------------------------------------------------------------------- */
static MdTitle *read_title(const char *key, const char *url)
{
    GString *text = g_string_new("{\"channel\":\"ZDF\",\"topic\":\"Krimi\",\"title\":\"Der Fall\"");
    for (size_t i = 0; i < G_N_ELEMENTS(URL_FIELDS); i++)
        g_string_append_printf(text, ",\"%s\":\"%s\"", URL_FIELDS[i].key,
                               !strcmp(URL_FIELDS[i].key, key) ? url : GOOD_URLS[0]);
    g_string_append_c(text, '}');
    Json j = {text->str, true};
    MdTitle *t = json_read_title(&j);
    g_assert_true(j.ok);
    g_string_free(text, TRUE);
    return t;
}

static void test_url_foreign_dropped(void)
{
    for (size_t f = 0; f < G_N_ELEMENTS(URL_FIELDS); f++)
        for (size_t u = 0; u < G_N_ELEMENTS(BAD_URLS); u++) {
            MdTitle *t = read_title(URL_FIELDS[f].key, BAD_URLS[u]);
            g_assert_cmpstr(URL_FIELD(t, f), ==, "");
            for (size_t other = 0; other < G_N_ELEMENTS(URL_FIELDS); other++)
                if (other != f) g_assert_cmpstr(URL_FIELD(t, other), ==, GOOD_URLS[0]);
            md_title_free(t);
        }
}

static void test_url_web_kept(void)
{
    for (size_t f = 0; f < G_N_ELEMENTS(URL_FIELDS); f++)
        for (size_t u = 0; u < G_N_ELEMENTS(GOOD_URLS); u++) {
            MdTitle *t = read_title(URL_FIELDS[f].key, GOOD_URLS[u]);
            g_assert_cmpstr(URL_FIELD(t, f), ==, GOOD_URLS[u]);
            md_title_free(t);
        }
}

/* ---------------------------------------------------------------------------
 * start_curl
 * ------------------------------------------------------------------------- */
/* Die zweite Adresse bekommt das -o aus der Datei, weil das -o von start_curl die erste nimmt */
static void test_curl_option_in_url(void)
{
    char *config = test_path("angriff.conf"), *foreign = test_path("fremder-pfad");
    char *part = test_path("option.part"), *url = server_url("film.mp4");
    char *argument = g_strconcat("-K", config, NULL);
    char *text = g_strdup_printf("url = \"%s\"\nurl = \"%s\"\noutput = \"%s\"\n", url, url, foreign);
    g_assert_true(g_file_set_contents(config, text, -1, NULL));
    g_assert_cmpint(wait_curl(start_curl(argument, part, 0)), !=, 0);
    g_assert_false(g_file_test(foreign, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(part, G_FILE_TEST_EXISTS));
    g_free(config);
    g_free(foreign);
    g_free(part);
    g_free(url);
    g_free(argument);
    g_free(text);
}

static void test_curl_file_scheme(void)
{
    char *secret = test_path("geheim"), *part = test_path("datei.part");
    char *url = g_strconcat("file://", secret, NULL);
    g_assert_true(g_file_set_contents(secret, "geheim", -1, NULL));
    g_assert_cmpint(wait_curl(start_curl(url, part, 0)), !=, 0);
    g_assert_false(g_file_test(part, G_FILE_TEST_EXISTS));
    g_free(secret);
    g_free(part);
    g_free(url);
}

static void test_curl_download(void)
{
    char *part = test_path("ganz.part"), *url = server_url("film.mp4");
    g_assert_cmpint(wait_curl(start_curl(url, part, 0)), ==, 0);
    g_assert_cmpint(g_atomic_int_get(&last_range), ==, -1);
    assert_file_is_body(part);
    g_free(part);
    g_free(url);
}

static void test_curl_resume(void)
{
    char *part = test_path("halb.part"), *url = server_url("film.mp4");
    g_assert_true(g_file_set_contents(part, (const char *)body, RESUME_AT, NULL));
    g_assert_cmpint(wait_curl(start_curl(url, part, 100 * 1024 * 1024)), ==, 0);
    g_assert_cmpint(g_atomic_int_get(&last_range), ==, RESUME_AT);
    assert_file_is_body(part);
    g_free(part);
    g_free(url);
}

/* ---------------------------------------------------------------------------
 * md_target_path
 * ------------------------------------------------------------------------- */
static void assert_file_name_fits(const MdTitle *t)
{
    char *path = md_target_path(t, "/ziel"), *file = g_path_get_basename(path);
    g_assert_cmpuint(strlen(file), <=, 230);
    g_assert_true(g_utf8_validate(file, -1, NULL));
    g_assert_true(g_str_has_suffix(file, ".mp4"));
    g_free(path);
    g_free(file);
}

/* Die Vorsilben verschieben den Schnitt, damit er auch mitten in ein Zeichen fallen würde */
static void test_path_long_names(void)
{
    static const char *const STARTS[] = {"", "x", "xx", "xxx"};
    static const char *const UNITS[] = {"ü", "€", "😀"};
    for (size_t s = 0; s < G_N_ELEMENTS(STARTS); s++)
        for (size_t u = 0; u < G_N_ELEMENTS(UNITS); u++) {
            MdTitle t = {.season = 2, .episode = 7};
            t.topic = repeated(STARTS[s], UNITS[u], 400);
            t.name = repeated(STARTS[s], UNITS[u], 400);
            assert_file_name_fits(&t);
            g_free(t.topic);
            t.topic = repeated("", "S", 400);
            assert_file_name_fits(&t);
            g_free(t.topic);
            t.topic = g_strdup("Kurz");
            assert_file_name_fits(&t);
            g_free(t.topic);
            g_free(t.name);
        }
}

static void test_path_short_names(void)
{
    MdTitle t = {.topic = "Tatort", .name = "Der Fall", .season = 3, .episode = 7};
    char *path = md_target_path(&t, "/ziel");
    g_assert_cmpstr(path, ==, "/ziel/Serien/Tatort/Staffel 03/Tatort - S03E07 - Der Fall.mp4");
    g_free(path);
    MdTitle film = {.topic = "Spielfilm", .name = "Der Fall", .season = -1, .is_film = true};
    path = md_target_path(&film, "/ziel");
    g_assert_cmpstr(path, ==, "/ziel/Filme/Der Fall.mp4");
    g_free(path);
    /* 34 + " - S01E02 - " + 180 + ".mp4": genau 230 Byte bleiben unverändert */
    char *topic = repeated("", "t", 34), *name = repeated("", "n", 180);
    MdTitle full = {.topic = topic, .name = name, .season = 1, .episode = 2};
    path = md_target_path(&full, "/ziel");
    char *file = g_path_get_basename(path);
    g_assert_cmpuint(strlen(file), ==, 230);
    g_assert_nonnull(strstr(file, name));
    g_free(path);
    g_free(file);
    g_free(topic);
    g_free(name);
}

/* Versionen werden Stelle für Stelle als Zahl verglichen, nicht als Text */
static void test_update_versions(void)
{
    g_assert_true(md_version_newer("v1.10.0", "1.9.0"));
    g_assert_true(md_version_newer("v2.0.0", "1.99.99"));
    g_assert_true(md_version_newer("1.9.1", "1.9.0"));
    g_assert_false(md_version_newer("v1.9.0", "1.9.0"));
    g_assert_false(md_version_newer("v1.8.9", "1.9.0"));
    g_assert_false(md_version_newer("kaputt", "1.9.0"));
    g_assert_false(md_version_newer(NULL, "1.9.0"));
}

/* SHA256SUMS: nur exakt passende Namen, Großbuchstaben werden klein */
static void test_update_sums(void)
{
    const char *hash = "AB12000000000000000000000000000000000000000000000000000000000000";
    char *sums = g_strdup_printf("%s  mediathek-downloader-1.10.0-setup.exe\n"
                                 "%s *mediathek-downloader_1.10.0-1_amd64.deb\n"
                                 "zu-kurz  andere.deb\n", hash, hash);
    char *found = listed_sha256(sums, "mediathek-downloader_1.10.0-1_amd64.deb");
    g_assert_cmpstr(found, ==, "ab12000000000000000000000000000000000000000000000000000000000000");
    g_free(found);
    g_assert_null(listed_sha256(sums, "mediathek-downloader_1.10.0-1_amd64"));
    g_assert_null(listed_sha256(sums, "andere.deb"));
    g_free(sums);
}

/* Testvideo w×h mit Inhalt im Bereich cw×ch ab x,y, der Rest schwarz; false = ffmpeg fehlt */
static bool make_video(const char *name, int w, int h, int cw, int ch, int x, int y)
{
    char *filter = g_strdup_printf("testsrc2=s=%dx%d:d=70:r=5,pad=%d:%d:%d:%d:black", cw, ch, w, h,
                                   x, y);
    char *argv[] = {"ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", filter, "-c:v",
                    "libx264", "-preset", "ultrafast", "-g", "5", (char *)name, NULL};
    bool ok = run_capture(argv, NULL, NULL) == 0;
    g_free(filter);
    return ok;
}

/* Testvideo, das 40 s mit Balken oben und unten läuft und danach 30 s das volle Bild zeigt */
static bool make_changing_video(const char *name)
{
    char *argv[] = {"ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                    "testsrc2=s=640x268:d=40:r=5,pad=640:360:0:46:black", "-f", "lavfi", "-i",
                    "testsrc2=s=640x360:d=30:r=5", "-filter_complex",
                    "[0:v][1:v]concat=n=2:v=1", "-c:v", "libx264", "-preset", "ultrafast", "-g",
                    "5", (char *)name, NULL};
    return run_capture(argv, NULL, NULL) == 0;
}

/* Der gefundene Bereich enthält den Inhalt x, y, w × h ganz, mit höchstens etwas Rand */
static void assert_contains(const MdCrop *c, int x, int y, int w, int h)
{
    g_assert_cmpint(c->x, <=, x);
    g_assert_cmpint(c->y, <=, y);
    g_assert_cmpint(c->x + c->width, >=, x + w);
    g_assert_cmpint(c->y + c->height, >=, y + h);
    g_assert_cmpint(c->width, <=, w + 2 * CROP_MARGIN + 2);
    g_assert_cmpint(c->height, <=, h + 2 * CROP_MARGIN + 2);
}

/* Balken oben/unten und links/rechts werden gefunden, ohne Inhalt abzuschneiden; ein volles
 * Bild und eines, das später voll wird, bleiben ganz */
/* Titel wie aus MediathekViewWeb, über den echten Parser */
static MdTitle *episode_title(const char *channel, const char *topic, const char *title)
{
    char *json = g_strdup_printf("{\"channel\":\"%s\",\"topic\":\"%s\",\"title\":\"%s\","
                                 "\"url_video\":\"https://example.org/%u.mp4\","
                                 "\"duration\":5280}", channel, topic, title,
                                 g_str_hash(title) ^ g_str_hash(channel));
    Json j = {json, true};
    MdTitle *t = json_read_title(&j);
    g_assert_true(j.ok);
    g_free(json);
    return t;
}

static bool has_title(GPtrArray *titles, const char *title)
{
    for (guint i = 0; i < titles->len; i++)
        if (!strcmp(((MdTitle *)titles->pdata[i])->title, title)) return true;
    return false;
}

/* Dieselbe Folge aus mehreren Mediatheken nur einmal; die mit Staffel bleibt */
/* Intro: gemeinsamer Abschnitt zweier Fingerabdrücke, leicht verrauscht und versetzt */
static void test_intro_match(void)
{
    GRand *rand = g_rand_new_with_seed(7);
    int na = 4000, nb = 4200, len = 240;    /* 240 Werte ≈ 30 s */
    guint32 *a = g_new(guint32, na), *b = g_new(guint32, nb);
    for (int i = 0; i < na; i++) a[i] = g_rand_int(rand);
    for (int i = 0; i < nb; i++) b[i] = g_rand_int(rand);
    for (int i = 0; i < len; i++) {
        guint32 v = g_rand_int(rand);
        a[1300 + i] = v;
        b[100 + i] = i % 7 == 3 ? v ^ 0x00010101u : v;  /* hier und da 3 Bits anders */
    }
    double start = 0, end = 0;
    g_assert_true(intro_match(a, na, b, nb, &start, &end));
    g_assert_cmpfloat(ABS(start - 1300 * INTRO_STEP), <, 0.5);
    g_assert_cmpfloat(ABS(end - (1300 + len - 1) * INTRO_STEP), <, 0.5);
    /* zu kurz (10 s) zählt nicht */
    for (int i = 0; i < nb; i++) b[i] = g_rand_int(rand);
    for (int i = 0; i < 80; i++) b[500 + i] = a[1300 + i];
    g_assert_false(intro_match(a, na, b, nb, &start, &end));
    g_free(a);
    g_free(b);
    g_rand_free(rand);
}

static void test_duplicate_episodes(void)
{
    static const struct {
        const char *channel, *topic, *title;
        bool kept;
    } CASES[] = {
        {"ARD", "Praxis mit Meerblick", "Mutter und Sohn (S01/E13) ()", false},
        {"ARD", "Praxis mit Meerblick", "Mutter und Sohn · Praxis mit Meerblick (13) (S03/E01)",
         true},
        {"ZDF", "Praxis mit Meerblick", "Praxis mit Meerblick – Mutter und Sohn", false},
        {"3sat", "Fernsehfilm", "Praxis mit Meerblick – Mutter und Sohn", false},
        {"ARD", "Praxis mit Meerblick", "Der Prozess (S01/E03) ()", true},
        {"ZDF", "Praxis mit Meerblick", "Praxis mit Meerblick - Der Prozess (S01/E03)", false},
        /* ohne Gegenstück mit Staffel bleibt alles */
        {"ARD", "Praxis mit Meerblick", "Alte Freunde (S01/E07) ()", true},
        {"ARD", "Praxis mit Meerblick", "Praxis mit Meerblick - Auf zu neuen Ufern", true},
        /* gleicher Name in zwei Staffeln ohne Zählnummer: zwei Folgen */
        {"ZDF", "Die Bergretter", "Weihnachten (S02/E05)", true},
        {"ZDF", "Die Bergretter", "Weihnachten (S07/E01)", true},
        /* zu unspezifisch zum Vergleichen */
        {"ARD", "Tatort", "Folge 5", true},
        {"ZDF", "Tatort", "Tatort (S01/E06)", true},
    };
    GPtrArray *titles = g_ptr_array_new_with_free_func(md_title_free);
    for (size_t i = 0; i < G_N_ELEMENTS(CASES); i++)
        g_ptr_array_add(titles, episode_title(CASES[i].channel, CASES[i].topic, CASES[i].title));
    drop_duplicate_episodes(titles);
    for (size_t i = 0; i < G_N_ELEMENTS(CASES); i++) {
        if (has_title(titles, CASES[i].title) != CASES[i].kept)
            g_error("%s: erwartet %s", CASES[i].title, CASES[i].kept ? "behalten" : "weg");
    }
    g_ptr_array_free(titles, TRUE);

    char *a = episode_name_key("Praxis mit Meerblick",
                               "Praxis mit Meerblick – S01E13 – Mutter und Sohn");
    char *b = episode_name_key("Praxis mit Meerblick", "Praxis mit Meerblick - Mutter und Sohn");
    g_assert_cmpstr(a, ==, b);
    g_assert_null(episode_name_key("Tatort", "Folge 12"));
    g_free(a);
    g_free(b);
}

static void test_crop_bars(void)
{
    char *ffmpeg = g_find_program_in_path("ffmpeg");
    g_free(ffmpeg);
    if (!ffmpeg || !make_video("letterbox.mp4", 640, 360, 640, 268, 0, 46) ||
        !make_video("pillarbox.mp4", 640, 360, 480, 360, 80, 0) ||
        !make_video("voll.mp4", 640, 360, 640, 360, 0, 0) ||
        !make_changing_video("wechsel.mp4")) {
        g_test_skip("ffmpeg fehlt");
        return;
    }
    MdCrop c;
    g_assert_true(md_detect_bars("letterbox.mp4", &c));
    assert_contains(&c, 0, 46, 640, 268);
    g_assert_true(md_detect_bars("pillarbox.mp4", &c));
    assert_contains(&c, 80, 0, 480, 360);
    g_assert_cmpint(c.frame_width, ==, 640);
    g_assert_false(md_detect_bars("voll.mp4", &c));
    g_assert_false(md_detect_bars("wechsel.mp4", &c));
    char *filter = crop_filter("pillarbox.mp4", 70);
    g_assert_cmpstr(filter, ==, "crop=488:360:76:0");
    g_free(filter);
}

#ifndef G_OS_WIN32
/* Autostart-Eintrag landet in XDG_CONFIG_HOME (hier der Testordner) und geht wieder weg */
static void test_autostart(void)
{
    g_assert_false(md_autostart_enabled());
    g_assert_true(md_autostart_set(true, NULL));
    g_assert_true(md_autostart_enabled());
    char *path = autostart_path(), *text = NULL;
    g_assert_true(g_str_has_prefix(path, test_dir));
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "Exec=mediathek-downloader --autostart\n"));
    g_assert_true(md_autostart_set(false, NULL));
    g_assert_false(md_autostart_enabled());
    g_assert_true(md_autostart_set(false, NULL));       /* schon weg: kein Fehler */
    g_free(path);
    g_free(text);
}
#endif

#ifndef G_OS_WIN32
static void write_test_file(const char *dir, const char *name, const char *text)
{
    char *path = g_build_filename(dir, name, NULL);
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
    g_free(path);
}

static bool test_file_exists(const char *dir, const char *name)
{
    char *path = g_build_filename(dir, name, NULL);
    bool exists = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    return exists;
}

/* Der Zwischenspeicher zieht im Hintergrund um: höchstens fünf Sekunden warten */
static bool wait_for_file(const char *dir, const char *name)
{
    for (int i = 0; i < 100 && !test_file_exists(dir, name); i++) g_usleep(50000);
    return test_file_exists(dir, name);
}

#define CACHE_NAME "0123456789abcdef0123456789abcdef01234567.info"
#define FOREIGN_CACHE_NAME "89abcdef0123456789abcdef0123456789abcdef.api"

/* Datenordner: Daten von 2.2 bleiben gültig, ein gewählter Ordner bekommt alles samt
 * Zwischenspeicher, aus einem alten Cacheordner kommen nur Dateien des Zwischenspeichers mit */
static void test_data_dir(void)
{
    char *legacy = g_build_filename(test_dir, LEGACY_APP_DIR, NULL);
    char *old_cache = g_build_filename(legacy, OLD_CACHE_DIR_NAME, NULL);
    g_mkdir_with_parents(old_cache, 0755);
    write_test_file(legacy, "einstellungen.ini", "[Einstellungen]\n");
    write_test_file(old_cache, CACHE_NAME, "Bild\n");
    md_init(false);
    g_assert_cmpstr(md_app_dir(), ==, legacy);
    char *cache = g_build_filename(legacy, CACHE_DIR_NAME, NULL);
    g_assert_true(test_file_exists(cache, CACHE_NAME));
    g_free(cache);

    char *parent = g_build_filename(test_dir, "Daten", NULL), *error = NULL;
    char *dir = g_build_filename(parent, DATA_DIR_NAME, NULL);
    g_assert_true(md_set_data_parent(parent, &error));
    g_assert_null(error);
    g_assert_cmpstr(md_app_dir(), ==, dir);
    g_assert_cmpstr(md_data_parent(), ==, parent);
    g_assert_true(test_file_exists(dir, "einstellungen.ini"));
    g_assert_false(test_file_exists(legacy, "einstellungen.ini"));
    cache = g_build_filename(dir, CACHE_DIR_NAME, NULL);
    g_assert_true(wait_for_file(cache, CACHE_NAME));
    char *pointer = pointer_path(), *saved = NULL;
    g_assert_true(g_file_get_contents(pointer, &saved, NULL, NULL));
    g_assert_cmpstr(saved, ==, parent);

    char *foreign = g_build_filename(test_dir, "Filme", NULL);
    g_mkdir_with_parents(foreign, 0755);
    write_test_file(foreign, FOREIGN_CACHE_NAME, "{}");
    write_test_file(foreign, "Film.mkv", "Video");
    md_adopt_cache(foreign);
    g_assert_true(wait_for_file(cache, FOREIGN_CACHE_NAME));
    g_assert_true(test_file_exists(foreign, "Film.mkv"));
    g_assert_false(test_file_exists(foreign, FOREIGN_CACHE_NAME));

    g_assert_true(md_set_data_parent(NULL, &error));
    g_assert_cmpstr(md_app_dir(), ==, legacy);
    g_assert_null(md_data_parent());
    g_assert_true(test_file_exists(legacy, "einstellungen.ini"));
    g_assert_false(g_file_test(pointer, G_FILE_TEST_EXISTS));
    g_free(cache);
    cache = g_build_filename(legacy, CACHE_DIR_NAME, NULL);
    g_assert_true(wait_for_file(cache, FOREIGN_CACHE_NAME));
    g_free(legacy);
    g_free(old_cache);
    g_free(cache);
    g_free(parent);
    g_free(dir);
    g_free(pointer);
    g_free(saved);
    g_free(foreign);
}
#endif

int main(int argc, char **argv)
{
    test_dir_create();
    g_test_init(&argc, &argv, NULL);
    g_test_set_nonfatal_assertions();
    g_test_add_func("/adressen/fremde-werden-geleert", test_url_foreign_dropped);
    g_test_add_func("/adressen/webadressen-bleiben", test_url_web_kept);
    g_test_add_func("/curl/option-als-adresse", test_curl_option_in_url);
    g_test_add_func("/curl/file-abgelehnt", test_curl_file_scheme);
    g_test_add_func("/curl/download", test_curl_download);
    g_test_add_func("/curl/fortsetzen", test_curl_resume);
    g_test_add_func("/pfad/lange-namen", test_path_long_names);
    g_test_add_func("/pfad/kurze-namen", test_path_short_names);
    g_test_add_func("/update/versionen", test_update_versions);
    g_test_add_func("/update/pruefsummen", test_update_sums);
    g_test_add_func("/suche/doppelte-folgen", test_duplicate_episodes);
    g_test_add_func("/player/intro-abschnitt", test_intro_match);
    g_test_add_func("/av1/schwarze-balken", test_crop_bars);
#ifndef G_OS_WIN32
    g_test_add_func("/autostart/eintrag", test_autostart);
    g_test_add_func("/daten/umzug", test_data_dir);
#endif
    GThread *server = server_start();
    int result = g_test_run();
    server_stop(server);
    test_dir_remove();
    return result;
}
