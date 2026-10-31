/*
 * Duplikat-Finder – gemeinsame Definitionen von Engine und Hilfsprozess
 */
#ifndef DF_PROTO_H
#define DF_PROTO_H

#include <stdint.h>

#define DF_PROTO_VERSION 1
#define DF_MAX_FRAMES 16        /* höchstens so viele Vergleichsbilder je Video */
#define DF_FLAT_CONTRAST 7      /* Standardabweichung (0–255) darunter: einfarbiges Bild */

enum {
    DF_FRAME_OK = 1,            /* Bild konnte gelesen werden */
    DF_FRAME_FLAT = 2,          /* einfarbig (Schwarzbild, Abblende): zählt beim Vergleich nicht */
};

/* ein Vergleichsbild (so auch in der Datenbank gespeichert, 20 Byte) */
typedef struct __attribute__((packed)) {
    uint64_t hash;              /* pHash (DCT 32x32 -> 8x8) */
    uint64_t hash_flip;         /* pHash des waagerecht gespiegelten Bilds */
    uint8_t mean;               /* mittlere Helligkeit */
    uint8_t contrast;           /* Standardabweichung der Helligkeit */
    uint8_t flags;              /* DF_FRAME_* */
    uint8_t reserved;
} DfFrame;

#endif
