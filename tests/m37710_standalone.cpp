// Synthetic M37710 instruction and peripheral programs. No game ROMs or BIOS keys.
#include "m37710.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>
struct Bus {
    std::vector<uint8_t> mem;
    unsigned reads16 = 0, writes16 = 0, acks = 0, last_irq = 0, diagnostics = 0, analog_calls = 0;
    unsigned port = 0, port_mask = 0, port_value = 0;
    Bus() : mem(1 << 24, 0xea) {}
    static uint8_t r8(void *p, uint32_t a) { return ((Bus *)p)->mem[a & 0xffffff]; }
    static void w8(void *p, uint32_t a, uint8_t v) { ((Bus *)p)->mem[a & 0xffffff] = v; }
    static uint16_t r16(void *p, uint32_t a) {
        Bus &b = *(Bus *)p;
        b.reads16++;
        return r8(p, a) | (r8(p, a + 1) << 8);
    }
    static void w16(void *p, uint32_t a, uint16_t v) {
        Bus &b = *(Bus *)p;
        b.writes16++;
        w8(p, a, v);
        w8(p, a + 1, v >> 8);
    }
    static uint8_t port_r(void *, unsigned p, uint8_t mask) { return uint8_t((0xa5 ^ p) & mask); }
    static void port_w(void *p, unsigned port, uint8_t v, uint8_t mask) {
        Bus &b = *(Bus *)p;
        b.port = port;
        b.port_mask = mask;
        b.port_value = v;
    }
    static uint16_t analog(void *p, unsigned c) {
        ((Bus *)p)->analog_calls++;
        return 0x120 + c;
    }
    static void ack(void *p, unsigned line) {
        Bus &b = *(Bus *)p;
        b.acks++;
        b.last_irq = line;
    }
    static void diag(void *p, unsigned, uint32_t, uint32_t) { ((Bus *)p)->diagnostics++; }
    void put(uint32_t a, std::initializer_list<uint8_t> bytes) {
        for (auto v : bytes)
            mem[a++] = v;
    }
    void word(uint32_t a, uint16_t v) {
        mem[a] = v;
        mem[a + 1] = v >> 8;
    }
    M37710Config config(unsigned model = M37710_MODEL_M37702S1) {
        M37710Config c = {};
        c.model = model;
        c.opaque = this;
        c.read8 = r8;
        c.write8 = w8;
        c.read16 = r16;
        c.write16 = w16;
        c.port_read = port_r;
        c.port_write = port_w;
        c.analog_read = analog;
        c.irq_ack = ack;
        c.diagnostic = diag;
        return c;
    }
    M37710 *cpu() {
        word(0xfffe, 0x1000);
        auto c = config();
        M37710 *cpu = m37710_create(&c);
        assert(cpu);
        return cpu;
    }
};
static M37710Status status(M37710 *c) {
    M37710Status s;
    m37710_get_status(c, &s);
    return s;
}
static void run(M37710 *c, uint64_t n = 1000) {
    m37710_run_cycles(c, n);
    assert(!status(c).fault);
}
static void widths() {
    Bus b;
    auto c = b.cpu();
    b.put(0x1000,
          {0xa9, 0x34, 0x12, 0x42, 0xa9, 0x78, 0x56, 0xa2, 0xbc, 0x9a, 0xa0, 0xf0, 0xde, 0xe2,
           0x30, 0xa9, 0x11, 0x42, 0xa9, 0x22, 0xa2, 0x33, 0xa0, 0x44, 0xc2, 0x30, 0xdb});
    run(c);
    auto s = status(c);
    assert(s.a == 0x1211 && s.b == 0x5622 && s.x == 0x9a33 && s.y == 0xde44 && !(s.ps & 0x30));
    assert(!s.illegal_opcodes);
    m37710_destroy(c);
    // Every width combination restores preserved high bytes, including SEM/CLM.
    for (unsigned mode = 0; mode < 4; mode++) {
        Bus t;
        auto d = t.cpu();
        m37710_set_register(d, M37710_PS, 4 | ((mode & 2) ? 0x20 : 0) | ((mode & 1) ? 0x10 : 0));
        m37710_set_register(d, M37710_A, 0xabcd);
        m37710_set_register(d, M37710_B, 0x1234);
        m37710_set_register(d, M37710_X, 0x5678);
        m37710_set_register(d, M37710_Y, 0x9abc);
        assert(m37710_get_register(d, M37710_A) == 0xabcd &&
               m37710_get_register(d, M37710_B) == 0x1234);
        assert(m37710_get_register(d, M37710_X) == 0x5678 &&
               m37710_get_register(d, M37710_Y) == 0x9abc);
        m37710_set_register(d, M37710_PS, 4);
        assert(status(d).a == 0xabcd && status(d).x == 0x5678);
        m37710_destroy(d);
    }
    std::puts("M37710 four widths / independent A-B / preserved X-Y high bytes passed");
}
static void arithmetic() {
    Bus b;
    auto c = b.cpu();
    b.put(0x1000, {0xa9, 0xff, 0x7f, 0x18, 0x69, 1,    0,    0x8d, 0, 0x40, 0x08, 0x68, 0xf8, 0xa9,
                   0x99, 0xc2, 8,    0xe2, 8,    0x18, 0x69, 1,    0, 0xd8, 0xc2, 8,    0xdb});
    // Store signed-overflow result, then exercise 8-bit BCD 99+1 =>00 with carry.
    run(c);
    assert(b.mem[0x4000] == 0 && b.mem[0x4001] == 0x80);
    assert((status(c).a & 255) == 0);
    assert(status(c).ps & 1);
    m37710_destroy(c);
    for (unsigned m = 0; m < 2; m++) {
        Bus t;
        auto d = t.cpu();
        m37710_set_register(d, M37710_PS, 4 | (m ? 0x20 : 0));
        if (m)
            t.put(0x1000, {0xa9, 0x12, 0x89, 0x09, 0x10, 0x89, 0x29, 0x0b, 0xdb});
        else
            t.put(0x1000, {0xa9, 0x34, 0x12, 0x89, 0x09, 0x78, 0x56, 0x89, 0x29, 0x21, 0x43, 0xdb});
        uint32_t product = m ? 0x12 * 0x10 : 0x1234 * 0x5678, divisor = m ? 0x0b : 0x4321;
        run(d);
        auto s = status(d);
        assert(s.a == product / divisor && s.b == product % divisor);
        m37710_destroy(d);
    }
    std::puts("M37710 binary / decimal / prefix multiply-divide vectors passed");
}
struct AluVector {
    unsigned mode, prefix, opcode, lhs, rhs, ps, result, flags, cycles;
};
#include "m37710_vectors.inc"
static void fixed_vectors() {
    Bus b;
    auto c = b.cpu();
    for (const auto &v : alu_vectors) {
        m37710_reset(c);
        m37710_set_register(c, M37710_PS, v.ps);
        m37710_set_register(c, M37710_A, v.prefix ? 0x5aa5 : v.lhs);
        m37710_set_register(c, M37710_B, v.prefix ? v.lhs : 0xa55a);
        unsigned p = 0x1000;
        if (v.prefix)
            b.mem[p++] = v.prefix;
        b.mem[p++] = v.opcode;
        b.mem[p++] = v.rhs;
        if (!(v.mode & 2))
            b.mem[p++] = v.rhs >> 8;
        auto before = status(c);
        auto cycles = m37710_run_cycles(c, 1);
        auto after = status(c);
        unsigned result = v.prefix ? after.b : after.a;
        if (result != v.result || (after.ps & 0xc3) != v.flags) {
            std::fprintf(
                stderr, "ALU mode%u prefix%x op%x %x,%x got %x flags%x wanted %x flags%x\n", v.mode,
                v.prefix, v.opcode, v.lhs, v.rhs, result, after.ps, v.result, v.flags);
            assert(false);
        }
        assert(cycles == v.cycles + (v.prefix ? 2 : 0) &&
               after.instructions == before.instructions + 1);
        assert(v.prefix ? after.a == 0x5aa5 : after.b == 0xa55a);
        assert(!after.fault);
    }
    m37710_destroy(c);
    std::printf("M37710 %zu independent binary ALU / timing fixed vectors passed\n",
                sizeof(alu_vectors) / sizeof(alu_vectors[0]));
}
static void addressing() {
    struct AddressCase {
        uint8_t load;
        unsigned size;
        uint32_t operand, target, pointer;
    };
    // DPR=0x80, X=7,Y=9,DT=0x12,S=0x120; all supported LDA addressing families.
    const AddressCase cases[] = {
        {0xa1, 1, 0x10, 0x124000, 0x97},  {0xa3, 1, 0x10, 0x130, 0},
        {0xa5, 1, 0x10, 0x90, 0},         {0xa7, 1, 0x10, 0x134000, 0x90},
        {0xad, 2, 0x4000, 0x124000, 0},   {0xaf, 3, 0x134000, 0x134000, 0},
        {0xb1, 1, 0x10, 0x124009, 0x90},  {0xb2, 1, 0x10, 0x124000, 0x90},
        {0xb3, 1, 0x10, 0x124009, 0x130}, {0xb5, 1, 0x10, 0x97, 0},
        {0xb7, 1, 0x10, 0x134009, 0x90},  {0xb9, 2, 0x4000, 0x124009, 0},
        {0xbd, 2, 0x4000, 0x124007, 0},   {0xbf, 3, 0x134000, 0x134007, 0}};
    Bus b;
    auto c = b.cpu();
    for (unsigned mode = 0; mode < 4; mode++)
        for (unsigned acc = 0; acc < 2; acc++)
            for (const auto &a : cases) {
                m37710_reset(c);
                m37710_set_register(c, M37710_PS,
                                    4 | ((mode & 2) ? 32 : 0) | ((mode & 1) ? 16 : 0));
                m37710_set_register(c, M37710_A, 0);
                m37710_set_register(c, M37710_B, 0);
                m37710_set_register(c, M37710_DPR, 0x80);
                m37710_set_register(c, M37710_DT, 0x12);
                m37710_set_register(c, M37710_X, 7);
                m37710_set_register(c, M37710_Y, 9);
                m37710_set_register(c, M37710_S, 0x120);
                if (a.pointer) {
                    m37710_write16(
                        c, a.pointer,
                        a.target - (a.load == 0xb1 || a.load == 0xb3 || a.load == 0xb7 ? 9 : 0));
                    m37710_write8(c, a.pointer + 2, a.target >> 16);
                }
                m37710_write16(c, a.target, 0xbeef);
                unsigned p = 0x1000;
                if (acc)
                    b.mem[p++] = 0x42;
                b.mem[p++] = a.load;
                for (unsigned j = 0; j < a.size; j++)
                    b.mem[p++] = a.operand >> (8 * j);
                m37710_run_cycles(c, 1);
                auto s = status(c);
                assert(!s.fault);
                if ((acc ? s.b : s.a) != ((mode & 2) ? 0xef : 0xbeef))
                    std::fprintf(stderr, "Address load op%x mode%u acc%u got%x target%x ptr%x\n",
                                 a.load, mode, acc, acc ? s.b : s.a, a.target, a.pointer);
                assert((acc ? s.b : s.a) == ((mode & 2) ? 0xef : 0xbeef));
                // Matching STA encoding is 0x20 lower than LDA, with the same addressing family.
                m37710_reset(c);
                m37710_set_register(c, M37710_PS,
                                    4 | ((mode & 2) ? 32 : 0) | ((mode & 1) ? 16 : 0));
                m37710_set_register(c, M37710_A, 0);
                m37710_set_register(c, M37710_B, 0);
                m37710_set_register(c, M37710_DPR, 0x80);
                m37710_set_register(c, M37710_DT, 0x12);
                m37710_set_register(c, M37710_X, 7);
                m37710_set_register(c, M37710_Y, 9);
                m37710_set_register(c, M37710_S, 0x120);
                if (a.pointer) {
                    m37710_write16(
                        c, a.pointer,
                        a.target - (a.load == 0xb1 || a.load == 0xb3 || a.load == 0xb7 ? 9 : 0));
                    m37710_write8(c, a.pointer + 2, a.target >> 16);
                }
                m37710_write16(c, a.target, 0xbeef);
                m37710_set_register(c, acc ? M37710_B : M37710_A, 0x5678);
                p = 0x1000;
                if (acc)
                    b.mem[p++] = 0x42;
                b.mem[p++] = a.load - 0x20;
                for (unsigned j = 0; j < a.size; j++)
                    b.mem[p++] = a.operand >> (8 * j);
                m37710_run_cycles(c, 1);
                assert(!status(c).fault);
                assert(m37710_read16(c, a.target) == ((mode & 2) ? 0xbe78 : 0x5678));
            }
    m37710_destroy(c);
    std::puts("M37710 224 A-B loads and stores across 14 addressing modes and four widths passed");
}
static void calls_bus() {
    Bus b;
    auto c = b.cpu();
    m37710_set_register(c, M37710_S, 0x27f);
    b.put(0x1000, {0x20, 0x20, 0x10, 0x22, 0x20, 0x10, 2, 0xdb});
    b.put(0x1020, {0xa9, 0x11, 0x22, 0x60});
    b.put(0x021020, {0x42, 0xa9, 0x33, 0x44, 0x6b});
    run(c);
    auto s = status(c);
    assert(s.a == 0x2211 && s.b == 0x4433 && s.s == 0x27f && s.pg == 0);
    m37710_destroy(c);
    Bus t;
    auto d = t.cpu();
    t.word(0x124001, 0xbeef);
    t.word(0x130004, 0x1234);
    t.put(0x1000, {0x89, 0xc2, 0x12, 0xad, 1, 0x40, 0x8d, 3, 0x40, 0xaf, 4, 0, 0x13, 0x8f, 6, 0,
                   0x13, 0xdb});
    run(d);
    assert(t.mem[0x124003] == 0xef && t.mem[0x124004] == 0xbe);
    assert(t.mem[0x130006] == 0x34 && t.mem[0x130007] == 0x12);
    assert(t.writes16 == 1 && t.reads16 >= 2 && status(d).dt == 0x12);
    m37710_destroy(d);
    // Sequential fetch crossing a bank and negative/positive branch within a bank.
    Bus z;
    auto e = z.cpu();
    m37710_set_register(e, M37710_PG, 1);
    m37710_set_register(e, M37710_PC, 0xffff);
    z.mem[0x1ffff] = 0xea;
    z.put(0x20000, {0x80, 2, 0xea, 0xea, 0xa9, 0x34, 0x12, 0xdb});
    run(e);
    assert(status(e).pg == 2 && status(e).a == 0x1234);
    m37710_destroy(e);
    std::puts("M37710 JSR-JSL / RTS-RTL / LE atomic bus / 24-bit banks passed");
}
static void interrupts() {
    Bus b;
    auto c = b.cpu();
    m37710_set_register(c, M37710_S, 0x27f);
    b.put(0x1000, {0x58, 0xcb, 0xa9, 0x34, 0x12, 0xdb});
    b.put(0x1100, {0x42, 0xa9, 0x78, 0x56, 0x40});
    b.word(0xfff2, 0x1100);
    m37710_write8(c, 0x7e, 3);
    run(c, 30);
    assert(status(c).stopped == 1);
    m37710_irq(c, M37710_LINE_IRQ1, 1);
    run(c, 100);
    auto s = status(c);
    assert(s.a == 0x1234 && s.b == 0x5678 && s.s == 0x27f && s.interrupts == 1 && s.ps >> 8 == 0);
    assert(b.last_irq == M37710_LINE_IRQ1 && !(s.pending_irqs & (1 << M37710_LINE_IRQ1)));
    m37710_destroy(c);
    Bus t;
    auto d = t.cpu();
    m37710_set_register(d, M37710_S, 0x27f);
    m37710_set_register(d, M37710_PS, 0x200);
    t.word(0xfff4, 0x1100);
    t.word(0xfff2, 0x1200);
    t.put(0x1100, {0xdb});
    t.put(0x1200, {0xdb});
    m37710_write8(d, 0x7d, 2);
    m37710_write8(d, 0x7e, 5);
    m37710_irq(d, M37710_LINE_IRQ0, 1);
    m37710_irq(d, M37710_LINE_IRQ1, 1);
    run(d, 30);
    assert(t.last_irq == M37710_LINE_IRQ1 && status(d).ps >> 8 == 5);
    assert(status(d).pending_irqs & (1 << M37710_LINE_IRQ0));
    m37710_destroy(d);
    Bus z;
    auto e = z.cpu();
    m37710_set_register(e, M37710_S, 0x27f);
    z.word(0xfffc, 0x1100);
    z.put(0x1000, {0x89, 0x29, 0, 0, 0xdb});
    z.put(0x1100, {0xa9, 0x78, 0x56, 0xdb});
    run(e);
    assert(status(e).a == 0x5678 && status(e).s == 0x27a);
    m37710_destroy(e);
    std::puts("M37710 interrupt vectors / priority / WAI / RTI / zero-divide passed");
}
static void peripherals() {
    Bus b;
    auto c = b.cpu();
    b.put(0x1000, {0xcb});
    m37710_write8(c, 4, 0xf0);
    m37710_write8(c, 2, 0x3c);
    assert(m37710_read8(c, 2) == 0x35);
    assert(b.port == 0 && b.port_mask == 0xf0 && b.port_value == 0x30);
    m37710_write8(c, 0x12, 0x22);
    assert(m37710_read8(c, 0x12) == (0xa5 ^ 8));
    m37710_write8(c, 0x13, 0x77);
    assert(m37710_read8(c, 0x13) == 0); // P9 is absent on these models.
    m37710_write8(c, 0x1e, 0x40 | 3);
    run(c, 227);
    assert(m37710_read8(c, 0x1e) & 64);
    run(c, 1);
    assert(!(m37710_read8(c, 0x1e) & 64));
    assert(m37710_read16(c, 0x26) == 0x123);
    assert(status(c).pending_irqs & (1 << M37710_LINE_ADC));
    m37710_write16(c, 0x46, 9);
    m37710_write8(c, 0x56, 0x40);
    m37710_write8(c, 0x40, 1);
    run(c, 79);
    assert(status(c).timer_ticks[0] == 0);
    run(c, 1);
    assert(status(c).timer_ticks[0] == 1);
    run(c, 800);
    assert(status(c).timer_ticks[0] == 11);
    m37710_write8(c, 0x40, 0);
    run(c, 1000);
    assert(status(c).timer_ticks[0] == 11);
    m37710_write8(c, 0x56, 1);
    m37710_write16(c, 0x46, 100);
    m37710_write8(c, 0x40, 1);
    m37710_timer_input(c, 0, 0);
    assert(m37710_read16(c, 0x46) == 100);
    m37710_timer_input(c, 0, 1);
    assert(m37710_read16(c, 0x46) == 99);
    m37710_write8(c, 0x44, 1);
    m37710_timer_input(c, 0, 1);
    assert(m37710_read16(c, 0x46) == 100);
    m37710_write8(c, 0x34, 0);
    assert(m37710_read8(c, 0x34) & 8);
    m37710_write8(c, 0x35, 0xff);
    assert(m37710_read8(c, 0x35) == 7);
    // Real C76 BIOS startup reloads the watchdog and clears one-shot start.
    // Neither register should diagnose an unsupported peripheral: upstream
    // ignores the reload and only stores one-shot start. Counter operation
    // above must remain unchanged, and write-only registers read as zero.
    const auto before = status(c);
    m37710_write8(c, 0x60, 0x00);
    m37710_write8(c, 0x60, 0xff);
    m37710_write8(c, 0x42, 0x00);
    m37710_write8(c, 0x42, 0x1f);
    assert(status(c).unsupported_accesses == before.unsupported_accesses);
    assert(m37710_read8(c, 0x60) == 0 && m37710_read8(c, 0x42) == 0);
    assert(status(c).timer_ticks[0] == before.timer_ticks[0]);
    m37710_reset(c);
    assert(m37710_read8(c, 4) == 0 && m37710_read8(c, 0x40) == 0 && m37710_read16(c, 0x46) == 100);
    m37710_destroy(c);
    Bus t;
    auto d = t.cpu();
    t.put(0x1000, {0x58, 0xcb});
    t.word(0xffee, 0x1100);
    t.put(0x1100, {0xa9, 0x66, 0x55, 0xdb});
    m37710_set_register(d, M37710_S, 0x27f);
    m37710_write8(d, 0x75, 2);
    m37710_write16(d, 0x46, 49);
    m37710_write8(d, 0x40, 1);
    run(d, 100);
    assert(status(d).a == 0x5566 && t.last_irq == M37710_LINE_TIMERA0);
    m37710_destroy(d);
    Bus z;
    auto e = z.cpu();
    z.put(0x1000, {0x89, 0x49, 0xe8, 3}); // One long RLA instruction, 1000 rotations.
    m37710_write8(e, 0x1e, 0x48);
    m37710_run_cycles(e, 1);
    assert(z.analog_calls == 6000 / 228);
    m37710_destroy(e);
    std::puts("M37710 GPIO / ADC timing / timer reload-stop-event / timer IRQ passed");
}
#include "m37710_smoke.inc"
static void instruction_smoke() {
    Bus b;
    auto c = b.cpu();
    for (unsigned mode = 0; mode < 4; mode++)
        for (const auto &op : smoke_opcodes) {
            m37710_reset(c);
            m37710_set_register(c, M37710_PS, 4 | ((mode & 2) ? 32 : 0) | ((mode & 1) ? 16 : 0));
            m37710_set_register(c, M37710_A, 1);
            m37710_set_register(c, M37710_B, 0);
            m37710_set_register(c, M37710_X, 0);
            m37710_set_register(c, M37710_Y, 0);
            m37710_set_register(c, M37710_S, 0x27f);
            unsigned p = 0x1000;
            if (op[0])
                b.mem[p++] = op[0];
            b.mem[p++] = op[1];
            for (unsigned j = 0; j < 8; j++)
                b.mem[p + j] = 0;
            if (!op[0] && (op[1] == 0x42 || op[1] == 0x89))
                b.mem[p] = 9; // Valid immediate prefix operation.
            auto before = status(c);
            m37710_run_cycles(c, 1);
            auto after = status(c);
            if (after.fault)
                std::fprintf(stderr, "Smoke mode%u prefix%x opcode%x fault\n", mode, op[0], op[1]);
            assert(!after.fault && after.instructions == before.instructions + 1 &&
                   after.cycles > before.cycles);
        }
    m37710_destroy(c);
    std::printf("M37710 %zu valid opcode dispatches across all four widths passed\n",
                4 * sizeof(smoke_opcodes) / sizeof(smoke_opcodes[0]));
}
static void creation_slices() {
    assert(!m37710_create(nullptr));
    Bus b;
    auto conf = b.config(M37710_MODEL_M37702M2);
    assert(!m37710_create(&conf));
    std::vector<uint8_t> rom(16384, 0xea);
    rom[0x3ffe] = 0;
    rom[0x3fff] = 0xc0;
    rom[0] = 0xa9;
    rom[1] = 0x34;
    rom[2] = 0x12;
    rom[3] = 0xdb;
    conf.internal_rom = rom.data();
    conf.internal_rom_size = rom.size();
    auto c = m37710_create(&conf);
    assert(c);
    rom[1] = 0;
    run(c);
    assert(status(c).a == 0x1234);
    m37710_write8(c, 0xc001, 0);
    assert(m37710_read8(c, 0xc001) == 0x34);
    m37710_write16(c, 0x100, 0x55aa);
    m37710_reset(c);
    assert(m37710_read16(c, 0x100) == 0x55aa);
    m37710_destroy(c);
    Bus model;
    auto s4 = model.config(M37710_MODEL_M37710S4);
    model.word(0xfffe, 0x1000);
    auto device = m37710_create(&s4);
    assert(device);
    m37710_write8(device, 0x87f, 0x55);
    assert(m37710_read8(device, 0x87f) == 0x55);
    model.mem[2] = 0x77;
    assert(m37710_read8(device, 2) == 0x77);
    m37710_write8(device, 0x13, 0x88);
    assert(m37710_read8(device, 0x13) == 0);
    m37710_destroy(device);
    Bus a, d;
    auto ca = a.cpu(), cd = d.cpu();
    a.put(0x1000, {0xea, 0x80, 0xfd});
    d.put(0x1000, {0xea, 0x80, 0xfd});
    for (int i = 0; i < 1000; i++)
        m37710_run_cycles(ca, 1);
    m37710_run_cycles(cd, 1000);
    auto sa = status(ca), sd = status(cd);
    assert(sa.pc == sd.pc && sa.cycles == sd.cycles && sa.instructions == sd.instructions);
    m37710_destroy(ca);
    m37710_destroy(cd);
    Bus z;
    auto e = z.cpu();
    z.put(0x1000, {0x89, 0xff});
    m37710_run_cycles(e, 20);
    assert(status(e).fault && status(e).illegal_opcodes == 1);
    m37710_run_cycles(e, 100);
    assert(status(e).illegal_opcodes == 1);
    m37710_destroy(e);
    std::puts("M37710 required BIOS / ROM-RAM / schedule slices / illegal diagnostics passed");
}
int main() {
    widths();
    arithmetic();
    fixed_vectors();
    addressing();
    instruction_smoke();
    calls_bus();
    interrupts();
    peripherals();
    creation_slices();
    return 0;
}
