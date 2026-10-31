/*
 * Mediathek Downloader – kurzer Fuzz-Lauf über die Leser fremder Antworten
 * ========================================================================
 * Verändert gültige Beispiele zufällig und gibt sie an parse_response, netzkino_parse,
 * ard_page_info und html_page_info. Fehler melden die Sanitizer und MALLOC_CHECK_, dazu
 * prüft check_title, was ein Titel unabhängig von der Eingabe einhalten muss.
 *
 *   FUZZ_SECONDS  Laufzeit in Sekunden (Standard 10)
 *   FUZZ_SEED     Startwert; wiederholt einen Lauf, den Wert nennt jeder Lauf am Anfang
 */
#include "../src/md_core.c"

#include "test_util.h"

#define FUZZ_MAX_INPUT (32 * 1024)
#define MAX_EDITS 8

typedef enum { TARGET_SEARCH, TARGET_NETZKINO, TARGET_ARD, TARGET_HTML, TARGET_COUNT } Target;

static const char *const SAMPLES[TARGET_COUNT][3] = {
    [TARGET_SEARCH] = {
        "{\"result\":{\"results\":[{\"channel\":\"ZDF\",\"topic\":\"Krimi\",\"title\":\"Der Fall "
        "(S02/E03) - Neue Folge\",\"description\":\"Text\",\"timestamp\":1700000000,\"duration\":"
        "5400,\"url_website\":\"https://www.zdf.de/krimi\",\"url_video_hd\":"
        "\"https://example.org/hd.mp4\",\"url_video\":\"https://example.org/m.mp4\","
        "\"url_video_low\":\"https://example.org/l.mp4\",\"url_subtitle\":"
        "\"https://example.org/u.vtt\"}],\"queryInfo\":{\"totalResults\":1,\"x\":[1]}},"
        "\"err\":null}",
        "{\"result\":{\"results\":[{\"channel\":\"ARD\",\"topic\":\"Filme\",\"title\":\"Folge 5 · "
        "Praxis mit Meerblick (1)\",\"url_video\":\"-K/pfad\",\"url_website\":\"file:///etc/passwd"
        "\"},{\"channel\":\"SWR\",\"topic\":\"\\u00dcberall\",\"title\":\"\\ud83d\\ude00 ()\","
        "\"url_video_hd\":\"gopher://x/\",\"url_subtitle\":\"-o/x\"}]}}",
        "{\"err\":\"kaputt\"}",
    },
    [TARGET_NETZKINO] = {
        "{\"posts\":[{\"id\":1,\"slug\":\"der-film\",\"title\":\"Der Film\",\"content\":\"Text\","
        "\"date\":\"2024-01-02T03:04:05Z\",\"custom_fields\":{\"Streaming\":[\"Great Movies/Film"
        "\"],\"FSK\":[\"12\"],\"featured_img_all_small\":[\"https://example.org/b.jpg\"]},"
        "\"properties\":[\"avod\"],\"genres\":[\"Krimi\"],\"thumbnail\":"
        "\"https://example.org/t.jpg\"}]}",
        "{\"count\":2,\"posts\":[]}",
        NULL,
    },
    [TARGET_ARD] = {
        "{\"maturityContentRating\":\"FSK12\",\"page_show\":\"Tatort Krimi\",\"geoblocked\":false,"
        "\"src\":\"https://api.ardmediathek.de/image-service/images/x?w={width}\",\"synopsis\":"
        "\"Ein Thriller und eine Komödie\"}",
        "\"src\":\"\\u0026{width}\"",
        NULL,
    },
    [TARGET_HTML] = {
        "<meta property=\"og:image\" content=\"https://www.zdf.de/assets/bild~384x216?cb=1\"><div "
        "data-testid=\"meta-data-tags\"><a href=\"/altersfreigabe-12\">FSK 12</a><a href=\"/krimi"
        "\">Krimi</a></div>",
        "{\"thumbnailUrl\":\"https://epg-image.zdf.de/f?layout=1&x=2\"}",
        NULL,
    },
};

static const char *const TOKENS[] = {
    "\"", "\\", "\\u", "\\ud83d", "\\ude00", "\\u0000", "{", "}", "[", "]", ",", ":", "null", "1e999",
    "-1", "\xff", "\xc3", "\xf0\x9f", "\n", " ", "~", "?", "&", "(S99/E99)", "()", "(7)", "Folge 3",
    " · ", " - Neue Folge", "\"-K/x\"", "\"file:///etc/passwd\"", "\"url_video\":\"gopher://x/\"",
    "\"url_website\":\"-o/x\",", "\"posts\":[", "\"slug\":\"s\",", "\"properties\":[\"avod\"],",
    "\"Streaming\":[\"a\"]", "\"thumbnailUrl\":\"", "property=\"og:image\" content=\"", "layout=",
    "{width}", "\"src\":\"", "\"geoblocked\"", "\"synopsis\":\"", "\"maturityContentRating\":\"FSK",
    "href=\"", "</a>", "</div>", "data-testid=\"meta-data-tags\"", "https://www.zdf.de/assets/",
    "https://epg-image.zdf.de/", "https://www.3sat.de/", "/altersfreigabe",
};

static char last_input[FUZZ_MAX_INPUT * 2 + MAX_EDITS * 64];
static guint32 seed;
static gint64 rounds, titles_checked;

/* Fehlermeldung mit allem, was man zum Wiederholen braucht */
static void fail(const char *what)
{
    char *escaped = g_strescape(last_input, NULL);
    fprintf(stderr, "Fuzz-Fehler in Runde %" G_GINT64_FORMAT " (FUZZ_SEED=%u): %s\nEingabe: %s\n",
            rounds, seed, what, escaped);
    g_free(escaped);
    test_dir_remove();
    abort();
}

static void mutate(GRand *rand, GString *text)
{
    int edits = g_rand_int_range(rand, 1, MAX_EDITS + 1);
    for (int i = 0; i < edits; i++) {
        gsize at = text->len ? (gsize)g_rand_int_range(rand, 0, (gint32)text->len + 1) : 0;
        gsize rest = text->len - at, size = (gsize)g_rand_int_range(rand, 1, 256);
        switch (g_rand_int_range(rand, 0, 6)) {
        case 0:
            if (at < text->len) text->str[at] = (char)g_rand_int_range(rand, 1, 256);
            break;
        case 1:
            g_string_erase(text, (gssize)at, (gssize)MIN(rest, size));
            break;
        case 2:
            g_string_truncate(text, at);
            break;
        case 3: {       /* Stück verdoppeln: lange Namen und tiefe Verschachtelung */
            char *piece = g_strndup(text->str + at, MIN(rest, size));
            for (int n = g_rand_int_range(rand, 1, 64); n > 0 && text->len < FUZZ_MAX_INPUT; n--)
                g_string_insert(text, (gssize)at, piece);
            g_free(piece);
            break;
        }
        default:
            g_string_insert(text, (gssize)at, TOKENS[g_rand_int_range(rand, 0, G_N_ELEMENTS(TOKENS))]);
        }
    }
}

static void check_url(const char *url)
{
    if (!url) fail("Adresse fehlt");
    if (*url && !g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://"))
        fail("fremde Adresse in einem Titel");
}

static void check_title(const MdTitle *t)
{
    titles_checked++;
    if (!t->name || !g_utf8_validate(t->name, -1, NULL)) fail("Name fehlt oder ist kein UTF-8");
    check_url(t->url_high);
    check_url(t->url_medium);
    check_url(t->url_low);
    check_url(t->url_subtitle);
    check_url(t->website);
    char *path = md_target_path(t, "/ziel"), *file = g_path_get_basename(path);
    if (strlen(file) > 230) fail("Dateiname zu lang");
    if (!g_utf8_validate(file, -1, NULL)) fail("Dateiname ist kein UTF-8");
    g_free(path);
    g_free(file);
}

/* Die erste Zeile des Merkzettels geht als Adresse an curl */
static void check_page_info(const GString *info)
{
    if (info->len && *info->str != '\n' && !g_str_has_prefix(info->str, "https://"))
        fail("fremde Bildadresse");
}

static void run_input(Target target, const char *input)
{
    GPtrArray *titles = g_ptr_array_new_with_free_func(md_title_free);
    GString *info = g_string_new(NULL);
    if (target == TARGET_SEARCH) {
        char *error = NULL;
        int total = 0;
        parse_response(input, titles, &total, &error);
        g_free(error);
    } else if (target == TARGET_NETZKINO) {
        MdQuery query = {.text = (rounds & 1) ? "film" : ""};
        GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        netzkino_parse(input, &query, titles, seen);
        g_hash_table_destroy(seen);
    } else if (target == TARGET_ARD) {
        ard_page_info(input, info);
    } else {
        html_page_info(COVER_3SAT_PAGE "krimi", input, info);
    }
    for (guint i = 0; i < titles->len; i++) check_title(titles->pdata[i]);
    check_page_info(info);
    g_ptr_array_free(titles, TRUE);
    g_string_free(info, TRUE);
}

int main(void)
{
    test_dir_create();
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING);
    const char *seed_text = g_getenv("FUZZ_SEED"), *seconds_text = g_getenv("FUZZ_SECONDS");
    seed = seed_text ? (guint32)g_ascii_strtoull(seed_text, NULL, 10) : g_random_int();
    double seconds = seconds_text ? g_ascii_strtod(seconds_text, NULL) : 10;
    printf("Fuzz-Lauf: %.0f s, FUZZ_SEED=%u\n", seconds, seed);
    fflush(stdout);
    md_set_cover_dir(test_dir);
    GRand *rand = g_rand_new_with_seed(seed);
    GTimer *timer = g_timer_new();
    gint64 counts[TARGET_COUNT] = {0};
    GString *text = g_string_new(NULL);
    while (g_timer_elapsed(timer, NULL) < seconds) {
        Target target = (Target)(rounds % TARGET_COUNT);
        const char *const *samples = SAMPLES[target];
        int sample;
        do sample = g_rand_int_range(rand, 0, 3); while (!samples[sample]);
        g_string_assign(text, samples[sample]);
        mutate(rand, text);
        g_strlcpy(last_input, text->str, sizeof last_input);
        run_input(target, text->str);
        counts[target]++;
        rounds++;
    }
    /* unveränderte Beispiele zuletzt: Sie müssen gültige Titel ergeben, sonst prüft der Lauf nichts */
    GPtrArray *titles = g_ptr_array_new_with_free_func(md_title_free);
    char *error = NULL;
    int total = 0;
    if (!parse_response(SAMPLES[TARGET_SEARCH][0], titles, &total, &error) || titles->len != 1)
        fail("Beispiel der Suche wird nicht gelesen");
    g_ptr_array_set_size(titles, 0);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    netzkino_parse(SAMPLES[TARGET_NETZKINO][0], &(MdQuery){.text = ""}, titles, seen);
    if (titles->len != 1) fail("Beispiel von Netzkino wird nicht gelesen");
    g_hash_table_destroy(seen);
    g_ptr_array_free(titles, TRUE);
    printf("Fuzz-Lauf fertig: %" G_GINT64_FORMAT " Eingaben (Suche %" G_GINT64_FORMAT ", Netzkino %"
           G_GINT64_FORMAT ", ARD %" G_GINT64_FORMAT ", Seiten %" G_GINT64_FORMAT "), %"
           G_GINT64_FORMAT " Titel geprüft\n", rounds, counts[TARGET_SEARCH],
           counts[TARGET_NETZKINO], counts[TARGET_ARD], counts[TARGET_HTML], titles_checked);
    g_string_free(text, TRUE);
    g_timer_destroy(timer);
    g_rand_free(rand);
    test_dir_remove();
    return 0;
}
