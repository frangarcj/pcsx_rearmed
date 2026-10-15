// Generated from MAME deb2f54fe7300e7a192690318beb860159bfd52a. BSD-3-Clause.
// See twinkle_serial_LICENSE.txt; framework adapter/defined bounds guards only.
device_serial_interface::device_serial_interface(device_t &device) :
	device_interface(device, "serial"),
	m_start_bit_hack_for_external_clocks(false),
	m_df_start_bit_count(0),
	m_df_word_length(0),
	m_df_parity(PARITY_NONE),
	m_df_stop_bit_count(STOP_BITS_0),
	m_df_min_rx_stop_bit_count(0),
	m_rcv_register_data(0x8000),
	m_rcv_flags(0),
	m_rcv_bit_count_received(0),
	m_rcv_bit_count(0),
	m_rcv_byte_received(0),
	m_rcv_framing_error(false),
	m_rcv_parity_error(false),
	m_tra_register_data(0),
	m_tra_flags(TRANSMIT_REGISTER_EMPTY),
	m_tra_bit_count_transmitted(0),
	m_tra_bit_count(0),
	m_rcv_clock(nullptr),
	m_tra_clock(nullptr),
	m_rcv_rate(attotime::never),
	m_tra_rate(attotime::never),
	m_rcv_line(0),
	m_tra_clock_state(false),
	m_rcv_clock_state(false)
{
}

device_serial_interface::~device_serial_interface()
{
}

void device_serial_interface::interface_pre_start()
{
	if (!m_rcv_clock)
		m_rcv_clock = device().timer_alloc(FUNC(device_serial_interface::rcv_clock), this);
	if (!m_tra_clock)
		m_tra_clock = device().timer_alloc(FUNC(device_serial_interface::tra_clock), this);

	m_rcv_clock_state = false;
	m_tra_clock_state = false;
}

void device_serial_interface::interface_post_start()
{
	save_item(NAME(m_df_start_bit_count));
	save_item(NAME(m_df_word_length));
	save_item(NAME(m_df_parity));
	save_item(NAME(m_df_stop_bit_count));
	save_item(NAME(m_df_min_rx_stop_bit_count));
	save_item(NAME(m_rcv_register_data));
	save_item(NAME(m_rcv_flags));
	save_item(NAME(m_rcv_bit_count_received));
	save_item(NAME(m_rcv_bit_count));
	save_item(NAME(m_rcv_byte_received));
	save_item(NAME(m_rcv_framing_error));
	save_item(NAME(m_rcv_parity_error));
	save_item(NAME(m_tra_register_data));
	save_item(NAME(m_tra_flags));
	save_item(NAME(m_tra_bit_count_transmitted));
	save_item(NAME(m_tra_bit_count));
	save_item(NAME(m_rcv_rate));
	save_item(NAME(m_tra_rate));
	save_item(NAME(m_rcv_line));
	save_item(NAME(m_tra_clock_state));
	save_item(NAME(m_rcv_clock_state));
}

void device_serial_interface::set_rcv_rate(const attotime &rate)
{
	m_rcv_rate = rate / 2;
	receive_register_reset();
	m_rcv_clock->adjust(attotime::never);
}

void device_serial_interface::set_tra_rate(const attotime &rate)
{
	m_tra_rate = rate / 2;
	transmit_register_reset();
	m_tra_clock->adjust(attotime::never);
}

void device_serial_interface::tra_edge()
{
	if (!is_transmit_register_empty())
	{
		tra_callback();
		if (is_transmit_register_empty())
			tra_complete();
	}

	if (is_transmit_register_empty() && !m_tra_rate.is_never())
	{
		m_tra_clock->adjust(attotime::never);
	}
}

void device_serial_interface::rcv_edge()
{
	rcv_callback();
	if (is_receive_register_full())
	{
		m_rcv_clock->adjust(attotime::never);
		rcv_complete();
	}
}

void device_serial_interface::tx_clock_w(int state)
{
	if (state != m_tra_clock_state)
	{
		m_tra_clock_state = state;
		if (m_tra_clock_state)
			tra_edge();
	}
}

void device_serial_interface::rx_clock_w(int state)
{
	if (state != m_rcv_clock_state)
	{
		m_rcv_clock_state = state;
		if (!m_rcv_clock_state)
			rcv_edge();
	}
}

void device_serial_interface::clock_w(int state)
{
	tx_clock_w(state);
	rx_clock_w(state);
}


void device_serial_interface::set_data_frame(int start_bit_count, int data_bit_count, parity_t parity, stop_bits_t stop_bits)
{
	LOGMASKED(LOG_SETUP, "Start bits: %d; Data bits: %d; Parity: %s; Stop bits: %s\n", start_bit_count, data_bit_count, parity_tostring(parity), stop_bits_tostring(stop_bits));

	bool df_changed = (start_bit_count != m_df_start_bit_count) || (data_bit_count != m_df_word_length) || (parity != m_df_parity);
	u8 orig_stop_bit_count = m_df_stop_bit_count;
	u8 orig_min_rx_stop_bit_count = m_df_min_rx_stop_bit_count;

	m_df_word_length = data_bit_count;

	switch (stop_bits)
	{
	case STOP_BITS_0:
	default:
		m_df_stop_bit_count = 0;
		m_df_min_rx_stop_bit_count = 0;
		break;

	case STOP_BITS_1:
		m_df_stop_bit_count = 1;
		m_df_min_rx_stop_bit_count = 1;
		break;

	case STOP_BITS_1_5:
		m_df_stop_bit_count = 2; // TODO: support 1.5 stop bits
		m_df_min_rx_stop_bit_count = 1;
		break;

	case STOP_BITS_2:
		m_df_stop_bit_count = 2;
		m_df_min_rx_stop_bit_count = 1;
		break;
	}

	df_changed = df_changed || (m_df_stop_bit_count != orig_stop_bit_count) || (m_df_min_rx_stop_bit_count != orig_min_rx_stop_bit_count);

	if (!df_changed)
	{
		LOGMASKED(LOG_SETUP, "No change to data frame, skipping\n");
		return;
	}

	m_df_parity = parity;
	m_df_start_bit_count = start_bit_count;

	/* Require at least one stop bit in async RX mode, none in sync RX mode. */
	m_rcv_bit_count = m_df_word_length + m_df_min_rx_stop_bit_count;

	if (m_df_parity != PARITY_NONE)
	{
		m_rcv_bit_count++;
	}

	receive_register_reset();
}

void device_serial_interface::receive_register_reset()
{
	m_rcv_bit_count_received = 0;
	m_rcv_flags &= ~RECEIVE_REGISTER_FULL;
	if (m_df_start_bit_count == 0)
	{
		m_rcv_flags |= RECEIVE_REGISTER_SYNCHRONISED;
		m_rcv_flags &= ~RECEIVE_REGISTER_WAITING_FOR_START_BIT;
	}
	else
	{
		m_rcv_flags &= ~RECEIVE_REGISTER_SYNCHRONISED;
		m_rcv_flags |= RECEIVE_REGISTER_WAITING_FOR_START_BIT;
	}
}

void device_serial_interface::rx_w(int state)
{
	m_rcv_line = state;
	if (m_rcv_flags & RECEIVE_REGISTER_SYNCHRONISED)
		return;
	receive_register_update_bit(state);
	if (m_rcv_flags & RECEIVE_REGISTER_SYNCHRONISED)
	{
		LOGMASKED(LOG_RX, "Receiver is synchronized\n");
		if (m_rcv_clock && !(m_rcv_rate.is_never()))
		{
			// make start delay half a cycle longer to make sure we are called after the sender
			m_rcv_clock->adjust(m_rcv_rate * 2, 0, m_rcv_rate);
		}
		else if (m_start_bit_hack_for_external_clocks)
			m_rcv_bit_count_received--;
	}
	return;
}

/* this is generic code to be used in serial chip implementations */
/* the majority of serial chips work in the same way and this code will work */
/* for them */

/* receive a bit */
void device_serial_interface::receive_register_update_bit(int bit)
{
	int previous_bit = BIT(m_rcv_register_data, 15);

	/* shift previous bit 7 out */
	m_rcv_register_data = m_rcv_register_data >> 1;
	/* shift new bit in */
	m_rcv_register_data = (m_rcv_register_data & 0x7fff) | (bit << 15);
	/* update bit count received */

	/* asynchronous mode */
	if (m_rcv_flags & RECEIVE_REGISTER_WAITING_FOR_START_BIT)
	{
		/* the previous bit is stored in uart.receive char bit 0 */
		/* has the bit state changed? */
		if ((previous_bit ^ bit) & 0x01)
		{
			/* yes */
			if (bit == 0)
			{
				LOGMASKED(LOG_RX, "Receiver saw start bit (%s)\n", device().machine().time().to_string());

				/* seen start bit! */
				/* not waiting for start bit now! */
				m_rcv_flags &= ~RECEIVE_REGISTER_WAITING_FOR_START_BIT;
				m_rcv_flags |= RECEIVE_REGISTER_SYNCHRONISED;
				/* reset bit count received */
				m_rcv_bit_count_received = 0;
				m_rcv_framing_error = false;
				m_rcv_parity_error = false;
			}
			else
			{
				LOGMASKED(LOG_RX, "Receiver saw stop bit (%s)\n", device().machine().time().to_string());
			}
		}
	}
	else if (m_rcv_flags & RECEIVE_REGISTER_SYNCHRONISED)
	{
		LOGMASKED(LOG_RX, "Received bit %d as %d (%s)\n", m_rcv_bit_count_received, bit, device().machine().time().to_string());
		m_rcv_bit_count_received++;

		if (!bit && (m_rcv_bit_count_received > (m_rcv_bit_count - m_df_min_rx_stop_bit_count)))
		{
			LOGMASKED(LOG_RX, "Framing error\n");
			m_rcv_framing_error = true;
		}

		/* received all bits? */
		if (m_rcv_bit_count_received == m_rcv_bit_count)
		{
			m_rcv_bit_count_received = 0;
			m_rcv_flags &= ~RECEIVE_REGISTER_SYNCHRONISED;
			m_rcv_flags |= RECEIVE_REGISTER_WAITING_FOR_START_BIT;
			LOGMASKED(LOG_RX, "Receive register full\n");
			m_rcv_flags |= RECEIVE_REGISTER_FULL;
		}
	}
}

void device_serial_interface::receive_register_extract()
{
	u8 data;

	receive_register_reset();

	/* strip off stop bits and parity */
	assert(m_rcv_bit_count > 0 && m_rcv_bit_count <= 16);
	data = m_rcv_register_data >> (16 - m_rcv_bit_count);

	/* mask off other bits so data byte has 0's in unused bits */
	data &= ~(0xff << m_df_word_length);

	m_rcv_byte_received = data;
	LOGMASKED(LOG_RX, "Receive data 0x%02x\n", m_rcv_byte_received);

	if (m_df_parity == PARITY_NONE)
		return;

	/* get state of parity bit received */
	u8 parity_received = (m_rcv_register_data >> (16 - m_rcv_bit_count + m_df_word_length)) & 0x01;

	/* parity enable? */
	switch (m_df_parity)
	{
	case PARITY_ODD:
		if (parity_received == BIT(std::popcount(data), 0))
			m_rcv_parity_error = true;
		break;

	case PARITY_EVEN:
		if (parity_received != BIT(std::popcount(data), 0))
			m_rcv_parity_error = true;
		break;

	case PARITY_MARK:
		if (!parity_received)
			m_rcv_parity_error = true;
		break;

	case PARITY_SPACE:
		if (parity_received)
			m_rcv_parity_error = true;
		break;
	}
}


/***** TRANSMIT REGISTER *****/

void device_serial_interface::transmit_register_reset()
{
	m_tra_flags |=TRANSMIT_REGISTER_EMPTY;
}

/* used to construct data in stream format */
void device_serial_interface::transmit_register_add_bit(int bit)
{
	/* combine bit */
	m_tra_register_data = m_tra_register_data << 1;
	m_tra_register_data |= (bit & 0x01);
	m_tra_bit_count++;
}


/* generate data in stream format ready for transfer */
void device_serial_interface::transmit_register_setup(u8 data_byte)
{
	u8 transmit_data;

	if (m_tra_clock && !m_tra_rate.is_never())
		m_tra_clock->adjust(m_tra_rate, 0, m_tra_rate);

	m_tra_bit_count_transmitted = 0;
	m_tra_bit_count = 0;
	m_tra_flags &= ~TRANSMIT_REGISTER_EMPTY;

	/* start bit */
	for (int i = 0; i < m_df_start_bit_count; i++)
	{
		transmit_register_add_bit(0);
	}

	/* data bits */
	transmit_data = data_byte;
	for (int i = 0; i < m_df_word_length; i++)
	{
		/* add bit to formatted byte */
		transmit_register_add_bit(BIT(transmit_data, 0));
		transmit_data >>= 1;
	}

	/* parity */
	if (m_df_parity != PARITY_NONE)
	{
		/* odd or even parity */
		u8 parity = 0;
		switch (m_df_parity)
		{
		case PARITY_ODD:
			// get parity
			// if parity[0] = 0, data has even parity - i.e. there is an even number of one bits in the data
			// if parity[0] = 1, data has odd parity - i.e. there is an odd number of one bits in the data
			parity = BIT(std::popcount(data_byte), 0) ^ 1;
			break;
		case PARITY_EVEN:
			parity = BIT(std::popcount(data_byte), 0);
			break;
		case PARITY_MARK:
			parity = 1;
			break;
		case PARITY_SPACE:
			parity = 0;
			break;
		}
		transmit_register_add_bit(parity);
	}

	/* TX stop bit(s) */
	for (int i = 0; i < m_df_stop_bit_count; i++)
		transmit_register_add_bit(1);
}


/* get a bit from the transmit register */
u8 device_serial_interface::transmit_register_get_data_bit()
{
	if (!m_tra_bit_count || m_tra_bit_count_transmitted >= m_tra_bit_count) throw Fault("unconfigured/exhausted serial frame");
	int bit = (m_tra_register_data >> (m_tra_bit_count - 1 - m_tra_bit_count_transmitted)) & 1;

	if (m_tra_bit_count_transmitted < m_df_start_bit_count)
		LOGMASKED(LOG_TX, "Transmitting start bit %d as %d (%s)\n", m_tra_bit_count_transmitted, bit, device().machine().time().to_string());
	else
		LOGMASKED(LOG_TX, "Transmitting bit %d as %d (%s)\n", m_tra_bit_count_transmitted - m_df_start_bit_count, bit, device().machine().time().to_string());
	m_tra_bit_count_transmitted++;

	/* have all bits of this stream formatted byte been sent? */
	if (m_tra_bit_count_transmitted==m_tra_bit_count)
	{
		/* yes - generate a new byte to send */
		LOGMASKED(LOG_TX, "Transmit register empty\n");
		m_tra_flags |= TRANSMIT_REGISTER_EMPTY;
	}

	return bit;
}

const char *device_serial_interface::parity_tostring(parity_t parity)
{
	switch (parity)
	{
	case PARITY_NONE:
		return "NONE";

	case PARITY_ODD:
		return "ODD";

	case PARITY_EVEN:
		return "EVEN";

	case PARITY_MARK:
		return "MARK";

	case PARITY_SPACE:
		return "SPACE";

	default:
		return "UNKNOWN";
	}
}

const char *device_serial_interface::stop_bits_tostring(stop_bits_t stop_bits)
{
	switch (stop_bits)
	{
	case STOP_BITS_0:
		return "0";

	case STOP_BITS_1:
		return "1";

	case STOP_BITS_1_5:
		return "1.5";

	case STOP_BITS_2:
		return "2";

	default:
		return "UNKNOWN";
	}
}

/* int's pending */
#define COM_INT_PENDING_RECEIVED_DATA_AVAILABLE 0x0001
#define COM_INT_PENDING_TRANSMITTER_HOLDING_REGISTER_EMPTY 0x0002
#define COM_INT_PENDING_RECEIVER_LINE_STATUS 0x0004
#define COM_INT_PENDING_MODEM_STATUS_REGISTER 0x0008
#define COM_INT_PENDING_CHAR_TIMEOUT 0x0011

static constexpr u8 INS8250_LSR_TSRE = 0x40;
static constexpr u8 INS8250_LSR_THRE = 0x20;
static constexpr u8 INS8250_LSR_BI = 0x10;
static constexpr u8 INS8250_LSR_FE = 0x08;
static constexpr u8 INS8250_LSR_PE = 0x04;
static constexpr u8 INS8250_LSR_OE = 0x02;
static constexpr u8 INS8250_LSR_DR = 0x01;

static constexpr u8 INS8250_MCR_DTR = 0x01;
static constexpr u8 INS8250_MCR_RTS = 0x02;
static constexpr u8 INS8250_MCR_OUT1 = 0x04;
static constexpr u8 INS8250_MCR_OUT2 = 0x08;
static constexpr u8 INS8250_MCR_LOOPBACK = 0x10;

static constexpr u8 INS8250_LCR_BITCOUNT_MASK= 0x03;
static constexpr u8 INS8250_LCR_2STOP_BITS = 0x04;
//static constexpr u8 INS8250_LCR_PEN = 0x08;
//static constexpr u8 INS8250_LCR_EVEN_PAR = 0x10;
//static constexpr u8 INS8250_LCR_PARITY = 0x20;
static constexpr u8 INS8250_LCR_BREAK = 0x40;
static constexpr u8 INS8250_LCR_DLAB = 0x80;

/* ints will continue to be set for as long as there are ints pending */
void ins8250_uart_device::update_interrupt()
{
	int state;

	/* if any bits are set and are enabled */
	if (((m_int_pending & m_regs.ier) & 0x0f) != 0)
	{
		/* trigger next highest priority int */

		/* set int */
		state = 1;
		m_regs.iir &= ~(0x08|0x04|0x02);

		/* highest to lowest */
		if (m_regs.ier & m_int_pending & COM_INT_PENDING_RECEIVER_LINE_STATUS)
			m_regs.iir |=0x04|0x02;
		else if (m_regs.ier & m_int_pending & COM_INT_PENDING_RECEIVED_DATA_AVAILABLE)
		{
			m_regs.iir |=0x04;
			if ((m_int_pending & COM_INT_PENDING_CHAR_TIMEOUT) == 0x11)
				m_regs.iir |= 0x08;
		}
		else if (m_regs.ier & m_int_pending & COM_INT_PENDING_TRANSMITTER_HOLDING_REGISTER_EMPTY)
			m_regs.iir |=0x02;

		/* int pending */
		m_regs.iir &= ~0x01;
	}
	else
	{
		/* clear int */
		state = 0;

		/* no ints pending */
		m_regs.iir |= 0x01;
		/* priority level */
		m_regs.iir &= ~(0x08|0x04|0x02);
	}

	/* set or clear the int */
	m_out_int_cb(state);
}

/* set pending bit and trigger int */
void ins8250_uart_device::trigger_int(int flag)
{
	m_int_pending |= flag;
	update_interrupt();
}

/* clear pending bit, if any ints are pending, then int will be triggered, otherwise it
will be cleared */
void ins8250_uart_device::clear_int(int flag)
{
	m_int_pending &= ~flag;
	update_interrupt();
}

int ins8250_uart_device::intrpt_r()
{
	return !BIT(m_regs.iir, 0);
}

// Baud rate generator is reset after writing to either byte of divisor latch
void ins8250_uart_device::update_baud_rate()
{
	LOG("%.1f baud selected (divisor = %d)\n", double(clock()) / (m_regs.dl * 16), m_regs.dl);
	set_rate(clock(), m_regs.dl * 16);

	// FIXME: Baud rate generator should not affect transmitter or receiver, but device_serial_interface resets them regardless.
	// If the transmitter is still running at this time and we don't flush it, the shift register will never be emptied!
	if (!(m_regs.lsr & INS8250_LSR_TSRE))
		tra_complete();
}

void ins8250_uart_device::ins8250_w(offs_t offset, u8 data)
{
	int tmp;

	switch (offset)
	{
		case 0:
			if (m_regs.lcr & INS8250_LCR_DLAB)
			{
				m_regs.dl = (m_regs.dl & 0xff00) | data;
				update_baud_rate();
			}
			else
			{
				m_regs.thr = data;
				m_regs.lsr &= ~INS8250_LSR_THRE;
				if((m_device_type >= dev_type::NS16550) && (m_regs.fcr & 1))
					push_tx(data);
				clear_int(COM_INT_PENDING_TRANSMITTER_HOLDING_REGISTER_EMPTY);
				if(m_regs.lsr & INS8250_LSR_TSRE)
					tra_complete();
			}
			break;
		case 1:
			if (m_regs.lcr & INS8250_LCR_DLAB)
			{
				m_regs.dl = (m_regs.dl & 0xff) | (data << 8);
				update_baud_rate();
			}
			else
			{
				if ((m_regs.lsr & INS8250_LSR_THRE) && (data & COM_INT_PENDING_TRANSMITTER_HOLDING_REGISTER_EMPTY))
					trigger_int(COM_INT_PENDING_TRANSMITTER_HOLDING_REGISTER_EMPTY);
				m_regs.ier = data;
				update_interrupt();
			}
			break;
		case 2:
			set_fcr(data);
			break;
		case 3:
			{
			bool break_state_changed = bool((m_regs.lcr ^ data) & INS8250_LCR_BREAK);
			bool data_frame_changed = bool((m_regs.lcr ^ data) & 0x3f);

			m_regs.lcr = data;

			int data_bit_count = (m_regs.lcr & INS8250_LCR_BITCOUNT_MASK) + 5;
			parity_t parity;
			stop_bits_t stop_bits;

			switch ((m_regs.lcr>>3) & 7)
			{
			case 1:
				parity = PARITY_ODD;
				break;

			case 3:
				parity = PARITY_EVEN;
				break;

			case 5:
				parity = PARITY_MARK;
				break;

			case 7:
				parity = PARITY_SPACE;
				break;

			default:
				parity = PARITY_NONE;
				break;
			}

			if (!(m_regs.lcr & INS8250_LCR_2STOP_BITS))
				stop_bits = STOP_BITS_1;
			else if (data_bit_count == 5)
				stop_bits = STOP_BITS_1_5;
			else
				stop_bits = STOP_BITS_2;

			if (break_state_changed)
			{
				int new_out_val = (m_regs.lcr & INS8250_LCR_BREAK) ? 0 : m_txd;

				if (m_regs.mcr & INS8250_MCR_LOOPBACK)
				{
					device_serial_interface::rx_w(new_out_val);
				}
				else
				{
					m_out_tx_cb(new_out_val);
				}
			}
			if (data_frame_changed)
			{
				set_data_frame(1, data_bit_count, parity, stop_bits);
			}
			}
			break;
		case 4:
			if ( ( m_regs.mcr & 0x1f ) != ( data & 0x1f ) )
			{
				m_regs.mcr = data & 0x1f;

				update_msr();

				if (m_regs.mcr & INS8250_MCR_LOOPBACK)
				{
					m_out_tx_cb(1);
					if ((m_regs.lcr & INS8250_LCR_BREAK) == 0)
					{
						device_serial_interface::rx_w(m_txd);
					}
					m_out_dtr_cb(1);
					m_out_rts_cb(1);
					m_out_out1_cb(1);
					m_out_out2_cb(1);
				}
				else
				{
					if ((m_regs.lcr & INS8250_LCR_BREAK) == 0)
					{
						m_out_tx_cb(m_txd);
					}
					device_serial_interface::rx_w(m_rxd);
					m_out_dtr_cb((m_regs.mcr & INS8250_MCR_DTR) ? 0 : 1);
					m_out_rts_cb((m_regs.mcr & INS8250_MCR_RTS) ? 0 : 1);
					m_out_out1_cb((m_regs.mcr & INS8250_MCR_OUT1) ? 0 : 1);
					m_out_out2_cb((m_regs.mcr & INS8250_MCR_OUT2) ? 0 : 1);
				}
			}
			break;
		case 5:
			/*
			  This register can be written, but if you write a 1 bit into any of
			  bits 5 - 0, you could cause an interrupt if the appropriate IER bit
			  is set.
			*/
			m_regs.lsr = (m_regs.lsr & (INS8250_LSR_TSRE|INS8250_LSR_THRE)) | (data & ~(INS8250_LSR_TSRE|INS8250_LSR_THRE));

			tmp = 0;
			tmp |= ( m_regs.lsr & INS8250_LSR_DR ) ? COM_INT_PENDING_RECEIVED_DATA_AVAILABLE : 0;
			tmp |= ( m_regs.lsr & 0x1e ) ? COM_INT_PENDING_RECEIVER_LINE_STATUS : 0;
			tmp |= ( m_regs.lsr & INS8250_LSR_THRE ) ? COM_INT_PENDING_TRANSMITTER_HOLDING_REGISTER_EMPTY : 0;
			trigger_int(tmp);

			break;
		case 6:
			/*
			  This register can be written, but if you write a 1 bit into any of
			  bits 3 - 0, you could cause an interrupt if the appropriate IER bit
			  is set.
			  Bits 7 - 4 are read-only.
			 */
			m_regs.msr = (m_regs.msr & 0xf0) | (data & 0x0f);

			if (m_regs.msr & 0x0f)
				trigger_int(COM_INT_PENDING_MODEM_STATUS_REGISTER);
			else
				clear_int(COM_INT_PENDING_MODEM_STATUS_REGISTER);
			break;
		case 7:
			if (m_device_type >= dev_type::INS8250A)
			{
				m_regs.scr = data;
			}
			break;
	}
}

u8 ins8250_uart_device::ins8250_r(offs_t offset)
{
	int data = 0x0ff;

	switch (offset)
	{
		case 0:
			if (m_regs.lcr & INS8250_LCR_DLAB)
				data = (m_regs.dl & 0xff);
			else
			{
				if (!machine().side_effects_disabled())
				{
					if ((m_device_type >= dev_type::NS16550) && (m_regs.fcr & 1))
						m_regs.rbr = pop_rx();
					else
					{
						clear_int(COM_INT_PENDING_RECEIVED_DATA_AVAILABLE);
						if (m_regs.lsr & INS8250_LSR_DR)
							m_regs.lsr &= ~INS8250_LSR_DR;
					}
				}
				data = m_regs.rbr;
			}
			break;
		case 1:
			if (m_regs.lcr & INS8250_LCR_DLAB)
				data = (m_regs.dl >> 8);
			else
				data = m_regs.ier & 0x0f;
			break;
		case 2:
			data = m_regs.iir;
			/* The documentation says that reading this register will
			clear the int if this is the source of the int */
			if (!machine().side_effects_disabled())
			{
				if ((m_regs.iir & 0x0f) == 0x02)
					clear_int(COM_INT_PENDING_TRANSMITTER_HOLDING_REGISTER_EMPTY);
			}
			break;
		case 3:
			data = m_regs.lcr;
			break;
		case 4:
			data = m_regs.mcr;
			break;
		case 5:
			data = m_regs.lsr;
			if (!machine().side_effects_disabled() && (m_regs.lsr & (INS8250_LSR_BI | INS8250_LSR_FE | INS8250_LSR_PE | INS8250_LSR_OE)) != 0)
			{
				m_regs.lsr &= 0xe1; /* clear FE, PE and OE and BREAK bits */

				/* reading line status register clears int */
				clear_int(COM_INT_PENDING_RECEIVER_LINE_STATUS);
			}
			break;
		case 6:
			data = m_regs.msr;
			if (!machine().side_effects_disabled())
			{
				m_regs.msr &= 0xf0; /* reset delta values */

				/* reading msr clears int */
				clear_int(COM_INT_PENDING_MODEM_STATUS_REGISTER);
			}
			break;
		case 7:
			if (m_device_type >= dev_type::INS8250A)
			{
				data = m_regs.scr;
			}
			break;
	}
	return data;
}

void ns16550_device::rcv_complete()
{
	if(!(m_regs.fcr & 1))
		return ins8250_uart_device::rcv_complete();

	receive_register_extract();

	if(m_rnum == 16)
	{
		m_regs.lsr |= INS8250_LSR_OE; //overrun
		trigger_int(COM_INT_PENDING_RECEIVER_LINE_STATUS);
		return;
	}

	u8 errors = 0;
	if (is_receive_framing_error())
		errors |= INS8250_LSR_FE;
	if (is_receive_parity_error())
		errors |= INS8250_LSR_PE;
	if (m_rnum == 0 && errors != 0)
	{
		m_regs.lsr |= errors;
		trigger_int(COM_INT_PENDING_RECEIVER_LINE_STATUS);
	}

	m_regs.lsr |= INS8250_LSR_DR;
	m_rfifo[m_rhead] = get_received_char();
	m_efifo[m_rhead] = errors;
	++m_rhead &= 0x0f;
	m_rnum++;
	if(m_rnum >= m_rintlvl)
		trigger_int(COM_INT_PENDING_RECEIVED_DATA_AVAILABLE);
	set_timer();
}

void ns16550_device::tra_complete()
{
	if(!(m_regs.fcr & 1))
		return ins8250_uart_device::tra_complete();

	if(m_ttail != m_thead)
	{
		transmit_register_setup(m_tfifo[m_ttail]);
		++m_ttail &= 0x0f;
		m_regs.lsr &= ~INS8250_LSR_TSRE;
		if(m_ttail == m_thead)
		{
			m_regs.lsr |= INS8250_LSR_THRE;
			trigger_int(COM_INT_PENDING_TRANSMITTER_HOLDING_REGISTER_EMPTY);
		}
	}
	else
		m_regs.lsr |= 0x40;
}

void ins8250_uart_device::rcv_complete()
{
	// According to datasheet (and HP82939 self-test) the received character is always
	// extracted and stored in RBR even in case of overrun
	receive_register_extract();
	if(m_regs.lsr & INS8250_LSR_DR)
	{
		m_regs.lsr |= INS8250_LSR_OE; //overrun
	}
	m_regs.lsr |= INS8250_LSR_DR;

	if (is_receive_framing_error())
		m_regs.lsr |= INS8250_LSR_FE;
	if (is_receive_parity_error())
		m_regs.lsr |= INS8250_LSR_PE;
	if ((m_regs.lsr & (INS8250_LSR_BI | INS8250_LSR_PE | INS8250_LSR_FE | INS8250_LSR_OE)) != 0)
		trigger_int(COM_INT_PENDING_RECEIVER_LINE_STATUS);

	m_regs.rbr = get_received_char();
	trigger_int(COM_INT_PENDING_RECEIVED_DATA_AVAILABLE);
}

void ins8250_uart_device::tra_complete()
{
	if(!(m_regs.lsr & INS8250_LSR_THRE))
	{
		transmit_register_setup(m_regs.thr);
		m_regs.lsr &= ~INS8250_LSR_TSRE;
		m_regs.lsr |= INS8250_LSR_THRE;
		trigger_int(COM_INT_PENDING_TRANSMITTER_HOLDING_REGISTER_EMPTY);
	}
	else
		m_regs.lsr |= INS8250_LSR_TSRE;
}

void ins8250_uart_device::tra_callback()
{
	m_txd = transmit_register_get_data_bit();

	if (m_regs.lcr & INS8250_LCR_BREAK)
	{
		// in break mode, don't change transmitted bit
		return;
	}

	if (m_regs.mcr & INS8250_MCR_LOOPBACK)
	{
		device_serial_interface::rx_w(m_txd);
	}
	else
	{
		m_out_tx_cb(m_txd);
	}
}

void ins8250_uart_device::update_msr()
{
	u8 data;
	int change;

	if (m_regs.mcr & INS8250_MCR_LOOPBACK)
	{
		data = (((m_regs.mcr & (INS8250_MCR_OUT1|INS8250_MCR_OUT2)) << 4) |
			((m_regs.mcr & INS8250_MCR_DTR) << 5) | ((m_regs.mcr & INS8250_MCR_RTS) << 3));
		change = (m_regs.msr ^ data) >> 4;
		if(!(m_regs.msr & 0x40) && (data & 0x40))
			change &= ~4;
	}
	else
	{
		data = (!m_dcd << 7) | (!m_ri << 6) | (!m_dsr << 5) | (!m_cts << 4);
		change = (m_regs.msr ^ data) >> 4;
	}

	m_regs.msr = data | change;

	if(change)
		trigger_int(COM_INT_PENDING_MODEM_STATUS_REGISTER);
}

void ins8250_uart_device::dcd_w(int state)
{
	m_dcd = state;
	update_msr();
}

void ins8250_uart_device::dsr_w(int state)
{
	m_dsr = state;
	update_msr();
}

void ins8250_uart_device::ri_w(int state)
{
	m_ri = state;
	update_msr();
}

void ins8250_uart_device::cts_w(int state)
{
	m_cts = state;
	update_msr();
}

void ins8250_uart_device::rx_w(int state)
{
	m_rxd = state;

	if (!(m_regs.mcr & INS8250_MCR_LOOPBACK))
		device_serial_interface::rx_w(m_rxd);
}

void ins8250_uart_device::device_start()
{
	set_tra_rate(0);
	set_rcv_rate(0);

	save_item(NAME(m_regs.thr));
	save_item(NAME(m_regs.rbr));
	save_item(NAME(m_regs.ier));
	save_item(NAME(m_regs.dl));
	save_item(NAME(m_regs.iir));
	save_item(NAME(m_regs.fcr));
	save_item(NAME(m_regs.lcr));
	save_item(NAME(m_regs.mcr));
	save_item(NAME(m_regs.lsr));
	save_item(NAME(m_regs.msr));
	save_item(NAME(m_regs.scr));
	save_item(NAME(m_int_pending));
	save_item(NAME(m_txd));
	save_item(NAME(m_rxd));
	save_item(NAME(m_dcd));
	save_item(NAME(m_dsr));
	save_item(NAME(m_ri));
	save_item(NAME(m_cts));
}

void ins8250_uart_device::device_reset()
{
	m_regs.ier = 0;
	m_regs.iir = 1;
	m_regs.lcr = 0;
	m_regs.mcr = 0;
	m_regs.lsr = INS8250_LSR_THRE | INS8250_LSR_TSRE;
	update_msr();
	m_regs.msr &= 0xf0;
	m_int_pending = 0;
	update_interrupt();
	receive_register_reset();
	transmit_register_reset();
	m_txd = 1;
	m_out_tx_cb(1);
	m_out_rts_cb(1);
	m_out_dtr_cb(1);
	m_out_out1_cb(1);
	m_out_out2_cb(1);
}

void ns16550_device::device_start()
{
	m_timeout = timer_alloc(FUNC(ns16550_device::timeout_expired), this);
	ins8250_uart_device::device_start();
	save_item(NAME(m_rintlvl));
	save_item(NAME(m_rfifo));
	save_item(NAME(m_efifo));
	save_item(NAME(m_tfifo));
	save_item(NAME(m_rhead));
	save_item(NAME(m_rtail));
	save_item(NAME(m_rnum));
	save_item(NAME(m_thead));
	save_item(NAME(m_ttail));
}

void ns16550_device::device_reset()
{
	std::fill(std::begin(m_rfifo), std::end(m_rfifo), 0);
	std::fill(std::begin(m_efifo), std::end(m_efifo), 0);
	std::fill(std::begin(m_tfifo), std::end(m_tfifo), 0);
	m_rhead = m_rtail = m_rnum = 0;
	m_thead = m_ttail = 0;
	m_timeout->adjust(attotime::never);
	ins8250_uart_device::device_reset();
}

TIMER_CALLBACK_MEMBER(ns16550_device::timeout_expired)
{
	trigger_int(COM_INT_PENDING_CHAR_TIMEOUT);
	m_timeout->adjust(attotime::never);
}

void ns16550_device::push_tx(u8 data)
{
	m_tfifo[m_thead] = data;
	++m_thead &= 0x0f;
}

u8 ns16550_device::pop_rx()
{
	u8 data = m_rfifo[m_rtail];
	clear_int(COM_INT_PENDING_CHAR_TIMEOUT & ~1); // don't clear bit 1 yet

	if(m_rnum)
	{
		++m_rtail &= 0x0f;
		m_rnum--;
		if (m_rnum > 0 && m_efifo[m_rtail] != 0)
		{
			m_regs.lsr |= m_efifo[m_rtail];
			trigger_int(COM_INT_PENDING_RECEIVER_LINE_STATUS);
		}
	}
	else
		data = 0;

	if(m_rnum < m_rintlvl)
		clear_int(COM_INT_PENDING_RECEIVED_DATA_AVAILABLE);

	if(m_rnum)
		set_timer();
	else
	{
		m_timeout->adjust(attotime::never);
		m_regs.lsr &= ~INS8250_LSR_DR;
	}

	return data;
}

void ns16550_device::set_fcr(u8 data)
{
	const int bytes_per_int[] = {1, 4, 8, 14};
	if(!(data & 1))
	{
		m_regs.fcr = 0;
		m_regs.iir &= ~0xc8;
		return;
	}
	if(!(m_regs.fcr & 1) && (data & 1))
		data |= 0x06;
	if(data & 2)
	{
		std::fill(std::begin(m_rfifo), std::end(m_rfifo), 0);
		std::fill(std::begin(m_efifo), std::end(m_efifo), 0);
		m_rhead = m_rtail = m_rnum = 0;
		clear_int(COM_INT_PENDING_CHAR_TIMEOUT | COM_INT_PENDING_RECEIVED_DATA_AVAILABLE);
		m_timeout->adjust(attotime::never);
	}
	if(data & 4)
	{
		std::fill(std::begin(m_tfifo), std::end(m_tfifo), 0);
		m_thead = m_ttail = 0;
		m_regs.lsr |= INS8250_LSR_THRE;
		trigger_int(COM_INT_PENDING_TRANSMITTER_HOLDING_REGISTER_EMPTY);
	}
	m_rintlvl = bytes_per_int[(data>>6)&3];
	m_regs.iir |= 0xc0;
	m_regs.fcr = data & 0xc9;
}

void fdc37c665gt_device::device_start()
{
	// Configuration registers and related bits aren't affected by soft resets
	// Default addresses
	com_addresses[0] = 0x3f8;
	com_addresses[1] = 0x2f8;
	com_addresses[2] = 0x338;
	com_addresses[3] = 0x238;

	device_addresses[LogicalDevice::IDE] = 0x1f0;
	device_addresses[LogicalDevice::FDC] = 0x3f0;
	device_addresses[LogicalDevice::Parallel] = 0x278;
	device_addresses[LogicalDevice::Serial1] = 0; // COM port
	device_addresses[LogicalDevice::Serial2] = 1; // COM port

	const uint8_t configuration_registers_defaults[] = {
		0x3b, 0x9f, 0xdc, 0x78, 0x00,
		0x00, 0xff, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x66, 0x01, 0x00
	};

	// Set the value first and then use write_configuration_register because some flags
	// rely on other flags being initialized properly first
	std::copy(std::begin(configuration_registers_defaults), std::end(configuration_registers_defaults), std::begin(configuration_registers));
	for (int i = 0; i < std::size(configuration_registers_defaults); i++) {
		write_configuration_register(i, configuration_registers_defaults[i]);
	}
}

uint8_t fdc37c665gt_device::read(offs_t offset)
{
	// TODO: IDE not implemented

	// Parallel port
	if (offset >= device_addresses[LogicalDevice::Parallel] && offset <= device_addresses[LogicalDevice::Parallel] + 2) {
		if (!enabled_logical[LogicalDevice::Parallel]) {
			return 0;
		}

		return m_lpt->read(offset - device_addresses[LogicalDevice::Parallel]);
	}

	// Serial 1
	if (offset >= com_addresses[device_addresses[LogicalDevice::Serial1]] && offset <= com_addresses[device_addresses[LogicalDevice::Serial1]] + 7) {
		if (!enabled_logical[LogicalDevice::Serial1]) {
			return 0;
		}

		return m_serial[0]->ins8250_r(offset - com_addresses[device_addresses[LogicalDevice::Serial1]]);
	}

	// Serial 2
	if (offset >= com_addresses[device_addresses[LogicalDevice::Serial2]] && offset <= com_addresses[device_addresses[LogicalDevice::Serial2]] + 7) {
		if (!enabled_logical[LogicalDevice::Serial2]) {
			return 0;
		}

		return m_serial[1]->ins8250_r(offset - com_addresses[device_addresses[LogicalDevice::Serial2]]);
	}

	// FDC, +6 is used by IDE
	if ((offset >= device_addresses[LogicalDevice::FDC] && offset <= device_addresses[LogicalDevice::FDC] + 5)
		|| offset == device_addresses[LogicalDevice::FDC] + 7) {
		if (!enabled_logical[LogicalDevice::FDC]) {
			return 0;
		}

		switch (offset - device_addresses[LogicalDevice::FDC]) {
			case 0: return m_fdc->sra_r();
			case 1: return m_fdc->srb_r();
			case 2: return m_fdc->dor_r();
			case 3: return m_fdc->tdr_r();
			case 4: return m_fdc->msr_r();
			case 5: return m_fdc->fifo_r();
			case 7: return m_fdc->dir_r();
		}
	}

	return 0;
}

void fdc37c665gt_device::write(offs_t offset, uint8_t data)
{
	// TODO: IDE not implemented

	// Parallel port
	if (offset >= device_addresses[LogicalDevice::Parallel] && offset <= device_addresses[LogicalDevice::Parallel] + 2) {
		if (!enabled_logical[LogicalDevice::Parallel]) {
			return;
		}

		m_lpt->write(offset - device_addresses[LogicalDevice::Parallel], data);
		return;
	}

	// Serial 1
	if (offset >= com_addresses[device_addresses[LogicalDevice::Serial1]] && offset <= com_addresses[device_addresses[LogicalDevice::Serial1]] + 7) {
		if (!enabled_logical[LogicalDevice::Serial1]) {
			return;
		}

		m_serial[0]->ins8250_w(offset - com_addresses[device_addresses[LogicalDevice::Serial1]], data);
		return;
	}

	// Serial 2
	if (offset >= com_addresses[device_addresses[LogicalDevice::Serial2]] && offset <= com_addresses[device_addresses[LogicalDevice::Serial2]] + 7) {
		if (!enabled_logical[LogicalDevice::Serial2]) {
			return;
		}

		m_serial[1]->ins8250_w(offset - com_addresses[device_addresses[LogicalDevice::Serial2]], data);
		return;
	}

	// FDC, +6 is used by IDE
	if ((offset >= device_addresses[LogicalDevice::FDC] && offset <= device_addresses[LogicalDevice::FDC] + 5)
		|| offset == device_addresses[LogicalDevice::FDC] + 7) {
		auto fdc_offset = offset - device_addresses[LogicalDevice::FDC];

		if ((!enabled_logical[LogicalDevice::FDC] && fdc_offset > 1)) {
			return;
		}

		switch (fdc_offset) {
			case 0: // FDC37C665GT Configuration
				if (mode == OperatingMode::Run) {
					if (data != 0x55) {
						config_key_step = 0;
						return;
					}

					config_key_step++;
					if (config_key_step > 1) {
						config_key_step = 0;
						mode = OperatingMode::Configuration;
					}
				} else {
					if (data == 0xaa) {
						mode = OperatingMode::Run;
						return;
					}

					config_index = data;
				}
				return;

			case 1: // FDC37C665GT Configuration
				if (mode == OperatingMode::Run) {
					config_key_step = 0;
					return;
				}

				write_configuration_register(config_index, data & 0xff);
				return;

			case 2: m_fdc->dor_w(data); return;
			case 3: m_fdc->tdr_w(data); return;
			case 4: m_fdc->dsr_w(data); return;
			case 5: m_fdc->fifo_w(data); return;
			case 7: m_fdc->ccr_w(data); return;
		}
	}
}

void fdc37c665gt_device::write_configuration_register(int index, int data)
{
	if (index < 0 || index >= 16) throw Fault("FDC configuration index outside source array");
	if (BIT(configuration_registers[1], 7) == 0) {
		// Bit 7 of CR1 is LOCK CRx
		// When this is set to 0, it can only be set back to 1 by a hard reset or power-up reset
		LOGCONFIG("IGNORED configuration register cr[%02x] = %02x\n", index, data);
		return;
	}

	configuration_registers[index] = data;
	LOGCONFIG("Modified configuration register cr[%02x] = %02x\n", index, data);

	if (index == 0) {
		enabled_logical[LogicalDevice::IDE] = BIT(configuration_registers[index], 1);
		enabled_logical[LogicalDevice::FDC] = BIT(configuration_registers[index], 3) && BIT(configuration_registers[index], 4);
	} else if (index == 1) {
		enabled_logical[LogicalDevice::Parallel] = BIT(configuration_registers[index], 2) && BIT(configuration_registers[index], 3);

		auto lpt_port = BIT(configuration_registers[index], 0, 2);
		if (lpt_port == 0) {
			enabled_logical[LogicalDevice::Parallel] = false; // Disabled
		} else if (lpt_port == 1) {
			device_addresses[LogicalDevice::Parallel] = 0x3bc;
		} else if (lpt_port == 2) {
			device_addresses[LogicalDevice::Parallel] = 0x378;
		} else if (lpt_port == 3) {
			device_addresses[LogicalDevice::Parallel] = 0x278; // Default
		}

		auto com34 = BIT(configuration_registers[index], 5, 2);
		if (com34 == 0) {
			com_addresses[2] = 0x338;
			com_addresses[3] = 0x238;
		} else if (com34 == 1) {
			com_addresses[2] = 0x3e8;
			com_addresses[3] = 0x2e8;
		} else if (com34 == 2) {
			com_addresses[2] = 0x2e8;
			com_addresses[3] = 0x2e0;
		} else if (com34 == 3) {
			com_addresses[2] = 0x220;
			com_addresses[3] = 0x228;
		}
	} else if (index == 2) {
		enabled_logical[LogicalDevice::Serial1] = BIT(configuration_registers[index], 2) && BIT(configuration_registers[index], 3);
		device_addresses[LogicalDevice::Serial1] = BIT(configuration_registers[index], 0, 2);

		enabled_logical[LogicalDevice::Serial2] = BIT(configuration_registers[index], 6) && BIT(configuration_registers[index], 7);
		device_addresses[LogicalDevice::Serial2] = BIT(configuration_registers[index], 4, 2);
	} else if (index == 3) {
		auto floppy_mode = BIT(configuration_registers[index], 5, 2);

		// 2 is reserved/unused
		if (floppy_mode == 3) {
			m_floppy_mode = upd765_family_device::mode_t::AT;
		} else if (floppy_mode == 1) {
			m_floppy_mode = upd765_family_device::mode_t::PS2;
		} else if (floppy_mode == 0) {
			m_floppy_mode = upd765_family_device::mode_t::M30;
		}

		m_fdc->set_mode(m_floppy_mode);
	} else if (index == 4) {
		// Set clock speeds for MIDI modes (clock divisor becomes 12 instead of 13)
		m_serial[0]->set_unscaled_clock(clock() / (13 - BIT(configuration_registers[4], 4)));
		m_serial[1]->set_unscaled_clock(clock() / (13 - BIT(configuration_registers[5], 5)));
	} else if (index == 5) {
		auto fdc_port = BIT(configuration_registers[index], 0);
		if (fdc_port == 0) {
			device_addresses[LogicalDevice::FDC] = 0x3f0;
		} else if (fdc_port == 1) {
			device_addresses[LogicalDevice::FDC] = 0x370;
		}
	}
}

void fdc37c665gt_device::irq_floppy_w(int state)
{
	if (!enabled_logical[LogicalDevice::FDC]) {
		return;
	}

	m_fintr_callback(state);
}

void fdc37c665gt_device::irq_parallel_w(int state)
{
	if (!enabled_logical[LogicalDevice::Parallel]) {
		return;
	}

	m_pintr1_callback(state);
}

void fdc37c665gt_device::irq_serial1_w(int state)
{
	if (!enabled_logical[LogicalDevice::Serial1]) {
		return;
	}

	m_irq4_callback(state);
}

void fdc37c665gt_device::txd_serial1_w(int state)
{
	if (!enabled_logical[LogicalDevice::Serial1]) {
		return;
	}

	m_txd1_callback(state);
}

void fdc37c665gt_device::dtr_serial1_w(int state)
{
	if (!enabled_logical[LogicalDevice::Serial1]) {
		return;
	}

	m_ndtr1_callback(state);
}

void fdc37c665gt_device::rts_serial1_w(int state)
{
	if (!enabled_logical[LogicalDevice::Serial1]) {
		return;
	}

	m_nrts1_callback(state);
}

void fdc37c665gt_device::irq_serial2_w(int state)
{
	if (!enabled_logical[LogicalDevice::Serial2]) {
		return;
	}

	m_irq3_callback(state);
}

void fdc37c665gt_device::txd_serial2_w(int state)
{
	if (!enabled_logical[LogicalDevice::Serial2]) {
		return;
	}

	m_txd2_callback(state);
}

void fdc37c665gt_device::dtr_serial2_w(int state)
{
	if (!enabled_logical[LogicalDevice::Serial2]) {
		return;
	}

	m_ndtr2_callback(state);
}

void fdc37c665gt_device::rts_serial2_w(int state)
{
	if (!enabled_logical[LogicalDevice::Serial2]) {
		return;
	}

	m_nrts2_callback(state);
}

void fdc37c665gt_device::rxd1_w(int state)
{
	m_serial[0]->rx_w(state);
}

void fdc37c665gt_device::ndcd1_w(int state)
{
	m_serial[0]->dcd_w(state);
}

void fdc37c665gt_device::ndsr1_w(int state)
{
	m_serial[0]->dsr_w(state);
}

void fdc37c665gt_device::nri1_w(int state)
{
	m_serial[0]->ri_w(state);
}

void fdc37c665gt_device::ncts1_w(int state)
{
	m_serial[0]->cts_w(state);
}

void fdc37c665gt_device::rxd2_w(int state)
{
	m_serial[1]->rx_w(state);
}

void fdc37c665gt_device::ndcd2_w(int state)
{
	m_serial[1]->dcd_w(state);
}

void fdc37c665gt_device::ndsr2_w(int state)
{
	m_serial[1]->dsr_w(state);
}

void fdc37c665gt_device::nri2_w(int state)
{
	m_serial[1]->ri_w(state);
}

void fdc37c665gt_device::ncts2_w(int state)
{
	m_serial[1]->cts_w(state);
}

void jvc_xvd701_device::device_start()
{
	int startbits = 1;
	int databits = 8;
	parity_t parity = PARITY_ODD;
	stop_bits_t stopbits = STOP_BITS_1;

	set_data_frame(startbits, databits, parity, stopbits);

	int txbaud = 9600;
	set_tra_rate(txbaud);

	int rxbaud = 9600;
	set_rcv_rate(rxbaud);

	output_rxd(1);

	// TODO: make this configurable
	output_dcd(0);
	output_dsr(0);
	output_ri(0);
	output_cts(0);

	m_timer_response = timer_alloc(FUNC(jvc_xvd701_device::send_response), this);
}

void jvc_xvd701_device::device_reset()
{
	memset(m_command, 0, sizeof(m_command));

	m_response_index = sizeof(m_response);

	m_jlip_id = 33; // Twinkle default
	m_is_powered = false;
	m_chapter = 0;
	m_playback_status = STATUS_STOP;
}

void jvc_xvd701_device::tra_callback()
{
	output_rxd(transmit_register_get_data_bit());
}

void jvc_xvd701_device::tra_complete()
{
	m_timer_response->adjust(attotime::from_msec(100));
}

unsigned char jvc_xvd701_device::sum(unsigned char *buffer, int length)
{
	int sum = 0x80;

	for (int i = 0; i < length; i++)
		sum -= buffer[i] & 0x7f;

	return sum & 0x7f;
}

void jvc_xvd701_device::create_packet(unsigned char status, const unsigned char response[6])
{
	m_response[0] = 0xfc;
	m_response[1] = 0xff;
	m_response[2] = m_jlip_id;
	m_response[3] = status;
	memcpy(&m_response[4], response, 6);
	m_response[10] = sum(m_response, sizeof(m_response) - 1);

	m_response_index = 0;
	m_timer_response->adjust(attotime::from_msec(100));
}

TIMER_CALLBACK_MEMBER(jvc_xvd701_device::send_response)
{
	if (m_response_index < sizeof(m_response) && is_transmit_register_empty())
	{
//      printf("sending %02x\n", m_response[m_response_index]);
		transmit_register_setup(m_response[m_response_index++]);
	}
}

bool jvc_xvd701_device::seek_chapter(int chapter)
{
	if (chapter <= 0)
	{
		// Chapters are from 1 and up
		return false;
	}

	m_chapter = chapter;

	if (m_playback_status != STATUS_PAUSE)
		m_playback_status = STATUS_PLAYING;

	return true;
}

void jvc_xvd701_device::rcv_complete()
{
	receive_register_extract();

	for (int i = 0; i < sizeof(m_command) - 1; i++)
		m_command[i] = m_command[i + 1];

	m_command[sizeof(m_command) - 1] = get_received_char();

	if (m_command[0] == 0xff &&
		m_command[1] == 0xff &&
		m_command[10] == sum(m_command, sizeof(m_command) - 1))
	{
		if (m_command[3] == 0x0c && m_command[4] == 0x43 && m_command[5] == 0x6d)
		{
			// FF FF 21 0C 43 6D 00 00 00 00 25 PAUSE
			LOGCMD("xvd701: Playback PAUSE\n");
			m_playback_status = STATUS_PAUSE;
			create_packet(STATUS_OK, NO_RESPONSE);
		}
		else if (m_command[3] == 0x0c && m_command[4] == 0x43 && m_command[5] == 0x75)
		{
			// FF FF 21 0C 43 75 00 00 00 00 1D PLAY
			LOGCMD("xvd701: Playback PLAY\n");

			auto status = STATUS_OK;
			if (m_playback_status == STATUS_STOP)
			{
				// Force video to load again if the video was stopped then started again
				if (!seek_chapter(m_chapter))
					status = STATUS_ERROR;
			}

			if (status == STATUS_OK)
				m_playback_status = STATUS_PLAYING;

			create_packet(status, NO_RESPONSE);
		}
		else if (m_command[3] == 0x0c && m_command[4] == 0x44 && m_command[5] == 0x60)
		{
			// FF FF 21 0C 44 60 00 00 00 00 31 STOP
			LOGCMD("xvd701: Playback STOP\n");

			m_playback_status = STATUS_STOP;
			create_packet(STATUS_OK, NO_RESPONSE);
		}
		else if (m_command[3] == 0x0c && m_command[4] == 0x50 && m_command[5] == 0x20)
		{
			// FF FF 21 0C 50 20 00 00 00 00 63 SEEK TO SPECIFIC CHAPTER
			auto chapter = ((m_command[6] % 10) * 100) + ((m_command[7] % 10) * 10) + (m_command[8] % 10);

			if (m_media_type == JVC_MEDIA_VCD)
			{
				// VCD can only go to 99, so it sticks the data in the first two spots
				chapter /= 10;
			}

			auto status = seek_chapter(chapter);
			LOGCMD("xvd701: Seek chapter %d -> %d\n", chapter, status);
			create_packet(status ? STATUS_OK : STATUS_ERROR, NO_RESPONSE);
		}
		else if (m_command[3] == 0x0c && m_command[4] == 0x50 && m_command[5] == 0x61)
		{
			// FF FF 21 0C 50 61 00 00 00 00 24 PREV (SEEK TO PREVIOUS CHAPTER)
			auto chapter = m_chapter - 1;
			if (m_playback_status != STATUS_PLAYING && chapter == 0)
				chapter = 1;

			auto status = seek_chapter(chapter);
			LOGCMD("xvd701: Seek prev -> %d\n", status);
			create_packet(status ? STATUS_OK : STATUS_ERROR, NO_RESPONSE);
		}
		else if (m_command[3] == 0x0c && m_command[4] == 0x50 && m_command[5] == 0x73)
		{
			// FF FF 21 0C 50 73 00 00 00 00 12 FF (SEEK TO NEXT CHAPTER)
			auto status = seek_chapter(m_chapter + 1);
			LOGCMD("xvd701: Seek FF -> %d\n", status);
			create_packet(status ? STATUS_OK : STATUS_ERROR, NO_RESPONSE);
		}

		else if (m_command[3] == 0x3e && m_command[4] == 0x40 && m_command[5] == 0x60)
		{
			// FF FF 21 3E 40 60 00 00 00 00 03 DEVICE OFF
			LOGCMD("xvd701: Device OFF\n");

			auto status = m_is_powered ? STATUS_OK : STATUS_ERROR;
			if (m_is_powered)
				m_is_powered = false;

			create_packet(status, NO_RESPONSE);

		}
		else if (m_command[3] == 0x3e && m_command[4] == 0x40 && m_command[5] == 0x70)
		{
			// FF FF 21 3E 40 70 00 00 00 00 73 DEVICE ON
			LOGCMD("xvd701: Device ON\n");

			auto status = !m_is_powered ? STATUS_OK : STATUS_ERROR;
			if (!m_is_powered)
				m_is_powered = true;

			create_packet(status, NO_RESPONSE);
		}
		else if (m_command[3] == 0x3e && m_command[4] == 0x4e && m_command[5] == 0x20)
		{
			LOGCMD("xvd701: Device power status request\n");
			const unsigned char response[6] = { m_is_powered, 0x20, 0, 0, 0, 0 };
			create_packet(STATUS_OK, response);
		}
		else if (m_command[3] == 0x7c && m_command[4] == 0x41)
		{
			auto new_id = m_command[5];
			LOGCMD("xvd701: Change JLIP ID to %02x\n", new_id);

			if (new_id > 0 && new_id < 64)
			{
				m_jlip_id = new_id;
				create_packet(STATUS_OK, NO_RESPONSE);
			}
			else
			{
				create_packet(STATUS_ERROR, NO_RESPONSE);
			}
		}
		else if (m_command[3] == 0x7c && m_command[4] == 0x45 && m_command[5] == 0x00)
		{
			LOGCMD("xvd701: Machine code request\n");

			const unsigned char response[6] = { 0x00, 0x01, 0x03, 0x00, 0x03, 0x01 };
			create_packet(STATUS_OK, response);
		}
		else if (m_command[3] == 0x7c && m_command[4] == 0x48 && m_command[5] == 0x20)
		{
			LOGCMD("xvd701: Baud rate request\n");

			// Hardcoded to 9600 baud
			const unsigned char response[6] = { 0x20, 0x00, 0x00, 0x00, 0x00, 0x00 };
			create_packet(STATUS_OK, response);
		}
		else if (m_command[3] == 0x7c && m_command[4] == 0x49 && m_command[5] == 0x00)
		{
			LOGCMD("xvd701: Device code request\n");

			const unsigned char response[6] = { 0x03, 0x0C, 0x7F, 0x7F, 0x7F, 0x7F };
			create_packet(STATUS_OK, response);
		}
		else if (m_command[3] == 0x7c && m_command[4] == 0x4c && m_command[5] == 0x00)
		{
			LOGCMD("xvd701: Device name first half request\n");

			const unsigned char response[6] = { 'D', 'V', 'D', ' ', 'P', 'L' };
			create_packet(STATUS_OK, response);
		}
		else if (m_command[3] == 0x7c && m_command[4] == 0x4d && m_command[5] == 0x00)
		{
			LOGCMD("xvd701: Device name last half request\n");

			const unsigned char response[6] = { 'A', 'Y', 'E', 'R', 0x7F, 0x7F };
			create_packet(STATUS_OK, response);
		}
		else if (m_command[3] == 0x7c && m_command[4] == 0x4e && m_command[5] == 0x20)
		{
			LOGCMD("xvd701: NOP request\n");

			const unsigned char response[6] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
			create_packet(STATUS_OK, response);
		}
	}
}
