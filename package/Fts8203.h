// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef FTS8203_H
#define FTS8203_H

#include <stddef.h>
#include <stdint.h>

#define FTS8203_CONTACTS 10
#define FTS8203_CHUNK 4096
#define FTS8203_WIRE_BYTES (FTS8203_CHUNK + 9)
#define FTS8203_FRAME_EMPTY (-11)
#define FTS8203_FRAME_BOOT (-19)

/* Fts8203IO supplies one complete, manually selected SPI transaction and reset timing. */
struct Fts8203IO {
    void *context;
    int (*transfer)(void *, const uint8_t *, uint8_t *, size_t);
    void (*delay_us)(void *, unsigned);
    void (*reset)(void *, int);
    void (*stage)(void *, const char *, unsigned, int);
};

/* Fts8203Frame contains the complete active contact set indexed by hardware contact ID. */
struct Fts8203Frame {
    uint16_t active;
    uint16_t down;
    uint16_t up;
    uint16_t x[FTS8203_CONTACTS];
    uint16_t y[FTS8203_CONTACTS];
};

/* Fts8203 owns bounded wire buffers; its caller serializes all transactions. */
struct Fts8203 {
    struct Fts8203IO io;
    uint8_t tx[FTS8203_WIRE_BYTES];
    uint8_t rx[FTS8203_WIRE_BYTES];
    uint8_t rom[2];
    uint8_t app[2];
    uint8_t version;
    uint8_t last_status;
    uint8_t frame_head[16];
};

int fts8203_start(struct Fts8203 *, const uint8_t *, size_t);
int fts8203_frame(struct Fts8203 *, struct Fts8203Frame *);
int fts8203_read(struct Fts8203 *, uint8_t, uint8_t *, size_t);
int fts8203_sleep(struct Fts8203 *);
/* fts8203_state returns 1 only for confirmed ROM 8201 with boot state 1, 0 for a supported app. */
int fts8203_state(struct Fts8203 *, int probe_boot);

#endif
