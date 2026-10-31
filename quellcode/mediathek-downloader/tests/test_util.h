/*
 * Mediathek Downloader – Helfer der Tests
 * =======================================
 * Alles, was die Tests anfassen, liegt in einem temporären Ordner. Er ist zugleich HOME,
 * damit weder ein ~/.curlrc noch der echte Ordner ~/.mediathek-downloader im Spiel ist.
 */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <ftw.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>

static char *test_dir;

/* Anlegen, bevor irgendetwas HOME liest: GLib merkt sich den Wert beim ersten Zugriff */
static void test_dir_create(void)
{
    GError *error = NULL;
    test_dir = g_dir_make_tmp("mediathek-test-XXXXXX", &error);
    if (!test_dir) {
        fprintf(stderr, "Kein temporärer Ordner: %s\n", error->message);
        exit(1);
    }
    g_setenv("HOME", test_dir, TRUE);
    g_setenv("XDG_CONFIG_HOME", test_dir, TRUE);
    g_setenv("XDG_CACHE_HOME", test_dir, TRUE);
    g_setenv("no_proxy", "*", TRUE);
    if (g_chdir(test_dir) != 0) exit(1);
}

static int test_remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st, (void)type, (void)ftw;
    return remove(path);
}

static void test_dir_remove(void)
{
    if (g_chdir("/") != 0) return;
    nftw(test_dir, test_remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    g_free(test_dir);
}

#endif
