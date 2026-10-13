#include "../libpcsxcore/arcade/sh2.h"
#include <cassert>
#include <cstring>
#include <cstdio>
static uint8_t ram[4096];
static uint32_t read(void *, uint32_t a, unsigned width) {
    assert(a + width <= sizeof(ram));
    uint32_t v = 0;
    for (unsigned i = 0; i < width; i++) v = (v << 8) | ram[a + i];
    return v;
}
static void write(void *, uint32_t a, unsigned width, uint32_t value) {
    assert(a + width <= sizeof(ram));
    for (unsigned i = 0; i < width; i++) ram[a + i] = value >> ((width - 1 - i) * 8);
}
int main() {
    write(nullptr, 0, 4, 0x100); write(nullptr, 4, 4, 0xff0);
    const uint16_t program[] = {
        0xe0ff, /* mov #-1,r0 */ 0x7002, /* add #2,r0 */
        0xe205, /* mov #5,r2 */ 0xa002, /* bra 010e */
        0x7203, /* delay: add #3,r2 */ 0xe200, /* skipped */
        0xe301, /* skipped */ 0x323c, /* add r3,r2 */
        0x0009, 0x001b /* nop; sleep */
    };
    for (unsigned i = 0; i < sizeof(program) / 2; i++) write(nullptr, 0x100 + i * 2, 2, program[i]);
    Sh2Config cfg = {nullptr, read, write, nullptr};
    auto *cpu = sh2_create(&cfg); assert(cpu);
    sh2_run_until(cpu, 20);
    Sh2State s; sh2_get_state(cpu, &s);
    assert(s.r[0] == 1 && s.r[2] == 8 && s.r[3] == 0 && s.sleep_mode == 1);
    assert(s.illegal_opcode == 0);
    /* Interrupt entry uses a big-endian vector and stacks PC and SR. */
    s.sr = 0; sh2_set_state(cpu, &s);
    write(nullptr, 40 * 4, 4, 0x200); write(nullptr, 0x200, 2, 0xe477);
    sh2_set_irq(cpu, 40, 8); sh2_run_until(cpu, 26); sh2_get_state(cpu, &s);
    assert(s.r[15] == 0xfe8 && ((s.sr >> 4) & 15) == 8);
    assert(read(nullptr, 0xfec, 4) == 0);
    sh2_set_irq(cpu,42,8); sh2_set_irq(cpu,40,8); sh2_set_irq(cpu,44,9);
    sh2_get_state(cpu,&s); assert(s.irq_vector==44 && s.irq_level==9);
    sh2_set_irq(cpu,44,0); sh2_get_state(cpu,&s);
    assert(s.irq_vector==40 && s.pending_irq[42]==8);
    /* A state roundtrip keeps masked requests until a lower SR permits them. */
    sh2_reset(cpu,0); sh2_set_state(cpu,&s); sh2_get_state(cpu,&s);
    assert(s.pending_irq[40]==8 && s.pending_irq[42]==8);
    sh2_set_irq(cpu,40,0); sh2_get_state(cpu,&s); assert(s.irq_vector==42);
    sh2_destroy(cpu);
    puts("SH-2 reset vectors, signed immediates, branch delay, sleep and interrupt entry PASS");
}
