/* SPDX-License-Identifier: BSD-3-Clause. Source depth/flag/9-bit/wrap/peek vectors. */
#include "fifo7200.h"
#include <assert.h>
#include <stdio.h>
struct Sink { int pins[3]; unsigned changes[3]; };
static void flag(void *opaque, unsigned pin, int level) {
    struct Sink *s = opaque; assert(pin < 3); s->pins[pin] = level; s->changes[pin]++;
}
int main(void) {
    for (unsigned depth = 256; depth <= 1024; depth *= 2) {
        struct Sink sink = {0}; Fifo7200 *f = 0; Fifo7200Status st;
        Fifo7200Config config = {depth, &sink, flag}; assert(fifo7200_create(&config, &f) == 0);
        assert(sink.pins[0] == 0 && sink.pins[1] == 1 && sink.pins[2] == 1);
        assert(fifo7200_read(f, 0) == 0x1ff); fifo7200_get_status(f, &st); assert(!st.underflow);
        assert(fifo7200_read(f, 1) == 0x1ff); fifo7200_get_status(f, &st); assert(st.underflow == 1);
        for (unsigned i = 0; i < depth; ++i) {
            fifo7200_write(f, i | 0x8000); fifo7200_get_status(f, &st);
            assert(st.used == i + 1 && st.ef == 1 && st.ff == (i + 1 < depth));
            assert(st.hf == (i + 1 < depth / 2));
        }
        assert(fifo7200_read(f, 0) == 0); fifo7200_write(f, 0x321);
        fifo7200_get_status(f, &st); assert(st.overflow == 1 && st.used == depth);
        for (unsigned i = 0; i < depth; ++i) assert(fifo7200_read(f, 1) == (i & 0x1ff));
        fifo7200_get_status(f, &st); assert(!st.used && st.ef == 0 && st.ff == 1 && st.hf == 1);
        // Multiple wraps with interleaved producer/consumer and byte-mode mask.
        for (unsigned i = 0; i < 3 * depth; ++i) {
            fifo7200_write(f, 0x255); assert((uint8_t)fifo7200_read(f, 1) == 0x55);
        }
        fifo7200_write(f, 1); fifo7200_reset(f); fifo7200_get_status(f, &st);
        assert(!st.used && st.ef == 0 && st.ff == 1 && st.hf == 1);
        assert(st.overflow == 1 && st.underflow == 1); fifo7200_destroy(f);
    }
    puts("IDT7200/7201/7202 source active-low flags, 9-bit mask, order, full/empty, wrap, peek, rejected overflow and reset PASS");
}
