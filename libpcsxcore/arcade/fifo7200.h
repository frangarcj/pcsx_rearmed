/* SPDX-License-Identifier: BSD-3-Clause. MAME 7200 FIFO port; see fifo7200_LICENSE.txt. */
#ifndef PCSX_FIFO7200_H
#define PCSX_FIFO7200_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Fifo7200 Fifo7200;
enum { FIFO7200_EF, FIFO7200_FF, FIFO7200_HF };
typedef struct Fifo7200Config {
    unsigned depth; /* source IDT7200/7201/7202: 256/512/1024 words of 9 bits */
    void *opaque;
    void (*flag)(void *, unsigned pin, int level); /* source active-low outputs */
} Fifo7200Config;
typedef struct Fifo7200Status {
    uint64_t writes, reads, overflow, underflow;
    unsigned depth, used;
    uint8_t ef, ff, hf; /* pin levels, not logical empty/full predicates */
} Fifo7200Status;
int fifo7200_create(const Fifo7200Config *, Fifo7200 **);
/* Do not destroy a FIFO from one of its synchronous flag callbacks. */
void fifo7200_destroy(Fifo7200 *);
void fifo7200_reset(Fifo7200 *);
void fifo7200_write(Fifo7200 *, uint16_t word);
/* side_effects=0 is the source debugger peek, including suppressed underflow. */
uint16_t fifo7200_read(Fifo7200 *, int side_effects);
void fifo7200_get_status(const Fifo7200 *, Fifo7200Status *);
int fifo7200_ef(const Fifo7200 *);
#ifdef __cplusplus
}
#endif
#endif
