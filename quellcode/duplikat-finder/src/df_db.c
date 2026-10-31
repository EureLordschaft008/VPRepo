/*
 * Duplikat-Finder – Datenbank (SQLite, ~/.duplikat-finder/dateien.db)
 * ===================================================================
 * Je Datei: Größe, Änderungszeit, Infos (Laufzeit, Auflösung, Codecs …) und
 * die Vergleichsbilder (je 20 Byte). Ändert sich eine Datei nicht, wird sie
 * nie wieder gelesen. Geschrieben wird während des Suchlaufs in kleinen
 * Paketen (WAL) – ein Absturz oder Abbruch verliert höchstens die letzten
 * Sekunden, und es gibt kein langes „Speichern“ am Ende.
 *
 * Dazu: Ton-Fingerabdrücke (Teilclips), Paare „kein Duplikat“ und ignorierte
 * Dateien.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#include "df_internal.h"

#define DB_SCHEMA 1

static sqlite3 *g_db;
static GRecMutex g_db_lock;

void df_db_lock(void)
{
    g_rec_mutex_lock(&g_db_lock);
}

void df_db_unlock(void)
{
    g_rec_mutex_unlock(&g_db_lock);
}

static bool exec(const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        df_log(false, "Datenbank: %s (%s)", err ? err : "?", sql);
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool df_db_open(void)
{
    df_db_lock();
    if (g_db) {
        df_db_unlock();
        return true;
    }
    if (sqlite3_open_v2(DF_DB_PATH, &g_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                        SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        df_log(false, "Datenbank %s nicht zu öffnen: %s", DF_DB_PATH, sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        df_db_unlock();
        return false;
    }
    sqlite3_busy_timeout(g_db, 10000);
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");
    exec("PRAGMA cache_size=-65536;");
    exec("PRAGMA foreign_keys=ON;");
    exec("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);"
         "CREATE TABLE IF NOT EXISTS files("
         " id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, size INTEGER, mtime INTEGER,"
         " inode INTEGER, k INTEGER, white INTEGER, status INTEGER, error TEXT,"
         " duration REAL, width INTEGER, height INTEGER, fps REAL, bitrate INTEGER,"
         " vcodec TEXT, acodec TEXT, asr INTEGER, ach INTEGER, abitrate INTEGER, image INTEGER,"
         " scanned INTEGER, frames BLOB);"
         "CREATE TABLE IF NOT EXISTS audio("
         " id INTEGER PRIMARY KEY REFERENCES files(id) ON DELETE CASCADE,"
         " size INTEGER, mtime INTEGER, status INTEGER, error TEXT, fp BLOB);"
         "CREATE TABLE IF NOT EXISTS notmatch(a INTEGER, b INTEGER, PRIMARY KEY(a, b))"
         " WITHOUT ROWID;"
         "CREATE TABLE IF NOT EXISTS ignored(path TEXT PRIMARY KEY) WITHOUT ROWID;");
    exec("INSERT OR IGNORE INTO meta VALUES('schema', '1');");
    df_db_unlock();
    return true;
}

void df_db_close(void)
{
    df_db_lock();
    if (g_db) {
        exec("PRAGMA optimize;");
        sqlite3_close(g_db);
    }
    g_db = NULL;
    df_db_unlock();
}

bool df_db_begin(void)
{
    return g_db && exec("BEGIN;");
}

bool df_db_commit(void)
{
    return g_db && exec("COMMIT;");
}

static const char *col_text(sqlite3_stmt *s, int i)
{
    const unsigned char *t = sqlite3_column_text(s, i);
    return t ? (const char *)t : "";
}

void df_db_entry_free(gpointer p)
{
    DfDbEntry *e = p;
    if (!e) return;
    df_file_clear(&e->f);
    g_free(e->error);
    g_free(e);
}

GHashTable *df_db_load_all(void)
{
    GHashTable *t = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, df_db_entry_free);
    df_db_lock();
    sqlite3_stmt *s = NULL;
    if (g_db && sqlite3_prepare_v2(g_db,
            "SELECT id, path, size, mtime, inode, k, white, status, error, duration, width, height,"
            " fps, bitrate, vcodec, acodec, asr, ach, abitrate, image, frames FROM files",
            -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            DfDbEntry *e = g_new0(DfDbEntry, 1);
            DfFile *f = &e->f;
            f->id = sqlite3_column_int64(s, 0);
            f->path = g_strdup(col_text(s, 1));
            f->size = sqlite3_column_int64(s, 2);
            f->mtime = sqlite3_column_int64(s, 3);
            e->inode = sqlite3_column_int64(s, 4);
            e->k = sqlite3_column_int(s, 5);
            e->white_borders = sqlite3_column_int(s, 6) != 0;
            e->status = sqlite3_column_int(s, 7);
            const char *err = col_text(s, 8);
            e->error = *err ? g_strdup(err) : NULL;
            f->duration = sqlite3_column_double(s, 9);
            f->width = sqlite3_column_int(s, 10);
            f->height = sqlite3_column_int(s, 11);
            f->fps = (float)sqlite3_column_double(s, 12);
            f->bitrate = sqlite3_column_int(s, 13);
            g_strlcpy(f->vcodec, col_text(s, 14), sizeof f->vcodec);
            g_strlcpy(f->acodec, col_text(s, 15), sizeof f->acodec);
            f->asr = sqlite3_column_int(s, 16);
            f->ach = sqlite3_column_int(s, 17);
            f->abitrate = sqlite3_column_int(s, 18);
            f->image = sqlite3_column_int(s, 19) != 0;
            int bytes = sqlite3_column_bytes(s, 20);
            const void *blob = sqlite3_column_blob(s, 20);
            if (blob && bytes > 0 && bytes % (int)sizeof(DfFrame) == 0 &&
                bytes / (int)sizeof(DfFrame) <= DF_MAX_FRAMES) {
                f->nframes = bytes / (int)sizeof(DfFrame);
                f->frames = g_memdup2(blob, (gsize)bytes);
            }
            g_hash_table_replace(t, f->path, e);
        }
    }
    sqlite3_finalize(s);
    df_db_unlock();
    return t;
}

int64_t df_db_put(const DfDbEntry *e)
{
    const DfFile *f = &e->f;
    static sqlite3_stmt *s;
    int64_t id = 0;
    df_db_lock();
    if (!g_db) {
        df_db_unlock();
        return 0;
    }
    if (!s)
        sqlite3_prepare_v3(g_db,
            "INSERT INTO files(path, size, mtime, inode, k, white, status, error, duration, width,"
            " height, fps, bitrate, vcodec, acodec, asr, ach, abitrate, image, scanned, frames)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,"
            " strftime('%s','now'),?20)"
            " ON CONFLICT(path) DO UPDATE SET size=?2, mtime=?3, inode=?4, k=?5, white=?6,"
            " status=?7, error=?8, duration=?9, width=?10, height=?11, fps=?12, bitrate=?13,"
            " vcodec=?14, acodec=?15, asr=?16, ach=?17, abitrate=?18, image=?19,"
            " scanned=strftime('%s','now'), frames=?20 RETURNING id",
            -1, SQLITE_PREPARE_PERSISTENT, &s, NULL);
    if (s) {
        sqlite3_bind_text(s, 1, f->path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(s, 2, f->size);
        sqlite3_bind_int64(s, 3, f->mtime);
        sqlite3_bind_int64(s, 4, e->inode);
        sqlite3_bind_int(s, 5, e->k);
        sqlite3_bind_int(s, 6, e->white_borders);
        sqlite3_bind_int(s, 7, e->status);
        if (e->error) sqlite3_bind_text(s, 8, e->error, -1, SQLITE_STATIC);
        else sqlite3_bind_null(s, 8);
        sqlite3_bind_double(s, 9, f->duration);
        sqlite3_bind_int(s, 10, f->width);
        sqlite3_bind_int(s, 11, f->height);
        sqlite3_bind_double(s, 12, f->fps);
        sqlite3_bind_int(s, 13, f->bitrate);
        sqlite3_bind_text(s, 14, f->vcodec, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 15, f->acodec, -1, SQLITE_STATIC);
        sqlite3_bind_int(s, 16, f->asr);
        sqlite3_bind_int(s, 17, f->ach);
        sqlite3_bind_int(s, 18, f->abitrate);
        sqlite3_bind_int(s, 19, f->image);
        if (f->nframes)
            sqlite3_bind_blob(s, 20, f->frames, (int)(sizeof(DfFrame) * (size_t)f->nframes),
                              SQLITE_STATIC);
        else
            sqlite3_bind_null(s, 20);
        if (sqlite3_step(s) == SQLITE_ROW) id = sqlite3_column_int64(s, 0);
        else df_log(false, "Datenbank: %s", sqlite3_errmsg(g_db));
        while (sqlite3_step(s) == SQLITE_ROW) {}
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
    }
    df_db_unlock();
    return id;
}

bool df_db_audio_get(int64_t id, long long size, long long mtime, uint32_t **fp, int *n,
                     int *status)
{
    *fp = NULL;
    *n = 0;
    *status = -1;
    bool found = false;
    df_db_lock();
    sqlite3_stmt *s = NULL;
    if (g_db && sqlite3_prepare_v2(g_db, "SELECT size, mtime, status, fp FROM audio WHERE id=?",
                                   -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id);
        if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int64(s, 0) == size &&
            sqlite3_column_int64(s, 1) == mtime) {
            found = true;
            *status = sqlite3_column_int(s, 2);
            int bytes = sqlite3_column_bytes(s, 3);
            const void *b = sqlite3_column_blob(s, 3);
            if (b && bytes >= 4) {
                *n = bytes / 4;
                *fp = g_memdup2(b, (gsize)*n * 4);
            }
        }
    }
    sqlite3_finalize(s);
    df_db_unlock();
    return found;
}

bool df_db_audio_put(int64_t id, long long size, long long mtime, const uint32_t *fp, int n,
                     int status, const char *error)
{
    bool ok = false;
    df_db_lock();
    sqlite3_stmt *s = NULL;
    if (g_db && sqlite3_prepare_v2(g_db, "INSERT OR REPLACE INTO audio VALUES(?,?,?,?,?,?)", -1, &s,
                                   NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id);
        sqlite3_bind_int64(s, 2, size);
        sqlite3_bind_int64(s, 3, mtime);
        sqlite3_bind_int(s, 4, status);
        if (error) sqlite3_bind_text(s, 5, error, -1, SQLITE_STATIC);
        else sqlite3_bind_null(s, 5);
        if (fp && n > 0) sqlite3_bind_blob(s, 6, fp, n * 4, SQLITE_STATIC);
        else sqlite3_bind_null(s, 6);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    df_db_unlock();
    return ok;
}

int64_t df_pair_key(int64_t a, int64_t b)
{
    if (a > b) {
        int64_t t = a;
        a = b;
        b = t;
    }
    return (int64_t)((uint64_t)a << 32 | (uint64_t)(uint32_t)b);
}

GHashTable *df_db_notmatch_load(void)
{
    GHashTable *t = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    df_db_lock();
    sqlite3_stmt *s = NULL;
    if (g_db && sqlite3_prepare_v2(g_db, "SELECT a, b FROM notmatch", -1, &s, NULL) == SQLITE_OK)
        while (sqlite3_step(s) == SQLITE_ROW) {
            gint64 *k = g_new(gint64, 1);
            *k = df_pair_key(sqlite3_column_int64(s, 0), sqlite3_column_int64(s, 1));
            g_hash_table_add(t, k);
        }
    sqlite3_finalize(s);
    df_db_unlock();
    return t;
}

GHashTable *df_db_ignored_load(void)
{
    GHashTable *t = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    df_db_lock();
    sqlite3_stmt *s = NULL;
    if (g_db && sqlite3_prepare_v2(g_db, "SELECT path FROM ignored", -1, &s, NULL) == SQLITE_OK)
        while (sqlite3_step(s) == SQLITE_ROW) g_hash_table_add(t, g_strdup(col_text(s, 0)));
    sqlite3_finalize(s);
    df_db_unlock();
    return t;
}

/* ---------------------------------------------------------------------------
 * Pflege (Menü "Datenbank")
 * ------------------------------------------------------------------------- */
static int count_of(const char *sql)
{
    sqlite3_stmt *s = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(g_db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n;
}

bool df_db_info(DfDbInfo *i)
{
    memset(i, 0, sizeof *i);
    df_db_lock();
    if (!g_db) {
        df_db_unlock();
        return false;
    }
    i->files = count_of("SELECT count(*) FROM files WHERE status=0");
    i->failed = count_of("SELECT count(*) FROM files WHERE status<>0");
    i->audio = count_of("SELECT count(*) FROM audio WHERE status=0");
    i->notmatch = count_of("SELECT count(*) FROM notmatch");
    i->ignored = count_of("SELECT count(*) FROM ignored");
    df_db_unlock();
    GStatBuf sb;
    if (g_stat(DF_DB_PATH, &sb) == 0) i->bytes = sb.st_size;
    char *wal = g_strconcat(DF_DB_PATH, "-wal", NULL);
    if (g_stat(wal, &sb) == 0) i->bytes += sb.st_size;
    g_free(wal);
    return true;
}

int df_db_prune_missing(void)
{
    int removed = 0;
    df_db_lock();
    sqlite3_stmt *s = NULL, *d = NULL;
    GPtrArray *gone = g_ptr_array_new_with_free_func(g_free);
    if (g_db && sqlite3_prepare_v2(g_db, "SELECT id, path FROM files", -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            if (df_cancel) break;
            if (g_access(col_text(s, 1), F_OK) != 0) {
                gint64 *id = g_new(gint64, 1);
                *id = sqlite3_column_int64(s, 0);
                g_ptr_array_add(gone, id);
            }
        }
    }
    sqlite3_finalize(s);
    if (gone->len && sqlite3_prepare_v2(g_db, "DELETE FROM files WHERE id=?", -1, &d, NULL) ==
                         SQLITE_OK) {
        exec("BEGIN;");
        for (guint i = 0; i < gone->len; i++) {
            sqlite3_bind_int64(d, 1, *(gint64 *)gone->pdata[i]);
            if (sqlite3_step(d) == SQLITE_DONE) removed++;
            sqlite3_reset(d);
        }
        exec("DELETE FROM notmatch WHERE a NOT IN (SELECT id FROM files) OR"
             " b NOT IN (SELECT id FROM files);");
        exec("COMMIT;");
    }
    sqlite3_finalize(d);
    g_ptr_array_free(gone, TRUE);
    if (removed > 1000) exec("VACUUM;");
    df_db_unlock();
    return removed;
}

bool df_db_clear(void)
{
    df_db_lock();
    bool ok = g_db && exec("DELETE FROM audio; DELETE FROM notmatch; DELETE FROM files; VACUUM;");
    df_db_unlock();
    return ok;
}

int df_db_forget_failed(void)
{
    df_db_lock();
    int n = 0;
    if (g_db && exec("DELETE FROM files WHERE status<>0;")) n = sqlite3_changes(g_db);
    df_db_unlock();
    return n;
}

/* Pfad-Anfang ersetzen (Laufwerk umbenannt, Ordner verschoben) */
int df_db_relocate(const char *old_prefix, const char *new_prefix)
{
    if (!old_prefix || !*old_prefix || !new_prefix) return 0;
    df_db_lock();
    int n = 0;
    sqlite3_stmt *s = NULL;
    if (g_db && sqlite3_prepare_v2(g_db,
            "UPDATE OR IGNORE files SET path = ?2 || substr(path, length(?1) + 1)"
            " WHERE substr(path, 1, length(?1)) = ?1", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, old_prefix, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, new_prefix, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_DONE) n = sqlite3_changes(g_db);
    }
    sqlite3_finalize(s);
    df_db_unlock();
    return n;
}

bool df_db_remove_path(const char *path)
{
    df_db_lock();
    sqlite3_stmt *s = NULL;
    bool ok = false;
    if (g_db && sqlite3_prepare_v2(g_db, "DELETE FROM files WHERE path=?", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    df_db_unlock();
    return ok;
}

bool df_db_rename_path(const char *old_path, const char *new_path)
{
    df_db_lock();
    sqlite3_stmt *s = NULL;
    bool ok = false;
    if (g_db && sqlite3_prepare_v2(g_db, "UPDATE OR REPLACE files SET path=?2 WHERE path=?1", -1, &s,
                                   NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, old_path, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, new_path, -1, SQLITE_STATIC);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    df_db_unlock();
    return ok;
}

int df_db_search(const char *text, int max, DfStrList *out)
{
    memset(out, 0, sizeof *out);
    df_db_lock();
    sqlite3_stmt *s = NULL;
    if (g_db && sqlite3_prepare_v2(g_db,
            "SELECT path, status, coalesce(error, '') FROM files WHERE instr(lower(path), lower(?1))"
            " ORDER BY path LIMIT ?2", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, text ? text : "", -1, SQLITE_STATIC);
        sqlite3_bind_int(s, 2, max);
        while (sqlite3_step(s) == SQLITE_ROW) {
            char *line = g_strdup_printf("%s\t%d\t%s", col_text(s, 0), sqlite3_column_int(s, 1),
                                         col_text(s, 2));
            df_strlist_add(out, line);
            g_free(line);
        }
    }
    sqlite3_finalize(s);
    df_db_unlock();
    return out->n;
}

bool df_notmatch_add(int64_t a, int64_t b)
{
    if (a == b || a <= 0 || b <= 0) return false;
    if (a > b) {
        int64_t t = a;
        a = b;
        b = t;
    }
    df_db_lock();
    sqlite3_stmt *s = NULL;
    bool ok = false;
    if (g_db && sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO notmatch VALUES(?,?)", -1, &s, NULL) ==
                    SQLITE_OK) {
        sqlite3_bind_int64(s, 1, a);
        sqlite3_bind_int64(s, 2, b);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    df_db_unlock();
    return ok;
}

int df_notmatch_clear(void)
{
    df_db_lock();
    int n = 0;
    if (g_db && exec("DELETE FROM notmatch;")) n = sqlite3_changes(g_db);
    df_db_unlock();
    return n;
}

bool df_ignore_add(const char *path)
{
    df_db_lock();
    sqlite3_stmt *s = NULL;
    bool ok = false;
    if (g_db && sqlite3_prepare_v2(g_db, "INSERT OR IGNORE INTO ignored VALUES(?)", -1, &s, NULL) ==
                    SQLITE_OK) {
        sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    df_db_unlock();
    return ok;
}

bool df_ignore_remove(const char *path)
{
    df_db_lock();
    sqlite3_stmt *s = NULL;
    bool ok = false;
    if (g_db && sqlite3_prepare_v2(g_db, "DELETE FROM ignored WHERE path=?", -1, &s, NULL) ==
                    SQLITE_OK) {
        sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    df_db_unlock();
    return ok;
}

int df_ignore_clear(void)
{
    df_db_lock();
    int n = 0;
    if (g_db && exec("DELETE FROM ignored;")) n = sqlite3_changes(g_db);
    df_db_unlock();
    return n;
}

int df_ignore_list(DfStrList *out)
{
    memset(out, 0, sizeof *out);
    df_db_lock();
    sqlite3_stmt *s = NULL;
    if (g_db && sqlite3_prepare_v2(g_db, "SELECT path FROM ignored ORDER BY path", -1, &s, NULL) ==
                    SQLITE_OK)
        while (sqlite3_step(s) == SQLITE_ROW) df_strlist_add(out, col_text(s, 0));
    sqlite3_finalize(s);
    df_db_unlock();
    return out->n;
}

/* ---------------------------------------------------------------------------
 * Export / Import (JSON, eine Datei je Zeile – auch für sehr große Datenbanken)
 * ------------------------------------------------------------------------- */
static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == '"' || *c == '\\') fprintf(f, "\\%c", *c);
        else if (*c < 0x20) fprintf(f, "\\u%04x", *c);
        else fputc(*c, f);
    }
    fputc('"', f);
}

bool df_db_export_json(const char *path, char *err, size_t errsz)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(err, errsz, "%s: %s", path, g_strerror(errno));
        return false;
    }
    df_db_lock();
    sqlite3_stmt *s = NULL;
    fprintf(f, "{\"format\": \"duplikat-finder-datenbank\", \"version\": %d, \"dateien\": [\n",
            DB_SCHEMA);
    bool first = true;
    if (g_db && sqlite3_prepare_v2(g_db,
            "SELECT path, size, mtime, inode, k, white, status, coalesce(error,''), duration, width,"
            " height, fps, bitrate, coalesce(vcodec,''), coalesce(acodec,''), asr, ach, abitrate,"
            " image, frames FROM files ORDER BY path", -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            fputs(first ? " {" : ",\n {", f);
            first = false;
            fputs("\"pfad\": ", f);
            json_string(f, col_text(s, 0));
            fprintf(f, ", \"groesse\": %lld, \"geaendert\": %lld, \"inode\": %lld, \"k\": %d,"
                    " \"weiss\": %d, \"status\": %d, \"fehler\": ",
                    (long long)sqlite3_column_int64(s, 1), (long long)sqlite3_column_int64(s, 2),
                    (long long)sqlite3_column_int64(s, 3), sqlite3_column_int(s, 4),
                    sqlite3_column_int(s, 5), sqlite3_column_int(s, 6));
            json_string(f, col_text(s, 7));
            fprintf(f, ", \"dauer\": %.3f, \"breite\": %d, \"hoehe\": %d, \"fps\": %.3f,"
                    " \"bitrate\": %d, \"vcodec\": ", sqlite3_column_double(s, 8),
                    sqlite3_column_int(s, 9), sqlite3_column_int(s, 10),
                    sqlite3_column_double(s, 11), sqlite3_column_int(s, 12));
            json_string(f, col_text(s, 13));
            fputs(", \"acodec\": ", f);
            json_string(f, col_text(s, 14));
            fprintf(f, ", \"asr\": %d, \"ach\": %d, \"abitrate\": %d, \"bild\": %d, \"bilder\": \"",
                    sqlite3_column_int(s, 15), sqlite3_column_int(s, 16), sqlite3_column_int(s, 17),
                    sqlite3_column_int(s, 18));
            int bytes = sqlite3_column_bytes(s, 19);
            const unsigned char *b = sqlite3_column_blob(s, 19);
            if (b && bytes > 0) {
                char *b64 = g_base64_encode(b, (gsize)bytes);
                fputs(b64, f);
                g_free(b64);
            }
            fputs("\"}", f);
        }
    }
    sqlite3_finalize(s);
    df_db_unlock();
    fputs("\n]}\n", f);
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) snprintf(err, errsz, "%s: Schreibfehler", path);
    return ok;
}
