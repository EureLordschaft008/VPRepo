/*
 * Duplikat-Finder – interne Schnittstellen der Engine (nicht für Front-ends)
 */
#ifndef DF_INTERNAL_H
#define DF_INTERNAL_H

#include <glib.h>
#include <sys/types.h>

#include "df_core.h"

/* ---------------------------------------------------------------------------
 * Hilfsprozess (df_proc.c)
 * ------------------------------------------------------------------------- */
typedef struct {
    pid_t pid;
    int in_fd, out_fd;          /* schreiben an / lesen vom Prozess */
    bool gpu;
    char name[96];              /* GPU-Name laut libva */
    char rbuf[65536];
    size_t rpos, rlen;
} DfProc;

typedef enum { DF_R_OK, DF_R_ERR, DF_R_SW, DF_R_DEAD, DF_R_TIMEOUT, DF_R_MEMORY } DfReplyStatus;

typedef struct {
    DfReplyStatus status;
    char *text;                 /* Text der Antwort (key=value bzw. Fehlergrund) */
    unsigned char *bin;
    size_t bin_len;
} DfReply;

/* Hilfsprozess starten (gpu_device NULL = CPU) -> true, wenn bereit */
bool df_proc_start(DfProc *p, const char *gpu_device, int nice_level, bool white_borders);
void df_proc_stop(DfProc *p);
/* Anfrage senden und auf Antwort warten. timeout in s; max_rss_mb: Speichergrenze.
 * Bei DEAD/TIMEOUT/MEMORY ist der Prozess beendet (neu starten!). */
DfReplyStatus df_proc_request(DfProc *p, const char *cmd, const char *path, double timeout,
                              long max_rss_mb, DfReply *r);
void df_reply_free(DfReply *r);
char *df_escape_path(const char *path);
/* Wert aus key=value-Text */
const char *df_kv(const char *text, const char *key, char *buf, size_t sz);
/* S-Antwort -> DfFile (frames: neuer Speicher) */
bool df_parse_scan_reply(const DfReply *r, DfFile *f);

/* ---------------------------------------------------------------------------
 * Datenbank (df_db.c)
 * ------------------------------------------------------------------------- */
typedef struct {
    DfFile f;
    long long inode;
    int k;                      /* mit so vielen Vergleichsbildern gelesen */
    int status;                 /* 0 = ok, 1 = nicht lesbar */
    char *error;
    bool white_borders;
} DfDbEntry;

bool df_db_open(void);
void df_db_close(void);
void df_db_lock(void);
void df_db_unlock(void);
/* alle Einträge -> Tabelle Pfad -> DfDbEntry* (frei mit g_hash_table_destroy) */
GHashTable *df_db_load_all(void);
void df_db_entry_free(gpointer e);
bool df_db_begin(void);
bool df_db_commit(void);
/* Eintrag schreiben (im laufenden Vorgang) -> id */
int64_t df_db_put(const DfDbEntry *e);
/* Ton-Fingerabdruck */
bool df_db_audio_get(int64_t id, long long size, long long mtime, uint32_t **fp, int *n,
                     int *status);
bool df_db_audio_put(int64_t id, long long size, long long mtime, const uint32_t *fp, int n,
                     int status, const char *error);
GHashTable *df_db_notmatch_load(void);          /* Schlüssel: Paar (a<<32|b) als gint64* */
GHashTable *df_db_ignored_load(void);           /* Pfade */
int64_t df_pair_key(int64_t a, int64_t b);

/* ---------------------------------------------------------------------------
 * Vergleich (df_scan.c)
 * ------------------------------------------------------------------------- */
int df_compare(const DfOptions *o, DfFile *files, int n, const bool *is_new, DfResult *res,
               DfStats *st);

/* ---------------------------------------------------------------------------
 * Hilfen (df_util.c)
 * ------------------------------------------------------------------------- */
double df_now(void);                            /* monotone Zeit in s */
void df_file_clear(DfFile *f);
void df_file_copy(DfFile *dst, const DfFile *src);
bool df_is_video_ext(const char *path);
bool df_is_image_ext(const char *path);
bool df_glob_or_contains(const char *path, const char *pattern);
long df_mem_available_mb(void);
bool df_read_cpu_times(unsigned long long *busy, unsigned long long *total);
long df_proc_rss_mb(pid_t pid);
double df_proc_cpu_secs(pid_t pid);

#endif
