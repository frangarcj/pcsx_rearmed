// Generated from MAME deb2f54fe7300e7a192690318beb860159bfd52a. BSD-3-Clause.
// See twinkle_serial_LICENSE.txt; framework adapter/defined bounds guards only.
class device_serial_interface : public device_interface
{
public:
	enum
	{
		/* receive is waiting for start bit. The transition from high-low indicates
		start of start bit. This is used to synchronise with the data being transferred */
		RECEIVE_REGISTER_WAITING_FOR_START_BIT = 0x01,

		/* receive is synchronised with data, data bits will be clocked in */
		RECEIVE_REGISTER_SYNCHRONISED = 0x02,

		/* set if receive register has been filled */
		RECEIVE_REGISTER_FULL = 0x04
	};

	enum
	{
		/* register is empty and ready to be filled with data */
		TRANSMIT_REGISTER_EMPTY = 0x0001
	};

	/* parity selections */
	/* if all the bits are added in a byte, if the result is:
	   even -> parity is even
	   odd -> parity is odd
	*/

	enum parity_t
	{
		PARITY_NONE,     /* no parity. a parity bit will not be in the transmitted/received data */
		PARITY_ODD,      /* odd parity */
		PARITY_EVEN,     /* even parity */
		PARITY_MARK,     /* one parity */
		PARITY_SPACE     /* zero parity */
	};

	enum stop_bits_t
	{
		STOP_BITS_0,
		STOP_BITS_1 = 1,
		STOP_BITS_1_5 = 2,
		STOP_BITS_2 = 3
	};

	/* Communication lines.  Beware, everything is active high */
	enum
	{
		CTS = 0x0001, /* Clear to Send.       (INPUT)  Other end of connection is ready to accept data */
		RTS = 0x0002, /* Request to Send.     (OUTPUT) This end is ready to send data, and requests if the other */
						/*                               end is ready to accept it */
		DSR = 0x0004, /* Data Set ready.      (INPUT)  Other end of connection has data */
		DTR = 0x0008, /* Data terminal Ready. (OUTPUT) TX contains new data. */
		RX  = 0x0010, /* Receive data.        (INPUT)  */
		TX  = 0x0020  /* TX = Transmit data.  (OUTPUT) */
	};

	// construction/destruction
	device_serial_interface(device_t &device);
	virtual ~device_serial_interface();

	void rx_w(int state);
	void tx_clock_w(int state);
	void rx_clock_w(int state);
	void clock_w(int state);

protected:
	void set_data_frame(int start_bit_count, int data_bit_count, parity_t parity, stop_bits_t stop_bits);

	void receive_register_reset();
	void receive_register_update_bit(int bit);
	void receive_register_extract();

	void set_rcv_rate(const attotime &rate);
	void set_tra_rate(const attotime &rate);
	void set_rcv_rate(u32 clock, int div) { set_rcv_rate((clock && div) ? (attotime::from_hz(clock) * div) : attotime::never); }
	void set_tra_rate(u32 clock, int div) { set_tra_rate((clock && div) ? (attotime::from_hz(clock) * div) : attotime::never); }
	void set_rcv_rate(int baud) { set_rcv_rate(baud ? attotime::from_hz(baud) : attotime::never); }
	void set_tra_rate(int baud) { set_tra_rate(baud ? attotime::from_hz(baud) : attotime::never); }
	void set_rate(const attotime &rate) { set_rcv_rate(rate); set_tra_rate(rate); }
	void set_rate(u32 clock, int div) { set_rcv_rate(clock, div); set_tra_rate(clock, div); }
	void set_rate(int baud) { set_rcv_rate(baud); set_tra_rate(baud); }

	void transmit_register_reset();
	void transmit_register_add_bit(int bit);
	void transmit_register_setup(u8 data_byte);
	u8 transmit_register_get_data_bit();

	bool is_receive_register_full() const { return m_rcv_flags & RECEIVE_REGISTER_FULL; }
	bool is_transmit_register_empty() const { return m_tra_flags & TRANSMIT_REGISTER_EMPTY; }
	bool is_receive_register_synchronized() const { return m_rcv_flags & RECEIVE_REGISTER_SYNCHRONISED; }
	bool is_receive_register_shifting() const { return m_rcv_bit_count_received > 0; }
	bool is_receive_framing_error() const { return m_rcv_framing_error; }
	bool is_receive_parity_error() const { return m_rcv_parity_error; }

	u8 get_received_char() const { return m_rcv_byte_received; }

	virtual void tra_callback() { }
	virtual void rcv_callback() { receive_register_update_bit(m_rcv_line); }
	virtual void tra_complete() { }
	virtual void rcv_complete() { }

	// interface-level overrides
	virtual void interface_pre_start() override;
	virtual void interface_post_start() override;

	bool m_start_bit_hack_for_external_clocks;

	const char *parity_tostring(parity_t stop_bits);
	const char *stop_bits_tostring(stop_bits_t stop_bits);

private:
	TIMER_CALLBACK_MEMBER(rcv_clock) { rx_clock_w(!m_rcv_clock_state); }
	TIMER_CALLBACK_MEMBER(tra_clock) { tx_clock_w(!m_tra_clock_state); }

	// Data frame
	// number of start bits
	int m_df_start_bit_count;
	// length of word in bits
	u8 m_df_word_length;
	// parity state
	u8 m_df_parity;
	// number of TX stop bits
	u8 m_df_stop_bit_count;
	// min. number of RX stop bits
	u8 m_df_min_rx_stop_bit_count;

	// Receive register
	/* data */
	u16 m_rcv_register_data;
	/* flags */
	u8 m_rcv_flags;
	/* bit count received */
	u8 m_rcv_bit_count_received;
	/* length of data to receive - includes data bits, parity bit and stop bit */
	u8 m_rcv_bit_count;
	/* the byte of data received */
	u8 m_rcv_byte_received;

	bool m_rcv_framing_error;
	bool m_rcv_parity_error;

	// Transmit register
	/* data */
	u16 m_tra_register_data;
	/* flags */
	u8 m_tra_flags;
	/* number of bits transmitted */
	u8 m_tra_bit_count_transmitted;
	/* length of data to send */
	u8 m_tra_bit_count;

	emu_timer *m_rcv_clock;
	emu_timer *m_tra_clock;
	attotime m_rcv_rate;
	attotime m_tra_rate;
	u8 m_rcv_line;

	int m_tra_clock_state, m_rcv_clock_state;

	void tra_edge();
	void rcv_edge();
};



class ins8250_uart_device : public device_t, public device_serial_interface
{
public:
	auto out_tx_callback() { return m_out_tx_cb.bind(); }
	auto out_dtr_callback() { return m_out_dtr_cb.bind(); }
	auto out_rts_callback() { return m_out_rts_cb.bind(); }
	auto out_int_callback() { return m_out_int_cb.bind(); }
	auto out_out1_callback() { return m_out_out1_cb.bind(); }
	auto out_out2_callback() { return m_out_out2_cb.bind(); }

	void ins8250_w(offs_t offset, u8 data);
	u8 ins8250_r(offs_t offset);

	void dcd_w(int state);
	void dsr_w(int state);
	void ri_w(int state);
	void cts_w(int state);
	void rx_w(int state);
	int intrpt_r();

protected:
	enum class dev_type {
		INS8250,
		INS8250A,
		NS16450,
		NS16550,
		NS16550A
	};

	ins8250_uart_device(Scheduler &,u32);

	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;
	virtual void rcv_complete() override;
	virtual void tra_complete() override;
	virtual void tra_callback() override;

	virtual void set_fcr(u8 data) { }
	virtual void push_tx(u8 data) { }
	virtual u8 pop_rx() { return 0; }

	void trigger_int(int flag);
	void clear_int(int flag);

	void update_baud_rate();

	const dev_type m_device_type;
	struct {
		u8 thr;  /* 0 -W transmitter holding register */
		u8 rbr;  /* 0 R- receiver buffer register */
		u8 ier;  /* 1 RW interrupt enable register */
		u16 dl;  /* 0/1 RW divisor latch (if DLAB = 1) */
		u8 iir;  /* 2 R- interrupt identification register */
		u8 fcr;
		u8 lcr;  /* 3 RW line control register (bit 7: DLAB) */
		u8 mcr;  /* 4 RW modem control register */
		u8 lsr;  /* 5 R- line status register */
		u8 msr;  /* 6 R- modem status register */
		u8 scr;  /* 7 RW scratch register */
	} m_regs;
private:
	u8 m_int_pending;

	Line    m_out_tx_cb;
	Line    m_out_dtr_cb;
	Line    m_out_rts_cb;
	Line    m_out_int_cb;
	Line    m_out_out1_cb;
	Line    m_out_out2_cb;

	void update_interrupt();
	void update_msr();

	int m_txd;
	int m_rxd;
	int m_dcd;
	int m_dsr;
	int m_ri;
	int m_cts;
};


class ns16550_device : public ins8250_uart_device
{
public:
	ns16550_device(Scheduler &,u32);
protected:
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;

	virtual void rcv_complete() override;
	virtual void tra_complete() override;
	virtual void set_fcr(u8 data) override;
	virtual void push_tx(u8 data) override;
	virtual u8 pop_rx() override;

	TIMER_CALLBACK_MEMBER(timeout_expired);
private:
	void set_timer() { if (!m_regs.dl) throw Fault("UART FIFO timeout with zero divisor"); m_timeout->adjust(attotime::from_hz((clock()*4*8)/(m_regs.dl*16))); }
	int m_rintlvl;
	u8 m_rfifo[16];
	u8 m_efifo[16];
	u8 m_tfifo[16];
	int m_rhead, m_rtail, m_rnum;
	int m_thead, m_ttail;
	emu_timer *m_timeout;
};
