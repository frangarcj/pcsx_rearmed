/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright Olivier Galibert (original MAME implementation); see h8_LICENSE.txt.
 */
#ifndef PCSX_ARCADE_H8_SCI_H
#define PCSX_ARCADE_H8_SCI_H

#include <cstdint>

namespace arcade {

/* Pin-level H8 SCI. Register indices are SMR, BRR, SCR, TDR, SSR and RDR.
 * The scheduler calls update() at next_event(); writes and pin edges use the
 * cycle supplied by NowCb. External-rate clocks take their period in
 * attoseconds plus the H8 system clock in Hz. */
class H8Sci {
public:
    typedef uint64_t (*NowCb)(void *opaque);
    typedef void (*PinCb)(void *opaque, int level);
    typedef void (*IrqRequest)(void *opaque, unsigned vector);
    typedef void (*RescheduleCb)(void *opaque);
    typedef void (*FrameTx)(void *opaque, unsigned channel, uint8_t byte);
    typedef void (*TimerAdjustCb)(void *opaque, unsigned channel,
                                   uint64_t delay_cycles);

    H8Sci();
    void setCallbacks(void *opaque, NowCb now, PinCb tx, PinCb clk,
                      IrqRequest irq, unsigned vector_base,
                      unsigned channel = 0, FrameTx frame_tx = 0,
                      RescheduleCb reschedule = 0, NowCb rx_now = 0,
                      TimerAdjustCb timer_adjust = 0, bool observe_frames = false);
    void setExternalClockPeriodAS(uint64_t period_as, uint64_t cpu_hz);
    void reset();

    uint8_t read(unsigned reg);
    void write(unsigned reg, uint8_t value);
    void rx_w(int level);
    void clk_w(int level);
    /* Compatibility injection for byte-oriented diagnostic clients. Board
     * wiring uses rx_w()/clk_w() so the shift register and timing run normally. */
    void receiveByte(uint8_t value);
    uint64_t update(uint64_t cycle);
    uint64_t next_event() const { return m_clock_event; }

private:
    enum State { ST_IDLE, ST_START, ST_BIT, ST_PARITY, ST_STOP, ST_LAST_TICK };
    enum { CLK_TX = 1, CLK_RX = 2 };
    enum ClockMode {
        INTERNAL_ASYNC,
        INTERNAL_ASYNC_OUT,
        EXTERNAL_ASYNC,
        EXTERNAL_RATE_ASYNC,
        INTERNAL_SYNC_OUT,
        EXTERNAL_SYNC,
        EXTERNAL_RATE_SYNC
    };
    enum {
        SMR_CA = 0x80, SMR_CHR = 0x40, SMR_PE = 0x20, SMR_OE = 0x10,
        SMR_STOP = 0x08, SMR_MP = 0x04, SMR_CKS = 0x03,
        SCR_TIE = 0x80, SCR_RIE = 0x40, SCR_TE = 0x20, SCR_RE = 0x10,
        SCR_MPIE = 0x08, SCR_TEIE = 0x04, SCR_CKE = 0x03,
        SCR_CKE1 = 0x02, SCR_CKE0 = 0x01,
        SSR_TDRE = 0x80, SSR_RDRF = 0x40, SSR_ORER = 0x20,
        SSR_FER = 0x10, SSR_PER = 0x08, SSR_TEND = 0x04,
        SSR_MPB = 0x02, SSR_MPBT = 0x01
    };

    void drive_tx(int level);
    void drive_clk(int level);
    void request_irq(unsigned offset);
    void reschedule();
    void adjust_scheduler_timer(uint64_t delay_cycles);
    bool has_recv_error() const;
    void clock_update();
    void clock_start(int mode);
    void clock_stop(int mode);
    void tx_start();
    void tx_async_tick();
    void tx_async_step();
    void tx_sync_tick();
    void tx_sync_step();
    void rx_start();
    void rx_done();
    void rx_async_tick();
    void rx_async_step();
    void rx_sync_tick();
    void rx_sync_step();
    void sync_rx_start();
    void frame_done();

    void *m_opaque;
    NowCb m_now;
    NowCb m_rx_now;
    PinCb m_tx_pin;
    PinCb m_clk_pin;
    IrqRequest m_irq;
    RescheduleCb m_reschedule;
    FrameTx m_frame_tx;
    TimerAdjustCb m_timer_adjust;
    unsigned m_vector_base;
    unsigned m_channel;
    bool m_observe_frames;

    double m_external_to_internal_ratio;
    double m_internal_to_external_ratio;
    uint64_t m_external_period_as;
    uint64_t m_cpu_hz;
    State m_tx_state, m_rx_state;
    int m_tx_bit, m_rx_bit, m_clock_state, m_tx_parity, m_rx_parity;
    int m_tx_clock_counter, m_rx_clock_counter;
    ClockMode m_clock_mode;
    bool m_ext_clock_value, m_rx_value;
    uint8_t m_rdr, m_tdr, m_smr, m_scr, m_ssr, m_ssr_read;
    uint8_t m_brr, m_rsr, m_tsr, m_tx_frame;
    uint64_t m_clock_event, m_clock_step, m_divider;
};

} // namespace arcade

#endif
