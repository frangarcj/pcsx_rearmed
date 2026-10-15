// SPDX-License-Identifier: BSD-3-Clause
// copyright-holders:smf
// Generated from MAME deb2f54fe7300e7a192690318beb860159bfd52a; do not edit.
struct ZS01 {
    uint8_t serial[8];
    uint8_t serial_byte(unsigned index) { return serial[index]; }
    void device_reset();
    void write_cs(int);
    void write_rst(int);
    void write_scl(int);
    void write_sda(int);
    int read_sda();
    void decrypt(uint8_t *, uint8_t *, int, uint8_t *, uint8_t);
    void decrypt2(uint8_t *, uint8_t *, int, uint8_t *, uint8_t);
    void encrypt(uint8_t *, uint8_t *, int, uint8_t *, uint32_t);
    uint16_t calc_crc(uint8_t *, uint32_t);
    int data_offset();
	enum size_t
	{
		SIZE_DATA_BUFFER = 8
	};

	enum command_t
	{
		COMMAND_WRITE = 0x00,
		COMMAND_READ = 0x01
	};

	enum state_t
	{
		STATE_STOP,
		STATE_RESPONSE_TO_RESET,
		STATE_LOAD_COMMAND,
		STATE_READ_DATA
	};

	enum status_t
	{
		STATUS_OK,
		STATUS_ERROR = 2,
	};

	enum configuration_registers_t
	{
		CONFIG_RR = 4, // Retry Register
		CONFIG_RC = 5  // Reset Counter
	};

	int m_cs;
	int m_rst;
	int m_scl;
	int m_sdaw;
	int m_sdar;
	int m_state;
	int m_shift;
	int m_bit;
	int m_byte;
	int m_previous_byte;
	uint8_t m_write_buffer[ 12 ];
	uint8_t m_read_buffer[ 12 ];
	uint8_t m_response_key[ 8 ];
	uint8_t m_response_to_reset[ 4 ];
	uint8_t m_command_key[ 8 ];
	uint8_t m_data_key[ 8 ];
	uint8_t m_configuration_registers[ 8 ];
	uint8_t m_data[ 112 ];
};
void ZS01::device_reset()
{
	memset( m_write_buffer, 0, sizeof( m_write_buffer ) );
	memset( m_read_buffer, 0, sizeof( m_read_buffer ) );
	memset( m_response_key, 0, sizeof( m_response_key ) );

	m_cs = 0;
	m_rst = 0;
	m_scl = 0;
	m_sdaw = 0;
	m_sdar = 0;
	m_state = STATE_STOP;
	m_shift = 0;
	m_bit = 0;
	m_byte = 0;
	m_previous_byte = 0;
}

void ZS01::write_rst(int state)
{
	if( m_rst != state )
	{

	}

	if( m_rst == 0 && state != 0 && m_cs == 0 )
	{

		m_state = STATE_RESPONSE_TO_RESET;
		m_bit = 0;
		m_byte = 0;
	}

	m_rst = state;
}

void ZS01::write_cs(int state)
{
	if( m_cs != state )
	{

	}

//  if( m_cs != 0 && state == 0 )
//  {
//      /* enable chip */
//      m_state = STATE_STOP;
//  }

//  if( m_cs == 0 && state != 0 )
//  {
//      /* disable chip */
//      m_state = STATE_STOP;
//      /* high impendence? */
//      m_sdar = 0;
//  }

	m_cs = state;
}

void ZS01::decrypt( uint8_t *destination, uint8_t *source, int length, uint8_t *key, uint8_t previous_byte )
{
	uint32_t a0;
	uint32_t v1;
	uint32_t v0;
	uint32_t a1;
	uint32_t t1;
	uint32_t t0;

	length--;
	if( length >= 0 )
	{
		do
		{
			t1 = source[ length ];
			a1 = 7;
			t0 = t1;

			do
			{
				v1 = key[ a1 ];
				a1--;
				v0 = v1 & 0x1f;
				v0 = t0 - v0;
				v1 >>= 5;
				v0 &= 0xff;
				a0 = (signed)v0 >> v1;
				v1 = 8 - v1;
				v1 &= 7;
				v0 = (signed)v0 << v1;
				t0 = a0 | v0;
			} while( a1 > 0 );

			v1 = key[ 0 ];
			a0 = previous_byte;
			v0 = t0 & 0xff;
			previous_byte = t1;
			v0 = v0 - v1;
			v0 = v0 ^ a0;

			destination[ length ] = v0;
			length--;
		}
		while( length >= 0 );
	}
}

void ZS01::decrypt2( uint8_t *destination, uint8_t *source, int length, uint8_t *key, uint8_t previous_byte )
{
	uint32_t a0;
	uint32_t v1;
	uint32_t v0;
	uint32_t a1;
	uint32_t t2;
	uint32_t t1;
	uint32_t t0;

	t2 = 0;
	if( length >= 0 )
	{
		do
		{
			t1 = source[ t2 ];
			a1 = 7;
			t0 = t1;

			do
			{
				v1 = key[ a1 ];
				a1--;
				v0 = v1 & 0x1f;
				v0 = t0 - v0;
				v1 >>= 5;
				v0 &= 0xff;
				a0 = (signed)v0 >> v1;
				v1 = 8 - v1;
				v1 &= 7;
				v0 = (signed)v0 << v1;
				t0 = a0 | v0;
			} while( a1 > 0 );

			v1 = key[ 0 ];
			a0 = previous_byte;
			v0 = t0 & 0xff;
			previous_byte = t1;
			v0 = v0 - v1;
			v0 = v0 ^ a0;

			destination[ t2 ] = v0;
			t2++;

		} while( t2 < length );
	}
}

void ZS01::encrypt( uint8_t *destination, uint8_t *source, int length, uint8_t *key, uint32_t previous_byte )
{
	uint32_t t0;
	uint32_t v0;
	uint32_t v1;
	uint32_t a0;
	uint32_t a1;

	length--;
	if( length >= 0 )
	{
		do
		{
			t0 = 1;
			v0 = source[ length ];
			v1 = previous_byte;
			a0 = key[ 0 ];
			v0 ^= v1;
			a0 += v0;

			do
			{
				a1 = key[ t0 ];
				t0++;
				a0 &= 0xff;
				v0 = a1 >> 5;
				v1 = a0 << v0;
				v0 = 8 - v0;
				v0 &= 7;
				a0 = (signed) a0 >> v0;
				v1 |= a0;
				v1 &= 0xff;
				a1 &= 0x1f;
				v1 += a1;
				v0 = (signed) t0 < 8;
				a0 = v1;

			} while( v0 != 0 );

			previous_byte = v1;

			destination[ length ] = a0;
			length--;

		} while( length >= 0 );
	}
}

uint16_t ZS01::calc_crc( uint8_t *buffer, uint32_t length )
{
	uint32_t v1;
	uint32_t a3;
	uint32_t v0;
	uint32_t a2;

	v1 = 0xffff;
	a3 = 0;

	if( length > 0 )
	{
		do
		{
			v0 = buffer[ a3 ];
			a2 = 7;
			v0 = v0 << 8;
			v1 = v1 ^ v0;
			v0 = v1 & 0x8000;

			do
			{
				if( v0 != 0 )
				{
					v0 = v1 << 1;
					v1 = v0 ^ 0x1021;
				}
				else
				{
					v0 = v1 << 1;
					v1 = v1 << 1;
				}

				a2--;
				v0 = v1 & 0x8000;
			} while( (signed) a2 >= 0 );

			a3++;
			v0 = (signed) a3 < (signed) length;
		} while ( v0 != 0 );
	}

	v0 = ~v1 ;
	v0 = v0 & 0xffff;

	return v0;
}

int ZS01::data_offset()
{
	return m_write_buffer[ 1 ] * SIZE_DATA_BUFFER;
}

void ZS01::write_scl(int state)
{
	if( m_scl != state )
	{

	}

	if( m_cs == 0 )
	{
		switch( m_state )
		{
		case STATE_STOP:
			break;

		case STATE_RESPONSE_TO_RESET:
			if( m_scl != 0 && state == 0 )
			{
				if( m_bit == 0 )
				{
					m_shift = m_response_to_reset[ m_byte ];

				}

				m_sdar = ( m_shift >> 7 ) & 1;
				m_shift <<= 1;
				m_bit++;

				if( m_bit == 8 )
				{
					m_bit = 0;
					m_byte++;

					if( m_byte == sizeof( m_response_to_reset ) )
					{
						m_sdar = 1;

						m_state = STATE_STOP;
					}
				}
			}
			break;

		case STATE_LOAD_COMMAND:
			// FIXME: Processing on the rising edge of the clock causes sda to change state while clock is high
			// which is not allowed. Also need to ensure that only valid device-id's and commands are acknowledged.
			if( m_scl == 0 && state != 0 )
			{
				if( m_bit < 8 )
				{

					m_shift <<= 1;

					if( m_sdaw != 0 )
					{
						m_shift |= 1;
					}

					m_bit++;
				}
				else
				{
					m_sdar = 0;

					switch( m_state )
					{
					case STATE_LOAD_COMMAND:
						m_write_buffer[ m_byte ] = m_shift;


						m_byte++;
						if( m_byte == sizeof( m_write_buffer ) )
						{
							decrypt( m_write_buffer, m_write_buffer, sizeof( m_write_buffer ), m_command_key, 0xff );

							// TODO: What is bit 1 of m_write_buffer[0]?

							// Bit 2 seems to be set when the sector is >= 4 and the sector is not 0xfc
							if( ( m_write_buffer[ 0 ] & 4 ) != 0 )
							{
								decrypt2( &m_write_buffer[ 2 ], &m_write_buffer[ 2 ], SIZE_DATA_BUFFER, m_data_key, m_previous_byte );
							}

							uint16_t crc = calc_crc( m_write_buffer, 10 );
							uint16_t msg_crc = ( ( m_write_buffer[ 10 ] << 8 ) | m_write_buffer[ 11 ] );



							if( crc == msg_crc )
							{
								m_configuration_registers[ CONFIG_RC ] = 0; // Reset password fail counter

								switch( m_write_buffer[ 0 ] & 1 )
								{
								case COMMAND_WRITE:
									memset( &m_read_buffer[ 0 ], 0, sizeof( m_write_buffer ) );
									m_read_buffer[ 0 ] = STATUS_OK;

									if ( m_write_buffer[ 1 ] == 0xfd )
									{
										// Erase
										std::fill( std::begin( m_data ), std::end( m_data ), 0 );
										std::fill( std::begin( m_data_key ), std::end( m_data_key ), 0 );
									}
									else if ( m_write_buffer[ 1 ] == 0xfe )
									{
										// Configuration register
										memcpy( m_configuration_registers, &m_write_buffer[ 2 ], SIZE_DATA_BUFFER );
									}
									else if ( m_write_buffer[ 1 ] == 0xff )
									{
										// Set password
										memcpy( m_data_key, &m_write_buffer[ 2 ], SIZE_DATA_BUFFER );
									}
									else if ( data_offset() < sizeof ( m_data ) )
									{
										memcpy( &m_data[ data_offset() ], &m_write_buffer[ 2 ], SIZE_DATA_BUFFER );
									}
									else
									{

									}

									break;

								case COMMAND_READ:
									m_read_buffer[ 0 ] = STATUS_OK;

									if ( m_write_buffer[ 1 ] == 0xfc )
									{
										// TODO: Unknown serial
										// The serial is verified by the same algorithm as the one read from 0x7e8 (DS2401 serial), but the serial is different.
										for (int i = 0; i < SIZE_DATA_BUFFER; i++)
										{
											m_read_buffer[2 + i] = serial_byte(SIZE_DATA_BUFFER - i - 1);
										}
									}
									else if ( m_write_buffer[ 1 ] == 0xfd )
									{
										// DS2401 serial
										/* TODO: use read/write to talk to the ds2401, which will require a timer. */
										for( int i = 0; i < SIZE_DATA_BUFFER; i++ )
										{
											m_read_buffer[ 2 + i ] = serial_byte( SIZE_DATA_BUFFER - i - 1 );
										}
									}
									else if ( m_write_buffer[ 1 ] == 0xfe )
									{
										// Configuration register
										memcpy( &m_read_buffer[ 2 ], m_configuration_registers, SIZE_DATA_BUFFER );
									}
									else if ( data_offset() < sizeof ( m_data ) )
									{
										memcpy( &m_read_buffer[ 2 ], &m_data[ data_offset() ], SIZE_DATA_BUFFER );
									}
									else
									{

									}

									memcpy( m_response_key, &m_write_buffer[ 2 ], sizeof( m_response_key ) );
									break;
								}
							}
							else
							{

								m_read_buffer[ 0 ] = STATUS_ERROR;

								m_configuration_registers[ CONFIG_RC ]++;
								if ( m_configuration_registers[ CONFIG_RC ] >= m_configuration_registers[ CONFIG_RR ] )
								{
									// Too many bad reads, erase data
									std::fill( std::begin( m_data ), std::end( m_data ), 0 );
									std::fill( std::begin( m_data_key ), std::end( m_data_key ), 0 );
								}
							}



							m_previous_byte = m_read_buffer[ 1 ];

							crc = calc_crc( m_read_buffer, 10 );
							m_read_buffer[ 10 ] = crc >> 8;
							m_read_buffer[ 11 ] = crc & 255;

							encrypt( m_read_buffer, m_read_buffer, sizeof( m_read_buffer ), m_response_key, 0xff );

							m_byte = 0;
							m_state = STATE_READ_DATA;
						}
						break;
					}

					m_bit = 0;
					m_shift = 0;
				}
			}
			break;

		case STATE_READ_DATA:
			// FIXME: Processing on the rising edge of the clock causes sda to change state while clock is high
			// which is not allowed.
			if( m_scl == 0 && state != 0 )
			{
				if( m_bit < 8 )
				{
					if( m_bit == 0 )
					{
						switch( m_state )
						{
						case STATE_READ_DATA:
							m_shift = m_read_buffer[ m_byte ];

							break;
						}
					}

					m_sdar = ( m_shift >> 7 ) & 1;
					m_shift <<= 1;
					m_bit++;
				}
				else
				{
					m_bit = 0;
					m_sdar = 0;

					if( m_sdaw == 0 )
					{

						m_byte++;

						if( m_byte == sizeof( m_read_buffer ) )
						{
							m_byte = 0;
							m_sdar = 1;
							m_state = STATE_LOAD_COMMAND;
						}
					}
					else
					{

					}
				}
			}
			break;
		}
	}

	m_scl = state;
}

void ZS01::write_sda(int state)
{
	if( m_sdaw != state )
	{

	}

	if( m_cs == 0 && m_scl != 0 )
	{
//      if( m_sdaw == 0 && state != 0 )
//      {
//
//          m_state = STATE_STOP;
//          m_sdar = 0;
//      }

		if( m_sdaw != 0 && state == 0 )
		{
			switch( m_state )
			{
			case STATE_STOP:

				m_state = STATE_LOAD_COMMAND;
				break;

//          default:
//
//              break;
			}

			m_bit = 0;
			m_byte = 0;
			m_shift = 0;
			m_sdar = 0;
		}
	}

	m_sdaw = state;
}

int ZS01::read_sda()
{
	if( m_cs != 0 )
	{

		return 1;
	}



	return m_sdar;
}
