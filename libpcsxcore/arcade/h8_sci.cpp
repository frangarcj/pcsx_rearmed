/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright Olivier Galibert (original MAME implementation); see h8_LICENSE.txt.
 * Source reference: MAME src/devices/cpu/h8/h8_sci.cpp at deb2f54fe7300e7a192690318beb860159bfd52a.
 */
#include "h8_sci.h"

namespace arcade {

H8Sci::H8Sci()
    : m_opaque(0), m_now(0), m_rx_now(0), m_tx_pin(0), m_clk_pin(0), m_irq(0),
      m_reschedule(0), m_frame_tx(0), m_timer_adjust(0),
      m_vector_base(0), m_channel(0), m_observe_frames(false),
      m_external_to_internal_ratio(0.0), m_internal_to_external_ratio(0.0),
      m_external_period_as(0), m_cpu_hz(0), m_tx_state(ST_IDLE),
      m_rx_state(ST_IDLE), m_tx_bit(0), m_rx_bit(0), m_clock_state(0),
      m_tx_parity(0), m_rx_parity(0), m_tx_clock_counter(0),
      m_rx_clock_counter(0), m_clock_mode(INTERNAL_ASYNC),
      m_ext_clock_value(true), m_rx_value(true), m_rdr(0), m_tdr(0xff),
      m_smr(0), m_scr(0), m_ssr(0x84), m_ssr_read(0), m_brr(0xff),
      m_rsr(0), m_tsr(0xff), m_tx_frame(0xff), m_clock_event(0),
      m_clock_step(0), m_divider(0) {}

void H8Sci::setCallbacks(void *opaque, NowCb now, PinCb tx, PinCb clk,
                         IrqRequest irq, unsigned vector_base,
                         unsigned channel, FrameTx frame_tx,
                         RescheduleCb reschedule_cb, NowCb rx_now,
                         TimerAdjustCb timer_adjust, bool observe_frames) {
    m_opaque = opaque;
    m_now = now;
    m_rx_now = rx_now;
    m_timer_adjust = timer_adjust;
    m_tx_pin = tx;
    m_clk_pin = clk;
    m_irq = irq;
    m_vector_base = vector_base;
    m_channel = channel;
    m_frame_tx = frame_tx;
    m_observe_frames = observe_frames;
    m_reschedule = reschedule_cb;
}

void H8Sci::setExternalClockPeriodAS(uint64_t period_as, uint64_t cpu_hz) {
    m_external_period_as = period_as;
    m_cpu_hz = cpu_hz;
    if (!period_as || !cpu_hz) {
        m_external_to_internal_ratio = 0.0;
        m_internal_to_external_ratio = 0.0;
    } else {
        // Match attotime::operator*(u32) before as_double(), using its
        // billion-sized split so every intermediate remains 64-bit.
        const uint32_t factor = static_cast<uint32_t>(cpu_hz);
        const uint64_t atto_second_sqrt = 1000000000ULL;
        const uint64_t whole_seconds = period_as / 1000000000000000000ULL;
        const uint64_t attoseconds = period_as % 1000000000000000000ULL;
        const uint32_t atto_high = static_cast<uint32_t>(attoseconds / atto_second_sqrt);
        const uint32_t atto_low = static_cast<uint32_t>(attoseconds % atto_second_sqrt);

        uint64_t product = static_cast<uint64_t>(atto_low) * factor;
        const uint32_t result_low = static_cast<uint32_t>(product % atto_second_sqrt);
        product /= atto_second_sqrt;
        product += static_cast<uint64_t>(atto_high) * factor;
        const uint32_t result_high = static_cast<uint32_t>(product % atto_second_sqrt);
        const uint64_t result_seconds = product / atto_second_sqrt + whole_seconds * factor;
        const uint64_t result_attoseconds = result_low +
            static_cast<uint64_t>(result_high) * atto_second_sqrt;
        m_external_to_internal_ratio = static_cast<double>(result_seconds) +
            static_cast<double>(result_attoseconds) / 1.0e18;
        m_internal_to_external_ratio = 1.0 / m_external_to_internal_ratio;
    }
    clock_update();
}

void H8Sci::reset() {
    m_rdr = 0x00;
    m_tdr = 0xff;
    m_smr = 0x00;
    m_scr = 0x00;
    m_ssr = 0x84;
    m_brr = 0xff;
    m_rsr = 0x00;
    m_tsr = 0xff;
    m_tx_frame = 0xff;
    m_rx_bit = 0;
    m_tx_bit = 0;
    m_tx_state = ST_IDLE;
    m_rx_state = ST_IDLE;
    m_clock_state = 0;
    m_clock_mode = INTERNAL_ASYNC;
    m_clock_event = 0;
    clock_update();
    m_ext_clock_value = true;
    m_tx_clock_counter = 0;
    m_rx_clock_counter = 0;
    drive_clk(1);
    drive_tx(1);
}

void H8Sci::drive_tx(int level) {
    if (m_tx_pin)
        m_tx_pin(m_opaque, level != 0);
}

void H8Sci::drive_clk(int level) {
    if (m_clk_pin)
        m_clk_pin(m_opaque, level != 0);
}

void H8Sci::request_irq(unsigned offset) {
    if (m_irq)
        m_irq(m_opaque, m_vector_base + offset);
}

void H8Sci::reschedule() {
    if (m_reschedule)
        m_reschedule(m_opaque);
}

void H8Sci::adjust_scheduler_timer(uint64_t delay_cycles) {
    if (m_timer_adjust)
        m_timer_adjust(m_opaque, m_channel, delay_cycles);
}

bool H8Sci::has_recv_error() const {
    return (m_ssr & (SSR_ORER | SSR_PER | SSR_FER)) != 0;
}

void H8Sci::clock_update() {
    m_divider = 2ULL << (2 * (m_smr & SMR_CKS));
    m_divider *= static_cast<uint64_t>(m_brr) + 1;

    if (m_smr & SMR_CA) {
        m_clock_mode = (m_scr & SCR_CKE1) ? EXTERNAL_SYNC : INTERNAL_SYNC_OUT;
    } else if (m_scr & SCR_CKE1) {
        m_clock_mode = EXTERNAL_ASYNC;
    } else if (m_scr & SCR_CKE0) {
        m_clock_mode = INTERNAL_ASYNC_OUT;
    } else {
        m_clock_mode = INTERNAL_ASYNC;
    }

    if (m_external_period_as) {
        if (m_clock_mode == EXTERNAL_ASYNC)
            m_clock_mode = EXTERNAL_RATE_ASYNC;
        else if (m_clock_mode == EXTERNAL_SYNC)
            m_clock_mode = EXTERNAL_RATE_SYNC;
    }
}

void H8Sci::sync_rx_start() {
    if (m_rx_state == ST_IDLE && (m_smr & SMR_CA) && (m_scr & SCR_RE) &&
        !(m_scr & SCR_TE) && !has_recv_error())
        rx_start();
}

uint8_t H8Sci::read(unsigned reg) {
    switch (reg) {
    case 0: return m_smr;
    case 1: return m_brr;
    case 2: return m_scr;
    case 3: return m_tdr;
    case 4: m_ssr_read = m_ssr; return m_ssr;
    case 5: return m_rdr;
    default: return 0xff;
    }
}

void H8Sci::write(unsigned reg, uint8_t value) {
    switch (reg) {
    case 0: // SMR
        m_smr = value;
        clock_update();
        sync_rx_start();
        break;
    case 1: // BRR
        m_brr = value;
        clock_update();
        break;
    case 2: { // SCR
        const uint8_t delta = m_scr ^ value;
        m_scr = value;
        clock_update();
        if ((delta & SCR_RE) && !(m_scr & SCR_RE)) {
            m_rx_state = ST_IDLE;
            clock_stop(CLK_RX);
        }
        if (delta & (SCR_RE | SCR_TE))
            sync_rx_start();
        if ((delta & SCR_TIE) && (m_scr & SCR_TIE) && (m_ssr & SSR_TDRE))
            request_irq(2);
        if ((delta & SCR_TEIE) && (m_scr & SCR_TEIE) && (m_ssr & SSR_TEND))
            request_irq(3);
        if ((delta & SCR_RIE) && (m_scr & SCR_RIE) && (m_ssr & SSR_RDRF))
            request_irq(1);
        if ((delta & SCR_RIE) && (m_scr & SCR_RIE) && has_recv_error())
            request_irq(0);
        break;
    }
    case 3: // TDR: transfer begins when software clears TDRE in SSR.
        m_tdr = value;
        break;
    case 4: // SSR, including MAME's read-before-clear behavior.
        if ((m_scr & SCR_TE) && (m_ssr & m_ssr_read & SSR_TDRE) &&
            !(value & SSR_TDRE))
            m_ssr &= static_cast<uint8_t>(~(SSR_TDRE | SSR_TEND));
        m_ssr = static_cast<uint8_t>(
            (m_ssr & static_cast<uint8_t>((~m_ssr_read | value | SSR_TDRE |
                                          SSR_TEND | SSR_MPB) & ~SSR_MPBT)) |
            (value & SSR_MPBT));
        m_ssr_read &= m_ssr;
        if (m_tx_state == ST_IDLE && !(m_ssr & SSR_TDRE))
            tx_start();
        sync_rx_start();
        break;
    default:
        break;
    }
}

void H8Sci::rx_w(int level) {
    const bool next = level != 0;
    if (next != m_rx_value && (m_clock_state & CLK_RX) &&
        (m_rx_clock_counter == 1 || m_rx_clock_counter == 15))
        m_rx_clock_counter = 0;

    if (m_rx_state == ST_IDLE && !(m_smr & SMR_CA) && (m_scr & SCR_RE) &&
        m_rx_value && !next)
        rx_start();
    m_rx_value = next;
}

void H8Sci::clk_w(int level) {
    const bool next = level != 0;
    if (m_ext_clock_value == next)
        return;
    m_ext_clock_value = next;
    if (!m_clock_state)
        return;
    if (m_clock_mode == EXTERNAL_ASYNC) {
        if (m_clock_state & CLK_TX) tx_async_tick();
        if (m_clock_state & CLK_RX) rx_async_tick();
    } else if (m_clock_mode == EXTERNAL_SYNC) {
        if (m_clock_state & CLK_TX) tx_sync_tick();
        if (m_clock_state & CLK_RX) rx_sync_tick();
    }
}

void H8Sci::receiveByte(uint8_t value) {
    if (!(m_scr & SCR_RE))
        return;
    if (m_ssr & SSR_RDRF)
        m_ssr |= SSR_ORER;
    else {
        m_rdr = value;
        m_ssr |= SSR_RDRF;
    }
    if (m_scr & SCR_RIE)
        request_irq(has_recv_error() ? 0 : 1);
}

uint64_t H8Sci::update(uint64_t cycle) {
    if (!m_clock_event || cycle < m_clock_event)
        return m_clock_event;

    if (m_clock_mode == INTERNAL_ASYNC || m_clock_mode == INTERNAL_ASYNC_OUT ||
        m_clock_mode == EXTERNAL_RATE_ASYNC) {
        if (m_clock_state & CLK_TX) tx_async_tick();
        if (m_clock_state & CLK_RX) rx_async_tick();
    } else if (m_clock_mode == INTERNAL_SYNC_OUT ||
               m_clock_mode == EXTERNAL_RATE_SYNC) {
        if (m_clock_state & CLK_TX) tx_sync_tick();
        if (m_clock_state & CLK_RX) rx_sync_tick();
    }

    if (m_clock_state) {
        if (m_clock_step) {
            m_clock_event += m_clock_step;
        } else if (m_clock_mode == EXTERNAL_RATE_ASYNC ||
                   m_clock_mode == EXTERNAL_RATE_SYNC) {
            m_clock_event = static_cast<uint64_t>(
                static_cast<uint64_t>(m_clock_event * m_internal_to_external_ratio + 1.0) *
                    m_external_to_internal_ratio + 1.0);
        } else {
            m_clock_event = 0;
        }
        if (m_clock_event) {
            const uint64_t now = m_rx_now ? m_rx_now(m_opaque) :
                (m_now ? m_now(m_opaque) : 0);
            const int64_t ticks = static_cast<int64_t>(m_clock_event - now);
            if (ticks >= 0)
                adjust_scheduler_timer(static_cast<uint64_t>(ticks));
            reschedule();
        }
    } else {
        m_clock_event = 0;
        if (m_clock_mode == INTERNAL_ASYNC_OUT || m_clock_mode == INTERNAL_SYNC_OUT)
            drive_clk(1);
    }
    return m_clock_event;
}

void H8Sci::clock_start(int mode) {
    if (m_clock_state & mode)
        return;
    if (mode == CLK_TX)
        m_tx_clock_counter = 15;
    else
        m_rx_clock_counter = 15;
    m_clock_state |= mode;
    if (m_clock_state != mode)
        return;

    m_clock_step = 0;
    switch (m_clock_mode) {
    case INTERNAL_ASYNC:
    case INTERNAL_ASYNC_OUT:
    case INTERNAL_SYNC_OUT: {
        m_clock_step = m_divider;
        const NowCb clock_now = mode == CLK_RX && m_rx_now ? m_rx_now : m_now;
        const uint64_t now = clock_now ? clock_now(m_opaque) : 0;
        m_clock_event = (now / m_clock_step + 1) * m_clock_step;
        adjust_scheduler_timer(m_clock_event - now);
        reschedule();
        break;
    }
    case EXTERNAL_RATE_ASYNC:
    case EXTERNAL_RATE_SYNC: {
        const NowCb clock_now = mode == CLK_RX && m_rx_now ? m_rx_now : m_now;
        const uint64_t now = clock_now ? clock_now(m_opaque) : 0;
        m_clock_event = static_cast<uint64_t>(
            static_cast<uint64_t>(now * m_internal_to_external_ratio + 1.0) *
                m_external_to_internal_ratio + 1.0);
        adjust_scheduler_timer(m_clock_event - now);
        reschedule();
        break;
    }
    case EXTERNAL_ASYNC:
    case EXTERNAL_SYNC:
        break;
    }
}

void H8Sci::clock_stop(int mode) {
    m_clock_state &= ~mode;
    if (!m_clock_state) {
        m_clock_event = 0;
        m_clock_step = 0;
    }
    reschedule();
}

void H8Sci::tx_start() {
    m_ssr |= SSR_TDRE;
    m_tsr = m_tdr;
    m_tx_frame = m_tsr;
    m_tx_parity = (m_smr & SMR_OE) ? 0 : 1;
    if (m_scr & SCR_TIE)
        request_irq(2);
    if (m_smr & SMR_CA) {
        m_tx_state = ST_BIT;
        m_tx_bit = 8;
    } else {
        m_tx_state = ST_START;
        m_tx_bit = 1;
    }
    clock_start(CLK_TX);
    if (m_rx_state == ST_IDLE && (m_smr & SMR_CA) && (m_scr & SCR_TE) &&
        (m_scr & SCR_RE) && !has_recv_error())
        rx_start();
}

void H8Sci::frame_done() {
    if (m_frame_tx && (m_observe_frames || (!m_tx_pin && !m_clk_pin)))
        m_frame_tx(m_opaque, m_channel, m_tx_frame);
}

void H8Sci::tx_async_tick() {
    m_tx_clock_counter = (m_tx_clock_counter + 1) & 15;
    if (m_tx_clock_counter == 0) {
        tx_async_step();
        if (m_clock_mode == INTERNAL_ASYNC_OUT)
            drive_clk(0);
    } else if (m_tx_clock_counter == 8 && m_clock_mode == INTERNAL_ASYNC_OUT) {
        drive_clk(1);
    }
}

void H8Sci::tx_async_step() {
    switch (m_tx_state) {
    case ST_START:
        drive_tx(0);
        m_tx_state = ST_BIT;
        m_tx_bit = (m_smr & SMR_CHR) ? 7 : 8;
        break;
    case ST_BIT:
        m_tx_parity ^= (m_tsr & 1);
        drive_tx(m_tsr & 1);
        m_tsr >>= 1;
        if (!--m_tx_bit) {
            if (m_smr & SMR_CA) {
                if (!(m_ssr & SSR_TDRE))
                    tx_start();
                else {
                    m_tx_state = ST_LAST_TICK;
                    m_tx_bit = 0;
                }
            } else if (m_smr & SMR_PE) {
                m_tx_state = ST_PARITY;
                m_tx_bit = 1;
            } else {
                m_tx_state = ST_STOP;
                m_tx_bit = (m_smr & SMR_STOP) ? 2 : 1;
            }
        }
        break;
    case ST_PARITY:
        drive_tx(m_tx_parity);
        m_tx_state = ST_STOP;
        m_tx_bit = (m_smr & SMR_STOP) ? 2 : 1;
        break;
    case ST_STOP:
        drive_tx(1);
        if (!--m_tx_bit) {
            if (!(m_ssr & SSR_TDRE)) {
                frame_done();
                tx_start();
            } else {
                m_tx_state = ST_LAST_TICK;
                m_tx_bit = 0;
            }
        }
        break;
    case ST_LAST_TICK:
        frame_done();
        m_tx_state = ST_IDLE;
        m_tx_bit = 0;
        clock_stop(CLK_TX);
        drive_tx(1);
        if (m_scr & SCR_TEIE)
            request_irq(3);
        if ((m_scr & SCR_TE) && !(m_ssr & SSR_TDRE))
            tx_start();
        else
            m_ssr |= SSR_TEND;
        break;
    default:
        break;
    }
}

void H8Sci::tx_sync_tick() {
    m_tx_clock_counter = (m_tx_clock_counter + 1) & 1;
    if (m_tx_clock_counter == 0) {
        tx_sync_step();
        if (m_clock_mode == INTERNAL_SYNC_OUT && m_tx_state != ST_IDLE)
            drive_clk(0);
    } else if (m_tx_clock_counter == 1 && m_clock_mode == INTERNAL_SYNC_OUT) {
        drive_clk(1);
    }
}

void H8Sci::tx_sync_step() {
    if (!m_tx_bit) {
        frame_done();
        m_tx_state = ST_IDLE;
        clock_stop(CLK_TX);
        drive_tx(1);
        if (m_scr & SCR_TEIE)
            request_irq(3);
        if ((m_scr & SCR_TE) && !(m_ssr & SSR_TDRE))
            tx_start();
        else
            m_ssr |= SSR_TEND;
    } else {
        drive_tx(m_tsr & 1);
        m_tsr >>= 1;
        m_tx_bit--;
    }
}

void H8Sci::rx_start() {
    m_rx_parity = (m_smr & SMR_OE) ? 0 : 1;
    m_rsr = 0;
    if (m_smr & SMR_CA) {
        m_rx_state = ST_BIT;
        m_rx_bit = 8;
    } else {
        m_rx_state = ST_START;
        m_rx_bit = 1;
    }
    clock_start(CLK_RX);
}

void H8Sci::rx_done() {
    if (!(m_ssr & SSR_FER)) {
        if ((m_smr & SMR_PE) && m_rx_parity) {
            m_ssr |= SSR_PER;
        } else if (m_ssr & SSR_RDRF) {
            m_ssr |= SSR_ORER;
        } else {
            m_ssr |= SSR_RDRF;
            m_rdr = m_rsr;
        }
    }
    if (m_scr & SCR_RIE) {
        if (has_recv_error())
            request_irq(0);
        else
            request_irq(1);
    }
    m_rx_state = ST_IDLE;
    clock_stop(CLK_RX);
    sync_rx_start();
}

void H8Sci::rx_async_tick() {
    m_rx_clock_counter = (m_rx_clock_counter + 1) & 15;
    if (m_rx_clock_counter == 8)
        rx_async_step();
}

void H8Sci::rx_async_step() {
    switch (m_rx_state) {
    case ST_START:
        if (m_rx_value) {
            m_rx_state = ST_IDLE;
            clock_stop(CLK_RX);
        } else {
            m_rx_state = ST_BIT;
            m_rx_bit = (m_smr & SMR_CHR) ? 7 : 8;
        }
        break;
    case ST_BIT:
        m_rsr >>= 1;
        if (m_rx_value) {
            m_rx_parity = !m_rx_parity;
            m_rsr |= ((m_smr & (SMR_CA | SMR_CHR)) == SMR_CHR) ? 0x40 : 0x80;
        }
        if (!--m_rx_bit) {
            if (m_smr & SMR_PE) {
                m_rx_state = ST_PARITY;
                m_rx_bit = 1;
            } else {
                m_rx_state = ST_STOP;
                m_rx_bit = 1;
            }
        }
        break;
    case ST_PARITY:
        m_rx_parity ^= m_rx_value;
        m_rx_state = ST_STOP;
        m_rx_bit = 1;
        break;
    case ST_STOP:
        if (!m_rx_value)
            m_ssr |= SSR_FER;
        else if ((m_smr & SMR_PE) && m_rx_parity)
            m_ssr |= SSR_PER;
        rx_done();
        break;
    default:
        break;
    }
}

void H8Sci::rx_sync_tick() {
    m_rx_clock_counter = (m_rx_clock_counter + 1) & 1;
    if (m_rx_clock_counter == 0 && m_clock_mode == INTERNAL_SYNC_OUT) {
        drive_clk(0);
    } else if (m_rx_clock_counter == 1) {
        if (m_clock_mode == INTERNAL_SYNC_OUT)
            drive_clk(1);
        rx_sync_step();
    }
}

void H8Sci::rx_sync_step() {
    m_rsr >>= 1;
    if (m_rx_value)
        m_rsr |= 0x80;
    if (!--m_rx_bit)
        rx_done();
}

} // namespace arcade
