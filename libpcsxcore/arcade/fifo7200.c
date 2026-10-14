/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright hap (upstream does not specify years).
 * Source: MAME deb2f54fe7300e7a192690318beb860159bfd52a,
 * src/devices/machine/7200fifo.cpp, ported without the device framework.
 * Source TODOs: RT retransmit pin and cascaded width expansion. */
#include "fifo7200.h"
#include <stdlib.h>
#include <string.h>
struct Fifo7200 {
    Fifo7200Config config;
    uint16_t *buffer;
    unsigned read_ptr, write_ptr;
    int empty, full, half;
    uint64_t writes, reads, overflow, underflow;
};
static unsigned used(const Fifo7200 *f) {
    if (f->full) return f->config.depth;
    return f->write_ptr >= f->read_ptr ? f->write_ptr - f->read_ptr :
        f->config.depth + f->write_ptr - f->read_ptr;
}
static void flag(Fifo7200 *f, unsigned pin, int level) {
    if (f->config.flag) f->config.flag(f->config.opaque, pin, level);
}
int fifo7200_create(const Fifo7200Config *config, Fifo7200 **out) {
    if (!out || !config || (config->depth != 256 && config->depth != 512 && config->depth != 1024)) return -1;
    *out = NULL;
    Fifo7200 *f = calloc(1, sizeof(*f));
    if (!f) return -1;
    f->config = *config; f->buffer = calloc(config->depth, sizeof(*f->buffer));
    if (!f->buffer) { free(f); return -1; }
    fifo7200_reset(f); *out = f; return 0;
}
void fifo7200_destroy(Fifo7200 *f) { if (f) { free(f->buffer); free(f); } }
void fifo7200_reset(Fifo7200 *f) {
    if (!f) return;
    memset(f->buffer, 0, f->config.depth * sizeof(*f->buffer));
    f->read_ptr = f->write_ptr = 0; f->empty = 1; f->full = f->half = 0;
    flag(f, FIFO7200_EF, 0); flag(f, FIFO7200_FF, 1); flag(f, FIFO7200_HF, 1);
}
void fifo7200_write(Fifo7200 *f, uint16_t data) {
    if (!f) return;
    if (f->full) { f->overflow++; return; }
    f->buffer[f->write_ptr] = data & 0x1ff;
    f->write_ptr = (f->write_ptr + 1) % f->config.depth; f->writes++;
    if (f->empty) { f->empty = 0; flag(f, FIFO7200_EF, 1); }
    else if (f->read_ptr == f->write_ptr) { f->full = 1; flag(f, FIFO7200_FF, 0); }
    else if (!f->half && used(f) >= f->config.depth / 2) { f->half = 1; flag(f, FIFO7200_HF, 0); }
}
uint16_t fifo7200_read(Fifo7200 *f, int side_effects) {
    if (!f) return 0x1ff;
    if (f->empty) { if (side_effects) f->underflow++; return 0x1ff; }
    uint16_t ret = f->buffer[f->read_ptr];
    if (side_effects) {
        f->read_ptr = (f->read_ptr + 1) % f->config.depth; f->reads++;
        if (f->full) { f->full = 0; flag(f, FIFO7200_FF, 1); }
        else if (f->read_ptr == f->write_ptr) { f->empty = 1; flag(f, FIFO7200_EF, 0); }
        else if (f->half && used(f) < f->config.depth / 2) { f->half = 0; flag(f, FIFO7200_HF, 1); }
    }
    return ret;
}
int fifo7200_ef(const Fifo7200 *f) { return f ? !f->empty : 0; }
void fifo7200_get_status(const Fifo7200 *f, Fifo7200Status *s) {
    if (!f || !s) return;
    *s = (Fifo7200Status){f->writes, f->reads, f->overflow, f->underflow,
        f->config.depth, used(f), !f->empty, !f->full, !f->half};
}
