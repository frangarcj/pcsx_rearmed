// Generated from MAME deb2f54fe7300e7a192690318beb860159bfd52a. BSD-3-Clause; smf.
// Framework logging elided by host macros; pin and page state machine unchanged.
constexpr int STATE_IDLE(0);
constexpr int STATE_DEVSEL(1);
constexpr int STATE_ADDRESSHIGH(2);
constexpr int STATE_ADDRESSLOW(3);
constexpr int STATE_DATAIN(4);
constexpr int STATE_READSELACK(5);
constexpr int STATE_DATAOUT(6);
constexpr int STATE_RESET(7);

constexpr int DEVSEL_RW(1);
constexpr int DEVSEL_ADDRESS(0xfe);


void M24c02::write_e0(int state)
{
	state &= 1;
	if( m_e0 != state )
	{
		LOGMASKED( LOG_WRITELINE, "%s: E0 = %d\n", machine().describe_context(), state );
		m_e0 = state;
	}
}

void M24c02::write_e1(int state)
{
	state &= 1;
	if( m_e1 != state )
	{
		LOGMASKED( LOG_WRITELINE, "%s: E1 = %d\n", machine().describe_context(), state );
		m_e1 = state;
	}
}

void M24c02::write_e2(int state)
{
	state &= 1;
	if( m_e2 != state )
	{
		LOGMASKED( LOG_WRITELINE, "%s: E2 = %d\n", machine().describe_context(), state );
		m_e2 = state;
	}
}

void M24c02::write_sda(int state)
{
	state &= 1;
	if( m_sdaw != state )
	{
		LOGMASKED( LOG_WRITELINE, "%s: SDA = %d @ %s (%d)\n", machine().describe_context(), state, machine().time().to_string(), state );
		m_sdaw = state;

		// Ignore transitions on SDA while device is driving it low
		if( m_scl && m_sdar )
		{
			if( m_sdaw )
			{
				if( m_page_written_size > 0 )
				{
					int base = data_offset();
					int root = base & ~( m_write_page_size - 1 );
					for( int i = 0; i < m_page_written_size; i++ )
						m_data[root | ((base + i) & (m_write_page_size - 1))] = m_page[i];
					LOGMASKED( LOG_STATE, "EEPROM data[ %04x to %04x ] = %x bytes\n", base, root | ((base + m_page_written_size - 1) & (m_write_page_size - 1)), m_page_written_size );

					m_page_written_size = 0;
				}
				LOGMASKED( LOG_STATE, "%s: I2C stop\n", machine().describe_context() );
				m_state = STATE_IDLE;
			}
			else
			{
				LOGMASKED( LOG_STATE, "%s: I2C start\n", machine().describe_context() );
				m_state = STATE_DEVSEL;
				m_bits = 0;
			}
		}
	}
}

void M24c02::write_scl(int state)
{
	if( m_scl != state )
	{
		m_scl = state;
		LOGMASKED( LOG_WRITELINE, "%s: SCL = %d @ %s (%d)\n", machine().describe_context(), m_scl, machine().time().to_string(), m_state );

		switch( m_state )
		{
		case STATE_DEVSEL:
		case STATE_ADDRESSHIGH:
		case STATE_ADDRESSLOW:
		case STATE_DATAIN:
			if( m_bits < 8 )
			{
				if( m_scl )
				{
					m_shift = ( ( m_shift << 1 ) | m_sdaw ) & 0xff;
					m_bits++;
				}
			}
			else
			{
				if( m_scl )
				{
					m_bits++;
				}
				else
				{
					if( m_bits == 8 )
					{
						switch( m_state )
						{
						case STATE_DEVSEL:
							m_devsel = m_shift;

							if( m_devsel == 0 && !m_devsel_address_low )
							{
								// TODO: Atmel datasheets document 2-wire software reset, but doesn't mention it will lower sda only that it will release it.
								// ltv_naru however requires it to be lowered, but we don't currently know the manufacturer of the chip used.
								LOGMASKED( LOG_STATE, "I2C software reset\n" );
								m_state = STATE_RESET;
							}
							else if( !select_device() )
							{
								LOGMASKED( LOG_STATE, "I2C devsel %02x: not this device\n", m_devsel );
								m_state = STATE_IDLE;
							}
							else if( ( m_devsel & DEVSEL_RW ) == 0 )
							{
								if (m_devsel_address_low)
								{
									LOGMASKED( LOG_STATE, "I2C devsel %02x: write (Xicor special, address %02x)\n", m_devsel, m_devsel >> 1);
									m_byteaddr = (m_devsel & DEVSEL_ADDRESS) >> 1;
									m_page_offset = 0;
									m_page_written_size = 0;
									m_state = STATE_DATAIN;
								}
								else
								{
									LOGMASKED( LOG_STATE, "I2C devsel %02x: write\n", m_devsel );
									m_state = skip_addresshigh() ? STATE_ADDRESSLOW : STATE_ADDRESSHIGH;
								}
							}
							else
							{
								if (m_devsel_address_low)
								{
									LOGMASKED( LOG_STATE, "I2C devsel %02x: read (Xicor special, address %02x)\n", m_devsel, m_devsel >> 1);
									m_byteaddr = (m_devsel & DEVSEL_ADDRESS) >> 1;
								}
								else
								{
									LOGMASKED( LOG_STATE, "I2C devsel %02x: read\n", m_devsel );
								}
								m_state = STATE_READSELACK;
							}
							break;

						case STATE_ADDRESSHIGH:
							m_addresshigh = m_shift;

							LOGMASKED( LOG_STATE, "EEPROM addresshigh %02x\n", m_addresshigh);

							m_state = STATE_ADDRESSLOW;
							break;

						case STATE_ADDRESSLOW:
							m_byteaddr = m_shift | (skip_addresshigh() ? ((m_devsel & DEVSEL_ADDRESS) << 7) & address_mask() : m_addresshigh << 8);
							m_page_offset = 0;
							m_page_written_size = 0;

							LOGMASKED( LOG_STATE, "EEPROM addresslow %02x (byteaddr %04x)\n", m_shift, m_byteaddr );

							m_state = STATE_DATAIN;
							break;

						case STATE_DATAIN:
							if( m_wc )
							{
								logerror( "write not enabled\n" );
								m_state = STATE_IDLE;
							}
							else if( m_write_page_size > 0 )
							{
								m_page[ m_page_offset ] = m_shift;
								LOGMASKED( LOG_STATE, "EEPROM page[ %04x ] <- %02x\n", m_page_offset, m_page[ m_page_offset ] );

								m_page_offset++;
								if( m_page_offset == m_write_page_size )
									m_page_offset = 0;
								m_page_written_size++;
								if( m_page_written_size > m_write_page_size)
									m_page_written_size = m_write_page_size;
							}
							else
							{
								int offset = data_offset();

								LOGMASKED( LOG_STATE, "EEPROM data[ %04x ] <- %02x\n", offset, m_shift );
								m_data[ offset ] = m_shift;

								m_byteaddr++;
							}
							break;
						}

						if( m_state != STATE_IDLE )
						{
							// I2C acknowledge
							m_sdar = 0;
						}
					}
					else
					{
						m_bits = 0;
						m_sdar = 1;
					}
				}
			}
			break;

		case STATE_READSELACK:
			m_bits = 0;
			m_state = STATE_DATAOUT;
			break;

		case STATE_DATAOUT:
			if( m_bits < 8 )
			{
				if( m_scl )
				{
					m_bits++;
				}
				else
				{
					if( m_bits == 0 )
					{
						int offset = data_offset();

						m_shift = m_data[offset];
						LOGMASKED( LOG_STATE, "EEPROM data[ %04x ] -> %02x\n", offset, m_shift );
						m_byteaddr = (m_byteaddr & ~(m_read_page_size - 1)) | ((m_byteaddr + 1) & (m_read_page_size - 1));
					}

					m_sdar = ( m_shift >> 7 ) & 1;

					m_shift = ( m_shift << 1 ) & 0xff;
				}
			}
			else
			{
				if( m_scl )
				{
					if( m_sdaw )
					{
						LOGMASKED( LOG_STATE, "%s: I2C nack\n", machine().describe_context() );
						m_state = STATE_IDLE;
					}

					m_bits = 0;
				}
				else
				{
					m_sdar = 1;
				}
			}
			break;

		case STATE_RESET:
			if( m_scl )
			{
				if( m_bits > 8 )
				{
					LOGMASKED( LOG_STATE, "%s: I2C software reset ack\n", machine().describe_context() );
					m_state = STATE_IDLE;
					m_sdar = 1;
				}
				m_bits++;
			}
			break;
		}
	}
}

void M24c02::write_wc(int state)
{
	state &= 1;
	if( m_wc != state )
	{
		LOGMASKED( LOG_WRITELINE, "%s: WC = %d\n", machine().describe_context(), state );
		m_wc = state;
	}
}

int M24c02::read_sda()
{
	int res = m_sdar & 1;
	// https://github.com/mamedev/mame/issues/13998
	// pullup write value if reading doesn't belong to this device
	if (m_state == STATE_IDLE)
		res = m_sdaw & 1;
	if( !machine().side_effects_disabled() )
		LOGMASKED( LOG_READLINE, "%s: SDA = %d (%d)\n", machine().describe_context(), res, m_state );

	return res;
}

int M24c02::address_mask()
{
	return (m_data_size - 1);
}

int M24c02::select_device()
{
	if ( m_devsel_address_low )
	{
		// Due to a full address and read/write flag fitting in one 8-bit packet, the Xicor X24C01 replies on all addresses.
		return 1;
	}

	int device = ( m_slave_address & 0xf0 ) | ( m_e2 << 3 ) | ( m_e1 << 2 ) | ( m_e0 << 1 );
	int mask = DEVSEL_ADDRESS & ~( skip_addresshigh() ? address_mask() >> 7 : 0 );

	if( ( m_devsel & mask ) == ( device & mask ) )
	{
		return 1;
	}

	return 0;
}

int M24c02::data_offset()
{
	return m_byteaddr & address_mask();
}
