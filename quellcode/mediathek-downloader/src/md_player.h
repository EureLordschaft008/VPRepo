/*
 * Mediathek Downloader – eingebauter Player (md_player.c)
 * =======================================================
 * Eine Seite mit Bild, Zeitleiste, Ton- und Untertitelwahl auf Grundlage von GStreamer, im
 * Stack des Hauptfensters. Es gibt höchstens einen Player; ein weiterer Aufruf spielt dort die
 * neue Datei. Wechselt man bei laufendem Video die Seite, spielt er in einem kleinen Fenster
 * weiter (Bild im Bild).
 * Alle Funktionen laufen im GTK-Hauptthread.
 */
#ifndef MD_PLAYER_H
#define MD_PLAYER_H

#include <gtk/gtk.h>
#include <stdbool.h>

/* Stand der Wiedergabe von path: alle paar Sekunden und beim Verlassen der Datei.
 * left: Datei wird verlassen (Fenster zu oder nächste Datei). Sekunden; duration 0 = unbekannt */
typedef void (*MdPlayerProgress)(const char *path, double position, double duration, bool left,
                                 gpointer data);
/* Datei nach path -> neu angelegter Pfad (und *title) oder NULL */
typedef char *(*MdPlayerNext)(const char *path, char **title, gpointer data);

typedef struct {
    MdPlayerProgress progress;
    MdPlayerNext next;          /* darf NULL sein */
    void (*closed)(gpointer data);  /* Player geht zu (Esc, Zurück); darf NULL sein */
    gpointer data;
} MdPlayerHooks;

/* Lässt sich abspielen? Sonst *why (g_free) mit dem fehlenden Teil */
bool md_player_available(char **why);
/* Datei im offenen Player; NULL = keiner offen */
const char *md_player_current(void);
/* path ab start Sekunden abspielen, als Seite "player" in stack; false = nicht verfügbar */
bool md_player_play(GtkStack *stack, const char *path, const char *title, double start,
                    const MdPlayerHooks *hooks);
/* Seite verlassen: läuft das Video, spielt es in einem kleinen Fenster weiter (true);
 * false = es läuft nichts, der Player sollte zugehen */
bool md_player_pip(void);
/* Player schließen und die Seite entfernen; ruft vorher hooks.closed */
void md_player_close(void);
/* Tasten des Players, je Zeile "Taste\tWirkung" */
const char *md_player_keys_text(void);

#endif
