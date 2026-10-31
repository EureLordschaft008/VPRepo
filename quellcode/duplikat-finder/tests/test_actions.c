/*
 * Duplikat-Finder – Regressionstests der Link-Aktionen und des Suchlaufs
 * ======================================================================
 * Alle Dateien liegen in einem temporären Ordner, der zugleich HOME ist. Der Suchlauf-Test
 * startet duplikat-finder-cli (Pfad in DF_TEST_CLI). Aufruf: make test
 */
#define _GNU_SOURCE
#include <ftw.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/df_core.h"

#define CONTENT "Inhalt, der erhalten bleiben muss"
#define IMAGE_SIZE 64
#define IMAGE_BLOCK 8
#define BMP_HEADER 54

static char *test_dir;
static int dir_count;

/* ---------------------------------------------------------------------------
 * Helfer
 * ------------------------------------------------------------------------- */
static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st, (void)type, (void)ftw;
    return remove(path);
}

/* neuer leerer Ordner je Testlauf */
static char *new_dir(void)
{
    char *name = g_strdup_printf("fall-%d", ++dir_count);
    char *dir = g_build_filename(test_dir, name, NULL);
    g_assert_cmpint(g_mkdir(dir, 0755), ==, 0);
    g_free(name);
    return dir;
}

static char *write_file(const char *dir, const char *name, const char *text)
{
    char *path = g_build_filename(dir, name, NULL);
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
    return path;
}

static void assert_content(const char *path, const char *text)
{
    char *data = NULL;
    g_assert_true(g_file_get_contents(path, &data, NULL, NULL));
    g_assert_cmpstr(data, ==, text);
    g_free(data);
}

static void assert_no_temp_file(const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    g_assert_nonnull(d);
    const char *name;
    while ((name = g_dir_read_name(d))) g_assert_null(strstr(name, ".df-link-"));
    g_dir_close(d);
}

static bool same_inode(const char *a, const char *b)
{
    GStatBuf sa, sb;
    return g_stat(a, &sa) == 0 && g_stat(b, &sb) == 0 && sa.st_ino == sb.st_ino &&
           sa.st_dev == sb.st_dev;
}

static bool is_symlink(const char *path)
{
    return g_file_test(path, G_FILE_TEST_IS_SYMLINK);
}

/* Die Aktion muss mit einer Meldung, die wording enthält, ablehnen und nichts verändern */
static void assert_refused(DfAction action, const char *path, const char *target, const char *dir,
                           const char *wording)
{
    char err[256] = "";
    g_assert_false(df_file_action(action, path, target, NULL, err, sizeof err));
    g_assert_nonnull(strstr(err, wording));
    g_assert_false(is_symlink(path));
    assert_content(path, CONTENT);
    assert_no_temp_file(dir);
}

static void assert_replaced(DfAction action, const char *path, const char *target)
{
    char err[256] = "";
    g_assert_true(df_file_action(action, path, target, NULL, err, sizeof err));
    g_assert_cmpstr(err, ==, "");
    if (action == DF_ACT_SYMLINK) g_assert_true(is_symlink(path));
    else g_assert_true(same_inode(path, target));
    assert_content(path, CONTENT);
    assert_content(target, CONTENT);
}

/* ---------------------------------------------------------------------------
 * Link-Aktionen
 * ------------------------------------------------------------------------- */
static void test_same_file_via_dir_link(gconstpointer data)
{
    DfAction action = (DfAction)GPOINTER_TO_INT(data);
    char *dir = new_dir(), *path = write_file(dir, "film.mp4", CONTENT);
    char *linked = g_build_filename(dir, "verlinkt", NULL);
    g_assert_cmpint(symlink(dir, linked), ==, 0);
    char *target = g_build_filename(linked, "film.mp4", NULL);
    assert_refused(action, path, target, dir, "dieselbe Datei");
    char *dotted = g_strconcat(dir, "/./film.mp4", NULL);
    assert_refused(action, path, dotted, dir, "dieselbe Datei");
    g_free(dir);
    g_free(path);
    g_free(linked);
    g_free(target);
    g_free(dotted);
}

static void test_target_links_to_path(gconstpointer data)
{
    DfAction action = (DfAction)GPOINTER_TO_INT(data);
    char *dir = new_dir(), *path = write_file(dir, "film.mp4", CONTENT);
    char *target = g_build_filename(dir, "link.mp4", NULL);
    g_assert_cmpint(symlink(path, target), ==, 0);
    assert_refused(action, path, target, dir, "dieselbe Datei");
    g_free(dir);
    g_free(path);
    g_free(target);
}

static void test_target_missing(gconstpointer data)
{
    DfAction action = (DfAction)GPOINTER_TO_INT(data);
    char *dir = new_dir(), *path = write_file(dir, "film.mp4", CONTENT);
    char *target = g_build_filename(dir, "fehlt.mp4", NULL);
    assert_refused(action, path, target, dir, "fehlt");
    g_free(dir);
    g_free(path);
    g_free(target);
}

/* rename tut nichts, wenn beide Namen schon dieselbe Datei sind */
static void test_two_hardlinks_leave_no_temp_file(void)
{
    char *dir = new_dir(), *path = write_file(dir, "film.mp4", CONTENT);
    char *target = g_build_filename(dir, "zweiter-name.mp4", NULL);
    g_assert_cmpint(link(path, target), ==, 0);
    assert_replaced(DF_ACT_HARDLINK, path, target);
    assert_no_temp_file(dir);
    g_free(dir);
    g_free(path);
    g_free(target);
}

static void test_real_copy_replaced(gconstpointer data)
{
    DfAction action = (DfAction)GPOINTER_TO_INT(data);
    char *dir = new_dir(), *path = write_file(dir, "kopie.mp4", CONTENT);
    char *target = write_file(dir, "film.mp4", CONTENT);
    assert_replaced(action, path, target);
    assert_no_temp_file(dir);
    g_free(dir);
    g_free(path);
    g_free(target);
}

/* Ein Link als Pfad verliert nichts, auch wenn er schon aufs Ziel zeigt */
static void test_link_as_path_replaced(gconstpointer data)
{
    DfAction action = (DfAction)GPOINTER_TO_INT(data);
    char *dir = new_dir(), *other = write_file(dir, "anderes.mp4", "andere Datei");
    char *target = write_file(dir, "film.mp4", CONTENT);
    char *path = g_build_filename(dir, "link.mp4", NULL);
    g_assert_cmpint(symlink(other, path), ==, 0);
    assert_replaced(action, path, target);
    assert_content(other, "andere Datei");
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_assert_cmpint(symlink(target, path), ==, 0);
    assert_replaced(action, path, target);
    assert_no_temp_file(dir);
    g_free(dir);
    g_free(other);
    g_free(target);
    g_free(path);
}

/* ---------------------------------------------------------------------------
 * Suchlauf
 * ------------------------------------------------------------------------- */
static void put32(guint8 *at, guint32 value)
{
    for (int i = 0; i < 4; i++) at[i] = (guint8)(value >> (8 * i));
}

/* BMP aus zufälligen Grauflächen: Der Hilfsprozess liest es, ein einfarbiges Bild zählt nicht */
static void write_image(const char *path, guint32 seed)
{
    guint8 image[BMP_HEADER + IMAGE_SIZE * IMAGE_SIZE * 3] = {'B', 'M'};
    guint8 blocks[IMAGE_SIZE / IMAGE_BLOCK][IMAGE_SIZE / IMAGE_BLOCK];
    put32(image + 2, sizeof image);
    put32(image + 10, BMP_HEADER);
    put32(image + 14, 40);
    put32(image + 18, IMAGE_SIZE);
    put32(image + 22, IMAGE_SIZE);
    image[26] = 1;
    image[28] = 24;
    GRand *rand = g_rand_new_with_seed(seed);
    for (int y = 0; y < IMAGE_SIZE / IMAGE_BLOCK; y++)
        for (int x = 0; x < IMAGE_SIZE / IMAGE_BLOCK; x++)
            blocks[y][x] = (guint8)g_rand_int_range(rand, 0, 256);
    g_rand_free(rand);
    for (int y = 0; y < IMAGE_SIZE; y++)
        for (int x = 0; x < IMAGE_SIZE; x++)
            memset(image + BMP_HEADER + (y * IMAGE_SIZE + x) * 3,
                   blocks[y / IMAGE_BLOCK][x / IMAGE_BLOCK], 3);
    g_assert_true(g_file_set_contents(path, (const char *)image, sizeof image, NULL));
}

/* Das echte Paar zeigt, dass der Lauf überhaupt Duplikate findet */
static void test_scan_symlink_is_no_duplicate(void)
{
    const char *cli = g_getenv("DF_TEST_CLI");
    g_assert_nonnull(cli);
    char *home = new_dir(), *files = new_dir(), *no_programs = new_dir();
    char *app_dir = g_build_filename(home, ".duplikat-finder", NULL);
    g_assert_cmpint(g_mkdir(app_dir, 0755), ==, 0);
    char *ini = g_strdup_printf("[Einstellungen]\nordner=%s;\nfotos=true\n"
                                "symlinks_ueberspringen=false\ncpu_prozesse=1\ngpu_prozesse=0\n"
                                "ruecksicht=false\n", files);
    g_free(write_file(app_dir, "gui.ini", ini));

    char *real = g_build_filename(files, "echt.bmp", NULL);
    char *linked = g_build_filename(files, "link.bmp", NULL);
    char *copy1 = g_build_filename(files, "kopie1.bmp", NULL);
    char *copy2 = g_build_filename(files, "kopie2.bmp", NULL);
    write_image(real, 1);
    g_assert_cmpint(symlink(real, linked), ==, 0);
    write_image(copy1, 2);
    write_image(copy2, 2);

    /* ein leerer PATH, damit --zeitplan keine Desktop-Meldung per notify-send schickt */
    char *cache = g_build_filename(home, ".cache", NULL);
    char *env_home = g_strconcat("HOME=", home, NULL);
    char *env_cache = g_strconcat("XDG_CACHE_HOME=", cache, NULL);
    char *env_path = g_strconcat("PATH=", no_programs, NULL);
    char *argv[] = {(char *)cli, "--zeitplan", NULL};
    char *envp[] = {env_home, env_cache, env_path, "LC_ALL=C", NULL};
    char *out = NULL, *messages = NULL;
    int status = -1;
    GError *error = NULL;
    g_assert_true(g_spawn_sync(files, argv, envp, G_SPAWN_DEFAULT, NULL, NULL, &out, &messages,
                               &status, &error));
    g_assert_no_error(error);
    g_test_message("%s%s", out, messages);
    g_assert_true(g_spawn_check_wait_status(status, NULL));
    g_assert_nonnull(strstr(out, "1 Gruppen, 1 Duplikate"));

    char *result_path = g_build_filename(app_dir, "geplantes-ergebnis.json", NULL), *result = NULL;
    g_assert_true(g_file_get_contents(result_path, &result, NULL, NULL));
    g_assert_nonnull(strstr(result, "kopie1.bmp"));
    g_assert_nonnull(strstr(result, "kopie2.bmp"));
    g_assert_null(strstr(result, "link.bmp"));

    g_free(home);
    g_free(files);
    g_free(no_programs);
    g_free(app_dir);
    g_free(ini);
    g_free(real);
    g_free(linked);
    g_free(copy1);
    g_free(copy2);
    g_free(cache);
    g_free(env_home);
    g_free(env_cache);
    g_free(env_path);
    g_free(out);
    g_free(messages);
    g_free(result_path);
    g_free(result);
}

static void add_for_both_links(const char *name, GTestDataFunc test)
{
    char *symbolic = g_strconcat("/symlink/", name, NULL);
    char *hard = g_strconcat("/hardlink/", name, NULL);
    g_test_add_data_func(symbolic, GINT_TO_POINTER(DF_ACT_SYMLINK), test);
    g_test_add_data_func(hard, GINT_TO_POINTER(DF_ACT_HARDLINK), test);
    g_free(symbolic);
    g_free(hard);
}

int main(int argc, char **argv)
{
    GError *error = NULL;
    test_dir = g_dir_make_tmp("duplikat-test-XXXXXX", &error);
    g_assert_no_error(error);
    /* vor dem ersten Zugriff setzen: GLib merkt sich den Wert */
    g_setenv("HOME", test_dir, TRUE);
    g_setenv("XDG_CACHE_HOME", test_dir, TRUE);
    g_assert_cmpint(g_chdir(test_dir), ==, 0);
    g_test_init(&argc, &argv, NULL);
    g_test_set_nonfatal_assertions();
    df_init();
    add_for_both_links("gleiche-datei-ueber-ordnerlink", test_same_file_via_dir_link);
    add_for_both_links("ziel-ist-link-auf-pfad", test_target_links_to_path);
    add_for_both_links("ziel-fehlt", test_target_missing);
    g_test_add_func("/hardlink/zwei-namen-ohne-reste", test_two_hardlinks_leave_no_temp_file);
    add_for_both_links("echte-kopie-wird-ersetzt", test_real_copy_replaced);
    add_for_both_links("link-als-pfad-wird-ersetzt", test_link_as_path_replaced);
    g_test_add_func("/suchlauf/symlink-kein-duplikat", test_scan_symlink_is_no_duplicate);
    int result = g_test_run();
    df_shutdown();
    if (g_chdir("/") == 0) nftw(test_dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    g_free(test_dir);
    return result;
}
