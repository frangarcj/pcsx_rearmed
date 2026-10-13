/* Standalone integration of MAME's SH-2 interpreter. BSD-3-Clause.
 * Copyright David Haywood and the SH-2 contributors; sh2_LICENSE.txt. */
#include "sh2.h"
#include <cstring>
#include <new>
#define REG_N ((opcode >> 8) & 15)
#define REG_M ((opcode >> 4) & 15)
#define SH_T 1u
#define SH_S 2u
#define SH_I 0xf0u
#define SH_Q 0x100u
#define SH_M 0x200u
#define SH_FLAGS (SH_T | SH_S | SH_I | SH_Q | SH_M)
#define BIT(value, bit) (((value) >> (bit)) & 1)
#define BUSY_LOOP_HACKS 0
namespace util { static int32_t sext(uint32_t value, unsigned bits) {
    uint32_t sign = 1u << (bits - 1);
    return (int32_t)((value ^ sign) - sign);
} }
static uint32_t rotl_32(uint32_t v, unsigned n) { return (v << n) | (v >> (32 - n)); }
static uint32_t rotr_32(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }
struct StandaloneSh2 {
    Sh2Config config;
    Sh2State s{};
    Sh2State *m_sh2_state = &s;
    uint32_t m_am = 0xffffffff;
    int m_test_irq = 0;
    uint8_t read_byte(uint32_t a) { return config.read(config.opaque, a, 1); }
    uint16_t read_word(uint32_t a) { return config.read(config.opaque, a, 2); }
    uint32_t read_long(uint32_t a) { return config.read(config.opaque, a, 4); }
    void write_byte(uint32_t a, uint8_t v) { config.write(config.opaque, a, 1, v); }
    void write_word(uint32_t a, uint16_t v) { config.write(config.opaque, a, 2, v); }
    void write_long(uint32_t a, uint32_t v) { config.write(config.opaque, a, 4, v); }
    void ADD(uint32_t m, uint32_t n);
    void ADDI(uint32_t i, uint32_t n);
    void ADDC(uint32_t m, uint32_t n);
    void ADDV(uint32_t m, uint32_t n);
    void AND(uint32_t m, uint32_t n);
    void ANDI(uint32_t i);
    void ANDM(uint32_t i);
    void BF(uint32_t d);
    void BFS(uint32_t d);
    void BRA(uint32_t d);
    void BRAF(uint32_t m);
    void BSR(uint32_t d);
    void BSRF(uint32_t m);
    void BT(uint32_t d);
    void BTS(uint32_t d);
    void CLRMAC();
    void CLRT();
    void CMPEQ(uint32_t m, uint32_t n);
    void CMPGE(uint32_t m, uint32_t n);
    void CMPGT(uint32_t m, uint32_t n);
    void CMPHI(uint32_t m, uint32_t n);
    void CMPHS(uint32_t m, uint32_t n);
    void CMPPL(uint32_t n);
    void CMPPZ(uint32_t n);
    void CMPSTR(uint32_t m, uint32_t n);
    void CMPIM(uint32_t i);
    void DIV0S(uint32_t m, uint32_t n);
    void DIV0U();
    void DIV1(uint32_t m, uint32_t n);
    void DMULS(uint32_t m, uint32_t n);
    void DMULU(uint32_t m, uint32_t n);
    void DT(uint32_t n);
    void EXTSB(uint32_t m, uint32_t n);
    void EXTSW(uint32_t m, uint32_t n);
    void EXTUB(uint32_t m, uint32_t n);
    void EXTUW(uint32_t m, uint32_t n);
    void JMP(uint32_t m);
    void JSR(uint32_t m);
    void LDCGBR(uint32_t m);
    void LDCVBR(uint32_t m);
    void LDCMGBR(uint32_t m);
    void LDCMVBR(uint32_t m);
    void LDSMACH(uint32_t m);
    void LDSMACL(uint32_t m);
    void LDSPR(uint32_t m);
    void LDSMMACH(uint32_t m);
    void LDSMMACL(uint32_t m);
    void LDSMPR(uint32_t m);
    void MAC_L(uint32_t m, uint32_t n);
    void MAC_W(uint32_t m, uint32_t n);
    void MOV(uint32_t m, uint32_t n);
    void MOVBS(uint32_t m, uint32_t n);
    void MOVWS(uint32_t m, uint32_t n);
    void MOVLS(uint32_t m, uint32_t n);
    void MOVBL(uint32_t m, uint32_t n);
    void MOVWL(uint32_t m, uint32_t n);
    void MOVLL(uint32_t m, uint32_t n);
    void MOVBM(uint32_t m, uint32_t n);
    void MOVWM(uint32_t m, uint32_t n);
    void MOVLM(uint32_t m, uint32_t n);
    void MOVBP(uint32_t m, uint32_t n);
    void MOVWP(uint32_t m, uint32_t n);
    void MOVLP(uint32_t m, uint32_t n);
    void MOVBS0(uint32_t m, uint32_t n);
    void MOVWS0(uint32_t m, uint32_t n);
    void MOVLS0(uint32_t m, uint32_t n);
    void MOVBL0(uint32_t m, uint32_t n);
    void MOVWL0(uint32_t m, uint32_t n);
    void MOVLL0(uint32_t m, uint32_t n);
    void MOVI(uint32_t i, uint32_t n);
    void MOVWI(uint32_t d, uint32_t n);
    void MOVLI(uint32_t d, uint32_t n);
    void MOVBLG(uint32_t d);
    void MOVWLG(uint32_t d);
    void MOVLLG(uint32_t d);
    void MOVBSG(uint32_t d);
    void MOVWSG(uint32_t d);
    void MOVLSG(uint32_t d);
    void MOVBS4(uint32_t d, uint32_t n);
    void MOVWS4(uint32_t d, uint32_t n);
    void MOVLS4(uint32_t m, uint32_t d, uint32_t n);
    void MOVBL4(uint32_t m, uint32_t d);
    void MOVWL4(uint32_t m, uint32_t d);
    void MOVLL4(uint32_t m, uint32_t d, uint32_t n);
    void MOVA(uint32_t d);
    void MOVT(uint32_t n);
    void MULL(uint32_t m, uint32_t n);
    void MULS(uint32_t m, uint32_t n);
    void MULU(uint32_t m, uint32_t n);
    void NEG(uint32_t m, uint32_t n);
    void NEGC(uint32_t m, uint32_t n);
    void NOP(void);
    void NOT(uint32_t m, uint32_t n);
    void OR(uint32_t m, uint32_t n);
    void ORI(uint32_t i);
    void ORM(uint32_t i);
    void ROTCL(uint32_t n);
    void ROTCR(uint32_t n);
    void ROTL(uint32_t n);
    void ROTR(uint32_t n);
    void RTS();
    void SETT();
    void SHAL(uint32_t n);
    void SHAR(uint32_t n);
    void SHLL(uint32_t n);
    void SHLL2(uint32_t n);
    void SHLL8(uint32_t n);
    void SHLL16(uint32_t n);
    void SHLR(uint32_t n);
    void SHLR2(uint32_t n);
    void SHLR8(uint32_t n);
    void SHLR16(uint32_t n);
    void STCSR(uint32_t n);
    void STCGBR(uint32_t n);
    void STCVBR(uint32_t n);
    void STCMSR(uint32_t n);
    void STCMGBR(uint32_t n);
    void STCMVBR(uint32_t n);
    void STSMACH(uint32_t n);
    void STSMACL(uint32_t n);
    void STSPR(uint32_t n);
    void STSMMACH(uint32_t n);
    void STSMMACL(uint32_t n);
    void STSMPR(uint32_t n);
    void SUB(uint32_t m, uint32_t n);
    void SUBC(uint32_t m, uint32_t n);
    void SUBV(uint32_t m, uint32_t n);
    void SWAPB(uint32_t m, uint32_t n);
    void SWAPW(uint32_t m, uint32_t n);
    void TAS(uint32_t n);
    void TST(uint32_t m, uint32_t n);
    void TSTI(uint32_t i);
    void TSTM(uint32_t i);
    void XOR(uint32_t m, uint32_t n);
    void XORI(uint32_t i);
    void XORM(uint32_t i);
    void XTRCT(uint32_t m, uint32_t n);
    void SLEEP();
    void op0010(uint16_t opcode);
    void op0011(uint16_t opcode);
    void op0110(uint16_t opcode);
    void op1000(uint16_t opcode);
    void op1100(uint16_t opcode);
    void execute_one_0000(uint16_t opcode);
    void execute_one_4000(uint16_t opcode);
    void execute_one(const uint16_t opcode);
    void LDCMSR(const uint16_t opcode);
    void LDCSR(const uint16_t opcode);
    void RTE();
    void TRAPA(uint32_t i);
    void ILLEGAL();
    void execute_one_f000(uint16_t opcode);
};
#include "sh2_ops.inc"
extern "C" {
Sh2CPU *sh2_create(const Sh2Config *config) {
    if (!config || !config->read || !config->write) return nullptr;
    auto *cpu = new(std::nothrow) StandaloneSh2;
    if (cpu) { cpu->config = *config; sh2_reset(cpu, 0); }
    return cpu;
}
void sh2_destroy(Sh2CPU *cpu) { delete cpu; }
void sh2_reset(Sh2CPU *cpu, uint64_t now) {
    if (!cpu) return;
    cpu->s = Sh2State{};
    cpu->s.sr = SH_I; cpu->s.cycles = now;
    cpu->s.pc = cpu->read_long(0); cpu->s.r[15] = cpu->read_long(4);
    cpu->m_test_irq = 0;
}
void sh2_set_irq(Sh2CPU *cpu, unsigned vector, unsigned level) {
    if (!cpu || vector > 255 || level > 15) return;
    cpu->s.pending_irq[vector] = level;
    cpu->s.irq_level = 0;
    for (unsigned i = 0; i < 256; i++)
        if (cpu->s.pending_irq[i] > cpu->s.irq_level) {
            cpu->s.irq_level = cpu->s.pending_irq[i]; cpu->s.irq_vector = i;
        }
}
int sh2_run_until(Sh2CPU *cpu, uint64_t target) {
    if (!cpu) return -1;
    auto &s = cpu->s;
    while (s.cycles < target) {
        if (!s.m_delay && s.irq_level > ((s.sr >> 4) & 15)) {
            unsigned vector = s.irq_vector, level = s.irq_level;
            s.r[15] -= 4; cpu->write_long(s.r[15], s.sr);
            s.r[15] -= 4; cpu->write_long(s.r[15], s.pc);
            s.sr = (s.sr & ~SH_I) | (level << 4);
            s.pc = cpu->read_long(s.vbr + vector * 4);
            sh2_set_irq(cpu, vector, 0); s.sleep_mode = 2; s.cycles += 5;
            if (cpu->config.irq_ack) cpu->config.irq_ack(cpu->config.opaque, vector);
        }
        uint16_t op = cpu->read_word(s.pc);
        if (s.m_delay) { s.pc = s.m_delay; s.m_delay = 0; }
        else s.pc += 2;
        s.icount = 0;
        cpu->execute_one(op);
        s.cycles += 1u + (unsigned)-s.icount;
        s.instructions++;
        if (s.sleep_mode == 1 && s.irq_level <= ((s.sr >> 4) & 15)) {
            s.cycles = target; break;
        }
    }
    return 0;
}
uint64_t sh2_current_cycle(const Sh2CPU *cpu) { return cpu ? cpu->s.cycles : 0; }
void sh2_get_state(const Sh2CPU *cpu, Sh2State *out) { if (cpu && out) *out = cpu->s; }
void sh2_set_state(Sh2CPU *cpu, const Sh2State *state) { if (cpu && state) { cpu->s = *state; cpu->m_test_irq = 0; } }
}
