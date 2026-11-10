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
/* Farbumrechnung fürs Bild in der Grafikkarte (VA-API) statt im Prozessor; gilt ab dem nächsten
 * Öffnen des Players. possible: gibt es das hier überhaupt? */
void md_player_set_gpu_convert(bool on);
bool md_player_gpu_convert_possible(void);
/* Erkannte Intros von selbst überspringen statt nur den Knopf zu zeigen */
void md_player_set_auto_skip_intro(bool on);
/* Tasten des Players, je Zeile "Taste\tWirkung"; gilt bis zum nächsten Aufruf */
const char *md_player_keys_text(void);
/* Tastenbelegung: Aktionen 0 … count−1 mit Namen für die Einstellungsdatei (id), Wirkung,
 * gewählter und Standardtaste. find: Aktion mit dieser Taste oder −1. label: Name der Taste zum
 * Anzeigen (g_free). set: neue Taste, gilt sofort; die Standardtaste setzt zurück. */
int md_player_key_count(void);
const char *md_player_key_id(int action);
const char *md_player_key_action(int action);
guint md_player_key_get(int action);
guint md_player_key_default(int action);
int md_player_key_find(guint key);
char *md_player_key_label(guint key);
void md_player_key_set(int action, guint key);

#endif
